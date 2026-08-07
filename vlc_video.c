#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include "vlc_core.h"
#include "vlc_dynamic.h"

/*
 * Video path
 * ==========
 *
 * libVLC composes video + SPU on its own vout thread; retro_run() consumes
 * frames on the frontend thread. Two independent constraints shape this code.
 *
 * 1. video_cb() must never be called with a lock held.
 *
 *    It blocks for as long as the frontend needs - vsync, texture upload,
 *    EmuVR compositing. An earlier version held the mutex that also guards the
 *    audio ring across that call, which stalled both the vout and the audio
 *    decoder once per frame. A DVD still-frame menu is driven entirely by vout
 *    refreshes, so stalling the vout is exactly the wrong thing to do there.
 *
 * 2. lock_cb() may be called again before unlock_cb() returns.
 *
 *    This is the part that differs between the two runtimes VLChannel ships, and
 *    it is not a detail:
 *
 *      3.0.x  vmem calls lock, copies the picture, calls unlock - strictly
 *             paired, one picture at a time.
 *      2.2.x  vmem installs these as the picture POOL's lock/unlock hooks.
 *             They fire when a picture is checked out of the pool and returned
 *             to it, and the pool hands out several pictures at once.
 *
 *    Holding one mutex from lock_cb to unlock_cb therefore deadlocks the vout
 *    thread on 2.2.8 the moment a second picture is requested while the first
 *    is still checked out - which is what happens when the pipeline refills
 *    after a seek. The picture freezes, subtitle switches appear to do nothing
 *    because the vout can no longer re-composite, and audio keeps playing
 *    because the audio thread is untouched.
 *
 * So the buffers are a small pool of slots instead of a swap chain, and
 * lock_cb releases the mutex before returning. Overlapping locks each get
 * their own slot; if the pool is ever exhausted, the picture is written to a
 * scratch buffer and dropped rather than blocking the vout thread.
 */

#define VIDEO_SLOT_COUNT 4
#define VIDEO_SLOT_BYTES ((size_t)MAX_W * (size_t)MAX_H * 4u)

static pthread_mutex_t vbuf_mutex = PTHREAD_MUTEX_INITIALIZER;

static uint32_t *slot_pixels[VIDEO_SLOT_COUNT];
static bool      slot_locked[VIDEO_SLOT_COUNT];  /* libVLC is writing into it */
static uint64_t  slot_serial[VIDEO_SLOT_COUNT];  /* completion order, 0 = empty */
static int       slot_presenting = -1;           /* handed to the frontend    */
static uint32_t *scratch_pixels = NULL;          /* overflow sink             */

static unsigned vbuf_width  = 0;
static unsigned vbuf_height = 0;
static unsigned vbuf_pitch  = 0;
static uint64_t vbuf_serial = 0;                 /* frames completed by libVLC */
static unsigned output_width  = MAX_W;
static unsigned output_height = MAX_H;

static bool overlap_reported = false;
static bool overflow_reported = false;

/* Both helpers must be called with vbuf_mutex held. */
static bool ensure_slot(int index) {
    if (slot_pixels[index])
        return true;
    slot_pixels[index] = (uint32_t *)calloc(1, VIDEO_SLOT_BYTES);
    if (!slot_pixels[index])
        fprintf(stderr, "[VLC] CRITICAL: video slot %d allocation failed\n",
                index);
    return slot_pixels[index] != NULL;
}

static bool ensure_scratch(void) {
    if (!scratch_pixels)
        scratch_pixels = (uint32_t *)calloc(1, VIDEO_SLOT_BYTES);
    return scratch_pixels != NULL;
}

