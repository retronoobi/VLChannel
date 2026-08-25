/*
 * VLChannel - a libretro core that plays IPTV channel lists through libVLC.
 *
 * Derived from VLCine, the DVD core, and deliberately narrower: there are no
 * menus, no chapters, no SPU navigation and no still frames here. What replaces
 * them is a channel list and everything a live network source drags in - a
 * connection that takes seconds to open, a picture size that is only known
 * after it does, and a stream that can simply stop.
 *
 * What was kept from VLCine, because it was the expensive part to get right:
 *
 *   - the isolated runtime loader (vlc_dynamic.c);
 *   - the vmem/amem callbacks and the triple buffered frame store;
 *   - the PCM ring buffer, its prebuffer and its fade in and out;
 *   - the frontend pause watchdog;
 *   - presenting every source inside a fixed output canvas, 1920x1080 by
 *     default or 1280x720 for frontends that have trouble with 1080p.
 *
 * What is new here:
 *
 *   - the M3U reader (iptv_playlist.c) and channel switching;
 *   - network caching and per channel libVLC options;
 *   - live-aware pause: a stream resumed after a long frontend pause is
 *     reopened rather than continued, because a live source has moved on.
 *
 * Scope of this version is on purpose the smallest thing that can be judged
 * honestly: it opens a list, plays a channel, changes channel and reports what
 * happened. Automatic reconnection, an on-screen channel list and quality
 * selection are the next steps, not this one.
 */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <math.h>
#include <ctype.h>
#include "vlc_core.h"
#include "libretro.h"
#include "vlc_dynamic.h"
#include "iptv_playlist.h"
#include "iptv_log.h"
#include "iptv_scrub.h"
#include "iptv_osd.h"
#include "iptv_text.h"
#include "iptv_zap.h"
#include "iptv_epg.h"
#include "iptv_sequence.h"
#include "iptv_ytdlp.h"
#include "iptv_streamlink.h"
#include "iptv_transport.h"
#include "iptv_drift.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <time.h>
#endif

vlc_core_ctx core = {0};
retro_video_refresh_t      video_cb = NULL;
retro_audio_sample_batch_t audio_batch_cb = NULL;
retro_environment_t        environ_cb = NULL;
retro_input_poll_t         input_poll_cb = NULL;
retro_input_state_t        input_state_cb = NULL;

/* ------------------------------------------------------------------ state */

static iptv_playlist playlist = {0};
static int current_channel = -1;
static char playlist_path[VLCHANNEL_PATH_MAX] = {0};
/*
 * True only until an address that came from a resolver reaches Playing.
 *
 * A resolver succeeding proves that an address was produced, not that the CDN
 * will serve it: a signed URL can be rejected on the spot with 403, an expired
 * signature or the wrong request headers. This flag gives that case one chance
 * through the proxy rather than counting it as a video that will not play.
 *
 * It used to be inferred from ".googlevideo.com/" appearing in the address,
 * which was true of yt-dlp's answers and of nothing else. streamlink answers
 * about Twitch with a ttvnw.net address, so the test would have quietly stopped
 * being about "did this come from a resolver" and started being about "is this
 * YouTube". It is passed in now, by the one caller that knows.
 */
static bool current_open_is_resolved = false;

/* Set when libVLC had no audio output to shift yet; see apply_audio_delay(). */
static bool audio_delay_pending = false;
static int audio_delay_pending_value = 0;


/*
 * A channel change is requested by input or by an option change and carried out
 * at the top of the next retro_run. Nothing calls libvlc_media_player_stop()
 * from inside the input handling: keeping every open and close on the libretro
 * thread, in one place, is what makes a hung channel a readable log line rather
 * than a race.
 */
static int  pending_channel = -1;
static bool pending_reload = false;
static const char *pending_reason = "";

static bool prev_left = false, prev_right = false;
static bool prev_up = false, prev_down = false;
static bool prev_reload = false, prev_first = false;
static bool prev_guide = false, prev_sched = false;
/* The transport four, edge triggered like the rest: a held button seeks ten
 * seconds once, not ten seconds per frame. */
static bool prev_subs = false, prev_track = false;
static bool prev_back = false, prev_forward = false;
static bool prev_pause = false;
static bool runtime_options_dirty = true;

/*
 * The vmem output asks libVLC to convert and scale every source directly to the
 * selected fixed canvas. The picture handed to libretro therefore keeps one
 * exact OSD coordinate system without a second scalar RGB scaling pass in this
 * core. MAX_W/MAX_H remain the allocation ceiling; the active canvas can be
 * smaller without allocating a second set of buffers.
 *
 * Two canvases are intentional: the clean one holds the stretched picture,
 * while the composed one is a disposable copy on which the OSD is drawn. This
 * prevents a static frame from accumulating the overlay each run.
 */
#define OUTPUT_MAX_WIDTH  MAX_W
#define OUTPUT_MAX_HEIGHT MAX_H

static uint32_t *osd_clean = NULL;
static uint32_t *osd_composed = NULL;
static float output_content_aspect = 16.0f / 9.0f;
static unsigned output_width = OUTPUT_MAX_WIDTH;
static unsigned output_height = OUTPUT_MAX_HEIGHT;
static unsigned output_pitch = OUTPUT_MAX_WIDTH * (unsigned)sizeof(uint32_t);
static unsigned output_view_width = OUTPUT_MAX_WIDTH;
static unsigned output_view_height = OUTPUT_MAX_HEIGHT;
static int output_shift_x_percent = 0;
static int output_shift_y_percent = 0;
static bool output_layout_dirty = true;
static bool output_clear_pending = true;
static uint64_t output_last_render_count = UINT64_MAX;
static unsigned output_last_source_width = 0;
static unsigned output_last_source_height = 0;
static bool no_signal_active = false;
static bool no_signal_frame_ready = false;
static bool output_resolution_initialised = false;
static bool output_resolution_reload_notice_shown = false;

static libvlc_state_t last_reported_state = libvlc_NothingSpecial;

/*
 * Frames since the current channel was opened, used only for the "still
 * opening" heartbeat. A channel that hangs while connecting produces no core
 * output at all otherwise: the first captured log of a frozen HLS playlist had
 * eight thousand libVLC lines and not one line from the core between "Opening
 * the stream" and shutdown.
 */
static uint32_t opening_frames = 0;

/*
 * Playlist reloads reported by libVLC since the channel was opened, counted in
 * log_cb. A live playlist is meant to be reloaded now and then; dozens of times
 * before the first segment means the server is serving a frozen one.
 */
static atomic_uint playlist_reloads = 0;
static bool frozen_playlist_reported = false;

/*
 * Sample aspect ratio of the current stream, harvested from libVLC's log.
 *
 * The frontend needs the display aspect, which is (width * sar_num) /
 * (height * sar_den). Anamorphic MPEG-2 channels are common on IPTV - 720x576
 * carrying 16:9 - and reporting width/height directly squeezes them. There is
 * no public libVLC call for this, but the log line
 *
 *   original format sz 1920x1080, of (0,0), vsz 1920x1080, 4cc I420, sar 1:1
 *
 * carries it, and the core already reads the log.
 */
static pthread_mutex_t video_sar_mutex = PTHREAD_MUTEX_INITIALIZER;
static int video_sar_num = 0;
static int video_sar_den = 0;
static bool video_sar_dirty = false;

/* -------------------------------------------------- frontend pause watchdog */

static pthread_t frontend_pause_thread;
static pthread_mutex_t frontend_pause_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool frontend_pause_thread_started = false;
static bool frontend_pause_stop = false;
static bool frontend_pause_in_progress = false;
static bool frontend_paused_vlc = false;
static uint64_t frontend_last_run_ms = 0;
static uint64_t frontend_run_generation = 0;
static uint64_t frontend_pause_started_ms = 0;
/* Published under frontend_pause_mutex and kept alive until the watchdog has
 * been joined, so that thread never reads core.mp concurrently. */
static libvlc_media_player_t *frontend_pause_player = NULL;

/*
 * How long the frontend may hold the core paused before the channel is
 * reopened instead of resumed. A live stream keeps moving while RetroArch's
 * menu is open, so continuing from the paused position means playing further
 * and further behind, with libVLC quietly buffering the difference.
 */
#define LIVE_RESUME_RELOAD_MS 4000

/*
 * The frontend's own log, when it offers one.
 *
 * RetroArch's --log-file does not capture the core's stderr - two independent
 * streams, a lesson inherited from VLCine - and under EmuVR stderr goes
 * nowhere at all: EmuVR launches RetroArch with --log-file and no console. The
 * verbose diagnosis still belongs on stderr, but the handful of lines that
 * answer "what did the core actually do" are mirrored here, so an EmuVR log is
 * useful on its own.
 */
static retro_log_printf_t frontend_log = NULL;

static void note(const char *format, ...) {
    char message[1024];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);

    /*
     * Scrubbed here as well as in iptv_log, because this line goes to two
     * places and only one of them is ours. The frontend's own log file is
     * RetroArch's, written by RetroArch, and it would otherwise keep a copy of
     * everything the core just took the trouble to remove.
     *
     * Doing it twice costs a scan of a line that has nothing left to find, and
     * iptv_scrub is idempotent precisely so that this can be the arrangement
     * rather than a rule about who calls what.
     */
    iptv_scrub(message);

    iptv_log("[VLChannel] %s\n", message);
    if (frontend_log)
        frontend_log(RETRO_LOG_INFO, "[VLChannel] %s\n", message);
}

