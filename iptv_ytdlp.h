#ifndef IPTV_YTDLP_H
#define IPTV_YTDLP_H

#include <stdbool.h>

/*
 * Turning a YouTube address into something libVLC can open, by asking yt-dlp.
 *
 * The riitube proxy is the default and stays the default, because it costs one
 * string substitution and no processes. But a proxy is somebody else's server,
 * and this one has already been down: the whole feature stops working and there
 * is nothing on this side to fix. yt-dlp is the answer that does not depend on
 * anyone staying online, at the price of a program on disk and a few seconds per
 * video.
 *
 * Three things about this module are not style choices:
 *
 * 1. It resolves on a thread and is polled, never waited on. A resolution takes
 *    one to five seconds. Blocking retro_run for that long stops the frontend,
 *    and under EmuVR that is a headset that stops drawing.
 *
 * 2. On Windows it uses CreateProcess and never popen(). The comment in the
 *    original VLC libretro core - Krisreto's, which this follows - records why:
 *    popen() on MinGW calls AllocConsole, Windows serialises console creation
 *    per process, and the render loop freezes even though the call was made on a
 *    background thread. A bug that only appears on the target platform, found by
 *    someone else, and free to avoid.
 *
 * 3. The resolved address is not the video's address. It is a signed, expiring,
 *    IP-bound URL that is valid for hours at most. It is never written to the
 *    playlist and never shown in the guide - what the viewer sees stays the
 *    YouTube address they wrote.
 */

typedef enum {
    IPTV_YT_IDLE = 0,   /* nothing asked for */
    IPTV_YT_WORKING,    /* the thread is running yt-dlp */
    IPTV_YT_DONE,       /* url() and audio_url() are ready */
    IPTV_YT_FAILED      /* no exe, no answer, or a timeout */
} iptv_yt_state;

/*
 * Where yt-dlp.exe lives: system\vlchannel, beside the libVLC runtime.
 * An optional Netscape-format cookies.txt in the same directory is used only
 * for a second attempt after the ordinary unauthenticated request fails.
 *
 * One directory and not the PATH, for the same reason the core refuses a libVLC
 * resolved from anywhere else. A tool found on the PATH is a tool whose version
 * depends on the machine, and "works here, not there" is the hardest kind of
 * report to act on. The log says where it looked.
 */
void iptv_ytdlp_set_directory(const char *directory);

/* True when the executable is where it should be. Cheap; checked once per
 * directory change, not per video. */
bool iptv_ytdlp_available(void);

/*
 * Starts resolving, returning immediately. A resolution already in flight is
 * abandoned - its thread finishes and its result is discarded, because the
 * viewer has moved on and an answer about the previous video is worse than no
 * answer at all.
 *
 * `live` selects which format is asked for, and it is a parameter rather than
 * something guessed from the address because the caller already knows: the
 * playlist reader decided it when the entry was read, and a second guess in a
 * second place is a second thing that can disagree.
 *
 * height is a preferred resolution (0 means best available). Select the best
 * stream up to it, or the smallest available if all streams exceed it.
 */
void iptv_ytdlp_begin(const char *youtube_url, bool live, unsigned height);

iptv_yt_state iptv_ytdlp_poll(void);

/*
 * Valid only while poll() reports DONE.
 *
 * `audio_url` is empty for a progressive stream, which is one file with both
 * tracks in it. When it is not empty, yt-dlp returned a DASH pair and the caller
 * must hand the audio to libVLC as `:input-slave=` - the price of asking for a
 * height YouTube no longer publishes as a single file.
 */
const char *iptv_ytdlp_url(void);
const char *iptv_ytdlp_audio_url(void);

/* Abandons any resolution in flight. Safe to call at any time. */
void iptv_ytdlp_cancel(void);

/* Waits for the thread and releases everything. For retro_deinit. */
void iptv_ytdlp_shutdown(void);

/* Video may include audio already, or require a separate audio URL. Live
 * prefers a combined stream, with a separate-track fallback when necessary.
 * Resolution and codec preferences are supplied through --format-sort. */
#define IPTV_YT_FORMAT "bestvideo*+bestaudio/best"
#define IPTV_YT_LIVE_FORMAT "best/bestvideo*+bestaudio"

#endif