static void *lock_cb(void *data, void **planes) {
    (void)data;

    pthread_mutex_lock(&vbuf_mutex);

    int chosen = -1;
    int already_locked = 0;

    if (vbuf_width > 0 && vbuf_height > 0) {
        for (int i = 0; i < VIDEO_SLOT_COUNT; i++)
            if (slot_locked[i])
                already_locked++;

        /* An empty slot first, otherwise the slot holding the oldest frame. */
        for (int i = 0; i < VIDEO_SLOT_COUNT; i++) {
            if (slot_locked[i] || i == slot_presenting)
                continue;
            if (slot_serial[i] == 0) {
                chosen = i;
                break;
            }
            if (chosen < 0 || slot_serial[i] < slot_serial[chosen])
                chosen = i;
        }
    }

    if (already_locked > 0 && !overlap_reported) {
        overlap_reported = true;
        fprintf(stderr,
                "[VLC] Video pool: libVLC holds %d picture(s) at once on this "
                "runtime; using one slot per picture\n", already_locked);
    }

    if (chosen >= 0 && ensure_slot(chosen)) {
        slot_locked[chosen] = true;
        slot_serial[chosen] = 0;      /* contents invalid while being written */
        *planes = slot_pixels[chosen];
        pthread_mutex_unlock(&vbuf_mutex);
        /* The id is echoed back to unlock_cb; +1 keeps NULL meaning "drop". */
        return (void *)(intptr_t)(chosen + 1);
    }

    /* Pool exhausted or format not negotiated: drop, never block. */
    if (!overflow_reported) {
        overflow_reported = true;
        fprintf(stderr,
                "[VLC] Video pool exhausted (%d slots); dropping a picture "
                "instead of stalling the vout thread\n", VIDEO_SLOT_COUNT);
    }
    *planes = ensure_scratch() ? scratch_pixels : NULL;
    pthread_mutex_unlock(&vbuf_mutex);
    return NULL;
}

static void unlock_cb(void *data, void *id, void *const *planes) {
    (void)data;
    (void)planes;

    int index = (int)(intptr_t)id - 1;
    if (index < 0 || index >= VIDEO_SLOT_COUNT)
        return;                        /* scratch picture, nothing to publish */

    pthread_mutex_lock(&vbuf_mutex);
    slot_locked[index] = false;
    slot_serial[index] = ++vbuf_serial;
    pthread_mutex_unlock(&vbuf_mutex);
}

static unsigned setup_format_cb(
    void **opaque,
    char *chroma,
    unsigned *width,
    unsigned *height,
    unsigned *pitches,
    unsigned *lines
) {
    (void)opaque;

    unsigned source_w = *width;
    unsigned source_h = *height;
    pthread_mutex_lock(&vbuf_mutex);
    unsigned w = output_width;
    unsigned h = output_height;
    pthread_mutex_unlock(&vbuf_mutex);

    fprintf(stderr,
            "[VLC] setup_format_cb: source %ux%u -> output area %ux%u "
            "(was %ux%u)\n",
            source_w, source_h, w, h,
            core.video_width, core.video_height);

    if (source_w == 0 || source_h == 0) {
        fprintf(stderr, "[VLC] setup_format_cb: rejecting degenerate source\n");
        return 0;
    }

    memcpy(chroma, "RV32", 4);

    /*
     * Request the final libretro canvas directly from libVLC. Its SIMD swscale
     * path can resize I420 and convert it to RV32 in one pass; previously it
     * converted at the native resolution and the core then performed a second,
     * scalar bilinear pass over every 1920x1080 output pixel.
     *
     * This deliberately stretches the source, matching the existing fixed
     * compositor behavior used by EmuVR. MAX_W/MAX_H are even, which also keeps
     * chroma conversion and SPU blending aligned.
     */

    pthread_mutex_lock(&vbuf_mutex);
    vbuf_width  = w;
    vbuf_height = h;
    vbuf_pitch  = w * 4u;

    /*
     * Everything held so far describes the previous format. Slots are never
     * freed - they stay allocated at the maximum size - so a slot libVLC is
     * still writing into remains valid memory; only its contents stop being
     * publishable.
     */
    for (int i = 0; i < VIDEO_SLOT_COUNT; i++)
        slot_serial[i] = 0;
    slot_presenting = -1;
    pthread_mutex_unlock(&vbuf_mutex);

    pthread_mutex_lock(&core.mutex);
    core.video_width  = w;
    core.video_height = h;
    core.video_pitch  = w * 4u;
    pthread_mutex_unlock(&core.mutex);

    *width   = w;
    *height  = h;
    *pitches = w * 4u;
    *lines   = h;

    fprintf(stderr, "[VLC] setup_format_cb: negotiated %ux%u pitch=%u\n",
            w, h, w * 4u);
    return 1;
}

