/*
 * M3U channel list reader
 * =======================
 *
 * IPTV lists in the wild are not a format so much as a habit. This reader
 * accepts what they actually contain:
 *
 *   #EXTM3U                                  optional, frequently missing
 *   #EXTINF:-1 tvg-chno="12" group-title="TV Aberta", SBT
 *   #EXTVLCOPT:http-user-agent=Mozilla/5.0   optional, per channel
 *   https://example.com/sbt/playlist.m3u8
 *
 * and also a bare list of URLs with no metadata at all.
 *
 * Two details are worth stating because getting them wrong is silent:
 *
 * - the display name starts after the first comma that is NOT inside quotes.
 *   Splitting on the last comma breaks group-title="Filmes, Series", and
 *   splitting on the first comma of the raw line breaks the same case the
 *   other way round;
 *
 * - a file with .m3u8 on the end may be an actual HLS manifest rather than a
 *   channel list. Feeding a manifest through this parser would produce one
 *   bogus "channel" per segment, so the presence of any #EXT-X- tag switches
 *   the whole file to a single entry that libVLC opens directly.
 */

#include "iptv_playlist.h"

#include <stdio.h>
#include "iptv_log.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* A 32 MB list is already around 100000 channels; beyond that something is
 * wrong with the file rather than with the limit. */
#define IPTV_MAX_FILE_BYTES (32u * 1024u * 1024u)

static void copy_bounded(char *dst, size_t size, const char *src) {
    if (!size)
        return;
    size_t length = strlen(src);
    if (length >= size)
        length = size - 1;
    memcpy(dst, src, length);
    dst[length] = '\0';
}

static void trim_in_place(char *text) {
    char *start = text;
    while (*start && isspace((unsigned char)*start))
        start++;

    size_t length = strlen(start);
    while (length > 0 && isspace((unsigned char)start[length - 1]))
        length--;

    if (start != text)
        memmove(text, start, length);
    text[length] = '\0';
}

typedef struct {
    iptv_channel *channel;
    size_t original_index;
} epg_order_item;

static int compare_epg_order_item(const void *left, const void *right) {
    const epg_order_item *a = (const epg_order_item *)left;
    const epg_order_item *b = (const epg_order_item *)right;
    int by_id = strcmp(a->channel->tvg_id, b->channel->tvg_id);
    if (by_id != 0)
        return by_id;
    if (a->original_index < b->original_index)
        return -1;
    if (a->original_index > b->original_index)
        return 1;
    return 0;
}

/*
 * Format 3 addresses an item by its position inside one tvg-id, not by its
 * position in the whole M3U. Sort temporary references by id and original
 * order, assign the local position, then discard the references. This stays
 * O(n log n) even for a list whose 50000 entries all share one id.
 */
static void assign_epg_indices(iptv_playlist *list) {
    if (!list || list->count == 0)
        return;

    epg_order_item *items = malloc(list->count * sizeof(*items));
    if (!items) {
        iptv_log("[VLChannel] Programme listing: not enough memory to index "
                 "playlist order; format 3 will use position zero\n");
        return;
    }

    for (size_t i = 0; i < list->count; i++) {
        items[i].channel = &list->channels[i];
        items[i].original_index = i;
    }
    qsort(items, list->count, sizeof(*items), compare_epg_order_item);

    size_t local_index = 0;
    for (size_t i = 0; i < list->count; i++) {
        if (i == 0 || strcmp(items[i - 1].channel->tvg_id,
                             items[i].channel->tvg_id) != 0)
            local_index = 0;
        else
            local_index++;
        items[i].channel->epg_index = local_index;
    }
    free(items);
}

/*
 * First comma outside quotes. Returns NULL when the line carries only
 * attributes, which some generators do.
 */
static const char *unquoted_comma(const char *text) {
    char quote = '\0';
    for (const char *p = text; *p; p++) {
        if (quote) {
            if (*p == quote)
                quote = '\0';
        } else if (*p == '"' || *p == '\'') {
            quote = *p;
        } else if (*p == ',') {
            return p;
        }
    }
    return NULL;
}

/*
 * Reads key="value" (or key=value) from an attribute region. The key must sit
 * on a token boundary so that tvg-id does not answer a request for id.
 */