static uint64_t monotonic_time_ms(void) {
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

static bool option_equals(const char *key, const char *value) {
    struct retro_variable var = { .key = key };
    return environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value &&
           strcmp(var.value, value) == 0;
}

static const char *option_value(const char *key, const char *fallback) {
    struct retro_variable var = { .key = key };
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
        return var.value;
    return fallback;
}

static void *frontend_pause_watchdog(void *unused) {
    (void)unused;

    for (;;) {
#ifdef _WIN32
        Sleep(50);
#else
        usleep(50000);
#endif

        uint64_t now = monotonic_time_ms();
        uint64_t captured_generation = 0;
        libvlc_media_player_t *mp = NULL;
        bool should_pause = false;

        pthread_mutex_lock(&frontend_pause_mutex);
        if (frontend_pause_stop) {
            pthread_mutex_unlock(&frontend_pause_mutex);
            break;
        }

        if (!frontend_paused_vlc &&
            !frontend_pause_in_progress &&
            frontend_last_run_ms > 0 &&
            now - frontend_last_run_ms >= 300) {
            frontend_pause_in_progress = true;
            captured_generation = frontend_run_generation;
            mp = frontend_pause_player;
            should_pause = mp != NULL;
        }
        pthread_mutex_unlock(&frontend_pause_mutex);

        if (!should_pause)
            continue;

        bool did_pause = false;
        if (libvlc_media_player_get_state(mp) == libvlc_Playing) {
            libvlc_media_player_set_pause(mp, 1);
            vlc_audio_flush();
            did_pause = true;
        }

        bool resume_immediately = false;
        pthread_mutex_lock(&frontend_pause_mutex);
        bool frontend_already_resumed =
            frontend_run_generation != captured_generation;
        frontend_pause_in_progress = false;
        if (did_pause && !frontend_pause_stop && !frontend_already_resumed) {
            frontend_paused_vlc = true;
            frontend_pause_started_ms = monotonic_time_ms();
        } else if (did_pause && !frontend_pause_stop &&
                   frontend_already_resumed) {
            resume_immediately = true;
        }
        pthread_mutex_unlock(&frontend_pause_mutex);

        if (resume_immediately)
            libvlc_media_player_set_pause(mp, 0);
    }

    return NULL;
}

static void request_reload(const char *reason);
static void apply_audio_delay(const char *reason);
static void apply_language(void);
static void apply_zap_volume(void);
static void apply_banner_info(void);
static void apply_osd_style(void);
static bool apply_youtube_proxy(void);
static bool begin_opening(int index, const char *reason);

/*
 * Which of the two audio alignments this entry uses.
 *
 * This used to be current_is_video(), and that was right for exactly as long as
 * YouTube was the only thing in the core that was not a live channel. It is a
 * different question from "does this end", and now that a file on disk answers
 * yes to one and no to the other, the two have to be asked separately.
 *
 * The PTS clock path exists for a resolved YouTube stream: a DASH pair whose
 * separate video and audio inputs have no relationship except the one libVLC's
 * clock describes. A local file has neither problem - it is one container read
 * from a disk that never stalls - and it played correctly on the live-TV
 * reserve before it was ever called a video. So it stays there, and the two
 * alignments that were each arrived at by measurement get nothing added to
 * them, which was the whole point of keeping local files separate.
 */
static bool uses_video_clock(iptv_source source) {
    return source == IPTV_SOURCE_YOUTUBE || source == IPTV_SOURCE_YOUTUBE_LIVE;
}

/* The current entry's kind, or DIRECT when there is no current entry - which
 * is the value that switches every feature keyed on this one off. */
static iptv_source current_source(void) {
    if (current_channel < 0 || (size_t)current_channel >= playlist.count)
        return IPTV_SOURCE_DIRECT;
    return playlist.channels[current_channel].source;
}

static bool current_uses_video_clock(void) {
    return current_channel >= 0 &&
           (size_t)current_channel < playlist.count &&
           uses_video_clock(playlist.channels[current_channel].source);
}


/*
 * The option used to be mirrored here, so that the open path could ask "are we
 * in yt-dlp mode" without going to the frontend. It is gone: the reader already
 * refuses to mark an entry for a resolver in any other mode, so an entry whose
 * source is not DIRECT is proof of the setting all by itself. Two places
 * holding the same fact is one place too many for it to be held wrongly in.
 */

/* Options read on per-frame paths stay cached. Asking the frontend every
 * retro_run is not free and also makes older RetroArch versions flood the log.
 * They are refreshed only when the frontend reports a variable change. */
static bool cached_loading_osd = false;
static int  cached_signal_noise_percent = 0;
static char cached_audio_min_queue[32] = "1200";
static int  cached_network_caching = 1500;

/* The shader this replaces subtracts a new random value from the picture on
 * every frame. Keep the 256 possible multipliers ready so the hot path only
 * needs one lookup and integer channel multiplication per pixel. */
static uint16_t signal_noise_factor[256];
static int      signal_noise_table_percent = -1;
static uint32_t signal_noise_frame = 0;

/*
 * What the audio queue is meant to sit at, in ms, resolved from the reserve
 * option. The sync report needs it to tell "the queue is at 1199" from "the
 * queue has left where it belongs" - which is the difference between a line
 * worth writing and one worth skipping.
 */
static int64_t sync_reserve_ms = 1200;

/*
 * When the sync report stops being periodic and starts being urgent.
 *
 * These are the numbers ferramentas/analisa-log.py already uses to fail a
 * session, and they are reused here deliberately: the log becomes loud exactly
 * when the analyser would have called the session bad. A threshold that means
 * one thing in the tool and another in the core is two thresholds.
 */
#define SYNC_QUEUE_TOLERANCE_MS 250   /* analisa-log.py: DESVIO_MAXIMO_MS */
static void retry_audio_delay(void);

static void note_frontend_run(libvlc_media_player_t *mp) {
    bool resume_vlc = false;
    uint64_t paused_for = 0;

    pthread_mutex_lock(&frontend_pause_mutex);
    frontend_last_run_ms = monotonic_time_ms();
    frontend_run_generation++;
    if (frontend_paused_vlc) {
        frontend_paused_vlc = false;
        resume_vlc = true;
        paused_for = frontend_pause_started_ms
                         ? monotonic_time_ms() - frontend_pause_started_ms
                         : 0;
        frontend_pause_started_ms = 0;
    }
    pthread_mutex_unlock(&frontend_pause_mutex);

    if (!resume_vlc || !mp)
        return;

    bool reload_allowed = !option_equals("vlchannel_reload_after_pause",
                                         "disabled");

    if (reload_allowed && paused_for >= LIVE_RESUME_RELOAD_MS) {
        /*
         * Reopening looks heavier than resuming, and is the cheaper option: the
         * alternative is a channel that plays several seconds behind live for
         * the rest of the session, with the difference sitting in libVLC's
         * buffers.
         */
        iptv_log(
                "[VLChannel] Frontend was paused for %llu ms; reopening the "
                "channel to return to live\n",
                (unsigned long long)paused_for);
        libvlc_media_player_set_pause(mp, 0);
        request_reload("resume after frontend pause");
        return;
    }

    vlc_audio_flush();
    core.audio_sent_frames = 0;
    core.sync_offset = 0;
    core.sync_offset_initialized = false;
    core.sample_accum_frac = 0.0;
    pthread_mutex_lock(&core.mutex);
    core.audio_mute_frames = 2;
    pthread_mutex_unlock(&core.mutex);
    libvlc_media_player_set_pause(mp, 0);
    iptv_log("[VLChannel] Resumed after frontend pause (%llu ms)\n",
            (unsigned long long)paused_for);
}

/* ------------------------------------------------------------------ output */

static bool ensure_output_buffers(void) {
    static bool allocation_error_reported = false;
    size_t pixels = (size_t)OUTPUT_MAX_WIDTH * OUTPUT_MAX_HEIGHT;

    if (!osd_clean)
        osd_clean = (uint32_t *)calloc(pixels, sizeof(uint32_t));
    if (!osd_composed)
        osd_composed = (uint32_t *)calloc(pixels, sizeof(uint32_t));

    if (!osd_clean || !osd_composed) {
        if (!allocation_error_reported) {
            iptv_log("[VLChannel] Could not allocate the maximum-sized output "
                     "canvases\n");
            allocation_error_reported = true;
        }
        return false;
    }

    allocation_error_reported = false;
    return true;
}

static void reset_output_picture(void) {
    output_clear_pending = true;
    output_layout_dirty = true;
    output_last_render_count = UINT64_MAX;
    output_last_source_width = 0;
    output_last_source_height = 0;
    no_signal_frame_ready = false;
    signal_noise_frame = 0;
}

/*
 * Selects the canvas before the frontend asks for AV information.
 *
 * Geometry is deliberately not changed while content is running. EmuVR is the
 * reason the 720p choice exists, and changing the video pipeline underneath a
 * running EmuVR session would trade a cosmetic workaround for a much less
 * predictable failure. A changed option is therefore picked up by reloading
 * the content, when RetroArch asks for one fresh, stable geometry.
 */
static void apply_output_resolution(void) {
    const char *choice = option_value("vlchannel_output_resolution",
                                      "1920x1080");
    unsigned width = strstr(choice, "1280") ? 1280u : 1920u;
    unsigned height = width == 1280u ? 720u : 1080u;
    bool changed = width != output_width || height != output_height;

    if (changed) {
        output_width = width;
        output_height = height;
        output_pitch = width * (unsigned)sizeof(uint32_t);
        output_view_width = width;
        output_view_height = height;
        reset_output_picture();
    }

    if (!output_resolution_initialised || changed)
        note("Fixed output resolution: %ux%u", output_width, output_height);

    output_resolution_initialised = true;
    output_resolution_reload_notice_shown = false;
}

static void report_pending_output_resolution(void) {
    const char *choice = option_value("vlchannel_output_resolution",
                                      "1920x1080");
    unsigned requested_width = strstr(choice, "1280") ? 1280u : 1920u;

    if (requested_width == output_width) {
        output_resolution_reload_notice_shown = false;
        return;
    }
    if (output_resolution_reload_notice_shown)
        return;

    output_resolution_reload_notice_shown = true;
    note("Fixed output resolution changed to %s; close and reload the content "
         "to apply it", requested_width == 1280u ? "1280x720" : "1920x1080");
}

/* Reads the adjustable safe area. Size changes are returned to the caller
 * because libVLC must renegotiate its vmem picture; position changes only move
 * that picture inside the selected fixed canvas. */
static bool apply_output_layout(void) {
    int width_percent = atoi(option_value("vlchannel_picture_width", "100%"));
    int height_percent = atoi(option_value("vlchannel_picture_height", "100%"));
    int shift_x = atoi(option_value("vlchannel_picture_shift_x", "0%"));
    int shift_y = atoi(option_value("vlchannel_picture_shift_y", "0%"));

    if (width_percent < 80) width_percent = 80;
    if (width_percent > 100) width_percent = 100;
    if (height_percent < 80) height_percent = 80;
    if (height_percent > 100) height_percent = 100;
    if (shift_x < -5) shift_x = -5;
    if (shift_x > 5) shift_x = 5;
    if (shift_y < -5) shift_y = -5;
    if (shift_y > 5) shift_y = 5;

    unsigned width = ((output_width * (unsigned)width_percent) / 100u) & ~1u;
    unsigned height = ((output_height * (unsigned)height_percent) / 100u) & ~1u;
    bool size_changed = width != output_view_width ||
                        height != output_view_height;
    bool layout_changed = size_changed || shift_x != output_shift_x_percent ||
                          shift_y != output_shift_y_percent;

    output_view_width = width;
    output_view_height = height;
    output_shift_x_percent = shift_x;
    output_shift_y_percent = shift_y;
    vlc_video_set_output_size(width, height);

    if (layout_changed) {
        reset_output_picture();
        iptv_log("[VLChannel] Picture area: %ux%u, shift %d%%,%d%%\n",
                 width, height, shift_x, shift_y);
    }
    return size_changed;
}

static void set_no_signal(bool active) {
    if (no_signal_active == active)
        return;

    no_signal_active = active;
    no_signal_frame_ready = false;
    output_layout_dirty = true;
    output_last_render_count = UINT64_MAX;
    if (active)
        iptv_osd_set_waiting(false);
}

/*
 * At 100% this keeps the established EmuVR behaviour and fills the canvas.
 * Smaller values leave a black safe area for displays or converters with
 * overscan, without changing the selected geometry reported to the frontend.
 */
static void output_viewport(
    float aspect, unsigned *x, unsigned *y, unsigned *width, unsigned *height
) {
    (void)aspect;
    int max_x = (int)(output_width - output_view_width);
    int max_y = (int)(output_height - output_view_height);
    int pos_x = max_x / 2 + ((int)output_width * output_shift_x_percent) / 100;
    int pos_y = max_y / 2 + ((int)output_height * output_shift_y_percent) / 100;
    if (pos_x < 0) pos_x = 0;
    if (pos_x > max_x) pos_x = max_x;
    if (pos_y < 0) pos_y = 0;
    if (pos_y > max_y) pos_y = max_y;
    *x = (unsigned)pos_x;
    *y = (unsigned)pos_y;
    *width = output_view_width;
    *height = output_view_height;
}

static void set_signal_noise_percent(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    if (percent == signal_noise_table_percent)
        return;

    for (unsigned i = 0; i < 256; i++) {
        unsigned attenuation =
            (i * (unsigned)percent * 256U + 12750U) / 25500U;
        signal_noise_factor[i] = (uint16_t)(256U - attenuation);
    }

    cached_signal_noise_percent = percent;
    signal_noise_table_percent = percent;
    if (percent > 0)
        note("Picture noise: %d%%", percent);
    else
        note("Picture noise: off");
}

static unsigned blend_channel(unsigned a, unsigned b, unsigned fraction) {
    return (a * (65536U - fraction) + b * fraction + 32768U) >> 16;
}

/*
 * Normally vmem has already produced the selected canvas size and this takes
 * the direct-copy path. The scaler remains as a defensive fallback for a
 * runtime that ignores the requested vmem geometry or renegotiates an
 * unexpected format.
 */
static void scale_frame_to_output(
    const uint32_t *source, unsigned source_width, unsigned source_height,
    unsigned source_pitch
) {
    unsigned dst_x, dst_y, dst_width, dst_height;
    output_viewport(output_content_aspect, &dst_x, &dst_y,
                    &dst_width, &dst_height);

    memset(osd_clean, 0, (size_t)output_pitch * output_height);

    if (source_width == dst_width && source_height == dst_height) {
        for (unsigned y = 0; y < dst_height; y++) {
            const uint8_t *src_row = (const uint8_t *)source +
                                     (size_t)y * source_pitch;
            uint32_t *dst_row = osd_clean +
                                (size_t)(dst_y + y) * output_width + dst_x;
            memcpy(dst_row, src_row, (size_t)dst_width * sizeof(uint32_t));
        }
        return;
    }

    static unsigned x0[OUTPUT_MAX_WIDTH];
    static unsigned x1[OUTPUT_MAX_WIDTH];
    static unsigned xf[OUTPUT_MAX_WIDTH];

    for (unsigned x = 0; x < dst_width; x++) {
        uint64_t position = dst_width > 1
            ? (((uint64_t)x * (source_width - 1)) << 16) / (dst_width - 1)
            : 0;
        x0[x] = (unsigned)(position >> 16);
        x1[x] = x0[x] + (x0[x] + 1 < source_width ? 1U : 0U);
        xf[x] = (unsigned)(position & 0xffffU);
    }

    for (unsigned y = 0; y < dst_height; y++) {
        uint64_t position = dst_height > 1
            ? (((uint64_t)y * (source_height - 1)) << 16) / (dst_height - 1)
            : 0;
        unsigned y0 = (unsigned)(position >> 16);
        unsigned y1 = y0 + (y0 + 1 < source_height ? 1U : 0U);
        unsigned yf = (unsigned)(position & 0xffffU);
        const uint32_t *row0 = (const uint32_t *)
            ((const uint8_t *)source + (size_t)y0 * source_pitch);
        const uint32_t *row1 = (const uint32_t *)
            ((const uint8_t *)source + (size_t)y1 * source_pitch);
        uint32_t *dst = osd_clean +
                        (size_t)(dst_y + y) * output_width + dst_x;

        for (unsigned x = 0; x < dst_width; x++) {
            uint32_t p00 = row0[x0[x]], p10 = row0[x1[x]];
            uint32_t p01 = row1[x0[x]], p11 = row1[x1[x]];
            unsigned r0 = blend_channel((p00 >> 16) & 0xffU,
                                        (p10 >> 16) & 0xffU, xf[x]);
            unsigned g0 = blend_channel((p00 >> 8) & 0xffU,
                                        (p10 >> 8) & 0xffU, xf[x]);
            unsigned b0 = blend_channel(p00 & 0xffU, p10 & 0xffU, xf[x]);
            unsigned r1 = blend_channel((p01 >> 16) & 0xffU,
                                        (p11 >> 16) & 0xffU, xf[x]);
            unsigned g1 = blend_channel((p01 >> 8) & 0xffU,
                                        (p11 >> 8) & 0xffU, xf[x]);
            unsigned b1 = blend_channel(p01 & 0xffU, p11 & 0xffU, xf[x]);
            unsigned r = blend_channel(r0, r1, yf);
            unsigned g = blend_channel(g0, g1, yf);
            unsigned b = blend_channel(b0, b1, yf);
            dst[x] = (r << 16) | (g << 8) | b;
        }
    }
}

/* Applies only the shader's animated monochrome grain. The PAL convolution,
 * curvature and phosphor mask deliberately remain frontend shader effects.
 * Two packed multiplications handle R+B and G independently. */
static void apply_signal_noise(void) {
    unsigned x, y, width, height;
    output_viewport(output_content_aspect, &x, &y, &width, &height);

    if (x != 0 || y != 0 || width != output_width || height != output_height)
        memcpy(osd_composed, osd_clean,
               (size_t)output_pitch * output_height);

    uint32_t state = 0x9e3779b9U ^
                     (++signal_noise_frame * 747796405U);
    for (unsigned row = 0; row < height; row++) {
        const uint32_t *src = osd_clean +
                              (size_t)(y + row) * output_width + x;
        uint32_t *dst = osd_composed +
                        (size_t)(y + row) * output_width + x;
        for (unsigned column = 0; column < width; column++) {
            state = state * 1664525U + 1013904223U;
            unsigned factor = signal_noise_factor[state >> 24];
            uint32_t pixel = src[column];
            uint32_t rb = (((pixel & 0x00ff00ffU) * factor) >> 8) &
                          0x00ff00ffU;
            uint32_t g = (((pixel & 0x0000ff00U) * factor) >> 8) &
                         0x0000ff00U;
            dst[column] = (pixel & 0xff000000U) | rb | g;
        }
    }
}

static uint32_t *compose_output(bool draw_osd, bool add_noise) {
    if (add_noise)
        apply_signal_noise();
    else
        memcpy(osd_composed, osd_clean,
               (size_t)output_pitch * output_height);

    if (!draw_osd)
        return osd_composed;

    unsigned x, y, width, height;
    output_viewport(output_content_aspect, &x, &y, &width, &height);
    uint32_t *view = osd_composed + (size_t)y * output_width + x;
    const uint32_t *clean_view = osd_clean + (size_t)y * output_width + x;
    iptv_osd_draw(view, clean_view, width, height, output_pitch,
                  &playlist, current_channel);
    return osd_composed;
}

/*
 * No core mutex is held across video_cb(): the frontend call can block for a
 * full vsync interval, and blocking the vout thread there stalls decoding.
 */
static void submit_current_video_frame(void) {
    const uint32_t *frame = NULL;
    unsigned width = 0, height = 0, pitch = 0;

    if (!video_cb || !ensure_output_buffers())
        return;

    /* Sample before acquiring. If libVLC finishes another frame between these
     * calls, the next run sees the newer count and performs one harmless extra
     * scale instead of accidentally caching a frame it never copied. */
    uint64_t render_count = vlc_video_render_count();
    bool have_frame = vlc_video_acquire_frame(&frame, &width, &height, &pitch);

    if (no_signal_active) {
        if (!no_signal_frame_ready) {
            unsigned x, y, view_width, view_height;
            output_viewport(output_content_aspect, &x, &y,
                            &view_width, &view_height);
            memset(osd_clean, 0, (size_t)output_pitch * output_height);
            uint32_t *view = osd_clean + (size_t)y * output_width + x;
            iptv_osd_draw_no_signal(view, view_width, view_height, output_pitch);
            no_signal_frame_ready = true;
            output_clear_pending = false;
        }
    } else if (have_frame &&
        (render_count != output_last_render_count || output_layout_dirty ||
         width != output_last_source_width ||
         height != output_last_source_height)) {
        scale_frame_to_output(frame, width, height, pitch);
        output_last_render_count = render_count;
        output_last_source_width = width;
        output_last_source_height = height;
        output_layout_dirty = false;
        output_clear_pending = false;
    } else if (!have_frame && output_clear_pending) {
        memset(osd_clean, 0, (size_t)output_pitch * output_height);
        output_clear_pending = false;
    }

    bool draw_osd = iptv_osd_visible();
    bool add_noise = cached_signal_noise_percent > 0;
    uint32_t *output = (draw_osd || add_noise)
        ? compose_output(draw_osd, add_noise)
        : osd_clean;
    video_cb(output, output_width, output_height, output_pitch);
}

/*
 * Records the source shape for diagnostics inside the fixed output canvas.
 *
 * "default" means the stream's own shape: pixel size times the sample aspect
 * ratio libVLC reported. Anything else is an explicit override like "16:9".
 * The final picture is deliberately stretched to fill the selected canvas. The
 * frontend geometry never changes after load, which keeps both the OSD
 * coordinate system and the libretro video pipeline stable between channels.
 */
static void notify_geometry(const char *aspect_str) {
    if (!aspect_str)
        aspect_str = option_value("vlchannel_aspect_ratio", "default");

    pthread_mutex_lock(&core.mutex);
    unsigned width = core.video_width;
    unsigned height = core.video_height;
    pthread_mutex_unlock(&core.mutex);

    if (width == 0 || height == 0)
        return;                        /* format not negotiated yet */

    pthread_mutex_lock(&video_sar_mutex);
    int sar_num = video_sar_num;
    int sar_den = video_sar_den;
    video_sar_dirty = false;
    pthread_mutex_unlock(&video_sar_mutex);

    float aspect_ratio;

    if (strcmp(aspect_str, "default") != 0) {
        float w = 16.0f, h = 9.0f;
        if (sscanf(aspect_str, "%f:%f", &w, &h) != 2 || w <= 0.0f || h <= 0.0f) {
            w = 16.0f;
            h = 9.0f;
        }
        aspect_ratio = w / h;
    } else {
        if (sar_num > 0 && sar_den > 0)
            aspect_ratio =
                ((float)width * sar_num) / ((float)height * sar_den);
        else
            aspect_ratio = (float)width / (float)height;
    }

    output_content_aspect = aspect_ratio;
    output_layout_dirty = true;

    unsigned view_x, view_y, view_width, view_height;
    output_viewport(aspect_ratio, &view_x, &view_y,
                    &view_width, &view_height);
    iptv_log("[VLChannel] Source geometry: %ux%u aspect %.3f (%s); "
             "stretched to %ux%u at %u,%u\n",
            width, height, aspect_ratio,
            strcmp(aspect_str, "default") != 0 ? aspect_str : "from source SAR",
            view_width, view_height, view_x, view_y);
}

static void resynchronise_audio(const char *reason, int mute_frames) {
    vlc_audio_flush();                       /* takes core.mutex internally */

    /* The ring and the resampler phase describe the same stream position.
     * Reset them together, before callbacks for the next media can arrive.
     * Waiting for the first output block to notice audio_sent_frames moving
     * backwards leaves a window where a new stream can inherit the fractional
     * position of the previous one. This keeps the learned live-TV ratio, as
     * flush_ring() is explicitly designed to do. */
    iptv_drift_flush_ring();

    pthread_mutex_lock(&core.mutex);
    core.audio_mute_frames = mute_frames;
    core.audio_sent_frames = 0;
    core.sync_offset = 0;
    core.sync_offset_initialized = false;
    core.last_audio_pts = 0;
    core.last_audio_count = 0;
    core.sample_accum_frac = 0.0;
    pthread_mutex_unlock(&core.mutex);

    iptv_log("[VLChannel] Audio resync (%s)\n", reason);
}

/* ----------------------------------------------------------------- log_cb */

static void log_cb(
    void *data,
    int level,
    const libvlc_log_t *ctx,
    const char *fmt,
    va_list args
) {
    (void)data;
    (void)level;
    (void)ctx;

    char msg[1024];
    vsnprintf(msg, sizeof(msg), fmt, args);

    /*
     * Counting the adaptive demuxer's playlist reloads.
     *
     * A frozen live playlist puts libVLC in a loop that is invisible from the
     * public API: it re-requests the same manifest, re-appends the same
     * segments with a growing timeline, and never reaches a segment download.
     * Measured on one dead channel: 407 requests, all answered with the same
     * 229 byte body, zero .ts fetched, and "Updated playlist ID ..., after 0s"
     * on every pass.
     *
     * Only counted here, on libVLC's threads. retro_run decides what it means,
     * where the channel state is known.
     */
    if (strncmp(msg, "Updated playlist ID", 19) == 0)
        atomic_fetch_add_explicit(&playlist_reloads, 1u,
                                  memory_order_relaxed);

    if (strstr(msg, "original format sz") != NULL) {
        int sar_num = 0, sar_den = 0;
        const char *sar = strstr(msg, "sar ");
        if (sar && sscanf(sar, "sar %d:%d", &sar_num, &sar_den) == 2 &&
            sar_num > 0 && sar_den > 0) {
            pthread_mutex_lock(&video_sar_mutex);
            bool changed =
                (sar_num != video_sar_num || sar_den != video_sar_den);
            video_sar_num = sar_num;
            video_sar_den = sar_den;
            if (changed)
                video_sar_dirty = true;
            pthread_mutex_unlock(&video_sar_mutex);
            if (changed)
                iptv_log("[VLChannel] Source sample aspect ratio %d:%d\n",
                        sar_num, sar_den);
        }
    }

    /*
     * Everything libVLC says is echoed. On IPTV this is the diagnosis: the
     * access module names the protocol, the adaptive demuxer names the chosen
     * representation, and a dead channel says so here long before the picture
     * does. Note that RetroArch's own --log-file does not capture this stream;
     * stdout and stderr have to be captured together.
     */
    iptv_log("[VLC-LOG] %s\n", msg);
}

/* ------------------------------------------------------------ channel open */

static bool url_is_remote(const char *url) {
    return strstr(url, "://") != NULL;
}

static void apply_media_options(libvlc_media_t *m, const iptv_channel *channel) {
    char option[IPTV_OPTION_MAX + 8];

    /*
     * Caching. Too low and every hiccup is a dropout; too high and zapping
     * feels slow and a pause costs seconds.
     *
     * network-caching and live-caching are set together because libVLC picks
     * between them by source, and a live HLS channel is served by the second
     * one. Setting only network-caching - which this core did until reading the
     * VLC Libretro source made the omission obvious - leaves the value that
     * actually governs a live stream at its default, and channels that would
     * not open started opening when it was fixed.
     */
    const char *caching = option_value("vlchannel_network_caching", "1500");
    snprintf(option, sizeof(option), ":network-caching=%d", atoi(caching));
    libvlc_media_add_option(m, option);
    snprintf(option, sizeof(option), ":live-caching=%d", atoi(caching));
    libvlc_media_add_option(m, option);

    /*
     * Reconnects the HTTP access when a server closes the connection between
     * segments. Always on, no option: there was never a reason to turn it off.
     */
    libvlc_media_add_option(m, ":http-reconnect");

    /*
     * ":http-continuous" used to be set here, next to http-reconnect, on the
     * assumption that a live stream is a continuously updated resource. VLC's
     * own description of the option says the opposite in as many words:
     *
     *   "Read a file that is being constantly updated (for example, a JPG file
     *    on a server). You should not globally enable this option as it will
     *    break all other types of HTTP streams."
     *
     * It was enabled on every channel, which is exactly what that sentence
     * warns against. Removed, not made optional: an option whose documentation
     * says it breaks the normal case does not deserve a place in the menu.
     */

    const char *aspect = option_value("vlchannel_aspect_ratio", "default");
    if (strcmp(aspect, "default") != 0) {
        snprintf(option, sizeof(option), ":aspect-ratio=%s", aspect);
        libvlc_media_add_option(m, option);
    }

    /*
     * Only the algorithm is set here. Whether to deinterlace at all is left to
     * libVLC's own detection unless the user forces it. Many IPTV channels are
     * genuinely interlaced 576i or 1080i, so this matters more than it did on
     * progressive film DVDs.
     */
    const char *deinterlace = option_value("vlchannel_deinterlace_mode", "auto");
    if (strcmp(deinterlace, "auto") != 0 && strcmp(deinterlace, "off") != 0) {
        snprintf(option, sizeof(option), ":deinterlace-mode=%s", deinterlace);
        libvlc_media_add_option(m, option);
    }

    int audio_track = atoi(option_value("vlchannel_audio_track", "0"));
    if (audio_track > 0) {
        snprintf(option, sizeof(option), ":audio-track=%d", audio_track);
        libvlc_media_add_option(m, option);
    }

    core.audio_desync_ms = atoi(option_value("vlchannel_audio_delay", "0"));
    if (core.audio_desync_ms != 0) {
        snprintf(option, sizeof(option), ":audio-desync=%d",
                 core.audio_desync_ms);
        libvlc_media_add_option(m, option);
    }

    /* Per channel options from #EXTVLCOPT, applied last so a list can override
     * anything above for the channel that needs it. */
    for (int i = 0; i < channel->option_count; i++) {
        if (channel->options[i][0] == ':')
            snprintf(option, sizeof(option), "%s", channel->options[i]);
        else
            snprintf(option, sizeof(option), ":%s", channel->options[i]);
        libvlc_media_add_option(m, option);
        iptv_log("[VLChannel] Channel option %s\n", option);
    }
}

/*
 * Opens `index`, optionally from an address other than the one in the list.
 *
 * `address` exists for yt-dlp, whose answer is a signed, expiring, IP-bound URL
 * that belongs to this playback and to nothing else. It is passed as an argument
 * rather than written into the entry: the guide, the banner and the log go on
 * showing the YouTube address the viewer wrote, which is the one that means
 * something to them and the only one that will still work tomorrow.
 *
 * NULL means "use the list's own address", which is every other caller.
 *
 * `from_resolver` says whether `address` came from yt-dlp or streamlink, as
 * opposed to from the proxy or from the list. Only a resolved address gets the
 * one proxy retry when playback refuses it - the proxy retrying itself is what
 * a loop would be made of.
 */
static bool open_channel_from(int index, const char *reason,
                              const char *address, const char *audio_slave,
                              bool from_resolver) {
    if (index < 0 || (size_t)index >= playlist.count)
        return false;
    if (!core.libvlc)
        return false;

    const iptv_channel *channel = &playlist.channels[index];
    const char *url = (address && address[0]) ? address : channel->url;
    current_open_is_resolved = from_resolver && address && address[0];
    bool previous_used_video_clock =
        current_channel >= 0 &&
        (size_t)current_channel < playlist.count &&
        uses_video_clock(playlist.channels[current_channel].source);
    bool uses_clock = uses_video_clock(channel->source);

    note("Channel %d/%zu: \"%s\"%s%s%s (%s)",
         index + 1, playlist.count, channel->name,
         channel->group[0] ? " [" : "",
         channel->group[0] ? channel->group : "",
         channel->group[0] ? "]" : "",
         reason);
    note("URL: %s", channel->url);
    if (url != channel->url)
        note("Resolved to: %.120s...", url);

    if (!core.mp) {
        core.mp = libvlc_media_player_new(core.libvlc);
        if (!core.mp) {
            iptv_log("[VLChannel] Failed to create the media player\n");
            return false;
        }
        vlc_video_setup_callbacks(core.mp);
        vlc_audio_setup_callbacks(core.mp);
        pthread_mutex_lock(&frontend_pause_mutex);
        frontend_pause_player = core.mp;
        pthread_mutex_unlock(&frontend_pause_mutex);
    } else {
        /*
         * The media player is created once and reused for every channel: only
         * the media is replaced. VLCine released and recreated it per load,
         * which is safe when loading happens once, but here the pause watchdog
         * holds a pointer to it from another thread, and a player that is never
         * released can never be used after free.
         *
         * stop() blocks until libVLC's input thread is gone, and on a stalled
         * network source that is not instant. It runs on the libretro thread,
         * so a slow close is a stutter in the frontend - which is why it is
         * measured and reported rather than hidden.
         */
        uint64_t started = monotonic_time_ms();
        libvlc_media_player_stop(core.mp);
        uint64_t elapsed = monotonic_time_ms() - started;
        if (elapsed > 250)
            iptv_log(
                    "[VLChannel] Closing the previous channel took %llu ms\n",
                    (unsigned long long)elapsed);
    }

    libvlc_media_t *m =
        url_is_remote(url)
            ? libvlc_media_new_location(core.libvlc, url)
            : libvlc_media_new_path(core.libvlc, url);

    if (!m) {
        iptv_log("[VLChannel] libVLC refused the URL: %s\n", url);
        return false;
    }

    /*
     * A DASH pair: video and audio are separate files, and libVLC plays the
     * second as a slave of the first. YouTube commonly publishes HD this way,
     * so a 1080p request normally comes back in two pieces.
     *
     * Added before the per-channel options so a list can still override it,
     * which is the same order everything else follows.
     */
    if (audio_slave && audio_slave[0]) {
        char slave[4200];
        snprintf(slave, sizeof(slave), ":input-slave=%s", audio_slave);
        libvlc_media_add_option(m, slave);
        note("Audio is a separate stream; passed to libVLC as input-slave");
    }

    apply_media_options(m, channel);
    libvlc_media_player_set_media(core.mp, m);
    libvlc_media_release(m);

    vlc_video_reset();
    pthread_mutex_lock(&video_sar_mutex);
    video_sar_num = 0;
    video_sar_den = 0;
    video_sar_dirty = false;
    pthread_mutex_unlock(&video_sar_mutex);
    reset_output_picture();

    /*
     * Muting eight retro_run frames is useful while a live source is changing:
     * it hides the decoder tail of the old channel. A YouTube entry is a finite
     * file (often a DASH video plus an audio slave), and stop() has already
     * joined the old input. Keeping the gate closed for ~133 ms of frontend
     * time can discard a much larger decode-ahead burst from the new file while
     * its video clock advances, creating a permanent offset on every transition.
     */
    resynchronise_audio("channel change", uses_clock ? 0 : 8);
    if (previous_used_video_clock != uses_clock)
        iptv_drift_reset();

    int open_timeout_s = atoi(option_value("vlchannel_open_timeout", "20"));
    if (open_timeout_s < 5) open_timeout_s = 5;

    pthread_mutex_lock(&core.mutex);
    core.transitioning = true;
    /* At the fixed 60 Hz cadence. Long enough for a slow server, short enough
     * that a dead channel says so instead of hanging silently - and, more to
     * the point, short enough that the core stops hammering a server that is
     * answering with a frozen playlist. */
    core.transition_timeout_frames = (uint32_t)open_timeout_s * 60u;
    pthread_mutex_unlock(&core.mutex);
    opening_frames = 0;
    atomic_store_explicit(&playlist_reloads, 0u, memory_order_relaxed);
    frozen_playlist_reported = false;
    iptv_seq_opening();          /* nothing has played on this channel yet */

    core.paused = false;
    core.pending_start = true;
    iptv_osd_channel_changed();
    /*
     * Beside the banner, and for the same reason: they are the two halves of
     * one event. This is also the moment the new channel starts its caching
     * window, so the noise plays over silence that was going to be there
     * anyway.
     *
     * Not on the first channel of the list, though. current_channel is still
     * the one being left at this point, and -1 means there was nothing to
     * leave: the list has just been loaded and nobody changed anything. A set
     * being switched on is not a channel change, and a noise nobody asked for
     * is worse in a room where the television may be across it.
     */
    if (current_channel >= 0)
        iptv_zap_trigger();
    audio_delay_pending = false;      /* belonged to the channel being left */
    /* Both belonged to the entry being left: the jump target it accumulated,
     * and whatever notice it put in the corner. */
    iptv_transport_reset();
    iptv_osd_clear_notice();
    current_channel = index;
    last_reported_state = libvlc_NothingSpecial;

    if (uses_clock) {
        if (vlchannel_libvlc_clock)
            note("YouTube audio sync uses the libVLC PTS clock");
        else
            note("libVLC PTS clock unavailable; YouTube audio uses the fixed "
                 "queue reserve");
    }

    /*
     * Clear the watchdog's view of the world. Opening a channel can block this
     * thread for seconds, which looks exactly like the frontend having stopped
     * calling retro_run; without this, a slow channel change could be followed
     * by an immediate "return to live" reload of the channel just opened.
     */
    pthread_mutex_lock(&frontend_pause_mutex);
    frontend_paused_vlc = false;
    frontend_pause_started_ms = 0;
    frontend_last_run_ms = monotonic_time_ms();
    pthread_mutex_unlock(&frontend_pause_mutex);

    return true;
}

/* The ordinary way in: the address is the one in the list. */
static bool open_channel(int index, const char *reason) {
    return open_channel_from(index, reason, NULL, NULL, false);
}

/*
 * An entry waiting on a resolver, which one is running, and the reason it was
 * opened.
 *
 * -1 means nothing is in flight. The reason is carried through because it is a
 * pointer to a string literal owned by whoever asked, and the message that
 * eventually appears in the log should say why the viewer ended up here, not
 * "resolved".
 */
typedef enum {
    RESOLVER_NONE = 0,
    RESOLVER_YTDLP,
    RESOLVER_STREAMLINK
} resolver;

static int resolving_channel = -1;
static resolver resolving_with = RESOLVER_NONE;
static const char *resolving_reason = NULL;
static uint64_t resolving_since = 0;

/*
 * Which tool is asked first, and which second.
 *
 * Both know about both kinds of address, so this is a preference and not a
 * capability: yt-dlp is the better answer for a YouTube video, streamlink is
 * the better answer for a live broadcast on a platform built around them. The
 * loser of each pairing is still tried, because "the specialist had nothing"
 * and "nobody has anything" are different answers and only the second one is
 * worth giving up on.
 */
static resolver first_resolver(iptv_source source) {
    return source == IPTV_SOURCE_PLATFORM ? RESOLVER_STREAMLINK
                                          : RESOLVER_YTDLP;
}

static resolver second_resolver(iptv_source source) {
    return source == IPTV_SOURCE_PLATFORM ? RESOLVER_YTDLP
                                          : RESOLVER_STREAMLINK;
}

static bool resolver_available(resolver which) {
    return which == RESOLVER_YTDLP      ? iptv_ytdlp_available()
         : which == RESOLVER_STREAMLINK ? iptv_streamlink_available()
                                        : false;
}

static void cancel_resolvers(void) {
    iptv_ytdlp_cancel();
    iptv_streamlink_cancel();
    resolving_channel = -1;
    resolving_with = RESOLVER_NONE;
}

/*
 * Starts `which` on `index`, or returns false if that tool is not installed.
 *
 * Only one runs at a time. Running both and taking whichever answers first
 * would be faster on a bad day and would also mean two processes per channel
 * change on every good one, for an answer the first tool was about to give.
 */
static bool start_resolver(int index, resolver which) {
    if (!resolver_available(which))
        return false;

    const iptv_channel *channel = &playlist.channels[index];
    bool live = channel->source == IPTV_SOURCE_YOUTUBE_LIVE;

    if (which == RESOLVER_YTDLP) {
        note(live ? "Asking yt-dlp what is on air on this YouTube channel"
                  : "Asking yt-dlp for this address");
        iptv_ytdlp_begin(channel->url, live);
    } else {
        note("Asking streamlink what is on air at this address");
        iptv_streamlink_begin(channel->url);
    }

    resolving_channel = index;
    resolving_with = which;
    resolving_since = monotonic_time_ms();
    return true;
}

/*
 * Starts opening `index`, which for a resolver entry means asking first.
 *
 * Every channel change goes through here, so the wait exists in exactly one
 * place. The banner is already up by then - it is raised by open_channel, which
 * is why an entry shows its name while the address is still being fetched
 * rather than showing nothing.
 */
static bool begin_opening(int index, const char *reason) {
    cancel_resolvers();           /* whatever was in flight is about stale news */

    if (index < 0 || (size_t)index >= playlist.count)
        return false;

    /* A new tune attempt replaces the terminal test pattern immediately. */
    set_no_signal(false);

    iptv_source source = playlist.channels[index].source;
    resolving_reason = reason;

    if (source != IPTV_SOURCE_DIRECT) {
        if (start_resolver(index, first_resolver(source)) ||
            start_resolver(index, second_resolver(source)))
            /* Started, not opened. The caller has nothing to fail on yet -
             * retro_run finishes this in a frame or two. */
            return true;

        /*
         * Neither tool is installed. Said before the failure rather than after
         * it, because the failure is a libVLC error on a web page and reads
         * like a dead link. A YouTube video still has the proxy after this; a
         * channel live page and a platform address do not, so for those the
         * missing tools are the whole of the reason.
         */
        if (source != IPTV_SOURCE_YOUTUBE)
            note("This entry is a live page, and opening one needs yt-dlp or "
                 "streamlink. Put yt-dlp.exe in system\\vlchannel, or "
                 "streamlink in system\\vlchannel\\streamlink.");
    }

    return open_channel(index, reason);
}

static void request_channel(int index, const char *reason) {
    if (playlist.count == 0)
        return;
    if (index < 0)
        index = (int)playlist.count - 1;
    if ((size_t)index >= playlist.count)
        index = 0;
    pending_channel = index;
    pending_reason = reason;
}

static void request_reload(const char *reason) {
    pending_reload = true;
    pending_reason = reason;
}

/* ----------------------------------------------------------- audio delay */

/*
 * The applied delay is the sum of two corrections that have different owners.
 *
 * The channel's value corrects the stream: the offset between audio and video
 * as the broadcaster sends it. It belongs in the list and is the same
 * everywhere.
 *
 * The global offset corrects the playback chain around the core. EmuVR does not
 * play the core's audio directly: it captures it and replays it on the virtual
 * television, spatialised, which adds a latency RetroArch does not have. That
 * latency is a property of the setup, identical on every channel, so folding it
 * into each channel's value would be wrong twice over - it would have to be
 * found again for every channel, and it would poison a list that is meant to be
 * portable.
 *
 * It lives in a core option because the frontend owns core options: EmuVR
 * appends its own emuvr_core_override.cfg, so the two frontends keep separate
 * values without the core having to know which one it is running under.
 */
static int channel_audio_delay_ms(void) {
    if (current_channel >= 0 && (size_t)current_channel < playlist.count &&
        playlist.channels[current_channel].audio_delay_set)
        return playlist.channels[current_channel].audio_delay_ms;
    return 0;
}

static int global_audio_offset_ms(void) {
    return atoi(option_value("vlchannel_audio_delay", "0"));
}

static int current_audio_delay_ms(void) {
    return channel_audio_delay_ms() + global_audio_offset_ms();
}

/*
 * The delay is applied through libVLC, and this is where a wrong theory cost
 * two versions.
 *
 * 0.2.0 replaced this call with a queue-length mechanism, on the reasoning that
 * libvlc_audio_set_delay() cannot reach a core whose audio output is amem. The
 * reasoning was wrong and the evidence against it was already there: the delay
 * had been working in RetroArch since the feature was written. libVLC's own
 * audio output stage honours the delay before the samples ever reach this core,
 * by feeding the ring earlier or later - which is exactly why capping the ring
 * broke it.
 *
 * The call does return -1 when there is no audio output yet, which is a real
 * race on a live stream: the picture can be ready before the audio elementary
 * stream is up. That part is kept - the request is retried until it takes.
 */
static void apply_audio_delay(const char *reason) {
    if (!core.mp || !vlchannel_libvlc_audio_set_delay)
        return;

    int delay = current_audio_delay_ms();
    core.audio_desync_ms = delay;

    if (libvlc_audio_set_delay(core.mp, (int64_t)delay * 1000) == 0) {
        audio_delay_pending = false;
        note("Audio delay %+d ms applied (%s): channel %+d, global offset %+d",
             delay, reason, channel_audio_delay_ms(), global_audio_offset_ms());
        return;
    }

    audio_delay_pending = true;
    audio_delay_pending_value = delay;
    note("Audio delay %+d ms refused by libVLC (%s): no audio output yet. "
         "Retrying until it takes.", delay, reason);
}

static void retry_audio_delay(void) {
    static uint32_t countdown = 0;

    if (!audio_delay_pending || !core.mp || !vlchannel_libvlc_audio_set_delay)
        return;
    if (countdown++ % 15 != 0)
        return;

    if (libvlc_audio_set_delay(core.mp,
                               (int64_t)audio_delay_pending_value * 1000) == 0) {
        audio_delay_pending = false;
        note("Audio delay %+d ms applied on retry", audio_delay_pending_value);
    }
}

/* ------------------------------------------------------------- retro API */

RETRO_API void retro_set_environment(retro_environment_t cb) {
    environ_cb = cb;
    static const struct retro_variable vars[] = {
        { "vlchannel_output_resolution",
          "Fixed output resolution (reload content); 1920x1080|1280x720" },
        { "vlchannel_picture_width",
          "Picture width (reopens the channel); "
          "100%|80%|82%|84%|86%|88%|90%|92%|94%|96%|98%" },
        { "vlchannel_picture_height",
          "Picture height (reopens the channel); "
          "100%|80%|82%|84%|86%|88%|90%|92%|94%|96%|98%" },
        { "vlchannel_picture_shift_x",
          "Horizontal position; 0%|-1%|1%|-2%|2%|-3%|3%|-4%|4%|-5%|5%" },
        { "vlchannel_picture_shift_y",
          "Vertical position; 0%|-1%|1%|-2%|2%|-3%|3%|-4%|4%|-5%|5%" },
        { "vlchannel_network_caching",
          "Network and live caching (ms) (reopens the channel); "
          "1500|500|1000|2000|3000|5000|8000" },
        /*
         * 1200 is first, and therefore the default, because it is the only
         * value in this list that was ever measured and confirmed.
         *
         * It came from the 0.2.2 session: channels in sync sat at about
         * 1050 ms of queue, channels out of sync at 48-112 ms, and a fixed
         * 1200 ms reserve put every one of them in sync. The very next version
         * replaced that number with the formula below - the reserve follows the
         * caching value - which reads better and was never compared against the
         * number it replaced. With caching at 1500 the formula asks for 1500,
         * about 300 ms past what the streams settle at, and the sound runs
         * late. Two sessions months apart have now confirmed 1200.
         *
         * A measured number beats an elegant one. The formula stays available
         * because every log from 0.2.3 onwards was recorded with it.
         */
        { "vlchannel_audio_min_queue",
          "Audio reserve (ms); 1200|1000|1500|800|500|match caching|"
          "80 ms (legacy)" },
        { "vlchannel_audio_delay",
          "Global audio offset (ms, added to every channel); "
          "0|-25|-50|-75|-100|-150|-200|-250|-300|25|50|75|100|150|200|250|300" },
        { "vlchannel_open_timeout",
          "Give up on a channel after (s); 20|10|30|60" },
        { "vlchannel_reload_after_pause",
          "Return to live after a frontend pause; enabled|disabled" },
        { "vlchannel_aspect_ratio",
          "Aspect ratio; default|16:9|4:3|2.35:1|1.85:1|5:4|16:10" },
        { "vlchannel_deinterlace_mode",
          "Deinterlace; auto|off|yadif|yadif2x|blend|bob|linear|mean|x" },
        { "vlchannel_audio_track", "Audio track; 0|1|2|3|4|5" },
        /*
         * Only the banner and the guide follow this. The options above and the
         * log stay English whatever it says - see iptv_text.h for why.
         *
         * "Portugues" is written without its accent because this string is
         * matched byte for byte against what the frontend hands back, and a
         * core option is not worth an encoding question.
         */
        { "vlchannel_language",
          "On-screen language (guide and banner); "
          "English|Portugues|auto (follow frontend)" },
        /*
         * Mixed over the output at the moment of the change, never queued -
         * see iptv_zap.h. The default is half volume because the takes were
         * recorded loud enough to be startling at full on a set that is
         * already at a comfortable level.
         */
        { "vlchannel_zap_sound",
          "Channel change noise; 50%|off|25%|75%|100%" },
        /*
         * Only has an effect where there is a programme listing: the banner
         * falls back to the category otherwise, whichever of these is picked.
         * "schedule" first because it is the short one - it says what is on and
         * what follows, and settles quickly. The synopsis is a real choice and
         * not a better default: it makes the line several times longer, and a
         * line that long is still scrolling when the banner would rather be
         * gone.
         */
        { "vlchannel_banner_info",
          "Cable TV banner second line (needs a programme listing); "
          "now and next|now, description and next" },
        { "vlchannel_osd",
          "OSD; TV|Cable TV|Cable TV (PIP)" },
        { "vlchannel_loading_osd",
          "Loading screen; disabled|enabled" },
        { "vlchannel_signal_noise",
          "Picture noise (OSD stays clean); off|10%|25%|35%|50%" },
        /*
         * libVLC cannot open a YouTube page, so an entry pointing at one is
         * rewritten to a proxy that serves the video, and marked as a video so
         * that its ending advances the list instead of reading as a fault.
         *
         * A switch and not a free text field: a core option is a menu on a
         * television, and typing a URL on one is not something to ask of
         * anybody. `off` leaves the address exactly as the list wrote it, which
         * is what someone whose list already points at a working proxy wants.
         *
         * Changing it reloads the list, because the rewriting happens when the
         * list is read.
         *
         * `alternative` is the resolvers - yt-dlp and streamlink - and it is
         * the only value that can open an address with no video id in it: a
         * channel's live page, or a Twitch or Kick page. The proxy takes an id
         * and nothing else, so for those entries it is not a fallback, it is
         * simply not applicable.
         *
         * The labels are left as they are, even though "alternative" now names
         * two programs. They are what a saved core option is matched against,
         * and renaming one would silently return everybody to the default.
         */
        { "vlchannel_video",
          "Video entries (reloads the list); proxy|alternative|disabled" },
        { "vlchannel_log_file",
          "Write the full core log to system/vlchannel-core.log (restart core); "
          "disabled|enabled" },
        { NULL, NULL },
    };
    environ_cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void*)vars);
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

