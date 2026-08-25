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

/*
 * What kind of address an entry holds, and therefore who can open it.
 *
 * The distinction that matters most here is the first one. Nearly every line in
 * a channel list is an address libVLC opens by itself, and those must never be
 * handed to an external resolver: a list of two hundred IPTV channels would
 * become two hundred processes, and streamlink asked about a plain .m3u8 would
 * answer about it - slowly, and with a different URL than the one the
 * broadcaster published.
 *
 * So DIRECT is the default and the other three are only reached deliberately:
 * by recognising a YouTube address, by a host in a short table the core carries
 * (see iptv_platform_host), or by an #EXTSTREAMLINK line the list author wrote.
 * Nothing is inferred from a URL merely looking unusual.
 */
typedef enum {
    /* libVLC opens it. Every ordinary channel, file and manifest. */
    IPTV_SOURCE_DIRECT = 0,

    /* A YouTube address with a video id in it. yt-dlp first, then streamlink,
     * then the proxy, which is the only one of the three that needs the id. */
    IPTV_SOURCE_YOUTUBE,

    /* A YouTube channel's live page: youtube.com/@Name/live. Same order, but
     * what is asked for is a broadcast rather than a file. */
    IPTV_SOURCE_YOUTUBE_LIVE,

    /*
     * Twitch, Kick, or anything an #EXTSTREAMLINK line marked. streamlink
     * first, because it is the specialist for these; yt-dlp second, because it
     * knows some of them too. No proxy: riitube serves YouTube video ids and
     * there is no id here to serve.
     *
     * The only one of the three that is not is_video, and that is the point of
     * it as much as the resolver order is. These are channels: one that is off
     * the air shows the no-signal screen and waits for the reconnect button,
     * the same as an IPTV channel whose server went away. The tag is therefore
     * two decisions in one line, and it is allowed to capture a YouTube address
     * as well - somebody who writes it there is asking for exactly that.
     */
    IPTV_SOURCE_PLATFORM,

    /*
     * A file on this machine. libVLC opens it directly - no resolver, no proxy,
     * nothing to ask anyone - so it shares that with DIRECT and nothing else.
     *
     * It is separate from DIRECT because a file has two properties a channel
     * does not. It ends, which makes it a video and puts it in the video guide
     * with the rest of them. And it can be navigated: an audio track, a
     * subtitle track and a position are all things that exist for a file and
     * are meaningless for a live broadcast, so the four transport buttons are
     * live only here.
     *
     * What it deliberately does NOT change is the audio alignment. A local file
     * was on the live-TV reserve before it was a video and it stays there; see
     * uses_video_clock() in the core. The two alignments in this core were each
     * arrived at by measurement, and the way to keep them working is to add
     * nothing to them.
     */
    IPTV_SOURCE_LOCAL
} iptv_source;

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
     *
     * A platform entry - Twitch, Kick, #EXTSTREAMLINK - is deliberately NOT
     * one of these, even though it too is opened by a resolver. See
     * IPTV_SOURCE_PLATFORM: what arrives there is a channel, and a channel that
     * goes off the air is a fault to report rather than a programme that
     * finished.
     */
    bool is_video;

    /*
     * Where this address has to be opened from.
     *
     * Recorded separately from is_video, which accompanies every value but
     * DIRECT, because the two facts are read by different code for different
     * reasons. is_video says what happens when playback stops; this says who is
     * asked to turn the address into something libVLC can open, and in what
     * order. See iptv_source.
     */
    iptv_source source;

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
 *
 * YTDLP recognises one form the other two cannot: a channel's live page, which
 * carries no video id. See iptv_youtube_live_channel below.
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

/*
 * True when the address is a YouTube channel's live page - the /@Name/live form
 * and its /c/, /channel/ and /user/ equivalents - rather than a link to a video.
 *
 * `out` receives the channel label out of the address, which is the only
 * readable thing in it, and is used as the display name when the list carries no
 * #EXTINF for the entry.
 *
 * Acted on only in IPTV_YOUTUBE_YTDLP mode: there is no video id in this form,
 * so the proxy has nothing it could be given, and rewriting it anyway would turn
 * a list that plays into a list of entries that fail at open time.
 *
 * Exposed for the same reason iptv_youtube_id is: the set of shapes accepted,
 * beside the set deliberately refused, is the feature.
 */
bool iptv_youtube_live_channel(const char *url, char *out, size_t out_size);

/*
 * True when the address is on a streaming platform the core knows by name.
 *
 * A short table and not a clever rule, because the question being answered is
 * "is this one of the sites people actually put in these lists", and a list of
 * them is the honest answer to that. It starts at twitch.tv and kick.com. Any
 * subdomain of either counts, which is how streamlink's own matchers read them.
 *
 * It is deliberately not the set of sites streamlink supports - that is over a
 * hundred plugins and grows with every release, and the core cannot know it
 * without asking streamlink, once per entry, at the cost of a process. For
 * everything outside the table the list author says so with #EXTSTREAMLINK,
 * which is one line and never wrong.
 *
 * An address that ends in a media extension is refused whatever its host: a
 * .m3u8 is a thing libVLC opens, and no amount of the right domain in front of
 * it makes that untrue.
 */
bool iptv_platform_host(const char *url);

/*
 * True when the address names a file on this machine rather than something on
 * a network: an absolute path, a file:// URL, or a relative entry the reader
 * has already resolved against the playlist's own directory.
 *
 * A playlist is not one of these. A local .m3u or .m3u8 is a list or an HLS
 * manifest, and both are read as such elsewhere; calling either a video would
 * give it a seek bar over something that has no single position.
 *
 * Everything else local is taken as a file, without an extension table. A
 * table would be a list of the formats libVLC happened to support on the day it
 * was written, and the failure it produces - a .m2ts treated as a live channel,
 * showing no signal at the end and refusing to seek - is far worse than the one
 * it prevents.
 */
bool iptv_local_file(const char *url);

/*
 * Marks the next entry as one a resolver has to open - the escape hatch for the
 * hundred-odd sites streamlink supports that the table above does not name.
 *
 * A comment line, so every other player ignores it:
 *
 *   #EXTSTREAMLINK
 *   https://dlive.tv/SomeChannel
 *
 * It applies to the next URL line only, like #EXTINF, and it carries two
 * decisions rather than one: which programs are asked, and what happens when
 * the stream stops. A tagged entry is a live channel, so it shows the
 * no-signal screen instead of advancing the list.
 *
 * It wins over the YouTube forms, which is the only way to say that about a
 * YouTube address. The cost is the proxy, which a tagged entry no longer falls
 * back to - nothing on a /@Name/live page, which never had a video id to give
 * the proxy anyway, and a real loss on a watch?v= address.
 */
#define IPTV_STREAMLINK_TAG "#EXTSTREAMLINK"

void iptv_playlist_free(iptv_playlist *playlist);

#endif
