#include "iptv_scrub.h"

#include <stddef.h>
#include <stdbool.h>
#include <string.h>

/*
 * A value ends where the thing containing it ends.
 *
 * The URL is not alone on the line - it sits inside a sentence libVLC wrote -
 * so a value runs to the next structural character or to the end of the word,
 * and whitespace and quotes end it as surely as a slash does.
 */
static bool ends_value(char c) {
    return c == '\0' || c == '/' || c == '?' || c == '&' || c == '#' ||
           c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
           c == '"' || c == '\'' || c == ',' || c == ')';
}

static bool ends_segment(char c) {
    return c == '\0' || c == '/' || c == '?' || c == '#' ||
           c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
           c == '"' || c == '\'' || c == ',' || c == ')';
}

/* Deletes `count` characters at `at`, closing the gap. */
static void cut(char *at, size_t count) {
    if (count == 0)
        return;
    memmove(at, at + count, strlen(at + count) + 1);
}

static bool equal_ci(const char *text, const char *word, size_t length) {
    for (size_t i = 0; i < length; i++) {
        char a = text[i], b = word[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b)
            return false;
    }
    return true;
}

/*
 * The named fields.
 *
 * `ip` and `mip` are the viewer. The rest are bearer tokens: whoever holds one
 * can fetch the stream until it expires, so a log with them in it is a log that
 * hands out the subscription.
 *
 * A table rather than a pattern, for the same reason the platform hosts are a
 * table: the list is the specification, and the next field somebody finds is
 * one line here instead of an unreadable expression to re-derive.
 *
 * `key`, `id` and `token`-like words that are too general are deliberately
 * absent. A log that has quietly deleted the interesting half of a line is
 * worse than one that never touched it, because nobody can tell.
 */
static const char *const secret_fields[] = {
    /* who is watching */
    "ip", "mip",
    /* what would let somebody else watch */
    "sig", "lsig", "signature", "pot", "token", "auth",
    /* what an IPTV subscription is sold as */
    "username", "password", "user", "pass", "pwd",
};

static const char *field_after(const char *at, char terminator) {
    for (size_t i = 0; i < sizeof(secret_fields) / sizeof(*secret_fields); i++) {
        size_t length = strlen(secret_fields[i]);
        if (equal_ci(at, secret_fields[i], length) && at[length] == terminator)
            return at + length + 1;
    }
    return NULL;
}

/*
 * Fields written as `/name/value/` - which is how googlevideo writes them - and
 * as `?name=value`.
 *
 * The name must start right after a structural character, so `?ip=` matches and
 * `/equip/` and `&clip=` do not. That check is the whole reason this is not a
 * substring search.
 */
static void scrub_fields(char *text) {
    for (char *p = text; *p; p++) {
        const char *value = NULL;

        if (*p == '/')
            value = field_after(p + 1, '/');
        else if (*p == '?' || *p == '&')
            value = field_after(p + 1, '=');

        if (!value)
            continue;

        char *at = (char *)value;
        size_t length = 0;
        while (!ends_value(at[length]))
            length++;

        cut(at, length);
        /* Continue from the delimiter before the now-empty value, so a run of
         * fields - `&user=&pass=` - is walked in one pass. */
        p = at - 1;
    }
}

/*
 * The other shape: an IPTV subscription in the path.
 *
 *   http://host:8080/joao/segredo123/45678.ts
 *   http://host:8080/live/joao/segredo123/45678.m3u8
 *
 * Recognised by what comes last. The final segment must be digits and a media
 * extension - that is the stream id, and it is what makes this shape this shape
 * rather than an ordinary path that happens to be three deep. The two segments
 * in front of it are the credentials, and they are what goes.
 *
 * Bounded on both sides on purpose. Requiring the numeric id keeps
 * `/hls/720p/index.m3u8` out of it; requiring exactly two or three segments in
 * front keeps a deep CDN path out of it. A rule that took either would be
 * deleting parts of ordinary channel URLs, and those are the URLs somebody is
 * reading the log to check.
 */
static bool numeric_stream_file(const char *segment, size_t length) {
    static const char *const extensions[] = {
        ".ts", ".m3u8", ".mp4", ".mkv", ".m3u"
    };

    size_t digits = 0;
    while (digits < length && segment[digits] >= '0' && segment[digits] <= '9')
        digits++;
    if (digits < 3)                       /* an Xtream id is never one digit */
        return false;

    size_t rest = length - digits;
    for (size_t i = 0; i < sizeof(extensions) / sizeof(*extensions); i++) {
        size_t n = strlen(extensions[i]);
        if (rest == n && equal_ci(segment + digits, extensions[i], n))
            return true;
    }
    return false;
}

static void scrub_subscription_paths(char *text) {
    for (char *p = text; *p; p++) {
        if (strncmp(p, "://", 3) != 0)
            continue;

        /* Past the host, to the first slash of the path. */
        char *at = p + 3;
        while (!ends_segment(*at))
            at++;
        if (*at != '/')
            continue;

        /* Up to five segments is all this shape ever has; anything longer is
         * some other kind of path and is left alone. */
        char *start[6];
        size_t length[6];
        size_t count = 0;
        char *walk = at;
        while (*walk == '/' && count < 6) {
            walk++;
            start[count] = walk;
            size_t n = 0;
            while (!ends_segment(walk[n]))
                n++;
            length[count] = n;
            walk += n;
            count++;
        }

        size_t first = 0;
        if (count == 4) {
            /* live/user/pass/id.ts and its siblings. */
            static const char *const sections[] = {
                "live", "movie", "series", "timeshift"
            };
            bool known = false;
            for (size_t i = 0; i < sizeof(sections) / sizeof(*sections); i++)
                if (length[0] == strlen(sections[i]) &&
                    equal_ci(start[0], sections[i], length[0]))
                    known = true;
            if (!known)
                continue;
            first = 1;
        } else if (count != 3) {
            continue;
        }

        if (!numeric_stream_file(start[first + 2], length[first + 2]))
            continue;
        if (length[first] == 0 && length[first + 1] == 0)
            continue;                                   /* already scrubbed */

        /* The second one first, so that cutting it does not move the first. */
        cut(start[first + 1], length[first + 1]);
        cut(start[first], length[first]);
        p = start[first];
    }
}

void iptv_scrub(char *text) {
    if (!text || !text[0])
        return;
    scrub_fields(text);
    scrub_subscription_paths(text);
}