void retro_init(void) {
    /*
     * Stamp the build into the log. Matching a captured log against the source
     * by file timestamps is unreliable once the DLL is copied between folders,
     * and mistaking an old binary for a new one costs a whole test cycle.
     */
    struct retro_log_callback log_interface = {0};
    if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log_interface))
        frontend_log = log_interface.log;

    /*
     * Opened before anything else, so the runtime loader and every [VLC-LOG]
     * line land in it. Under EmuVR this file is the only place the core's
     * diagnosis exists at all.
     */
    if (option_equals("vlchannel_log_file", "enabled")) {
        const char *system_dir = NULL;
        if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_dir) &&
            system_dir && system_dir[0]) {
            char path[VLCHANNEL_PATH_BUFFER];
            snprintf(path, sizeof(path), "%s\\vlchannel-core.log", system_dir);
            iptv_log_open(path);
        }
    }

    note("Core build: %s %s", __DATE__, __TIME__);
    apply_output_resolution();
    apply_language();
    apply_zap_volume();
    apply_osd_style();
    apply_banner_info();
    if (iptv_log_path()[0])
        note("Full core log: %s", iptv_log_path());
    pthread_mutex_init(&core.mutex, NULL);

    pthread_mutex_lock(&frontend_pause_mutex);
    frontend_pause_stop = false;
    frontend_pause_in_progress = false;
    frontend_paused_vlc = false;
    frontend_last_run_ms = 0;
    frontend_run_generation = 0;
    frontend_pause_started_ms = 0;
    frontend_pause_player = NULL;
    pthread_mutex_unlock(&frontend_pause_mutex);

    frontend_pause_thread_started =
        pthread_create(&frontend_pause_thread, NULL,
                       frontend_pause_watchdog, NULL) == 0;
    if (!frontend_pause_thread_started)
        iptv_log("[VLChannel] Failed to start the frontend pause "
                        "watchdog\n");

    /*
     * One runtime, one location: system\vlchannel.
     *
     * Searching historical names and generation subdirectories made the loaded
     * VLC depend on whichever stale installation happened to exist first. The
     * core, libVLC, its plugins, yt-dlp and cookies.txt now share this single,
     * explicit directory.
     */
    const char *system_directory = NULL;
    char runtime_directory[VLCHANNEL_PATH_MAX] = {0};
    bool runtime_loaded = false;

    if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_directory) &&
        system_directory && system_directory[0]) {
        snprintf(runtime_directory, sizeof(runtime_directory), "%s\\vlchannel",
                 system_directory);
        iptv_log("[VLChannel] Runtime directory: %s\n", runtime_directory);
        runtime_loaded = vlchannel_load_libvlc(runtime_directory);
        iptv_ytdlp_set_directory(runtime_directory);
        iptv_streamlink_set_directory(runtime_directory);
    } else {
        iptv_log("[VLChannel] RetroArch system directory is unavailable\n");
    }

    if (runtime_loaded && vlchannel_runtime_is_legacy())
        iptv_log("[VLChannel] Running on libVLC 2.x. It plays HLS through the "
                 "httplive stream filter rather than the adaptive demuxer: no "
                 "bitrate adaptation, and an old TLS stack. Replace the files "
                 "in system/vlchannel to change the runtime.\n");

    /*
     * clock-jitter bounds how much libVLC may grow its PTS delay when a PCR
     * arrives late. VLCine ships 0 because dvdnav resets the clock constantly
     * and a large allowance made libVLC buffer seconds of audio at once.
     *
     * A network stream is the opposite case: late PCRs are normal, they come
     * from the network rather than from the demuxer, and 0 would make libVLC
     * reset its clock on ordinary jitter. libVLC's own default of 5000 is kept
     * here, with the lower values available for comparison.
     */
    /* libVLC's own default. It was briefly a core option, on the theory that
     * network jitter needed tuning; nobody ever had a reason to move it. */
    static const char *clock_jitter_arg = "--clock-jitter=5000";

    /*
     * Decoder threads. VLCine forces 1 because DVD menus flush the decoder on
     * every cell change and a 720x480 picture costs nothing. IPTV is 720p and
     * 1080p H.264 or HEVC, where single threaded decoding simply cannot keep
     * up, so the default here is 0: let avcodec pick.
     */
    /* 0 lets avcodec choose. VLCine forces 1 for DVD menus; that would not
     * keep up with the 1080p H.264 this core plays. */
    static const char *threads_arg = "--avcodec-threads=0";

    /* Software decoding. Hardware decoding with vmem means a copy back from
     * the GPU every frame, and it was never measured to help here. */
    static const char *hw_arg = "--avcodec-hw=none";

    char network_caching_arg[64];
    snprintf(network_caching_arg, sizeof(network_caching_arg),
             "--network-caching=%d",
             atoi(option_value("vlchannel_network_caching", "1500")));

    /*
     * Frame dropping. VLCine disables it because a dropped menu frame is never
     * redrawn. On live television the trade is reversed: dropping a late frame
     * is how playback stays close to live after a network stall, so it is
     * enabled by default here.
     */
    /* Dropping a late frame is how live playback stays close to live, which is
     * the opposite of what a DVD core wants. */
    const bool allow_frame_dropping = true;

    /*
     * Argument sets are split so that an option one libVLC generation does not
     * recognise cannot take the whole instance down with it. libvlc_new() fails
     * outright on an unknown option name - that is how "--ac3-float" once broke
     * VLCine's startup - so the tuning options are dropped and retried if
     * creation fails, instead of leaving the core dead.
     */
    static const char* essential_args[] = {
        "--no-video-title-show",
        "--vout=vmem",
        "--aout=amem",
        NULL
    };

    const char* tuning_args[16];
    int tuning_count = 0;
    tuning_args[tuning_count++] = network_caching_arg;
    tuning_args[tuning_count++] = clock_jitter_arg;
    tuning_args[tuning_count++] = threads_arg;
    tuning_args[tuning_count++] = hw_arg;
    if (!allow_frame_dropping) {
        tuning_args[tuning_count++] = "--no-drop-late-frames";
        tuning_args[tuning_count++] = "--no-skip-frames";
    }

    int essential_count = 0;
    while (essential_args[essential_count] != NULL) essential_count++;

    iptv_log("[VLChannel] %s %s %s %s, frame dropping %s\n",
            network_caching_arg, clock_jitter_arg, threads_arg, hw_arg,
            allow_frame_dropping ? "enabled" : "disabled");

    core.libvlc = NULL;
    if (runtime_loaded) {
        int total = essential_count + tuning_count;
        const char** args = malloc((total + 1) * sizeof(char*));
        if (args) {
            for (int i = 0; i < essential_count; i++)
                args[i] = essential_args[i];
            for (int i = 0; i < tuning_count; i++)
                args[essential_count + i] = tuning_args[i];
            args[total] = NULL;

            core.libvlc = libvlc_new(total, args);
            free(args);
        }

        if (!core.libvlc) {
            iptv_log(
                    "[VLChannel] libvlc_new failed with the tuning options; "
                    "retrying with the essential set only\n");
            core.libvlc = libvlc_new(essential_count, essential_args);
        }
    }

    if (core.libvlc) {
        libvlc_log_set(core.libvlc, log_cb, NULL);
        iptv_log("[VLChannel] libVLC instance created\n");

        static const struct retro_frame_time_callback frame_time_cb =
            { NULL, 1000 };
        environ_cb(RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK,
                   (void*)&frame_time_cb);
    } else {
        iptv_log("[VLChannel] Failed to create the libVLC instance\n");
    }

    /*
     * Keep the libretro cadence at a fixed 60 Hz. Frames are produced
     * asynchronously by libVLC and repeated as needed; tying retro_run to the
     * source rate causes large audio batches and audible underruns, and on
     * IPTV the source rate is not even known until the stream opens.
     */
    core.video_fps = 60.0;
    core.is_playing = false;
    core.audio_sent_frames = 0;
    core.sync_offset = 0;
    core.sync_offset_initialized = false;
    core.paused = false;
    core.last_audio_pts = 0;
    core.last_audio_count = 0;
    core.transitioning = false;
    core.transition_timeout_frames = 0;
    core.video_width = 0;
    core.video_height = 0;
    core.video_pitch = 0;
    core.sample_accum_frac = 0.0;
    core.max_width = output_width;
    core.max_height = output_height;
    core.audio_mute_frames = 0;
    core.audio_prebuffering = true;
    core.audio_fade_in_frames = 0;
    core.audio_last_left = 0;
    core.audio_last_right = 0;
    core.audio_desync_ms = 0;
}

