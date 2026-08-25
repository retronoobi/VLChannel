#ifndef IPTV_STREAMLINK_H
#define IPTV_STREAMLINK_H

#include <stdbool.h>

/*
 * The second resolver: streamlink.
 *
 * yt-dlp knows about video sites. streamlink knows about live broadcasts, and
 * it knows about a lot of them - a hundred-odd plugins, Twitch and Kick among
 * them, sites that publish nothing yt-dlp recognises. The two overlap on
 * YouTube and disagree often enough on a bad day that having both is worth more
 * than having either twice.
 *
 * The order is set by the core, not here, and it is not the same order for
 * every entry: a YouTube address goes to yt-dlp first, and a platform address
 * goes to streamlink first, because each tool is the specialist for one of
 * them. This module only answers when asked.
 *
 * It is deliberately the same shape as iptv_ytdlp: set a directory, ask, poll,
 * read one URL. Two resolvers that behave differently would be two things to
 * remember while reading the core; the process work they share lives in
 * iptv_child, and what is left in each is only the part that is about that
 * particular program.
 *
 * One real difference: streamlink answers with a single manifest address that
 * already carries both tracks, so there is no separate audio stream to hand
 * libVLC. That is why there is no audio_url() here.
 */

typedef enum {
    IPTV_SL_IDLE = 0,   /* nothing asked for */
    IPTV_SL_WORKING,    /* the thread is running streamlink */
    IPTV_SL_DONE,       /* url() is ready */
    IPTV_SL_FAILED      /* no exe, no answer, or a timeout */
} iptv_sl_state;

/*
 * Where streamlink lives, given the directory the VLC runtime is in.
 *
 * The portable Windows build is a directory and not a loose executable: it
 * carries its own Python and its own ffmpeg, so it is unpacked whole and the
 * program sits inside it.
 *
 *   system\vlchannel\streamlink\bin\streamlink.exe
 *
 * The argument is `system\vlchannel` itself - the same string yt-dlp is given -
 * so that the core has one idea of where its tools live and this module knows
 * the rest. streamlinkw.exe, beside it, is the windowless variant; it is not
 * used, because the console window is already suppressed by the way the process
 * is created and streamlinkw detaches the stdout this needs to read.
 */
void iptv_streamlink_set_directory(const char *vlchannel_directory);

/* True when the executable is where it should be. Checked once per directory
 * change, not per channel. */
bool iptv_streamlink_available(void);

/*
 * Starts resolving, returning immediately. A resolution already in flight is
 * abandoned, for the same reason it is in iptv_ytdlp: the viewer has moved on,
 * and an answer about the previous channel would open the wrong thing.
 */
void iptv_streamlink_begin(const char *url);

iptv_sl_state iptv_streamlink_poll(void);

/* Valid only while poll() reports DONE. One address, both tracks. */
const char *iptv_streamlink_url(void);

/* Abandons any resolution in flight. Safe to call at any time. */
void iptv_streamlink_cancel(void);

/* Waits for the thread and releases everything. For retro_deinit. */
void iptv_streamlink_shutdown(void);

/*
 * What is asked for, exposed so a test can assert on it rather than on a copy.
 *
 * A comma separated list of fallbacks, tried left to right. The ceiling matches
 * the yt-dlp selectors': this core runs on old machines and inside EmuVR, and a
 * live broadcast cannot be re-buffered from the start when the decoder falls
 * behind. `best` closes the list so that a channel offering nothing under 720p
 * still plays rather than failing over a preference.
 *
 * Twitch in particular offers 1080p60 as `best`, which is exactly the stream
 * this ceiling exists to avoid choosing by accident.
 */
#define IPTV_SL_STREAMS "480p,720p,best"

#endif