static void extract_attribute(
    const char *attributes,
    const char *key,
    char *out,
    size_t out_size
) {
    if (out_size)
        out[0] = '\0';

    size_t key_length = strlen(key);
    const char *cursor = attributes;

    while ((cursor = strstr(cursor, key)) != NULL) {
        bool at_boundary =
            (cursor == attributes) ||
            isspace((unsigned char)cursor[-1]) ||
            cursor[-1] == ',';
        const char *value = cursor + key_length;

        if (at_boundary && *value == '=') {
            value++;
            char quote = '\0';
            if (*value == '"' || *value == '\'') {
                quote = *value;
                value++;
            }

            const char *end = quote ? strchr(value, quote) : NULL;
            if (!end)
                end = value + strcspn(value, quote ? "" : " \t");

            size_t length = (size_t)(end - value);
            if (length >= out_size)
                length = out_size ? out_size - 1 : 0;
            if (out_size) {
                memcpy(out, value, length);
                out[length] = '\0';
            }
            return;
        }

        cursor += key_length;
    }
}

static bool url_has_scheme(const char *url) {
    const char *separator = strstr(url, "://");
    if (!separator)
        return false;
    for (const char *p = url; p < separator; p++)
        if (!isalnum((unsigned char)*p) && *p != '+' && *p != '-' && *p != '.')
            return false;
    return separator != url;
}

static bool path_is_absolute(const char *path) {
    if (path[0] == '/' || path[0] == '\\')
        return true;
    return isalpha((unsigned char)path[0]) && path[1] == ':';
}

static void directory_of(const char *path, char *out, size_t out_size) {
    copy_bounded(out, out_size, path);
    char *last_slash = strrchr(out, '/');
    char *last_backslash = strrchr(out, '\\');
    char *cut = last_slash > last_backslash ? last_slash : last_backslash;
    if (cut)
        *cut = '\0';
    else if (out_size)
        out[0] = '\0';
}

/* Fallback display name: the last path segment of the URL, without query
 * string or extension. Better than "channel 37" when the list has no EXTINF. */
static void name_from_url(const char *url, char *out, size_t out_size) {
    const char *start = url;
    for (const char *p = url; *p; p++)
        if (*p == '/' || *p == '\\')
            start = p + 1;

    char buffer[IPTV_NAME_MAX];
    copy_bounded(buffer, sizeof(buffer), start);

    char *query = strpbrk(buffer, "?#");
    if (query)
        *query = '\0';
    char *dot = strrchr(buffer, '.');
    if (dot && dot != buffer)
        *dot = '\0';

    trim_in_place(buffer);
    copy_bounded(out, out_size, buffer);
}

/* ------------------------------------------------------------- YouTube */

static char youtube_proxy[512] = IPTV_YOUTUBE_PROXY_DEFAULT;
static iptv_youtube_mode youtube_mode = IPTV_YOUTUBE_PROXY;

void iptv_playlist_set_youtube_proxy(const char *base) {
    snprintf(youtube_proxy, sizeof(youtube_proxy), "%s", base ? base : "");
    /* Kept so that the older two-state callers - and the tests written against
     * them - still say what they meant: a base is the proxy, none is off. */
    youtube_mode = youtube_proxy[0] ? IPTV_YOUTUBE_PROXY : IPTV_YOUTUBE_OFF;
}

void iptv_playlist_set_youtube_mode(iptv_youtube_mode mode) {
    youtube_mode = mode;
    if (mode == IPTV_YOUTUBE_PROXY && !youtube_proxy[0])
        snprintf(youtube_proxy, sizeof(youtube_proxy), "%s",
                 IPTV_YOUTUBE_PROXY_DEFAULT);
}

/* A YouTube video id: exactly eleven of these, and nothing else. Checking the
 * length as well as the alphabet is what keeps "/watch?v=" followed by a whole
 * query string from being mistaken for an id. */
#define YT_ID_LEN 11

static bool id_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_';
}

static bool take_id(const char *at, char *out, size_t out_size) {
    if (out_size <= YT_ID_LEN)
        return false;

    size_t n = 0;
    while (n < YT_ID_LEN && id_char(at[n]))
        n++;
    if (n != YT_ID_LEN)
        return false;
    /* What follows must end the id: a separator or the end of the string. A
     * twelfth id character means this was never an id. */
    if (at[n] && at[n] != '&' && at[n] != '?' && at[n] != '/' && at[n] != '#')
        return false;

    memcpy(out, at, YT_ID_LEN);
    out[YT_ID_LEN] = '\0';
    return true;
}

/* Case-insensitive prefix match, for the host only. Paths and query keys on
 * YouTube are case sensitive and are compared as they are. */
static const char *skip_prefix_ci(const char *text, const char *prefix) {
    size_t i = 0;
    for (; prefix[i]; i++) {
        char a = text[i], b = prefix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b)
            return NULL;
    }
    return text + i;
}