/*
 * Turns whatever the frontend passed into an absolute path.
 *
 * EmuVR launches RetroArch with a relative content path:
 *
 *   retroarch.exe --emuvr 7efda -L "cores\vlchannel_libretro.dll"
 *                 "..\Games\DirecTV\Directv.m3u8"
 *
 * and RetroArch hands that straight to the core. A relative path only means
 * anything while the process keeps the working directory it started in, which
 * is not something a core should bet on - and it is why writing the audio
 * delays worked under RetroArch and not under EmuVR. Resolving once, at load,
 * removes the bet: every later open, read and write uses the absolute form.
 */
static void resolve_content_path(const char *path, char *out, size_t out_size) {
#ifdef _WIN32
    DWORD length = GetFullPathNameA(path, (DWORD)out_size, out, NULL);
    if (length == 0 || length >= out_size)
        snprintf(out, out_size, "%s", path);
#else
    if (!realpath(path, out))
        snprintf(out, out_size, "%s", path);
#endif
}

RETRO_API bool retro_load_game(const struct retro_game_info *info) {
    if (!info || !info->path) {
        iptv_log("[VLChannel] No content path supplied\n");
        return false;
    }
    if (!core.libvlc)
        return false;

    iptv_log("[VLChannel] Loading list: %s\n", info->path);

    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) {
        note("The frontend rejected the required XRGB8888 pixel format");
        return false;
    }

    /* So RetroArch's control menu names the buttons instead of showing a
     * generic pad with no meaning. */
    static const struct retro_input_descriptor descriptors[] = {
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "Previous channel" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Next channel" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Reload channel" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "First channel" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Channel guide" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "Guide: up" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "Channel info / guide: down" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Schedule" },
        /* Local files only. Declared unconditionally because the descriptors
         * are sent once, before any content is loaded, and a list with no local
         * file in it simply never fires them. */
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,  "Video: subtitle track" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,  "Video: audio track" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2, "Video: back 10 s" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2, "Video: forward 10 s" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Video: pause" },
        { 0, 0, 0, 0, NULL },
    };
    environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, (void*)descriptors);

    resolve_content_path(info->path, playlist_path, sizeof(playlist_path));
    if (strcmp(playlist_path, info->path) != 0)
        note("Content path resolved: %s -> %s", info->path, playlist_path);

    /* Before the list is read, because the rewriting happens as it is read. */
    apply_output_resolution();
    apply_output_layout();
    apply_youtube_proxy();

    iptv_playlist_free(&playlist);
    if (!iptv_playlist_load(playlist_path, &playlist)) {
        note("No channel could be read from %s", playlist_path);
        return false;
    }
    note("List loaded: %zu channels", playlist.count);



    current_channel = -1;
    pending_channel = -1;
    pending_reload = false;

    /*
     * The listing, if one was prepared beside the playlist. Missing is the
     * normal case and says nothing in the log.
     */
    iptv_epg_load_for(playlist_path);

    /*
     * begin_opening and not open_channel, so that the first entry goes through
     * exactly the same door as every other one.
     *
     * It used to call open_channel directly, and that was the whole of a bug
     * the log described perfectly: on a list of YouTube videos, entry one was
     * handed to libVLC as a youtube.com address, which libVLC cannot open, so
     * it "ended" at once and the sequence stepped to entry two. Every video
     * after the first was resolved, because every video after the first arrived
     * through retro_run.
     *
     * Two ways in for one operation, and only one of them had the new stage.
     * The second way in is now three lines of wrapper that cannot forget.
     */
    if (!begin_opening(0, "first channel")) {
        iptv_playlist_free(&playlist);
        return false;
    }

    return true;
}

