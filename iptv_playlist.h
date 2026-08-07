#ifndef IPTV_PLAYLIST_H
#define IPTV_PLAYLIST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Sizes are fixed where a ceiling is a real property of the field and not just
 * a number someone had to pick. A channel list is read once, kept for the whole
 * session and indexed by a single integer, so a flat array is worth more than
 * the bytes a string pool would save - for names, categories and identifiers,
 * which are written by people and are short because people write them.
 *
 * The URL is not one of those. See the note above `url` in iptv_channel: it is
 * written by a machine, it can carry a signed token longer than everything else
 * in the entry put together, and every fixed size chosen for it so far has been
 * too small. It is a pointer.
 *
 * An entry now costs about 1.2 KB, so a 20000 channel list costs around 24 MB
 * plus the text of its URLs, and a warning is logged well before that.
 */
#define IPTV_NAME_MAX      96

/*
 * The group used to be 48, and that was a display bug hiding in the parser.
 *
 * A category of 62 characters came back cut at 47 on screen, and the scrolling
 * label was blamed first - it was walking dutifully to the end of a string that
 * had already been truncated on the way in. No amount of scrolling shows text
 * that was thrown away at parse time.
 *
 * 160 matches the option strings, and costs 112 bytes per channel against an
 * entry that is already 2656 - four per cent, on a field that decides whether
 * the viewer can read the category at all. The overlay is also meant to carry
 * other text later, programme information among it, so the ceiling had to stop
 * being the constraint.
 */
#define IPTV_GROUP_MAX     160
/*
 * tvg-id, the key that links a channel to its programme listing. Read but not
 * used by the core itself: it is matched against the .epg file that
 * ferramentas/prepara-epg.py writes beside the playlist.
 */
#define IPTV_TVG_ID_MAX    64

/*
 * The URL is the one field that is not fixed, and it is the field that proved
 * a fixed size cannot be chosen at all.
 *
 * It used to be 1536. A PlutoTV list addresses every channel through an ad
 * stitcher, and those URLs carry a signed token: 3254 characters, of which the
 * last 1700 are a JWT. All 172 channels were cut in the middle of the
 * signature, and the server answered 401 - not "list out of date", not "stream
 * ended", an authentication failure produced entirely inside this parser. The
 * log even said the truncation out loud, 172 times, and it still looked like a
 * dead list because a cut URL is a syntactically perfect URL.
 *
 * Raising the number would only move the wall. The shape is wrong: the median
 * URL in an ordinary list is about eighty characters, so a field wide enough
 * for the worst case wastes ninety-eight per cent of itself on every other
 * entry, and a 20000 channel list would spend 160 MB to hold maybe 2 MB of
 * text. The pointer costs eight bytes and has no ceiling.
 *
 * It is never NULL. On allocation failure it points at a static empty string,
 * which libVLC refuses with a message the viewer can act on, rather than
 * crashing six call sites that have never had to check.
 */
#define IPTV_MAX_OPTIONS   6
#define IPTV_OPTION_MAX    160
#define IPTV_MAX_CHANNELS  50000

typedef struct {
    char name[IPTV_NAME_MAX];
    char group[IPTV_GROUP_MAX];
    char tvg_id[IPTV_TVG_ID_MAX];
    /* The station/playlist name from tvg-name. Kept separately because in an
     * ordered EPG the display name after the comma is the current item title. */
    char tvg_name[IPTV_NAME_MAX];
    /* Zero-based position among entries with this same tvg-id. Format 3 EPG
     * files describe ordered playlists with this position instead of a clock. */
    size_t epg_index;
    const char *url;                    /* owned by the playlist, never NULL */
    int  number;                                    /* tvg-chno, 0 if absent */
    char options[IPTV_MAX_OPTIONS][IPTV_OPTION_MAX];/* from #EXTVLCOPT lines */
    int  option_count;

    /*
     * Audio delay for this channel, in milliseconds, read from
     * "#EXTVLCOPT:audio-desync=" when the list carries one.
     *
     * The core used to let the controller tune this and wrote the result back
     * into the list. That whole mechanism existed to work around an alignment
     * that landed somewhere new on every channel open; once the audio reserve
     * was made to match the caching window the alignment became deterministic
     * and the per channel corrections turned out to be measurements of the
     * old race, not properties of the streams.
     *
     * What survives is reading the line, because it is standard M3U and costs
     * nothing: a channel whose broadcaster really does send audio offset can be
     * corrected by hand, and VLC desktop honours the same line.
     */
    int  audio_delay_ms;
    bool audio_delay_set;

    /*
     * This entry is a video of finite length, not a live channel.
     *
     * Set only for entries the reader rewrote from a YouTube address. It exists
     * because "the stream ended" means two opposite things: on a live channel it
     * is a fault worth a message and a button press, and on a video it is
     * Tuesday. Without the distinction the core would either treat the end of
     * every video as an error, or treat every dead channel as a finished
     * programme and walk quietly down the whole list.
     *
     * Deliberately not inferred at playback time from "the stream had a
     * duration": a broken live channel can report one too, and the moment that
     * happens the guess becomes a list that scrolls past by itself.
     */
    bool is_video;

} iptv_channel;