/*
 * The forms a YouTube link actually arrives in.
 *
 * Written out as a list rather than as one regular expression, because the list
 * is the specification: every entry here is a shape somebody pastes, and the
 * next one that turns up is a line and not a rewrite of an unreadable pattern.
 *
 * `watch?v=` is looked for anywhere in the query rather than only first, since
 * `?list=...&v=ID` and `?v=ID&t=30` are both common and neither is malformed.
 */
bool iptv_youtube_id(const char *url, char *out, size_t out_size) {
    if (!url || !out || out_size <= YT_ID_LEN)
        return false;

    const char *after = skip_prefix_ci(url, "https://");
    if (!after) after = skip_prefix_ci(url, "http://");
    if (!after) after = url;                       /* a bare youtu.be/ID */

    const char *host = after;
    if (skip_prefix_ci(host, "www.")) host += 4;
    else if (skip_prefix_ci(host, "m.")) host += 2;

    const char *path = skip_prefix_ci(host, "youtu.be/");
    if (path)
        return take_id(path, out, out_size);

    path = skip_prefix_ci(host, "youtube.com/");
    if (!path)
        path = skip_prefix_ci(host, "youtube-nocookie.com/");
    if (!path)
        return false;

    static const char *direct[] = { "shorts/", "embed/", "live/", "v/" };
    for (size_t i = 0; i < sizeof(direct) / sizeof(*direct); i++) {
        const char *at = skip_prefix_ci(path, direct[i]);
        if (at)
            return take_id(at, out, out_size);
    }

    if (!skip_prefix_ci(path, "watch"))
        return false;

    /* `v=` as a whole query key, so that `list_v=` and `sv=` do not match. */
    for (const char *p = path; *p; p++) {
        if ((*p == '?' || *p == '&') && p[1] == 'v' && p[2] == '=')
            return take_id(p + 3, out, out_size);
    }
    return false;
}

/*
 * The other shape of a YouTube address: a channel's permanent live page.
 *
 *   https://www.youtube.com/@PicaPau/live
 *   https://www.youtube.com/c/Name/live
 *   https://www.youtube.com/channel/UCxxxxxxxxxxxxxxxxxxxxxx/live
 *   https://www.youtube.com/user/Name/live
 *
 * It is a different kind of link from every form above, and the difference is
 * the whole reason it needs its own function. Those forms name a video; this one
 * names a place where a video will be, and which video that is changes without
 * the address changing. There is no eleven character id in it to find, so
 * iptv_youtube_id() correctly refuses it - and so does the riitube proxy, which
 * is an id-in video-out service and has nothing here to be handed.
 *
 * That leaves yt-dlp, which follows the page and answers with whatever is on air
 * at the moment it is asked. So this form is recognised only in yt-dlp mode; in
 * the other two modes the entry stays exactly as the list wrote it, like any
 * address this reader does not claim to understand.
 *
 * `out` receives the channel's label - "@PicaPau", "Name", the UC id - purely so
 * that a list written without #EXTINF shows something better than "live", which
 * is what the last path segment would otherwise give every single one of them.
 *
 * Deliberately not matched: /@Name/streams and /@Name/videos. Those are listings
 * of many videos, and yt-dlp would answer with the first of them rather than
 * with a live broadcast. A link that plays something is worse than a link that
 * plays nothing, because it looks like it worked.
 */
bool iptv_youtube_live_channel(const char *url, char *out, size_t out_size) {
    if (!url || !out || out_size == 0)
        return false;
    out[0] = '\0';

    const char *after = skip_prefix_ci(url, "https://");
    if (!after) after = skip_prefix_ci(url, "http://");
    if (!after) after = url;

    const char *host = after;
    if (skip_prefix_ci(host, "www.")) host += 4;
    else if (skip_prefix_ci(host, "m.")) host += 2;

    const char *path = skip_prefix_ci(host, "youtube.com/");
    if (!path)
        path = skip_prefix_ci(host, "youtube-nocookie.com/");
    if (!path)
        return false;

    /*
     * The label is either a handle written straight after the slash, or the
     * segment after one of the three older prefixes. Anything else is not a
     * channel address and is left alone.
     */
    const char *label = NULL;
    if (path[0] == '@') {
        label = path;
    } else {
        static const char *prefixes[] = { "channel/", "c/", "user/" };
        for (size_t i = 0; i < sizeof(prefixes) / sizeof(*prefixes); i++) {
            const char *at = skip_prefix_ci(path, prefixes[i]);
            if (at) {
                label = at;
                break;
            }
        }
    }
    if (!label || !label[0] || label[0] == '/')
        return false;

    /*
     * The label runs to the next slash; after it comes "live" and then the end
     * of the address. A trailing slash, a query or a fragment may follow -
     * nothing else, so that /@Name/livestreams is not read as /@Name/live.
     */
    const char *slash = strchr(label, '/');
    if (!slash)
        return false;

    const char *tail = skip_prefix_ci(slash + 1, "live");
    if (!tail)
        return false;
    if (*tail == '/')
        tail++;
    if (*tail && *tail != '?' && *tail != '#')
        return false;

    size_t length = (size_t)(slash - label);
    if (length >= out_size)
        length = out_size - 1;
    memcpy(out, label, length);
    out[length] = '\0';
    return true;
}