/*
 * Reading the left stick as well as the d-pad.
 *
 * Edge triggered, like the buttons: a held stick changes one channel, not one
 * per frame. The threshold is half deflection, well past where a stick rests.
 */
static bool stick_pressed(unsigned axis, int sign) {
    int16_t value = input_state_cb(0, RETRO_DEVICE_ANALOG,
                                   RETRO_DEVICE_INDEX_ANALOG_LEFT, axis);
    return sign > 0 ? value > 16000 : value < -16000;
}

/*
 * Four functions, and nothing else.
 *
 * Everything the DVD core needed and this one does not has been removed:
 * pause, because a live channel cannot be paused into anything useful; aspect
 * ratio, because the frontend this targets does not honour the change; audio
 * track cycling, inherited from VLCine and never once useful on a television
 * channel; the shoulder buttons, which duplicated the d-pad; and the audio
 * delay on the triggers, which existed to work around an alignment problem
 * that turned out to have a cause and a fix.
 */
static void handle_input(void) {
    if (!input_poll_cb || !input_state_cb)
        return;

    input_poll_cb();

    #define PRESSED(id) (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, (id)) != 0)

    bool left  = PRESSED(RETRO_DEVICE_ID_JOYPAD_LEFT) ||
                 stick_pressed(RETRO_DEVICE_ID_ANALOG_X, -1);
    bool right = PRESSED(RETRO_DEVICE_ID_JOYPAD_RIGHT) ||
                 stick_pressed(RETRO_DEVICE_ID_ANALOG_X, +1);
    bool up    = PRESSED(RETRO_DEVICE_ID_JOYPAD_UP) ||
                 stick_pressed(RETRO_DEVICE_ID_ANALOG_Y, -1);
    bool down  = PRESSED(RETRO_DEVICE_ID_JOYPAD_DOWN) ||
                 stick_pressed(RETRO_DEVICE_ID_ANALOG_Y, +1);
    bool confirm = PRESSED(RETRO_DEVICE_ID_JOYPAD_A);  /* right face, Xbox B */
    bool cancel  = PRESSED(RETRO_DEVICE_ID_JOYPAD_B);  /* bottom face, Xbox A */
    bool sched   = PRESSED(RETRO_DEVICE_ID_JOYPAD_Y);  /* left face, Xbox X */
    bool guide   = PRESSED(RETRO_DEVICE_ID_JOYPAD_X);  /* top face, Xbox Y */

    /* The transport four. Read every frame like the rest; whether they mean
     * anything is decided below, by what is playing. */
    bool subs    = PRESSED(RETRO_DEVICE_ID_JOYPAD_L);   /* L1 */
    bool track   = PRESSED(RETRO_DEVICE_ID_JOYPAD_R);   /* R1 */
    bool back    = PRESSED(RETRO_DEVICE_ID_JOYPAD_L2);
    bool forward = PRESSED(RETRO_DEVICE_ID_JOYPAD_R2);
    bool pause   = PRESSED(RETRO_DEVICE_ID_JOYPAD_START);

    #undef PRESSED

    /*
     * Each button opens its own list, closes its own list, and swaps out of the
     * other one. Pressing schedule while the guide is up goes to the schedule
     * rather than doing nothing: the two are alternative answers to "what is
     * there", and having to close one before opening the other is a step that
     * exists only because the code was written one list at a time.
     */
    if (guide && !prev_guide) {
        if (iptv_osd_guide_open() && !iptv_osd_schedule_open())
            iptv_osd_close_guide();
        else
            iptv_osd_open_guide(&playlist, current_channel);
    }

    if (sched && !prev_sched) {
        if (iptv_osd_schedule_open())
            iptv_osd_close_guide();
        else
            iptv_osd_open_schedule(&playlist, current_channel);
    }

    /*
     * While the guide is open it owns the pad. Letting the channel keep
     * changing underneath a list the viewer is reading would be its own kind
     * of bug, and the two face buttons mean different things in each mode.
     */
    if (iptv_osd_guide_open()) {
        int tune = -1;
        if (up && !prev_up)
            tune = iptv_osd_key_pressed(IPTV_OSD_UP, &playlist);
        if (down && !prev_down)
            tune = iptv_osd_key_pressed(IPTV_OSD_DOWN, &playlist);
        if (left && !prev_left)
            tune = iptv_osd_key_pressed(IPTV_OSD_LEFT, &playlist);
        if (right && !prev_right)
            tune = iptv_osd_key_pressed(IPTV_OSD_RIGHT, &playlist);
        if (confirm && !prev_reload)
            tune = iptv_osd_key_pressed(IPTV_OSD_CONFIRM, &playlist);
        if (cancel && !prev_first)
            tune = iptv_osd_key_pressed(IPTV_OSD_CANCEL, &playlist);

        if (tune >= 0 && tune != current_channel)
            request_channel(tune, "chosen from the guide");
    } else {
        if (right && !prev_right)
            request_channel(current_channel + 1, "next channel");
        else if (left && !prev_left)
            request_channel(current_channel - 1, "previous channel");

        if (confirm && !prev_reload)
            request_reload("manual reload");

        if (cancel && !prev_first && current_channel != 0)
            request_channel(0, "first channel");

        /*
         * Down brings the banner up, and puts it away again. It does nothing
         * while the guide is open, where the same key moves the cursor.
         */
        if (down && !prev_down)
            iptv_osd_toggle_banner();

        /*
         * The four that only a file on disk has.
         *
         * Gated on the source rather than on is_video, and the difference
         * matters: a YouTube video ends like a file, but the address behind it
         * expires, its audio may be a separate input, and seeking it means
         * asking a CDN to serve a range of a signed URL. None of the four is
         * the simple operation here that it is on a file, so none of them is
         * offered there. A live channel has no position to seek to at all.
         *
         * Outside the guide only, like every other key in this branch: while a
         * list is open the pad belongs to the list.
         */
        if (current_source() == IPTV_SOURCE_LOCAL && core.mp) {
            char notice[IPTV_TRANSPORT_NOTICE_MAX];

            if (subs && !prev_subs) {
                iptv_transport_next_subtitle(core.mp, notice, sizeof(notice));
                if (notice[0])
                    iptv_osd_show_notice(notice);
            }

            if (track && !prev_track) {
                /*
                 * A resync, because libVLC tears down and rebuilds the audio
                 * output for the new track: what is already in the ring belongs
                 * to the old one, and playing it out would be the previous
                 * language finishing its sentence after the new one started.
                 */
                if (iptv_transport_next_audio(core.mp, notice, sizeof(notice)))
                    resynchronise_audio("audio track", 8);
                if (notice[0])
                    iptv_osd_show_notice(notice);
            }

            if (pause && !prev_pause) {
                /*
                 * The core's own pause, and deliberately not the frontend's.
                 *
                 * There is already a pause in this core: a watchdog notices
                 * when retro_run stops being called, pauses libVLC, and on a
                 * live channel reopens it afterwards to get back to live. That
                 * one is about the frontend disappearing. This one is a viewer
                 * stopping a film, and the two must not be confused - a film
                 * paused for ten minutes must not come back reopened from the
                 * start.
                 *
                 * They stay apart because the watchdog only acts on a player
                 * that is Playing, and only its own path sets the flag that
                 * makes the resume reopen anything. A deliberately paused
                 * player is simply invisible to it.
                 *
                 * Nothing below this needs a paused case: every audio branch in
                 * retro_run is already gated on core.is_playing, which a paused
                 * player is not, so the frontend keeps getting silence and the
                 * last frame for as long as this lasts.
                 */
                core.paused = !core.paused;
                libvlc_media_player_set_pause(core.mp, core.paused ? 1 : 0);

                if (core.paused) {
                    /* Stops the sound now instead of letting the ring drain a
                     * second and a half past the frozen picture. */
                    vlc_audio_flush();
                    iptv_osd_hold_notice(iptv_text(IPTV_TEXT_PAUSED));
                    note("Paused");
                } else {
                    resynchronise_audio("resume from pause", 8);
                    iptv_osd_clear_notice();
                    note("Resumed");
                }
            }

            if ((back && !prev_back) || (forward && !prev_forward)) {
                int step = (back && !prev_back) ? -IPTV_TRANSPORT_STEP_SECONDS
                                                : IPTV_TRANSPORT_STEP_SECONDS;
                /*
                 * Same reason, more so: after a jump the ring holds a second
                 * and a half of where the viewer just left, and playing it
                 * sounds exactly like a seek that did not work.
                 */
                if (iptv_transport_seek(core.mp, step, notice, sizeof(notice)))
                    resynchronise_audio("seek", 8);
                if (notice[0])
                    iptv_osd_show_notice(notice);
            }
        }
    }

    prev_up = up; prev_down = down; prev_guide = guide; prev_sched = sched;
    prev_left = left; prev_right = right;
    prev_reload = confirm; prev_first = cancel;
    prev_subs = subs; prev_track = track;
    prev_back = back; prev_forward = forward;
    prev_pause = pause;
}

/*
 * Options that can be changed while a channel plays. Anything that libVLC only
 * reads when the media is created - network caching above all - reopens the
 * channel instead, which is both simpler and honest about what is happening.
 */
/*
 * Resolves the on-screen language, and says so in the log the first time and
 * whenever it changes.
 *
 * "auto" asks the frontend. It is worth having and worth not trusting: EmuVR
 * has never been seen to answer, RetroArch answers with its own menu language,
 * and a frontend that does not implement the call leaves the variable untouched
 * - which is why it is initialised to English before the call rather than
 * after. Anything that is not a Portuguese variant becomes English, because a
 * guide in a language the core does not have is a guide in no language.
 */
static void apply_language(void) {
    static iptv_language previous = IPTV_LANG_COUNT;   /* nothing applied yet */

    const char *choice = option_value("vlchannel_language", "English");
    iptv_language language;
    const char *how = choice;

    if (strncmp(choice, "auto", 4) == 0) {
        unsigned frontend = RETRO_LANGUAGE_ENGLISH;
        if (!environ_cb(RETRO_ENVIRONMENT_GET_LANGUAGE, &frontend))
            frontend = RETRO_LANGUAGE_ENGLISH;

        if (frontend == RETRO_LANGUAGE_PORTUGUESE_BRAZIL ||
            frontend == RETRO_LANGUAGE_PORTUGUESE_PORTUGAL) {
            language = IPTV_LANG_PORTUGUESE;
            how = "auto -> frontend asked for Portuguese";
        } else {
            language = IPTV_LANG_ENGLISH;
            how = "auto -> frontend did not ask for Portuguese";
        }
    } else if (strcmp(choice, "Portugues") == 0) {
        language = IPTV_LANG_PORTUGUESE;
    } else {
        language = IPTV_LANG_ENGLISH;
    }

    if (language == previous)
        return;

    previous = language;
    iptv_text_set_language(language);
    note("On-screen language: %s (%s)",
         language == IPTV_LANG_PORTUGUESE ? "Portugues" : "English", how);
}

/*
 * Matched on the leading word rather than the whole string: the option label
 * carries commas and spaces that read well in a menu and are exactly the kind
 * of thing that gets reworded later. "now and next" losing its "and" should
 * change the menu, not the behaviour.
 */
/*
 * Returns true when the setting changed, because the caller has to reload the
 * list for it to mean anything - the rewriting is done at parse time, and a
 * list already in memory holds whichever addresses were chosen when it was
 * read.
 */
/*
 * Rereads the channel list from disk and stays where the viewer was.
 *
 * Staying put is the whole point: the alternative is that toggling a setting
 * drops someone back at channel one, which reads as a crash. The index is kept
 * rather than the name because a list reread from the same file has the same
 * entries in the same order - and if it does not, it is a different list and
 * the first entry is as good an answer as any.
 */
static void reload_playlist(const char *reason) {
    int keep = current_channel;
    iptv_playlist replacement = {0};

    /* Load into a separate object. The media player may keep playing when the
     * file is temporarily missing, and its channel metadata must remain usable
     * too: destroying the live list before knowing the replacement is valid
     * left the guide empty and made every subsequent channel request a no-op. */
    if (!iptv_playlist_load(playlist_path, &replacement)) {
        note("Rereading the list after %s found nothing usable; "
             "keeping the channel that is playing", reason);
        return;
    }

    iptv_playlist previous = playlist;
    playlist = replacement;
    iptv_playlist_free(&previous);
    note("List reread (%s): %zu channels", reason, playlist.count);

    iptv_epg_load_for(playlist_path);
    iptv_osd_close_guide();          /* its cursors point into the old list */

    if (keep < 0 || (size_t)keep >= playlist.count)
        keep = 0;
    current_channel = -1;            /* so the reopen is treated as a fresh one */
    request_channel(keep, reason);
}

