#ifndef VLC_CORE_H
#define VLC_CORE_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <vlc/vlc.h>
#include "libretro.h"

/*
 * Maximum picture the core will hand the frontend. RetroArch allocates its
 * texture from this, so it is a ceiling, not a target: 1080p covers every
 * channel seen so far, and 4K streams would need this raised together with the
 * memory that implies.
 */
#define MAX_W 1920
#define MAX_H 1080

/* Path buffer sizes live in vlc_dynamic.h, which the loader also includes. */
#include "vlc_dynamic.h"
#define AUDIO_BUFFER_SIZE (4 * 1024 * 1024)
#define AUDIO_TARGET_RATE 48000

typedef struct {
    libvlc_instance_t *libvlc;
    libvlc_media_player_t *mp;

    /*
     * Frame storage lives in vlc_video.c (triple buffered). Only the
     * negotiated geometry is mirrored here, guarded by core.mutex.
     */
    unsigned video_width;
    unsigned video_height;
    unsigned video_pitch;

    int16_t audio_ring[AUDIO_BUFFER_SIZE];
    size_t audio_read_pos;
    size_t audio_write_pos;

    int64_t audio_sent_frames;
    int64_t sync_offset;
    bool sync_offset_initialized;

    double video_fps;
    int64_t last_audio_pts;
    unsigned last_audio_count;

    bool is_playing;
    bool paused;                      // user pause state

    /*
     * True from the moment a channel is opened until libVLC has negotiated a
     * picture format and produced a frame. On a network stream this window is
     * long and variable - DNS, TCP, TLS, manifest, first segment - so the core
     * keeps output silent and holds the previous geometry until it closes.
     */
    bool transitioning;
    uint32_t transition_timeout_frames;
    pthread_mutex_t mutex;
    double sample_accum_frac;          // fractional accumulator for audio output
    bool pending_start;                 // delay initial playback until first retro_run
    int audio_mute_frames;              // silence after a channel change
    bool audio_prebuffering;             // absorb irregular frontend frame pacing
    int audio_fade_in_frames;            // smooth transition from silence to PCM
    int16_t audio_last_left;              // used for a smooth emergency fade-out
    int16_t audio_last_right;
    int audio_desync_ms;
    unsigned max_width;
    unsigned max_height;
} vlc_core_ctx;

extern vlc_core_ctx core;

void vlc_video_setup_callbacks(libvlc_media_player_t *mp);
void vlc_video_set_output_size(unsigned width, unsigned height);

/*
 * Returns the newest complete frame produced by libVLC. The pointer stays
 * valid until the next call. Never call this while holding core.mutex.
 */
bool vlc_video_acquire_frame(
    const uint32_t **frame,
    unsigned *width,
    unsigned *height,
    unsigned *pitch
);
bool vlc_video_has_frame(void);
uint64_t vlc_video_render_count(void);
void vlc_video_reset(void);
void vlc_video_shutdown(void);

void vlc_audio_setup_callbacks(libvlc_media_player_t *mp);
void vlc_audio_flush(void);

#endif