/* --------------------------------------------------- Streaming platforms */

/*
 * Where the host starts: after the scheme, and after a www. or m. that carries
 * no meaning. Shared with the two YouTube recognisers above in spirit; kept
 * separate in code because those two also want the path and this one does not.
 */
static const char *host_of(const char *url) {
    const char *after = skip_prefix_ci(url, "https://");
    if (!after) after = skip_prefix_ci(url, "http://");
    if (!after) after = url;

    if (skip_prefix_ci(after, "www.")) return after + 4;
    if (skip_prefix_ci(after, "m.")) return after + 2;
    return after;
}

/*
 * The guard that keeps a channel list out of the resolvers.
 *
 * A path ending in one of these is a thing libVLC opens directly, and that is
 * true no matter what host it is on. It is checked before the table rather than
 * after, so a domain landing in the table later cannot quietly capture the
 * manifests somebody was serving from it.
 */
static bool looks_like_media(const char *url) {
    static const char *const extensions[] = {
        ".m3u8", ".m3u", ".mpd", ".ts", ".mp4", ".mkv", ".flv", ".webm",
        ".avi", ".mov", ".mp3", ".aac", ".ogg"
    };

    /* The path only: a query string can carry anything, including something
     * that looks like a file name and is not one. */
    size_t length = strcspn(url, "?#");
    for (size_t i = 0; i < sizeof(extensions) / sizeof(*extensions); i++) {
        size_t n = strlen(extensions[i]);
        if (length < n)
            continue;
        const char *at = url + length - n;
        size_t j = 0;
        for (; j < n; j++) {
            char a = at[j], b = extensions[i][j];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (a != b)
                break;
        }
        if (j == n)
            return true;
    }
    return false;
}

/* The host is this domain, or a subdomain of it. `player.twitch.tv` counts;
 * `twitch.tv.example.com` does not, which is the whole reason this is not a
 * substring search. */
static bool host_is(const char *host, const char *domain) {
    size_t host_length = strcspn(host, "/:?#");
    size_t domain_length = strlen(domain);
    if (host_length < domain_length)
        return false;

    const char *at = host + host_length - domain_length;
    for (size_t i = 0; i < domain_length; i++) {
        char a = at[i], b = domain[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b)
            return false;
    }
    return host_length == domain_length || at[-1] == '.';
}

/* The path part, lower-cased comparison, ends with `suffix`. */
static bool path_ends_with(const char *url, const char *suffix) {
    size_t length = strcspn(url, "?#");
    size_t n = strlen(suffix);
    if (length < n)
        return false;
    const char *at = url + length - n;
    for (size_t i = 0; i < n; i++) {
        char a = at[i], b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != b)
            return false;
    }
    return true;
}

bool iptv_local_file(const char *url) {
    if (!url || !url[0])
        return false;

    /* A list or a manifest is read as one, not played as a file. */
    if (path_ends_with(url, ".m3u") || path_ends_with(url, ".m3u8"))
        return false;

    if (skip_prefix_ci(url, "file://"))
        return true;
    if (url_has_scheme(url))
        return false;

    /* No scheme left: a Windows path, a UNC path, a Unix path, or a relative
     * entry the reader has already joined to the playlist's directory. All of
     * them are this machine. */
    return true;
}

bool iptv_platform_host(const char *url) {
    if (!url || !url[0])
        return false;
    if (looks_like_media(url))
        return false;

    static const char *const platforms[] = { "twitch.tv", "kick.com" };
    const char *host = host_of(url);
    for (size_t i = 0; i < sizeof(platforms) / sizeof(*platforms); i++)
        if (host_is(host, platforms[i]))
            return true;
    return false;
}

/*
 * The URL of a channel, copied onto the heap and owned by the playlist.
 *
 * Never returns NULL. Six call sites read `channel->url` without checking, and
 * they are right not to: a channel with no URL is not a state this program can
 * do anything with, so out of memory yields the empty string, which libVLC
 * refuses with a message that names the channel. The alternative - a NULL that
 * every reader must remember to test - trades a rare bad channel for a rare
 * crash, in code that runs on someone's television.
 */