static bool apply_youtube_proxy(void) {
    static int previous = -1;

    /*
     * Matched on a distinctive word rather than the whole label, for the same
     * reason the banner option is: the labels carry parenthesised hints that
     * exist to be reworded, and rewording a menu should not change behaviour.
     */
    const char *choice = option_value("vlchannel_video", "proxy");
    iptv_youtube_mode mode = strstr(choice, "alternative") ? IPTV_YOUTUBE_YTDLP
                            : strstr(choice, "disabled") ? IPTV_YOUTUBE_OFF
                                                         : IPTV_YOUTUBE_PROXY;

    iptv_playlist_set_youtube_mode(mode);

    bool changed = previous >= 0 && previous != (int)mode;
    previous = (int)mode;
    if (changed || previous < 0)
        note("Video and live entries: %s",
             mode == IPTV_YOUTUBE_YTDLP
                 ? "resolved by yt-dlp and streamlink, with the proxy last"
           : mode == IPTV_YOUTUBE_OFF   ? "left as written"
                                        : "rewritten to the riitube proxy");
    return changed;
}

static void apply_banner_info(void) {
    static int previous = -1;

    const char *choice = option_value("vlchannel_banner_info", "now and next");
    iptv_banner_info info = strstr(choice, "description")
                                ? IPTV_BANNER_SCHEDULE_DESC
                                : IPTV_BANNER_SCHEDULE;

    if ((int)info == previous)
        return;
    previous = (int)info;
    iptv_osd_set_banner_info(info);
    note("Banner second line: %s",
         info == IPTV_BANNER_SCHEDULE_DESC ? "now, description and next"
                                           : "now and next");
}

static void apply_osd_style(void) {
    static int previous = -1;

    const char *choice = option_value("vlchannel_osd", "TV");
    iptv_osd_style style = strstr(choice, "PIP")
                               ? IPTV_OSD_STYLE_CABLE_TV_PIP
                           : strcmp(choice, "Cable TV") == 0
                               ? IPTV_OSD_STYLE_CABLE_TV
                               : IPTV_OSD_STYLE_TV;

    if ((int)style == previous)
        return;
    previous = (int)style;
    iptv_osd_set_style(style);
    note("OSD style: %s",
         style == IPTV_OSD_STYLE_TV ? "TV"
       : style == IPTV_OSD_STYLE_CABLE_TV_PIP ? "Cable TV (PIP)"
                                               : "Cable TV");
}

/*
 * The option is a percentage written with a per cent sign, and "off". atoi
 * reads the digits and stops at the sign, and gives 0 for "off" - which is the
 * right answer for both without a table.
 */
static void apply_zap_volume(void) {
    static int previous = -1;

    int percent = atoi(option_value("vlchannel_zap_sound", "50%"));
    if (percent == previous)
        return;

    previous = percent;
    iptv_zap_set_volume(percent);
    if (percent > 0)
        note("Channel change noise: %d%%", percent);
    else
        note("Channel change noise: off");
}

static void apply_runtime_options(void) {
    static struct {
        char aspect[16];
        char deinterlace[16];
        int audio_track;
        int audio_delay;
        int network_caching;
        bool initialised;
    } previous = {0};

    /* Outside the first-run guard below: the language is the one option here
     * that has to be honoured on the very first pass, because the banner is
     * drawn before anything reaches this function a second time. */
    apply_language();
    apply_zap_volume();
    apply_osd_style();
    apply_banner_info();
    report_pending_output_resolution();
    if (apply_output_layout() && current_channel >= 0) {
        iptv_log("[VLChannel] Picture size changed; reopening the channel\n");
        request_reload("picture size changed");
    }

    /* The per-frame ones, read here so that nothing on a hot path has to ask
     * the frontend. See the note beside their declarations. */
    cached_loading_osd =
        option_equals("vlchannel_loading_osd", "enabled");
    set_signal_noise_percent(
        atoi(option_value("vlchannel_signal_noise", "off")));
    snprintf(cached_audio_min_queue, sizeof(cached_audio_min_queue), "%s",
             option_value("vlchannel_audio_min_queue", "1200"));
    cached_network_caching = atoi(option_value("vlchannel_network_caching",
                                               "1500"));

    /*
     * Rereading the list is the only way this one takes effect, since the
     * addresses were decided when the list was parsed. Doing it from here keeps
     * the channel the viewer is on: reload_playlist tunes back to the same
     * index rather than starting over at the first entry.
     */
    if (apply_youtube_proxy())
        reload_playlist("YouTube rewriting changed");

    if (!previous.initialised) {
        snprintf(previous.aspect, sizeof(previous.aspect), "%s",
                 option_value("vlchannel_aspect_ratio", "default"));
        snprintf(previous.deinterlace, sizeof(previous.deinterlace), "%s",
                 option_value("vlchannel_deinterlace_mode", "auto"));
        previous.audio_track = atoi(option_value("vlchannel_audio_track", "0"));
        previous.audio_delay = global_audio_offset_ms();
        previous.network_caching =
            atoi(option_value("vlchannel_network_caching", "1500"));
        previous.initialised = true;
        return;
    }

    const char *aspect = option_value("vlchannel_aspect_ratio", "default");
    if (strcmp(aspect, previous.aspect) != 0) {
        notify_geometry(aspect);
        snprintf(previous.aspect, sizeof(previous.aspect), "%s", aspect);
    }

    const char *deinterlace = option_value("vlchannel_deinterlace_mode", "auto");
    if (strcmp(deinterlace, previous.deinterlace) != 0) {
        if (vlchannel_libvlc_video_set_deinterlace) {
            if (strcmp(deinterlace, "auto") == 0)
                iptv_log("[VLChannel] Deinterlace: automatic again on the "
                                "next channel\n");
            else if (strcmp(deinterlace, "off") == 0)
                libvlc_video_set_deinterlace(core.mp, NULL);
            else
                libvlc_video_set_deinterlace(core.mp, deinterlace);
        }
        snprintf(previous.deinterlace, sizeof(previous.deinterlace), "%s",
                 deinterlace);
    }

    int audio_track = atoi(option_value("vlchannel_audio_track", "0"));
    if (audio_track != previous.audio_track) {
        if (audio_track > 0) {
            libvlc_audio_set_track(core.mp, audio_track);
            iptv_log("[VLChannel] Audio track option set to %d\n",
                    audio_track);
        }
        previous.audio_track = audio_track;
    }

    /* Applied on top of every channel, and live: this is the knob used to find
     * the frontend's own latency by ear. */
    int audio_delay = global_audio_offset_ms();
    if (audio_delay != previous.audio_delay) {
        previous.audio_delay = audio_delay;
        apply_audio_delay("global offset changed");
    }

    /*
     * Both of these are only read when the media is created, so changing them
     * reopens the channel instead of pretending to take effect.
     */
    int network_caching = atoi(option_value("vlchannel_network_caching", "1500"));
    if (network_caching != previous.network_caching) {
        previous.network_caching = network_caching;
        iptv_log("[VLChannel] Caching changed to %d ms; reopening the "
                        "channel\n", network_caching);
        request_reload("caching changed");
    }
}

/* --------------------------------------------------- videos in a channel list
 *
 * A YouTube entry is a video: it ends, and the end is not a fault. When one
 * ends the next entry in the list starts, wrapping round at the bottom, so a
 * list of videos behaves like a channel that is always on - which is the whole
 * point of putting them in a channel list rather than in a player.
 *
 * Everything below exists to keep that from becoming a stampede.
 */

static bool current_is_video(void) {
    return current_channel >= 0 &&
           (size_t)current_channel < playlist.count &&
           playlist.channels[current_channel].is_video;
}

static void advance_after_video(void) {
    uint64_t now = monotonic_time_ms();
    uint64_t played = iptv_seq_played_ms(now);

    switch (iptv_seq_ended(now)) {
    case IPTV_SEQ_QUIET:
        return;

    case IPTV_SEQ_HOLD:
        note("%d videos ended in a row without playing. Stopping here rather "
             "than walking the list: this is what a proxy that is down looks "
             "like, not what %d missing videos look like. Check the [VLC-LOG] "
             "lines and press the right face button.",
             iptv_seq_chain(), iptv_seq_chain());
        return;

    case IPTV_SEQ_ADVANCE: {
        size_t next = iptv_seq_next((size_t)current_channel, playlist.count);
        note("Video ended after %llu s; going to %zu/%zu",
             (unsigned long long)(played / 1000), next + 1, playlist.count);
        request_channel((int)next, "video ended");
        return;
    }
    }
}

/*
 * A resolver succeeding only proves that an address was produced. The CDN can
 * still reject it when libVLC opens it. Retry the same item through the proxy
 * once; open_channel_from is told the proxy URL did not come from a resolver,
 * so this cannot loop.
 *
 * Only a YouTube video reaches the second half of this. A channel live page and
 * a platform address carry no video id, so the proxy has nothing it could be
 * given - and a live broadcast that stops has usually simply ended, which is
 * the ordinary case and not a failure worth a second attempt.
 */
static bool retry_resolved_with_proxy(const char *failure) {
    if (!current_open_is_resolved || !current_is_video())
        return false;

    current_open_is_resolved = false;

    if (playlist.channels[current_channel].source != IPTV_SOURCE_YOUTUBE)
        return false;

    char id[16];
    if (!iptv_youtube_id(playlist.channels[current_channel].url,
                         id, sizeof(id)))
        return false;

    char fallback[640];
    snprintf(fallback, sizeof(fallback), "%s%s",
             IPTV_YOUTUBE_PROXY_DEFAULT, id);
    note("The resolved URL %s before playback; retrying this video through the "
         "proxy", failure);
    return open_channel_from(current_channel,
                             "resolved URL failed; proxy fallback",
                             fallback, NULL, false);
}

static void report_state_change(void) {
    if (!core.mp)
        return;

    libvlc_state_t state = libvlc_media_player_get_state(core.mp);
    if (state == last_reported_state)
        return;
    last_reported_state = state;

    switch (state) {
    case libvlc_Opening:
        note("Opening the stream");
        break;
    case libvlc_Buffering:
        note("Buffering");
        break;
    case libvlc_Playing:
        set_no_signal(false);
        note("Playing");
        iptv_seq_playing(monotonic_time_ms());
        current_open_is_resolved = false;
        break;
    case libvlc_Ended:
        if (current_is_video()) {
            if (retry_resolved_with_proxy("ended"))
                break;
            advance_after_video();
            break;
        }
        note("The stream ended. A live channel should not end: the server "
             "closed it or the list is out of date. Press the right face "
             "button to reconnect.");
        core.transitioning = false;
        set_no_signal(true);
        break;
    case libvlc_Error:
        note("libVLC reported an error on this channel. The [VLC-LOG] lines "
             "name the cause - resolution failure, 403, timeout or an "
             "unsupported protocol.");
        /* A video that cannot be opened is stepped over rather than sat on: one
         * removed video should not stop a list of two hundred. The same chain
         * guard applies, so a proxy that errors on everything stops after three
         * instead of walking the list. */
        if (current_is_video()) {
            if (!retry_resolved_with_proxy("was rejected"))
                advance_after_video();
        } else {
            core.transitioning = false;
            set_no_signal(true);
        }
        break;
    default:
        break;
    }
}

/*
 * The single door every audio block leaves through.
 *
 * There are seven places in retro_run that hand a block to the frontend, and
 * five of them are silence: muted, no PTS yet, still prebuffering, starved
 * mid-block, not calibrated. A channel change passes through those five, which
 * is precisely where the change noise belongs - it plays over a gap that
 * already existed rather than over the programme.
 *
 * Routing all seven through here is what keeps the noise out of the audio path
 * proper. Nothing above this line knows the noise exists; the block is computed
 * exactly as it was before, and the noise is summed onto it on the way out. With
 * the volume off, this function is a call to audio_batch_cb and nothing else.
 */
static void emit_audio(int16_t *stereo, unsigned frames) {
    if (!audio_batch_cb || frames == 0)
        return;

    iptv_zap_mix(stereo, frames);
    audio_batch_cb(stereo, frames);
}