typedef struct {
    iptv_channel *channels;
    size_t count;
    size_t capacity;

    /*
     * True when the loaded file was not a channel list but a real HLS manifest
     * (it carries #EXT-X- tags). The playlist then holds a single entry
     * pointing at the file itself, which libVLC opens directly.
     */
    bool single_stream;

    /*
     * True when every entry is a video - in practice, a list of nothing but
     * YouTube addresses. The on-screen guide then calls itself a video guide
     * and counts videos, because calling a list of clips "12 channels" is the
     * kind of small wrongness that makes a program feel like it was built for
     * something else.
     *
     * All, and not most, and not any. A list with one television channel in it
     * still has a channel in it, and the word on the screen should be true of
     * everything the screen is listing. "Any" would have renamed a two hundred
     * channel guide over a single clip; "most" would have made the title depend
     * on a ratio nobody can see while reading the list.
     *
     * Only the wording changes. The guide, the schedule and every button behave
     * exactly as they do for channels - a video list is not a different program,
     * it is the same program being accurate about what it is holding.
     */
    bool all_videos;
} iptv_playlist;

/*
 * Reads a channel list. Returns false only when nothing usable was found; the
 * reason is always logged. On success the caller owns the playlist and must
 * release it with iptv_playlist_free().
 */
bool iptv_playlist_load(const char *path, iptv_playlist *out);

/*
 * Where a YouTube address is sent instead.
 *
 * libVLC cannot open a YouTube page - the core has no YouTube extractor and is
 * not going to grow one - so an entry pointing at one is rewritten to a proxy
 * that serves the video directly. The base is settable rather than compiled in
 * because the proxy is somebody else's service: the day it moves or dies, every
 * YouTube entry in every list dies with it, and that should cost a setting and
 * not a rebuild.
 *
 * Pass NULL or an empty string to turn the rewriting off, which leaves the URL
 * exactly as the list wrote it.
 */
#define IPTV_YOUTUBE_PROXY_DEFAULT "http://riitube.rc24.xyz/video/wii/?q="

void iptv_playlist_set_youtube_proxy(const char *base);

/*
 * How a YouTube entry is turned into something libVLC can open.
 *
 * PROXY rewrites the address as the list is read and is done with it. YTDLP
 * leaves the YouTube address in place and resolves it when the entry is opened,
 * because asking yt-dlp about two hundred entries at load time would take
 * minutes - so in that mode the reader's job is only to recognise the entry and
 * mark it, not to change it.
 *
 * The entry is marked `is_video` either way. What it is does not depend on how
 * its address will be fetched.
 */
typedef enum {
    IPTV_YOUTUBE_OFF = 0,   /* leave the address exactly as written */
    IPTV_YOUTUBE_PROXY,     /* rewrite to the riitube base, at load time */
    IPTV_YOUTUBE_YTDLP      /* keep the address, resolve when opened */
} iptv_youtube_mode;

void iptv_playlist_set_youtube_mode(iptv_youtube_mode mode);

/*
 * The eleven character video id inside a YouTube address, or false.
 *
 * Exposed for the tests, which is the only reason it is not static: the forms
 * this has to recognise are the whole of the feature, and a table of them next
 * to a table of what must *not* be recognised is worth more than the function
 * being private.
 */
bool iptv_youtube_id(const char *url, char *out, size_t out_size);

void iptv_playlist_free(iptv_playlist *playlist);

#endif
