#include "iptv_epg.h"
#include "iptv_log.h"
#include "iptv_playlist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * Titles are held in one growing buffer with the entries pointing into it,
 * rather than a fixed array per entry.
 *
 * A day of listings for a large list runs to tens of thousands of entries, and
 * a fixed title field would decide, today, how long a programme name is allowed
 * to be - which is exactly the mistake the 48 byte group field already made
 * once in this project. Here the only limit is how much text the file has.
 */
/* Its own, rather than vlc_dynamic.h's: that header pulls in libVLC, and
 * nothing here needs libVLC to find a file next to another file. */
#define EPG_PATH_MAX 4096

typedef struct {
    const char *tvg_id;      /* into the pool */
    const char *title;       /* into the pool */
    const char *desc;        /* into the pool, "" when the source had none */
    long long   start;
    long long   stop;
    size_t      index;
    long long   duration;
} entry;

static entry *entries = NULL;
static size_t entry_count = 0;
static size_t entry_capacity = 0;

static char  *pool = NULL;
static size_t pool_used = 0;
static size_t pool_capacity = 0;

static long long horizon = 0;
static long long generated = 0;
static int epg_format = 0;
static bool loaded = false;

/*
 * Returns an offset into the pool, not a pointer, and POOL_FAILED on failure.
 *
 * It used to return the pointer, and that was wrong in a way that never showed:
 * add_entry called it twice in a row, and the second call can reallocate. The
 * rebasing loop below repairs every pointer stored in `entries`, but the id
 * returned by the first call is not in `entries` yet - it is a local, holding
 * an address inside a block that has just been freed. It survived because glibc
 * usually grows a large block in place and because the window is two lines
 * wide. The third field for the description would have made the window wider.
 *
 * An offset cannot dangle. The caller turns it into a pointer after all the
 * additions are done.
 */
#define POOL_FAILED ((size_t)-1)

static size_t pool_add(const char *text) {
    size_t need = strlen(text) + 1;
    if (pool_used + need > pool_capacity) {
        size_t want = pool_capacity ? pool_capacity * 2 : 65536;
        while (want < pool_used + need)
            want *= 2;
        /*
         * Do not use realloc here. Once realloc succeeds, every pointer into
         * the old object is invalid, so subtracting it from the old base in the
         * rebasing loop is already undefined behaviour. Allocate separately,
         * calculate offsets while the old pool is still alive, then free it.
         */
        char *bigger = malloc(want);
        if (!bigger)
            return POOL_FAILED;
        if (pool_used > 0)
            memcpy(bigger, pool, pool_used);

        if (pool) {
            for (size_t i = 0; i < entry_count; i++) {
                entries[i].tvg_id = bigger + (entries[i].tvg_id - pool);
                entries[i].title  = bigger + (entries[i].title  - pool);
                entries[i].desc   = bigger + (entries[i].desc   - pool);
            }
        }
        free(pool);
        pool = bigger;
        pool_capacity = want;
    }

    size_t at = pool_used;
    memcpy(pool + at, text, need);
    pool_used += need;
    return at;
}

void iptv_epg_free(void) {
    free(entries);
    free(pool);
    entries = NULL;
    pool = NULL;
    entry_count = entry_capacity = 0;
    pool_used = pool_capacity = 0;
    horizon = generated = 0;
    epg_format = 0;
    loaded = false;
}

static bool add_entry(const char *id, long long start, long long stop,
                      size_t index, long long duration, const char *title,
                      const char *desc) {
    if (entry_count == entry_capacity) {
        size_t want = entry_capacity ? entry_capacity * 2 : 512;
        entry *bigger = realloc(entries, want * sizeof(*bigger));
        if (!bigger)
            return false;
        entries = bigger;
        entry_capacity = want;
    }

    /* All three added first, all three turned into pointers afterwards: any of
     * them can move the pool out from under the ones before it. */
    size_t id_at = pool_add(id);
    size_t title_at = pool_add(title);
    size_t desc_at = pool_add(desc);
    if (id_at == POOL_FAILED || title_at == POOL_FAILED ||
        desc_at == POOL_FAILED)
        return false;

    entries[entry_count].tvg_id = pool + id_at;
    entries[entry_count].title = pool + title_at;
    entries[entry_count].desc = pool + desc_at;
    entries[entry_count].start = start;
    entries[entry_count].stop = stop;
    entries[entry_count].index = index;
    entries[entry_count].duration = duration;
    entry_count++;
    return true;
}