RETRO_API void retro_run(void) {
    if (!core.libvlc)
        return;

    /* Told once a frame, from the one place that knows: a channel is opening,
     * or a yt-dlp resolution is still running - both are waits with nothing to
     * show, and both are the same thing to whoever is looking at the screen. */
    iptv_osd_set_waiting(
        cached_loading_osd &&
        (core.transitioning || resolving_channel >= 0));

    note_frontend_run(core.mp);
    iptv_osd_tick();
    handle_input();

    /* Channel changes are carried out here, on the libretro thread, and never
     * from the input handler or the watchdog. */
    if (pending_reload) {
        pending_reload = false;
        begin_opening(current_channel, pending_reason);
    } else if (pending_channel >= 0) {
        int index = pending_channel;
        pending_channel = -1;
        begin_opening(index, pending_reason);
    }

    /*
     * A resolution in flight. Polled, never waited on: a resolver takes one to
     * five seconds, and a retro_run that does not return for five seconds is a
     * frontend that has stopped drawing - under EmuVR, a headset that has
     * stopped drawing.
     *
     * Two tools can be in this position, so the poll asks the one that was
     * started. They deliberately do not run at the same time; see
     * start_resolver.
     */
    if (resolving_channel >= 0) {
        int index = resolving_channel;
        const char *name = resolving_with == RESOLVER_STREAMLINK ? "streamlink"
                                                                 : "yt-dlp";
        bool working, done;
        const char *resolved = NULL;
        const char *resolved_audio = NULL;

        if (resolving_with == RESOLVER_STREAMLINK) {
            iptv_sl_state now = iptv_streamlink_poll();
            working = now == IPTV_SL_WORKING;
            done = now == IPTV_SL_DONE;
            resolved = iptv_streamlink_url();
        } else {
            iptv_yt_state now = iptv_ytdlp_poll();
            working = now == IPTV_YT_WORKING;
            done = now == IPTV_YT_DONE;
            resolved = iptv_ytdlp_url();
            resolved_audio = iptv_ytdlp_audio_url();
        }

        if (working) {
            /* Nothing to do this frame. */
        } else if (done) {
            resolving_channel = -1;
            resolving_with = RESOLVER_NONE;
            note("%s resolved in %llu ms", name,
                 (unsigned long long)(monotonic_time_ms() - resolving_since));
            open_channel_from(index, resolving_reason,
                              resolved, resolved_audio, true);
        } else {
            /*
             * No answer, or a timeout. The other tool gets a turn before
             * anything is given up on: they fail for different reasons, and a
             * site one of them has stopped tracking is usually a site the other
             * still handles.
             */
            iptv_source source = playlist.channels[index].source;
            resolver other = (resolving_with == first_resolver(source))
                                 ? second_resolver(source)
                                 : RESOLVER_NONE;

            note("%s did not resolve this address", name);
            resolving_channel = -1;
            resolving_with = RESOLVER_NONE;

            if (other == RESOLVER_NONE || !start_resolver(index, other)) {
                /*
                 * Both tools are out of answers. A YouTube video still has the
                 * proxy - someone who forgot to copy one file gets a list that
                 * plays, and the log says which path was taken. The cost is
                 * that "alternative" is not always a resolver, which is a
                 * smaller surprise than a list where nothing works.
                 */
                char fallback[640];
                char id[16];
                if (source == IPTV_SOURCE_YOUTUBE &&
                    iptv_youtube_id(playlist.channels[index].url,
                                    id, sizeof(id))) {
                    snprintf(fallback, sizeof(fallback), "%s%s",
                             IPTV_YOUTUBE_PROXY_DEFAULT, id);
                    note("Using the proxy for this video instead");
                    open_channel_from(index, resolving_reason,
                                      fallback, NULL, false);
                } else {
                    /*
                     * The commonest answer here is not a fault: the channel is
                     * simply not broadcasting. Said in those words, because
                     * "did not resolve" sends whoever reads it looking for a
                     * broken installation, and the address itself is still
                     * perfectly good for the next broadcast.
                     *
                     * The entry is still opened, and it still fails - libVLC
                     * cannot read a web page. That is on purpose: what happens
                     * to an entry that will not play lives in one place, driven
                     * by the player's own state, and it is not the same thing
                     * for the two kinds. A YouTube video is stepped over; a
                     * platform channel shows no signal and waits. A second copy
                     * of that decision here would be a second thing to keep in
                     * step with the first.
                     */
                    note(playlist.channels[index].source ==
                                 IPTV_SOURCE_PLATFORM
                             ? "Nothing is on air at this address, or no tool "
                               "here can open it. This is a live channel, so "
                               "it shows no signal; press the right face "
                               "button to try again."
                             : "Nothing is on air at this address, or no tool "
                               "here can open it. The entry is stepped over "
                               "and the address stays good for next time.");
                    open_channel(index, resolving_reason);
                }
            }
        }
    }

    if (!core.mp)
        return;

    double fps = (core.video_fps > 0.0) ? core.video_fps : 60.0;
    int samples_per_frame = (int)(AUDIO_TARGET_RATE / fps + 0.5);
    static int16_t silence_buffer[48000 * 2];

    if (core.pending_start) {
        iptv_log("[VLChannel] Starting playback\n");
        libvlc_media_player_play(core.mp);
        core.pending_start = false;
    }

    report_state_change();
    core.is_playing =
        (libvlc_media_player_get_state(core.mp) == libvlc_Playing);

    if (core.is_playing)
        retry_audio_delay();


    /*
     * Where the audio actually sits relative to the picture.
     *
     * libvlc_media_player_get_time() follows the input clock, which is what
     * paces the picture. The audio this core is about to play is the oldest
     * sample in the ring, which is the newest PTS libVLC handed us minus
     * everything still queued. The difference between the two is the drift the
     * user hears - and measuring it is the only way to tell a constant offset,
     * which a fixed delay can correct, from an alignment that lands somewhere
     * new on every channel open, which it cannot.
     */
    if (core.is_playing) {
        static uint32_t offset_countdown = 0;
        if (offset_countdown++ % 120 == 0) {
            /*
             * How often the three sync lines are written.
             *
             * They used to go out on every one of these passes - one set every
             * two seconds, for as long as the core ran. Measured on a 28 minute
             * EmuVR session: 1990 lines, 159 KB, which is 5.6 KB a minute and
             * about 8 MB across a day of a set left switched on. That is not a
             * fault, but it is the whole of what the core writes, and a log
             * where ninety-seven per cent of the lines say "still fine" buries
             * the ones that do not.
             *
             * Fast for the first minute of each channel and slow afterwards,
             * because those two stretches answer different questions. The first
             * minute is where the reserve fills and the loop settles - it is
             * what ferramentas/analisa-log.py measures the settling time from,
             * and it needs samples close together. After that the interesting
             * event is a departure, not a value, and one sample every twenty
             * seconds is plenty to plot a queue that moves by single
             * milliseconds.
             *
             * The line formats are untouched on purpose. The analyser reads
             * them, and so do the reference logs from the drift work - 41 and
             * 43 minutes that are the only evidence any of this ever worked.
             * Making the log cheaper is not worth making that evidence
             * unreadable.
             */
            #define SYNC_FAST_PASSES  30      /* 30 x 2 s = the first minute */
            #define SYNC_SLOW_EVERY   10      /* then one pass in ten: 20 s */
            static uint32_t passes_this_channel = 0;
            static int last_channel_seen = -1;

            if (last_channel_seen != current_channel) {
                last_channel_seen = current_channel;
                passes_this_channel = 0;
            }
            uint32_t pass = passes_this_channel++;

            bool due = (pass < SYNC_FAST_PASSES) ||
                       (pass % SYNC_SLOW_EVERY == 0);
            pthread_mutex_lock(&core.mutex);
            size_t r = core.audio_read_pos;
            size_t w = core.audio_write_pos;
            size_t avail = (w >= r) ? (w - r) : (AUDIO_BUFFER_SIZE - r + w);
            int64_t queued_ms =
                (int64_t)(avail / 2) * 1000 / AUDIO_TARGET_RATE;
            int64_t audio_pts_ms = core.last_audio_pts / 1000;
            pthread_mutex_unlock(&core.mutex);

            libvlc_time_t player_ms = libvlc_media_player_get_time(core.mp);

            /* A queue away from its reserve is the symptom the whole drift
             * loop exists for, so it is said the moment it happens rather than
             * waiting for the next slot. */
            int64_t off_target = queued_ms - sync_reserve_ms;
            if (off_target < 0) off_target = -off_target;
            if (off_target > SYNC_QUEUE_TOLERANCE_MS)
                due = true;

            if (audio_pts_ms > 0 && player_ms > 0 && due) {
                note("Audio queue %lld ms (player at %lld ms)",
                     (long long)queued_ms, (long long)player_ms);
                if (current_uses_video_clock() && vlchannel_libvlc_clock)
                    note("YouTube PTS queue target %lld ms",
                         (long long)sync_reserve_ms);
            }

            /*
             * The drift, measured over a sliding window.
             *
             * The first version of this took one origin sample and compared
             * every later sample against it. Two logs killed it. Under
             * RetroArch the origin was captured while the queue was still
             * empty, so the report divided the whole initial fill by the time
             * elapsed and announced "+39.9 ms/s" falling to "+3.6 ms/s" - the
             * signature of a constant divided by a growing number, not of a
             * drift. Under EmuVR the origin was fine, but a two point slope
             * over an ever growing arm still decayed toward zero: -4.1 ms/s
             * early, -0.7 ms/s at the end, on a queue that was in fact draining
             * at a steady -3.5 ms/s the whole time.
             *
             * A drift is a slope, and a slope wants a least squares fit over a
             * fixed window, not two points ever further apart. Thirty samples
             * at one every two seconds is a minute of arm: long enough that the
             * sawtooth of the queue averages out, short enough that the number
             * describes now instead of the whole session.
             *
             * This still measures and reports only. Nothing is added, dropped
             * or resized in the audio path.
             */
#define DRIFT_SAMPLES 30
            static int64_t drift_t[DRIFT_SAMPLES];
            static int64_t drift_q[DRIFT_SAMPLES];
            static int     drift_count = 0;
            static int     drift_next = 0;
            static int     drift_channel = -2;
            static int64_t drift_sent = -1;

            /* A channel change or an audio resync both zero audio_sent_frames,
             * and both invalidate every sample collected so far. */
            if (drift_channel != current_channel ||
                core.audio_sent_frames < drift_sent) {
                drift_channel = current_channel;
                drift_count = 0;
                drift_next = 0;
            }
            drift_sent = core.audio_sent_frames;

            /*
             * Samples taken while the queue is still filling would describe the
             * prebuffer, not the drift. That is exactly the mistake the first
             * version made, so the guard is explicit rather than assumed.
             */
            static int64_t drift_last_sample_ms = 0;
            int64_t sample_now = (int64_t)monotonic_time_ms();

            if (!core.audio_prebuffering && queued_ms > 100) {
                drift_t[drift_next] = sample_now;
                drift_q[drift_next] = queued_ms;
                drift_next = (drift_next + 1) % DRIFT_SAMPLES;
                drift_last_sample_ms = sample_now;
                if (drift_count < DRIFT_SAMPLES)
                    drift_count++;
            }

            /*
             * A window that stopped being fed must stop being reported.
             *
             * The guard above skips samples while the queue is under 100 ms, so
             * a queue that has bottomed out freezes the window and the same
             * slope gets printed forever. One log did exactly that: thirty two
             * seconds of "-3.5 ms/s" while the queue sat at 14 ms and was not
             * moving at all. The slope was true of a minute that had already
             * passed, which is the most convincing way for a measurement to
             * lie.
             */
            if (drift_count == DRIFT_SAMPLES &&
                sample_now - drift_last_sample_ms > 6000) {
                note("Audio queue starved at %lld ms and is not refilling; "
                     "drift no longer measurable", (long long)queued_ms);
            } else if (drift_count == DRIFT_SAMPLES) {
                /*
                 * Least squares, in integers. Times are taken relative to the
                 * oldest sample so the sums stay small: a minute of window is
                 * 60000 ms, and n*sum(dt*dt) then peaks around 3e12, well
                 * inside int64.
                 */
                int oldest = drift_next;          /* the ring's tail */
                int64_t base = drift_t[oldest];
                int64_t n = DRIFT_SAMPLES;
                int64_t sum_t = 0, sum_q = 0, sum_tq = 0, sum_tt = 0;

                for (int k = 0; k < DRIFT_SAMPLES; k++) {
                    int idx = (oldest + k) % DRIFT_SAMPLES;
                    int64_t dt = drift_t[idx] - base;
                    int64_t q  = drift_q[idx];
                    sum_t  += dt;
                    sum_q  += q;
                    sum_tq += dt * q;
                    sum_tt += dt * dt;
                }

                int64_t den = n * sum_tt - sum_t * sum_t;
                int64_t span_s = (drift_t[(oldest + DRIFT_SAMPLES - 1) %
                                          DRIFT_SAMPLES] - base) / 1000;

                if (den > 0 && span_s > 0) {
                    int64_t num = n * sum_tq - sum_t * sum_q;
                    /* ms per ms -> tenths of ms per second */
                    int64_t rate_tenths = (num * 10000) / den;

                    int32_t ratio = iptv_drift_ratio();
                    int64_t ppm = ((int64_t)(ratio - IPTV_DRIFT_ONE) * 1000000)
                                  / IPTV_DRIFT_ONE;

                    /*
                     * The other reason to speak out of turn: the correction
                     * arriving at, or leaving, its ceiling. At the ceiling the
                     * loop has spent all its authority, and whatever the queue
                     * does next is no longer its doing - which is the one state
                     * worth interrupting a quiet log for.
                     *
                     * The edge and not the state. Measured on a 28 minute
                     * session, the ratio was pinned for 28% of the samples but
                     * crossed in or out only 31 times: reporting the state
                     * would have kept a fifth of the lines this change exists
                     * to remove, and said the same thing two hundred times.
                     *
                     * The instantaneous drift is deliberately *not* a trigger.
                     * It reads above 0.5 ms/s - the value the analyser fails a
                     * session for - in 82% of the samples of a session with no
                     * starvation, no clock resets and a median queue of 1171 ms
                     * against a target of 1200. Over a 30 sample window it is a
                     * noisy quantity, and a trigger that fires four times out of
                     * five is not a trigger, it is the cadence with extra steps.
                     */
                    static bool was_saturated = false;
                    int32_t step = ratio - IPTV_DRIFT_ONE;
                    if (step < 0) step = -step;
                    bool saturated = step >= IPTV_DRIFT_MAX_STEP;
                    if (saturated != was_saturated) {
                        was_saturated = saturated;
                        due = true;
                    }

                    if (due) {
                        if (rate_tenths != 0) {
                            /*
                             * The sign is written by hand, not by "%+lld".
                             *
                             * A rate of -0.5 ms/s has an integer part of zero, and
                             * "%+lld" prints "+0" for it - the report would say the
                             * queue is growing while it drains. Half the value of
                             * this line is its direction, and that is exactly the
                             * half the obvious format string throws away.
                             */
                            char sign = rate_tenths < 0 ? '-' : '+';
                            int64_t magnitude = rate_tenths < 0 ? -rate_tenths
                                                                : rate_tenths;
                            note("Audio drift %c%lld.%lld ms/s over the last %llds "
                                 "(queue now %lld ms); 100 ms of error every %llds",
                                 sign,
                                 (long long)(magnitude / 10),
                                 (long long)(magnitude % 10),
                                 (long long)span_s,
                                 (long long)queued_ms,
                                 (long long)((100 * 10) / magnitude));
                        } else {
                            note("Audio drift below 0.1 ms/s over the last %llds "
                                 "(queue now %lld ms)",
                                 (long long)span_s, (long long)queued_ms);
                        }

                        /*
                         * What the loop is doing about it, next to what it is
                         * reacting to. Reported as parts per million of the
                         * identity so a correction of a third of a percent is a
                         * legible -3500 rather than a ratio nobody can read; the
                         * identity is exactly 0, which is the value RetroArch
                         * should show.
                         */
                        note("Audio resample %c%lld ppm (%s)",
                             ppm < 0 ? '-' : '+',
                             (long long)(ppm < 0 ? -ppm : ppm),
                             ppm == 0 ? "identity, byte for byte the old path"
                                      : "correcting");
                    }
                }
            }
        }
    }

    bool variables_updated = false;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &variables_updated) &&
        variables_updated)
        runtime_options_dirty = true;
    if (runtime_options_dirty) {
        runtime_options_dirty = false;
        apply_runtime_options();
    }

    /*
     * A channel is "in transition" from the moment it is opened until libVLC
     * has negotiated a format and produced a picture. On a network source that
     * is seconds, not frames, so output stays silent and black instead of
     * showing whatever the previous channel left behind.
     */
    /*
     * While yt-dlp is being asked, this watchdog belongs to nothing.
     *
     * It counts from the moment a channel was handed to libVLC, and during a
     * resolution no channel has been handed to libVLC yet - the one it is still
     * counting is the entry the viewer has already left. The captured log shows
     * exactly that: "Still opening after 3 s" printed while the core was two
     * seconds into resolving the *next* video, on a stopwatch started by the
     * previous one.
     *
     * Harmless as a message, and not harmless at the end: a resolution slower
     * than the give-up timeout would make the watchdog stop a player that is
     * not playing anything and, because the entry is a video, advance the list -
     * with a resolution for the entry it just left still in flight. Whichever
     * answer arrives first wins, which is the definition of a race.
     */
    if (core.transitioning && resolving_channel < 0) {
        pthread_mutex_lock(&core.mutex);
        unsigned negotiated_width = core.video_width;
        unsigned negotiated_height = core.video_height;
        bool geometry_known = negotiated_width > 0 && negotiated_height > 0;
        pthread_mutex_unlock(&core.mutex);
        bool ready = geometry_known && vlc_video_has_frame();
        unsigned reload_count = atomic_load_explicit(
            &playlist_reloads, memory_order_relaxed);

        opening_frames++;

        /*
         * Reported once, and only reported: the channel is left to the ordinary
         * timeout instead of being stopped early. Nothing here is certain
         * enough to justify cutting off a channel that might still come good,
         * and the point is to replace a silent black screen with a sentence
         * that names the cause.
         */
        if (!ready && !frozen_playlist_reported && reload_count > 20) {
            frozen_playlist_reported = true;
            note("This playlist has been reloaded %u times without a single "
                 "segment being fetched. Open the same URL in VLC desktop "
                 "before blaming the server: the first channel that produced "
                 "this pattern played perfectly there, and the cause was an "
                 "option this core was passing to libVLC, not the stream.",
                 reload_count);
        }

        if (!ready && opening_frames % 180 == 0)
            note("Still opening after %u s (libVLC state %d). No picture yet.",
                 opening_frames / 60,
                 (int)libvlc_media_player_get_state(core.mp));

        if (ready || --core.transition_timeout_frames == 0) {
            bool reopened = false;
            if (ready) {
                /*
                 * Only the source rectangle inside the fixed canvas changes.
                 * Rebuilding the frontend's AV pipeline mid-session is what
                 * froze the picture in EmuVR during VLCine's development.
                 */
                notify_geometry(NULL);
                apply_audio_delay("channel ready");
                note("Channel ready: %ux%u", negotiated_width,
                     negotiated_height);
            } else {
                /*
                 * Stop, do not just give up on the picture. libVLC keeps
                 * retrying on its own, and a server answering with a frozen
                 * HLS playlist gets re-requested about sixteen times a second
                 * - the first captured failure made 437 requests in 27
                 * seconds and downloaded no segment at all. Leaving that
                 * running behind a black screen is both useless and rude.
                 */
                iptv_log(
                        "[VLChannel] No picture %u s after opening this channel; "
                        "stopping it. Check the [VLC-LOG] lines above: a "
                        "playlist that reloads without ever downloading a .ts "
                        "segment is a dead or expired URL, not a decoding "
                        "problem. Press the right face button to try again.\n",
                        opening_frames / 60);
                libvlc_media_player_stop(core.mp);
                vlc_video_reset();
                reset_output_picture();

                /*
                 * On a video, stopping is not the end of the story: the list is
                 * meant to keep running. Stepping over it is the same decision
                 * as on an error, through the same guard - and it has to happen
                 * after the stop, because the entry being stepped over must
                 * stop hammering the server before the next one starts.
                 */
                if (current_is_video()) {
                    if (retry_resolved_with_proxy("timed out"))
                        reopened = true;
                    else
                        advance_after_video();
                } else {
                    set_no_signal(true);
                }
            }

            /* open_channel_from has already established all of this for the
             * proxy retry. Do not tear its new transition down with the cleanup
             * that belongs to the failed googlevideo URL. */
            if (!reopened) {
                core.transitioning = false;
                pthread_mutex_lock(&core.mutex);
                core.audio_mute_frames = 0;
                pthread_mutex_unlock(&core.mutex);
                core.audio_sent_frames = 0;
                core.sync_offset_initialized = false;
                core.sample_accum_frac = 0.0;
            }
        }
    }

    /* The SAR log can arrive a little after the first decoded picture. Once the
     * transition is over, apply such a late correction without renegotiating
     * anything with the frontend. */
    pthread_mutex_lock(&video_sar_mutex);
    bool source_aspect_changed = video_sar_dirty;
    pthread_mutex_unlock(&video_sar_mutex);
    if (!core.transitioning && source_aspect_changed)
        notify_geometry(NULL);

    /*
     * Audio, kept from VLCine unchanged in substance: a PCM ring with an 80 ms
     * prebuffer, one complete block per frame, and fades instead of hard cuts.
     * This is the part that took the longest to stabilise there, and a live
     * stream stresses it in the same ways a DVD transition did.
     */
    if (core.is_playing && audio_batch_cb) {
        pthread_mutex_lock(&core.mutex);
        if (core.audio_mute_frames > 0) {
            core.audio_mute_frames--;
            pthread_mutex_unlock(&core.mutex);
            memset(silence_buffer, 0, samples_per_frame * 2 * sizeof(int16_t));
            emit_audio(silence_buffer, samples_per_frame);
            submit_current_video_frame();
            return;
        }

        int64_t pts_us = core.last_audio_pts;
        unsigned pts_count = core.last_audio_count;
        if (pts_us <= 0) {
            pthread_mutex_unlock(&core.mutex);
            memset(silence_buffer, 0, samples_per_frame * 2 * sizeof(int16_t));
            emit_audio(silence_buffer, samples_per_frame);
            submit_current_video_frame();
            return;
        }

        int64_t pts_frames = (pts_us * (int64_t)AUDIO_TARGET_RATE) / 1000000LL;

        double audio_fps = (core.video_fps > 0.0) ? core.video_fps : 60.0;
        double exact_frames = (double)AUDIO_TARGET_RATE / audio_fps;
        int to_output = (int)(exact_frames + core.sample_accum_frac);
        core.sample_accum_frac += exact_frames - to_output;
        if (to_output > 5000) to_output = 5000;

        size_t r = core.audio_read_pos;
        size_t w = core.audio_write_pos;
        size_t avail = (w >= r) ? (w - r) : (AUDIO_BUFFER_SIZE - r + w);
        size_t frames_avail = avail / 2;

        /*
         * The ring has to be drained, not only filled.
         *
         * This core consumed exactly one block per retro_run and dropped the
         * backlog only while prebuffering. Everything libVLC pushed after that
         * point simply accumulated: it decodes ahead to fill its caching window
         * and hands the result to amem in one burst, and nothing here ever gave
         * it back. Measured on one EmuVR session, the queue settled at 84-144 ms
         * on two channels and at 1052-1133 ms on two others - a full second of
         * audio latency, differing per channel, permanent for as long as the
         * channel played.
         *
         * That is what made the audio delay feel random. It was never a matter
         * of tens of milliseconds to be tuned per channel; it was a second of
         * queue that landed wherever the burst happened to land.
         *
         * So the queue is bounded. Above the ceiling, the excess is dropped in
         * one go and playback continues from the target, with the same short
         * fade used elsewhere so the correction is a blip rather than a click.
         * It normally fires once, shortly after a channel opens.
         */
        /*
         * The reserve, and why it is the size of the caching window.
         *
         * libVLC hands audio to amem as soon as it is decoded, but hands video
         * to vmem paced by its own clock. The audio therefore arrives about one
         * caching window early, and this ring is what holds it back: the queue
         * length is what lines the sound up with the matching picture.
         *
         * Measured across two sessions of the same list, before this was
         * understood. Under RetroArch every channel happened to settle at
         * 1.0-1.2 s of queue and none was out of sync. Under EmuVR two channels
         * settled there and played correctly, while two settled at 48-112 ms
         * and were out of sync for the whole session - the state was decided by
         * whether the reserve filled before libVLC's opening burst arrived, and
         * it never recovered.
         *
         * So the reserve is no longer 80 ms and a race. It is the caching value
         * itself, which makes the alignment deterministic instead of lucky, and
         * ties the two numbers that were always describing the same window.
         */
        size_t prebuffer_frames;
        const char *reserve = cached_audio_min_queue;
        if (strncmp(reserve, "match", 5) == 0) {
            int caching_ms = cached_network_caching;
            prebuffer_frames =
                (size_t)(AUDIO_TARGET_RATE * (size_t)caching_ms) / 1000;
        } else {
            /* "80 ms (legacy)" is the pre-0.2.3 behaviour, kept for comparison
             * because it is the one every earlier log was recorded with. */
            int reserve_ms = atoi(reserve);
            if (reserve_ms < 40) reserve_ms = 40;
            prebuffer_frames =
                (size_t)(AUDIO_TARGET_RATE * (size_t)reserve_ms) / 1000;
        }

        /*
         * A libVLC audio PTS is not the position inside the media. It is the
         * monotonic wall-clock time at which the block is expected to play.
         * For a YouTube DASH pair this is the authoritative relationship
         * between its separate video and audio inputs.
         *
         * last_audio_pts names the start of the newest block. Adding that
         * block's duration gives the expected play time at the tail of our
         * ring. The distance from now to that tail is therefore exactly how
         * much audio the ring should hold. Unlike the fixed 1200 ms live-TV
         * reserve, this target survives a video change and a different amount
         * of decode-ahead without inventing a new A/V offset.
        */
        size_t sync_target_frames = prebuffer_frames;
        bool pts_target_active = false;
        if (current_uses_video_clock() && vlchannel_libvlc_clock &&
            pts_count > 0) {
            int64_t tail_pts = pts_us +
                ((int64_t)pts_count * 1000000LL) / AUDIO_TARGET_RATE;
            int64_t delay_us = tail_pts - libvlc_clock();

            /* Forty milliseconds keeps two complete 60 Hz blocks available.
             * Five seconds rejects a bogus timestamp without allowing it to
             * turn into an enormous unsigned queue target. */
            if (delay_us < 40000)
                delay_us = 40000;
            if (delay_us > 5000000)
                delay_us = 5000000;

            sync_target_frames =
                (size_t)((delay_us * AUDIO_TARGET_RATE) / 1000000LL);
            pts_target_active = true;
        }

        /*
         * Said out loud whenever it changes, which is normally once per session.
         *
         * "match caching" is not a value, it is a formula, and the number it
         * produces depends on an option on a different line of the menu. That
         * was the point - the two were always describing the same window - but
         * it also means the reserve can move without anyone having touched the
         * reserve. A log that reports the setting by name cannot tell that
         * apart; one that reports the number it resolved to can.
         */
        {
            static size_t reported_reserve = (size_t)-1;
            /* Also kept for the sync report, which needs to know what the queue
             * is supposed to be before it can say the queue has left it. */
            sync_reserve_ms = (int64_t)(
                (pts_target_active ? sync_target_frames : prebuffer_frames) *
                1000) / AUDIO_TARGET_RATE;

            if (prebuffer_frames != reported_reserve) {
                reported_reserve = prebuffer_frames;
                note("Audio reserve resolved: \"%s\" -> %zu frames (%zu ms)",
                     reserve, prebuffer_frames,
                     (prebuffer_frames * 1000) / AUDIO_TARGET_RATE);
            }
        }

        if (core.audio_prebuffering) {
            if (frames_avail < sync_target_frames) {
                pthread_mutex_unlock(&core.mutex);
                memset(silence_buffer, 0, to_output * 2 * sizeof(int16_t));
                emit_audio(silence_buffer, to_output);
                submit_current_video_frame();
                return;
            }

            /*
             * Discard everything older than the reserve before resuming. Each
             * underrun emits a block of silence, and resuming from where the
             * ring left off means that silence permanently delays the audio
             * against the picture.
             */
            if (frames_avail > sync_target_frames) {
                size_t excess = frames_avail - sync_target_frames;
                core.audio_read_pos =
                    (core.audio_read_pos + excess * 2) % AUDIO_BUFFER_SIZE;
                iptv_log(
                        "[VLChannel] Audio prebuffer ready: %zu frames, dropped "
                        "%zu stale (%zu ms) to stay in sync\n",
                        frames_avail, excess,
                        (excess * 1000) / AUDIO_TARGET_RATE);
                frames_avail = sync_target_frames;
            } else {
                iptv_log("[VLChannel] Audio prebuffer ready: %zu frames\n",
                        frames_avail);
            }

            core.audio_prebuffering = false;
            core.audio_fade_in_frames = (AUDIO_TARGET_RATE * 10) / 1000;
            core.sync_offset_initialized = false;
        }

        /*
         * Never mix a short real block with silence: the discontinuity is heard
         * as a click. Refill the reserve and fade out smoothly if timing jitter
         * ever consumes the complete block.
         */
        if (frames_avail < (size_t)to_output) {
            int16_t last_left = core.audio_last_left;
            int16_t last_right = core.audio_last_right;
            core.audio_last_left = 0;
            core.audio_last_right = 0;
            core.audio_prebuffering = true;
            core.audio_fade_in_frames = 0;
            core.sync_offset_initialized = false;
            pthread_mutex_unlock(&core.mutex);

            memset(silence_buffer, 0, to_output * 2 * sizeof(int16_t));
            int fade_frames = to_output < 256 ? to_output : 256;
            for (int i = 0; i < fade_frames; i++) {
                int remaining = fade_frames - i;
                silence_buffer[i * 2] =
                    (int16_t)(((int32_t)last_left * remaining) / fade_frames);
                silence_buffer[i * 2 + 1] =
                    (int16_t)(((int32_t)last_right * remaining) / fade_frames);
            }
            emit_audio(silence_buffer, to_output);
            submit_current_video_frame();
            return;
        }

        /* Keep a stable PTS lead so short network stalls do not starve
         * output. */
        int64_t target_delay_frames = (AUDIO_TARGET_RATE * 120) / 1000;

        if (!core.sync_offset_initialized && pts_frames > 0) {
            core.sync_offset =
                pts_frames - core.audio_sent_frames - target_delay_frames;
            core.sync_offset_initialized = true;
            iptv_log(
                    "[VLChannel] PTS sync calibrated: target lead %lld frames "
                    "(120 ms)\n", (long long)target_delay_frames);
        }

        if (!core.sync_offset_initialized) {
            pthread_mutex_unlock(&core.mutex);
            memset(silence_buffer, 0, samples_per_frame * 2 * sizeof(int16_t));
            emit_audio(silence_buffer, samples_per_frame);
            submit_current_video_frame();
            return;
        }

        int64_t error =
            pts_frames - core.sync_offset - core.audio_sent_frames;

        /*
         * A jump of more than a second means the timeline moved under us: a
         * channel change, a discontinuity in the transport stream, or the
         * server restarting the segment sequence. Recalibrate instead of trying
         * to catch up.
         */
        const int64_t JUMP_THRESHOLD = AUDIO_TARGET_RATE;
        if (llabs(error) > JUMP_THRESHOLD) {
            iptv_log(
                    "[VLChannel] Timeline jump of %lld frames, recalibrating\n",
                    (long long)error);
            core.sync_offset =
                pts_frames - core.audio_sent_frames - target_delay_frames;
        }

        int64_t real_to_send = to_output;
        int16_t out_buffer[to_output * 2];

        /*
         * The block handed over is always to_output frames. Only the number of
         * frames taken from the ring to build it varies - see iptv_drift.h.
         */
        static int64_t drift_seen_sent = -1;
        size_t consumed = (size_t)real_to_send;

        /*
         * A flush leaves a fractional phase pointing into samples that no
         * longer exist, and a learned correction that belonged to the channel
         * being left. audio_sent_frames going backwards is the same signal the
         * drift report uses, and it costs no new state elsewhere.
         */
        if (core.audio_sent_frames < drift_seen_sent)
            iptv_drift_flush_ring();
        drift_seen_sent = core.audio_sent_frames;

        if (real_to_send > 0) {
            int32_t drift_ratio = iptv_drift_next_ratio(
                (int64_t)frames_avail * 1000 / AUDIO_TARGET_RATE,
                (int64_t)sync_target_frames * 1000 / AUDIO_TARGET_RATE);

            /* Not enough to interpolate through is the same starvation the
             * block above handles; fall back to the identity for this one
             * block rather than reading past the write position. */
            if (frames_avail < iptv_drift_frames_needed((size_t)real_to_send,
                                                        drift_ratio))
                drift_ratio = IPTV_DRIFT_ONE;

            consumed = iptv_drift_read(core.audio_ring, AUDIO_BUFFER_SIZE, r,
                                       out_buffer, (size_t)real_to_send,
                                       drift_ratio);

            if (core.audio_fade_in_frames > 0) {
                const int total_fade_frames = (AUDIO_TARGET_RATE * 10) / 1000;
                int fade_now = core.audio_fade_in_frames;
                if (fade_now > real_to_send)
                    fade_now = (int)real_to_send;
                int already_faded = total_fade_frames - core.audio_fade_in_frames;
                for (int i = 0; i < fade_now; i++) {
                    int gain = already_faded + i + 1;
                    out_buffer[i * 2] =
                        (int16_t)(((int32_t)out_buffer[i * 2] * gain) /
                                  total_fade_frames);
                    out_buffer[i * 2 + 1] =
                        (int16_t)(((int32_t)out_buffer[i * 2 + 1] * gain) /
                                  total_fade_frames);
                }
                core.audio_fade_in_frames -= fade_now;
            }

            core.audio_last_left = out_buffer[(real_to_send - 1) * 2];
            core.audio_last_right = out_buffer[(real_to_send - 1) * 2 + 1];
            core.audio_read_pos = (r + consumed * 2) % AUDIO_BUFFER_SIZE;
            /*
             * Counted in frames consumed from the ring, not frames handed over.
             * audio_sent_frames is compared against libVLC's PTS, which advances
             * with the stream; counting output frames instead would drift apart
             * from it at exactly the rate being corrected here and would trip
             * the one second "timeline jump" recalibration every few minutes.
             */
            core.audio_sent_frames += (int64_t)consumed;
        } else {
            memset(out_buffer, 0, to_output * 2 * sizeof(int16_t));
        }

        pthread_mutex_unlock(&core.mutex);
        emit_audio(out_buffer, to_output);
    } else if (audio_batch_cb) {
        /*
         * Not playing: connecting, buffering, paused or stopped. The frontend
         * still expects a complete block every frame - starving it is what
         * turns a slow channel into a buzzing sound in RetroArch.
         */
        memset(silence_buffer, 0, samples_per_frame * 2 * sizeof(int16_t));
        emit_audio(silence_buffer, samples_per_frame);
    }

    submit_current_video_frame();
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info) {
    double fps = (core.video_fps > 0) ? core.video_fps : 60.0;

    info->geometry.base_width   = output_width;
    info->geometry.base_height  = output_height;
    info->geometry.max_width    = output_width;
    info->geometry.max_height   = output_height;
    info->geometry.aspect_ratio = 16.0f / 9.0f;
    info->timing.fps            = fps;
    info->timing.sample_rate    = AUDIO_TARGET_RATE;

    iptv_log("[VLChannel] Reporting AV info: %ux%u, %.3f fps\n",
            info->geometry.base_width, info->geometry.base_height, fps);
}

