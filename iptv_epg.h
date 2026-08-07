#ifndef IPTV_EPG_H
#define IPTV_EPG_H

#include <stdbool.h>
#include <stddef.h>

/*
 * What is on right now, read from a file prepared beside the playlist.
 *
 * The core does not fetch or parse XMLTV. ferramentas/prepara-epg.py does that
 * - the download, the gzip, the XML, and the timezone arithmetic - and writes
 * `<playlist>.epg`, which is tab separated text with times already resolved to
 * whole seconds since the epoch. All that is left here is a linear scan and a
 * comparison of two integers, which is the amount of listing code this core can
 * carry without becoming a different program.
 *
 * The format, for anyone reading a file by hand:
 *
 *     #VLCHANNEL-EPG 2
 *     #generated 1785522359
 *     #horizon   1785695159
 *     #source    https://...
 *     <tvg-id>\t<start>\t<stop>\t<title>\t<description>
 *
 * Format 1 is the same line without the description, and is still read: an .epg
 * already sitting beside a playlist keeps working, it just has nothing to put on
 * the description line. Refusing it would have turned a new field into a broken
 * install, which is a bad trade for one column.
 *
 * Format 3 describes a finite playlist rather than a broadcast schedule:
 *
 *     #VLCHANNEL-EPG 3
 *     #channel <tvg-id> <item-count>
 *     <tvg-id>\t<index>\t<duration>\t<title>\t<description>
 *
 * Its index is zero-based inside the tvg-id, its duration may be zero when the
 * M3U did not provide one, and its last item wraps to the first. It carries no
 * horizon because playlist order does not become stale with the clock.
 *
 * `horizon` is the point past which the file stops being used. Listings go
 * stale, and a stale listing that still looks live is worse than none: it says
 * the wrong programme with the same confidence as the right one, and nobody
 * watching can tell.
 */

/*
 * Loads the listing that belongs to `playlist_path`, replacing the extension
 * with .epg. Missing file is not a failure - most lists have no listing - and
 * leaves the module empty and silent.
 */
void iptv_epg_load_for(const char *playlist_path);
void iptv_epg_free(void);

/* True when a usable, unexpired listing is loaded. */
bool iptv_epg_available(void);

/*
 * One programme. `title` and `desc` are never NULL; `desc` is the empty string
 * when the source carried none, which is common and is not an error. The
 * pointers belong to the module and stay valid until the next load or free.
 */
typedef struct {
    const char *title;
    const char *desc;
    long long   start;
    long long   stop;
    size_t      index;
    long long   duration;
    bool        ordered;
} iptv_epg_programme;

/*
 * What is on air, and what follows it, for this tvg-id. False is the normal
 * answer for a channel with no tvg-id, a channel the listing does not cover, a
 * gap in the schedule, or - for _next - the end of the loaded grid; the caller
 * falls back to the group.
 *
 * These used to be one function returning the title as a bare string. The
 * caller now needs the clock times as well, and a title alone cannot say when
 * the programme ends: the same string is drawn very differently by someone who
 * knows it has four minutes left.
 */
bool iptv_epg_now(const char *tvg_id, iptv_epg_programme *out);
bool iptv_epg_next(const char *tvg_id, iptv_epg_programme *out);

/* The indexed variants select the current item in a format 3 listing. For
 * formats 1 and 2 `index` is ignored and the wall clock is used exactly as in
 * the original API above. */
bool iptv_epg_now_at(const char *tvg_id, size_t index,
                     iptv_epg_programme *out);
bool iptv_epg_next_at(const char *tvg_id, size_t index,
                      iptv_epg_programme *out);

/* How many entries were loaded, and how many channels they cover. For the log. */
size_t iptv_epg_entry_count(void);

#endif