void iptv_epg_load_for(const char *playlist_path) {
    iptv_epg_free();

    if (!playlist_path || !playlist_path[0])
        return;

    char path[EPG_PATH_MAX];
    snprintf(path, sizeof(path), "%s", playlist_path);

    /* Replace the extension, and only if the dot belongs to the file name -
     * a directory called "tv.stuff" must not eat the last path component. */
    char *dot = strrchr(path, '.');
    char *slash = strrchr(path, '/');
    char *back = strrchr(path, '\\');
    char *sep = slash;
    if (!sep || (back && back > sep))
        sep = back;
    if (dot && (!sep || dot > sep))
        *dot = '\0';
    if (strlen(path) + 5 >= sizeof(path))
        return;
    strcat(path, ".epg");

    FILE *f = fopen(path, "r");
    if (!f)
        return;                       /* the normal case: no listing */

    /* Wide enough for a title and a synopsis on one line. The producer caps the
     * description well below this; the slack is so that a source with a long
     * one loses the tail of a sentence instead of the whole entry, which is
     * what a line split in half would cost. */
    char line[8192];
    int format = 0;
    size_t bad = 0;

    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        if (!line[0])
            continue;

        if (line[0] == '#') {
            /*
             * Both spellings. The core was called VLCiptv when this format was
             * written, and every .epg generated before the rename says so. A
             * name change is the project's business and not the reader's: it
             * should not be able to make a file that was correct yesterday
             * unreadable today.
             */
            if (sscanf(line, "#VLCHANNEL-EPG %d", &format) == 1 ||
                sscanf(line, "#VLCIPTV-EPG %d", &format) == 1)
                continue;
            long long value;
            if (sscanf(line, "#horizon %lld", &value) == 1)
                horizon = value;
            else if (sscanf(line, "#generated %lld", &value) == 1)
                generated = value;
            continue;
        }

        char *tab1 = strchr(line, '\t');
        if (!tab1) { bad++; continue; }
        char *tab2 = strchr(tab1 + 1, '\t');
        if (!tab2) { bad++; continue; }
        char *tab3 = strchr(tab2 + 1, '\t');
        if (!tab3) { bad++; continue; }

        /* The fifth field is the description, and its absence is not an error:
         * that is what a format 1 line looks like, and a format 2 line whose
         * source gave no synopsis looks the same. */
        char *tab4 = strchr(tab3 + 1, '\t');
        const char *desc = "";
        if (tab4) {
            *tab4 = '\0';
            desc = tab4 + 1;
        }

        *tab1 = *tab2 = *tab3 = '\0';
        long long first = atoll(tab1 + 1);
        long long second = atoll(tab2 + 1);
        long long start = 0, stop = 0, duration = 0;
        size_t index = 0;

        if (format == 3) {
            if (first < 0 || second < 0) { bad++; continue; }
            index = (size_t)first;
            duration = second;
        } else {
            start = first;
            stop = second;
            if (stop <= start) { bad++; continue; }
        }

        if (!add_entry(line, start, stop, index, duration,
                       tab3 + 1, desc)) {
            iptv_log("[VLChannel] Programme listing: out of memory after %zu "
                     "entries; using what was read\n", entry_count);
            break;
        }
    }
    fclose(f);

    /*
     * Format 1 has no description column and is read exactly as before. A file
     * generated before this core learned about descriptions keeps working; it
     * simply has nothing for that part of the banner. Refusing it would have
     * turned an added field into a broken install.
     */
    if (format != 1 && format != 2 && format != 3) {
        iptv_log("[VLChannel] Programme listing %s is format %d, this core reads "
                 "1, 2 and 3; ignoring it\n", path, format);
        iptv_epg_free();
        return;
    }

    long long now = (long long)time(NULL);
    if (format != 3 && horizon > 0 && now > horizon) {
        long long hours = (now - horizon) / 3600;
        iptv_log("[VLChannel] Programme listing %s expired %lld hours ago; "
                 "showing the group instead. Run ferramentas/prepara-epg.py "
                 "again.\n", path, hours);
        iptv_epg_free();
        return;
    }

    epg_format = format;
    loaded = entry_count > 0;
    iptv_log("[VLChannel] Programme listing: %zu entries from %s (format %d)%s\n",
             entry_count, path, epg_format,
             bad ? " (some malformed lines skipped)" : "");
}