RETRO_API void retro_get_system_info(struct retro_system_info *i) {
    static char library_name[]    = "VLChannel";
    static char library_version[] = "1.2";
    /* Both extensions hold the same thing: a channel list, or an HLS manifest
     * that the reader detects and opens as a single stream. */
    static char valid_extensions[] = "m3u|m3u8";

    i->library_name     = library_name;
    i->library_version  = library_version;
    i->valid_extensions = valid_extensions;
    i->need_fullpath    = true;
    i->block_extract    = false;
}

RETRO_API void retro_unload_game(void) {
    if (core.mp)
        libvlc_media_player_stop(core.mp);
    iptv_playlist_free(&playlist);
    playlist_path[0] = '\0';
    current_channel = -1;
    pending_channel = -1;
    pending_reload = false;
    set_no_signal(false);
    reset_output_picture();
    iptv_zap_reset();     /* nothing should survive into the next content */
    iptv_epg_free();
    iptv_drift_reset();   /* including the cadence learned for this frontend */
}

RETRO_API void retro_deinit(void) {
    iptv_log("[VLChannel] retro_deinit()\n");

    /* Before libVLC goes: the resolver owns a thread, and a thread still
     * running when the library it will report into is gone is the shape of
     * crash that only happens on someone else's machine. */
    iptv_ytdlp_shutdown();
    iptv_streamlink_shutdown();
    resolving_channel = -1;
    resolving_with = RESOLVER_NONE;

    if (frontend_pause_thread_started) {
        pthread_mutex_lock(&frontend_pause_mutex);
        frontend_pause_stop = true;
        pthread_mutex_unlock(&frontend_pause_mutex);
        pthread_join(frontend_pause_thread, NULL);
        frontend_pause_thread_started = false;
    }

    pthread_mutex_lock(&frontend_pause_mutex);
    frontend_pause_player = NULL;
    pthread_mutex_unlock(&frontend_pause_mutex);

    if (core.mp) {
        libvlc_media_player_stop(core.mp);
        libvlc_media_player_release(core.mp);
        core.mp = NULL;
    }
    if (core.libvlc)
        libvlc_release(core.libvlc);
    core.libvlc = NULL;
    vlchannel_unload_libvlc();

    vlc_video_shutdown();
    iptv_playlist_free(&playlist);
    playlist_path[0] = '\0';

    free(osd_clean);
    free(osd_composed);
    osd_clean = NULL;
    osd_composed = NULL;
    reset_output_picture();

    pthread_mutex_destroy(&core.mutex);
    output_resolution_initialised = false;
    output_resolution_reload_notice_shown = false;
    iptv_log_close();
}

/* Required stubs */
RETRO_API unsigned retro_api_version(void) { return RETRO_API_VERSION; }
RETRO_API unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
RETRO_API void retro_reset(void) { request_reload("core reset"); }
RETRO_API size_t retro_serialize_size(void) { return 0; }
RETRO_API bool retro_serialize(void *data, size_t size) { (void)data; (void)size; return false; }
RETRO_API bool retro_unserialize(const void *data, size_t size) { (void)data; (void)size; return false; }
RETRO_API void retro_cheat_reset(void) {}
RETRO_API void retro_cheat_set(unsigned index, bool enabled, const char *code) { (void)index; (void)enabled; (void)code; }
RETRO_API bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num_info) { (void)type; (void)info; (void)num_info; return false; }
RETRO_API void *retro_get_memory_data(unsigned id) { (void)id; return NULL; }
RETRO_API size_t retro_get_memory_size(unsigned id) { (void)id; return 0; }
RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device) { (void)port; (void)device; }