static void video_display_cb(void *data, void *id) {
    (void)data;
    (void)id;
    /* Publication already happened in unlock_cb(). */
}

void vlc_video_setup_callbacks(libvlc_media_player_t *mp) {
    libvlc_video_set_callbacks(mp, lock_cb, unlock_cb, video_display_cb, NULL);
    libvlc_video_set_format_callbacks(mp, setup_format_cb, NULL);
}

void vlc_video_set_output_size(unsigned width, unsigned height) {
    if (width < 2 || width > MAX_W || height < 2 || height > MAX_H)
        return;

    /* RV32 has no chroma restriction, but even dimensions keep libVLC's
     * preceding YUV conversion on its fastest and most compatible path. */
    width &= ~1u;
    height &= ~1u;

    pthread_mutex_lock(&vbuf_mutex);
    output_width = width;
    output_height = height;
    pthread_mutex_unlock(&vbuf_mutex);
}

/*
 * Hands out the newest complete frame. The slot stays reserved until the next
 * call, so the pointer remains valid for the video_cb() that follows.
 */
bool vlc_video_acquire_frame(
    const uint32_t **frame,
    unsigned *width,
    unsigned *height,
    unsigned *pitch
) {
    bool have_frame = false;

    pthread_mutex_lock(&vbuf_mutex);

    int newest = -1;
    for (int i = 0; i < VIDEO_SLOT_COUNT; i++) {
        if (slot_locked[i] || slot_serial[i] == 0)
            continue;
        if (newest < 0 || slot_serial[i] > slot_serial[newest])
            newest = i;
    }
    if (newest >= 0)
        slot_presenting = newest;

    if (slot_presenting >= 0 &&
        slot_pixels[slot_presenting] &&
        vbuf_width > 0 && vbuf_height > 0) {
        if (frame)  *frame  = slot_pixels[slot_presenting];
        if (width)  *width  = vbuf_width;
        if (height) *height = vbuf_height;
        if (pitch)  *pitch  = vbuf_pitch;
        have_frame = true;
    }

    pthread_mutex_unlock(&vbuf_mutex);
    return have_frame;
}

bool vlc_video_has_frame(void) {
    pthread_mutex_lock(&vbuf_mutex);
    bool ok = false;
    if (vbuf_width > 0 && vbuf_height > 0) {
        if (slot_presenting >= 0 && slot_pixels[slot_presenting])
            ok = true;
        for (int i = 0; !ok && i < VIDEO_SLOT_COUNT; i++)
            if (!slot_locked[i] && slot_serial[i] > 0)
                ok = true;
    }
    pthread_mutex_unlock(&vbuf_mutex);
    return ok;
}

/* Number of frames libVLC has actually finished compositing. */
uint64_t vlc_video_render_count(void) {
    pthread_mutex_lock(&vbuf_mutex);
    uint64_t count = vbuf_serial;
    pthread_mutex_unlock(&vbuf_mutex);
    return count;
}

void vlc_video_reset(void) {
    pthread_mutex_lock(&vbuf_mutex);
    for (int i = 0; i < VIDEO_SLOT_COUNT; i++) {
        if (slot_pixels[i] && !slot_locked[i])
            memset(slot_pixels[i], 0, VIDEO_SLOT_BYTES);
        slot_serial[i] = 0;
    }
    slot_presenting = -1;
    pthread_mutex_unlock(&vbuf_mutex);
}

void vlc_video_shutdown(void) {
    pthread_mutex_lock(&vbuf_mutex);
    for (int i = 0; i < VIDEO_SLOT_COUNT; i++) {
        free(slot_pixels[i]);
        slot_pixels[i] = NULL;
        slot_locked[i] = false;
        slot_serial[i] = 0;
    }
    free(scratch_pixels);
    scratch_pixels = NULL;
    slot_presenting = -1;
    vbuf_width = vbuf_height = vbuf_pitch = 0;
    overlap_reported = false;
    overflow_reported = false;
    pthread_mutex_unlock(&vbuf_mutex);
}
