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
 * It matters because the two cases have no format in common. A video is asked
 * for as a progressive file; a live broadcast has no progressive file at all,
 * only HLS renditions, so the ordinary selector matches nothing and yt-dlp
 * answers with a refusal instead of a URL.
 */
void iptv_ytdlp_begin(const char *youtube_url, bool live);

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

/*
 * The format asked of yt-dlp, exposed so a test can assert on it rather than on
 * a copy of it.
 *
 *   1. format 18, YouTube's broadly compatible H.264/AAC progressive file;
 *   2. otherwise the best MP4 H.264/AAC progressive file up to 480p.
 *
 * This deliberately matches the selector proven by another old-PC player.
 * A DASH video+audio pair can reach 1080p, but current authenticated YouTube
 * URLs for those tracks answer libVLC's direct HTTP request with 403 even when
 * yt-dlp resolved them successfully. A progressive file is the proxy-like
 * path: one ordinary URL that libVLC can open. Format 18 is normally 360p; the
 * core's fixed 1920x1080 canvas scales that picture but does not turn the source
 * into 1080p.
 */
#define IPTV_YT_FORMAT \
    "18/best[height<=480][ext=mp4][vcodec^=avc1][acodec^=mp4a]"

/*
 * The same request, for a broadcast that is happening now.
 *
 * A live YouTube stream is not published as a file. There is no format 18, so
 * IPTV_YT_FORMAT's first and preferred alternative can never match; what exists
 * is a set of HLS renditions, and the entry falls through to that selector's
 * second alternative - which has no fallback of its own. On a well behaved
 * broadcast that is fine and the two selectors agree. On the two that are not,
 * it resolves to nothing at all:
 *
 *   - a channel broadcasting only in 1080p, where the 480p ceiling excludes
 *     every rendition on offer;
 *   - a broadcast published as VP9 and Opus, which the avc1/mp4a constraint
 *     excludes for the same reason.
 *
 * Both end with yt-dlp refusing rather than answering, and a refusal reaches the
 * viewer as a channel that will not open for no stated reason. Hence a selector
 * of its own, which gives up the codec constraint and then the height, in that
 * order: preference first, and something playing rather than nothing.
 *
 * What comes back is one m3u8 address with both tracks already in it, so there
 * is no audio slave to pass to libVLC, and libVLC opens it exactly the way it
 * opens every other IPTV channel in this core.
 *
 * The 480p preference matches the video selector's on purpose. This core exists
 * to run on old machines and inside EmuVR, and a live broadcast is the one thing
 * that cannot be re-buffered from the start when the decoder falls behind.
 */
#define IPTV_YT_LIVE_FORMAT \
    "best[height<=480][vcodec^=avc1][acodec^=mp4a]/best[height<=480]/best"

#endif