bool iptv_epg_available(void) {
    return loaded;
}

size_t iptv_epg_entry_count(void) {
    return entry_count;
}

/* Shared guard: no listing, no identifier, or a grid that went stale while the
 * channel was playing. */
static bool usable(const char *tvg_id) {
    if (!loaded || !tvg_id || !tvg_id[0])
        return false;
    long long now = (long long)time(NULL);
    if (epg_format != 3 && horizon > 0 && now > horizon)
        return false;
    return true;
}

static void fill(iptv_epg_programme *out, const entry *e) {
    out->title = e->title;
    out->desc = e->desc;
    out->start = e->start;
    out->stop = e->stop;
    out->index = e->index;
    out->duration = e->duration;
    out->ordered = epg_format == 3;
}

/*
 * Linear scans, on purpose. They run when the banner is drawn, not per frame,
 * and even a large listing is tens of thousands of entries - work a modern
 * machine does in well under a millisecond. An index would be faster and would
 * be one more thing that can be wrong.
 */
bool iptv_epg_now_at(const char *tvg_id, size_t index,
                     iptv_epg_programme *out) {
    if (!usable(tvg_id))
        return false;

    if (epg_format == 3) {
        for (size_t i = 0; i < entry_count; i++) {
            if (entries[i].index == index &&
                strcmp(entries[i].tvg_id, tvg_id) == 0) {
                fill(out, &entries[i]);
                return true;
            }
        }
        return false;
    }

    long long now = (long long)time(NULL);
    for (size_t i = 0; i < entry_count; i++) {
        if (entries[i].start <= now && now < entries[i].stop &&
            strcmp(entries[i].tvg_id, tvg_id) == 0) {
            fill(out, &entries[i]);
            return true;
        }
    }
    return false;
}

bool iptv_epg_next_at(const char *tvg_id, size_t index,
                      iptv_epg_programme *out) {
    if (!usable(tvg_id))
        return false;

    if (epg_format == 3) {
        const entry *first = NULL;
        const entry *next = NULL;
        for (size_t i = 0; i < entry_count; i++) {
            if (strcmp(entries[i].tvg_id, tvg_id) != 0)
                continue;
            if (!first || entries[i].index < first->index)
                first = &entries[i];
            if (entries[i].index > index &&
                (!next || entries[i].index < next->index))
                next = &entries[i];
        }
        next = next ? next : first;              /* last item wraps to first */
        if (!next)
            return false;
        fill(out, next);
        return true;
    }

    long long now = (long long)time(NULL);
    /*
     * The earliest programme that has not started, not "the line after the
     * current one": the file is in whatever order the XMLTV had, and a schedule
     * with a gap in it has no line after the current one at all.
     */
    const entry *best = NULL;
    for (size_t i = 0; i < entry_count; i++) {
        if (entries[i].start <= now)
            continue;
        if (best && entries[i].start >= best->start)
            continue;
        if (strcmp(entries[i].tvg_id, tvg_id) != 0)
            continue;
        best = &entries[i];
    }
    if (!best)
        return false;
    fill(out, best);
    return true;
}

bool iptv_epg_now(const char *tvg_id, iptv_epg_programme *out) {
    return iptv_epg_now_at(tvg_id, 0, out);
}

bool iptv_epg_next(const char *tvg_id, iptv_epg_programme *out) {
    return iptv_epg_next_at(tvg_id, 0, out);
}