static const char url_none[] = "";

static const char *store_url(const char *url) {
    size_t size = strlen(url) + 1;
    char *copy = (char *)malloc(size);
    if (!copy) {
        iptv_log("[VLChannel] Out of memory storing a %zu character URL\n",
                size - 1);
        return url_none;
    }
    memcpy(copy, url, size);
    return copy;
}

/* Releases a URL, tolerating both the never-set NULL of a zeroed entry and the
 * shared empty string that store_url() hands back when malloc fails. */
static void release_url(const char *url) {
    if (url && url != url_none)
        free((void *)url);
}

static bool ensure_capacity(iptv_playlist *playlist) {
    if (playlist->count < playlist->capacity)
        return true;

    size_t capacity = playlist->capacity ? playlist->capacity * 2 : 64;
    iptv_channel *channels =
        (iptv_channel *)realloc(playlist->channels,
                                capacity * sizeof(iptv_channel));
    if (!channels) {
        iptv_log("[VLChannel] Out of memory growing the channel list to "
                        "%zu entries\n", capacity);
        return false;
    }

    playlist->channels = channels;
    playlist->capacity = capacity;
    return true;
}

static char *read_whole_file(const char *path, size_t *out_size) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        iptv_log("[VLChannel] Cannot open %s\n", path);
        return NULL;
    }

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (size < 0) {
        fclose(file);
        iptv_log("[VLChannel] Cannot measure %s\n", path);
        return NULL;
    }
    if ((unsigned long)size > IPTV_MAX_FILE_BYTES) {
        fclose(file);
        iptv_log("[VLChannel] %s is %ld bytes, above the %u byte limit\n",
                path, size, IPTV_MAX_FILE_BYTES);
        return NULL;
    }

    char *buffer = (char *)malloc((size_t)size + 1);
    if (!buffer) {
        fclose(file);
        iptv_log("[VLChannel] Out of memory reading %s\n", path);
        return NULL;
    }

    size_t read = fread(buffer, 1, (size_t)size, file);
    fclose(file);
    buffer[read] = '\0';
    *out_size = read;
    return buffer;
}

bool iptv_playlist_load(const char *path, iptv_playlist *out) {
    memset(out, 0, sizeof(*out));

    size_t size = 0;
    char *buffer = read_whole_file(path, &size);
    if (!buffer)
        return false;

    char *text = buffer;
    if (size >= 3 && (unsigned char)text[0] == 0xEF &&
        (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
        text += 3;                                   /* UTF-8 byte order mark */

    /*
     * A real HLS manifest, not a channel list. One entry pointing at the file
     * itself; the core hands the path straight to libVLC.
     */
    if (strstr(text, "#EXT-X-") != NULL) {
        if (!ensure_capacity(out)) {
            free(buffer);
            return false;
        }
        iptv_channel *channel = &out->channels[out->count++];
        memset(channel, 0, sizeof(*channel));
        channel->url = store_url(path);
        name_from_url(path, channel->name, sizeof(channel->name));
        if (!channel->name[0])
            copy_bounded(channel->name, sizeof(channel->name), "Stream");
        out->single_stream = true;
        free(buffer);
        iptv_log(
                "[VLChannel] %s is an HLS manifest, not a channel list; opening "
                "it as a single stream\n", path);
        return true;
    }

    /* Sized for a path, not for a URL: a relative entry is resolved against
     * the directory the list came from. */
    char base_directory[4096];
    directory_of(path, base_directory, sizeof(base_directory));

    char pending_name[IPTV_NAME_MAX] = {0};
    char pending_tvg_name[IPTV_NAME_MAX] = {0};
    char pending_group[IPTV_GROUP_MAX] = {0};
    char pending_tvg_id[IPTV_TVG_ID_MAX] = {0};
    char pending_options[IPTV_MAX_OPTIONS][IPTV_OPTION_MAX] = {{0}};
    int pending_option_count = 0;
    int pending_number = 0;
    int pending_audio_delay = 0;
    bool pending_audio_delay_set = false;
    bool truncated_warning_shown = false;

    /*
     * The line of the file, counted here rather than derived from the channel
     * index. It used to be printed as `out->count + 1`, which called the URL on
     * line 3 "line 1" and made the message useless for finding the entry - the
     * one thing a line number is for.
     */
    size_t line_number = 0;
    size_t youtube_rewritten = 0;
    size_t youtube_live_marked = 0;
    size_t platform_marked = 0;
    size_t local_files = 0;
    size_t streamlink_tags_ignored = 0;
    bool pending_streamlink = false;

    char *cursor = text;
    while (*cursor) {
        char *line = cursor;
        char *newline = strchr(cursor, '\n');
        if (newline) {
            *newline = '\0';
            cursor = newline + 1;
        } else {
            cursor = line + strlen(line);
        }
        line_number++;

        trim_in_place(line);
        if (!line[0])
            continue;

        if (line[0] == '#') {
            if (strncmp(line, "#EXTINF:", 8) == 0) {
                const char *body = line + 8;
                const char *comma = unquoted_comma(body);

                char attributes[512];
                size_t attribute_length =
                    comma ? (size_t)(comma - body) : strlen(body);
                if (attribute_length >= sizeof(attributes))
                    attribute_length = sizeof(attributes) - 1;
                memcpy(attributes, body, attribute_length);
                attributes[attribute_length] = '\0';

                /* The attributes it reads come out of `attributes`, so nothing
                 * longer than that can arrive here. */
                char value[sizeof(attributes)];
                extract_attribute(attributes, "group-title",
                                  value, sizeof(value));
                copy_bounded(pending_group, sizeof(pending_group), value);

                extract_attribute(attributes, "tvg-chno",
                                  value, sizeof(value));
                pending_number = value[0] ? atoi(value) : 0;

                extract_attribute(attributes, "tvg-id", value, sizeof(value));
                copy_bounded(pending_tvg_id, sizeof(pending_tvg_id), value);

                extract_attribute(attributes, "tvg-name", value, sizeof(value));
                copy_bounded(pending_tvg_name, sizeof(pending_tvg_name), value);

                if (comma) {
                    char name[IPTV_NAME_MAX];
                    copy_bounded(name, sizeof(name), comma + 1);
                    trim_in_place(name);
                    copy_bounded(pending_name, sizeof(pending_name), name);
                } else {
                    pending_name[0] = '\0';
                }

                if (!pending_name[0]) {
                    copy_bounded(pending_name, sizeof(pending_name),
                                 pending_tvg_name);
                }
            } else if (strncmp(line, "#EXTGRP:", 8) == 0) {
                char group[IPTV_GROUP_MAX];
                copy_bounded(group, sizeof(group), line + 8);
                trim_in_place(group);
                if (group[0])
                    copy_bounded(pending_group, sizeof(pending_group), group);
            } else if (strncmp(line, IPTV_STREAMLINK_TAG,
                               sizeof(IPTV_STREAMLINK_TAG) - 1) == 0 &&
                       (line[sizeof(IPTV_STREAMLINK_TAG) - 1] == '\0' ||
                        line[sizeof(IPTV_STREAMLINK_TAG) - 1] == ':' ||
                        line[sizeof(IPTV_STREAMLINK_TAG) - 1] == ' ')) {
                /*
                 * "The next address is a live page, open it with a resolver."
                 *
                 * The trailing character is checked so that a tag somebody
                 * later invents by adding a suffix - #EXTSTREAMLINKQUALITY,
                 * say - is not silently read as this one.
                 */
                pending_streamlink = true;
                if (youtube_mode != IPTV_YOUTUBE_YTDLP)
                    streamlink_tags_ignored++;
            } else if (strncmp(line, "#EXTVLCOPT:audio-desync=", 24) == 0) {
                /*
                 * Captured as the channel's delay instead of being passed
                 * through as a media option. Everything that touches audio
                 * delay then goes through one path, so a value from the list
                 * and a value nudged from the controller cannot both apply and
                 * add up.
                 */
                pending_audio_delay = atoi(line + 24);
                pending_audio_delay_set = true;
            } else if (strncmp(line, "#EXTVLCOPT:", 11) == 0) {
                /*
                 * Per channel libVLC options, most often http-user-agent or
                 * http-referrer. Channels that need them fail with a plain 403
                 * otherwise, which looks like a dead link rather than a missing
                 * header, so they are worth carrying even in a first version.
                 */
                const char *option = line + 11;
                while (*option == ' ' || *option == ':')
                    option++;
                if (*option && pending_option_count < IPTV_MAX_OPTIONS) {
                    copy_bounded(pending_options[pending_option_count],
                                 IPTV_OPTION_MAX, option);
                    pending_option_count++;
                } else if (*option && !truncated_warning_shown) {
                    truncated_warning_shown = true;
                    iptv_log(
                            "[VLChannel] More than %d #EXTVLCOPT lines on one "
                            "channel; the extra ones are ignored\n",
                            IPTV_MAX_OPTIONS);
                }
            }
            /* Everything else starting with # is a comment or a tag this
             * version has no use for. */
            continue;
        }

        if (out->count >= IPTV_MAX_CHANNELS) {
            iptv_log(
                    "[VLChannel] Channel limit of %d reached; the rest of %s was "
                    "ignored\n", IPTV_MAX_CHANNELS, path);
            break;
        }

        if (!ensure_capacity(out)) {
            free(buffer);
            iptv_playlist_free(out);
            return false;
        }

        iptv_channel *channel = &out->channels[out->count];
        memset(channel, 0, sizeof(*channel));

        /*
         * A YouTube address is rewritten here, at the one place a URL enters
         * the program, rather than at the moment it is opened. A rewrite at
         * open time would leave the guide, the banner and the log showing an
         * address the core never actually asks for, and the first person to
         * compare the log with the list would be reading two different stories.
         */
        char video_id[16];
        char live_label[IPTV_NAME_MAX];
        if (youtube_mode == IPTV_YOUTUBE_YTDLP &&
            (pending_streamlink || iptv_platform_host(line))) {
            /*
             * A platform channel: Twitch, Kick, or whatever the list author
             * marked. The address is a place rather than a stream, so a
             * resolver has to be asked what is behind it right now.
             *
             * NOT is_video, and that is the whole difference between this and
             * the two YouTube cases below. A YouTube list is a rotation - each
             * entry ends and the next one starts, which is what makes a list of
             * clips behave like a channel that is always on. A Twitch channel
             * is a channel: when it goes off the air the right answer is the
             * no-signal screen and a button to try again, exactly as for an
             * IPTV channel whose server went away. Walking to the next entry
             * because somebody stopped streaming would be the television
             * changing channel by itself.
             *
             * Checked before the YouTube forms so that #EXTSTREAMLINK can say
             * this about a YouTube address too. Nothing in the host table
             * matches youtube.com, so without the tag this branch cannot take
             * one by accident.
             */
            channel->url = store_url(line);
            channel->source = IPTV_SOURCE_PLATFORM;
            platform_marked++;
            if (!pending_name[0] &&
                iptv_youtube_live_channel(line, live_label, sizeof(live_label)))
                copy_bounded(pending_name, sizeof(pending_name), live_label);
        } else if (youtube_mode != IPTV_YOUTUBE_OFF &&
            iptv_youtube_id(line, video_id, sizeof(video_id))) {
            if (youtube_mode == IPTV_YOUTUBE_PROXY) {
                char rewritten[640];
                snprintf(rewritten, sizeof(rewritten), "%s%s",
                         youtube_proxy, video_id);
                channel->url = store_url(rewritten);
            } else {
                /* yt-dlp mode: the address stays as written, and is resolved
                 * when this entry is opened. */
                channel->url = store_url(line);
                channel->source = IPTV_SOURCE_YOUTUBE;
            }
            channel->is_video = true;
            youtube_rewritten++;
        } else if (youtube_mode == IPTV_YOUTUBE_YTDLP &&
                   iptv_youtube_live_channel(line, live_label,
                                             sizeof(live_label))) {
            /*
             * A channel's live page. Kept as written for the same reason every
             * yt-dlp entry is, and only in this mode because the proxy cannot
             * take an address with no video id in it.
             *
             * Marked is_video with the rest of them: what arrives is one
             * broadcast with an end, and when it ends the list should move on
             * rather than sit on a dead page waiting for a channel that is not
             * coming back. The next broadcast will be a different video behind
             * the same address, which is exactly what re-opening the entry
             * fetches.
             */
            channel->url = store_url(line);
            channel->is_video = true;
            channel->source = IPTV_SOURCE_YOUTUBE_LIVE;
            youtube_live_marked++;
            if (!pending_name[0])
                copy_bounded(pending_name, sizeof(pending_name), live_label);
        } else if (url_has_scheme(line) || path_is_absolute(line) ||
            !base_directory[0]) {
            channel->url = store_url(line);
        } else {
            /* A relative entry resolved against the directory the list came
             * from. Sized for a path, which does have a ceiling on every
             * filesystem this core runs on, unlike a URL. */
            char resolved[4096];
            int written = snprintf(resolved, sizeof(resolved), "%s\\%s",
                                   base_directory, line);
            if (written < 0 || (size_t)written >= sizeof(resolved))
                iptv_log(
                        "[VLChannel] Resolved path on line %zu is longer than %zu "
                        "characters and was truncated: %s\n",
                        line_number, sizeof(resolved) - 1, line);
            channel->url = store_url(resolved);
        }

        /*
         * Decided from the stored address rather than from the line, because a
         * relative entry only becomes a path once it has been joined to the
         * playlist's directory - and that happens in the branch above.
         *
         * Only DIRECT is reconsidered. An entry a resolver claimed is not a
         * file no matter what its address looks like.
         */
        if (channel->source == IPTV_SOURCE_DIRECT &&
            iptv_local_file(channel->url)) {
            channel->source = IPTV_SOURCE_LOCAL;
            channel->is_video = true;
            local_files++;
        }

        copy_bounded(channel->name, sizeof(channel->name), pending_name);
        if (!channel->name[0])
            name_from_url(channel->url, channel->name, sizeof(channel->name));
        if (!channel->name[0])
            snprintf(channel->name, sizeof(channel->name), "Canal %zu",
                     out->count + 1);

        copy_bounded(channel->group, sizeof(channel->group), pending_group);
        copy_bounded(channel->tvg_id, sizeof(channel->tvg_id), pending_tvg_id);
        copy_bounded(channel->tvg_name, sizeof(channel->tvg_name),
                     pending_tvg_name);
        channel->number = pending_number;
        for (int i = 0; i < pending_option_count; i++)
            copy_bounded(channel->options[i], IPTV_OPTION_MAX,
                         pending_options[i]);
        channel->option_count = pending_option_count;
        channel->audio_delay_ms = pending_audio_delay;
        channel->audio_delay_set = pending_audio_delay_set;

        out->count++;

        pending_name[0] = '\0';
        pending_tvg_name[0] = '\0';
        pending_group[0] = '\0';
        pending_tvg_id[0] = '\0';
        pending_number = 0;
        pending_option_count = 0;
        pending_audio_delay = 0;
        pending_audio_delay_set = false;
        pending_streamlink = false;
    }

    free(buffer);

    if (out->count == 0) {
        iptv_log(
                "[VLChannel] %s has no channel URL. A channel list holds one URL "
                "per line, optionally preceded by #EXTINF\n", path);
        iptv_playlist_free(out);
        return false;
    }

    assign_epg_indices(out);

    /* Decided here, once, rather than recounted by whoever draws a screen: the
     * answer cannot change while the list is loaded, and a screen that works it
     * out for itself is a screen that can disagree with the next one. */
    out->all_videos = true;
    for (size_t i = 0; i < out->count; i++)
        if (!out->channels[i].is_video) {
            out->all_videos = false;
            break;
        }

    iptv_log("[VLChannel] %s: %zu %s\n", path, out->count,
             out->all_videos ? "videos" : "channels");
    if (youtube_rewritten) {
        /* Said out loud because in proxy mode the list on disk and the URL in
         * the log will not match, and someone comparing the two deserves to be
         * told why rather than to work it out. */
        if (youtube_mode == IPTV_YOUTUBE_PROXY)
            iptv_log("[VLChannel] %zu YouTube entries rewritten to %s and "
                     "marked as videos: they advance to the next entry when "
                     "they end\n", youtube_rewritten, youtube_proxy);
        else
            iptv_log("[VLChannel] %zu YouTube entries marked as videos; each "
                     "address is resolved by yt-dlp when it is opened\n",
                     youtube_rewritten);
    }
    if (youtube_live_marked)
        iptv_log("[VLChannel] %zu YouTube channel live pages; yt-dlp resolves "
                 "whichever broadcast is on air when each one is opened\n",
                 youtube_live_marked);
    if (platform_marked)
        iptv_log("[VLChannel] %zu platform entries; streamlink is asked first "
                 "for these, and yt-dlp if it has nothing. They behave as live "
                 "channels: one that is off the air shows no signal rather "
                 "than advancing the list\n", platform_marked);
    if (local_files)
        iptv_log("[VLChannel] %zu local files; they are videos, so they "
                 "advance the list when they end, and they take the audio, "
                 "subtitle and jump buttons\n", local_files);
    if (streamlink_tags_ignored)
        iptv_log("[VLChannel] %zu " IPTV_STREAMLINK_TAG " lines had no effect: "
                 "the resolvers only run when Video entries is set to "
                 "alternative\n", streamlink_tags_ignored);
    return true;
}

void iptv_playlist_free(iptv_playlist *playlist) {
    for (size_t i = 0; i < playlist->count; i++)
        release_url(playlist->channels[i].url);
    free(playlist->channels);
    playlist->channels = NULL;
    playlist->count = 0;
    playlist->capacity = 0;
    playlist->single_stream = false;
    playlist->all_videos = false;
}
