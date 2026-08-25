#include "iptv_transport.h"
#include "iptv_text.h"
#include "iptv_log.h"

#include <stdio.h>
#include <string.h>

/*
 * The most tracks that will be cycled through.
 *
 * A file with more than this many audio or subtitle tracks exists, and on one
 * the extra tracks simply cannot be reached with this button. That is a real
 * limit and it is written down rather than hidden: a fixed array here is worth
 * more than an allocation on a path the viewer drives with a controller, and
 * the alternative - walking libVLC's linked list twice on every press - would
 * be the same limit expressed as a slower loop.
 */
#define TRACK_MAX 32

typedef struct {
    int id;
    char name[64];
} track;

static long long last_reported_ms = -1;

void iptv_transport_reset(void) {
    last_reported_ms = -1;
}

/*
 * libVLC's track list, copied into an array and released immediately.
 *
 * Copied because the list is owned by libVLC and has to be handed back, and
 * because deciding "which one comes after the current one" over a linked list
 * that must be freed on every exit path is how a leak gets written.
 */
static size_t collect(libvlc_track_description_t *list, track *out,
                      size_t max) {
    size_t count = 0;
    for (libvlc_track_description_t *at = list; at && count < max;
         at = at->p_next) {
        out[count].id = at->i_id;
        snprintf(out[count].name, sizeof(out[count].name), "%s",
                 at->psz_name ? at->psz_name : "");
        count++;
    }
    if (list && libvlc_track_description_list_release)
        libvlc_track_description_list_release(list);
    return count;
}

/* Where `id` sits in the array, or -1. */
static int index_of(const track *tracks, size_t count, int id) {
    for (size_t i = 0; i < count; i++)
        if (tracks[i].id == id)
            return (int)i;
    return -1;
}

/* A track with no name is not worth showing as an empty string. Files written
 * by a muxer that skipped the language tag are common enough that this is the
 * ordinary case, not the odd one. */
static const char *label_of(const track *t, int position, char *scratch,
                            size_t scratch_size) {
    if (t->name[0])
        return t->name;
    snprintf(scratch, scratch_size, "%d", position + 1);
    return scratch;
}

bool iptv_transport_next_audio(libvlc_media_player_t *player,
                               char *notice, size_t notice_size) {
    if (notice && notice_size)
        notice[0] = '\0';
    if (!player || !libvlc_audio_get_track_description ||
        !libvlc_audio_get_track || !libvlc_audio_set_track)
        return false;

    track tracks[TRACK_MAX];
    size_t count = collect(libvlc_audio_get_track_description(player),
                           tracks, TRACK_MAX);

    /*
     * "Disable" comes back in the same list, with the id libVLC uses for "no
     * track". Dropped here rather than skipped in the loop, so that "how many
     * tracks are there" is answered by one number that is true.
     */
    size_t kept = 0;
    for (size_t i = 0; i < count; i++)
        if (tracks[i].id != -1)
            tracks[kept++] = tracks[i];
    count = kept;

    if (count == 0) {
        snprintf(notice, notice_size, "%s", iptv_text(IPTV_TEXT_ONE_AUDIO));
        return false;
    }
    if (count == 1) {
        char scratch[16];
        snprintf(notice, notice_size, "%s: %s", iptv_text(IPTV_TEXT_AUDIO),
                 label_of(&tracks[0], 0, scratch, sizeof(scratch)));
        return false;
    }

    int current = index_of(tracks, count, libvlc_audio_get_track(player));
    int next = (current < 0) ? 0 : (current + 1) % (int)count;

    if (libvlc_audio_set_track(player, tracks[next].id) != 0) {
        iptv_log("[VLChannel] libVLC refused audio track %d\n", tracks[next].id);
        return false;
    }

    char scratch[16];
    snprintf(notice, notice_size, "%s: %s", iptv_text(IPTV_TEXT_AUDIO),
             label_of(&tracks[next], next, scratch, sizeof(scratch)));
    iptv_log("[VLChannel] Audio track %d of %zu\n", next + 1, count);
    return true;
}

bool iptv_transport_next_subtitle(libvlc_media_player_t *player,
                                  char *notice, size_t notice_size) {
    if (notice && notice_size)
        notice[0] = '\0';
    if (!player || !libvlc_video_get_spu_description ||
        !libvlc_video_get_spu || !libvlc_video_set_spu) {
        /* A runtime without the subtitle symbols. Said plainly rather than
         * silently doing nothing, because the button is on the pad either
         * way. */
        snprintf(notice, notice_size, "%s",
                 iptv_text(IPTV_TEXT_NO_SUBTITLES));
        return false;
    }

    track tracks[TRACK_MAX];
    size_t count = collect(libvlc_video_get_spu_description(player),
                           tracks, TRACK_MAX);

    /*
     * "Disable" is kept here, and it is what makes one button enough: the cycle
     * runs through every track and then through off, which is where somebody
     * who turned subtitles on by mistake gets back to.
     *
     * libVLC lists it first, so it is also where the cycle starts on a file
     * that opened without subtitles - the first press gives the first real
     * track, which is what the press meant.
     */
    if (count == 0 || (count == 1 && tracks[0].id == -1)) {
        snprintf(notice, notice_size, "%s",
                 iptv_text(IPTV_TEXT_NO_SUBTITLES));
        return false;
    }

    int current = index_of(tracks, count, libvlc_video_get_spu(player));
    int next = (current < 0) ? 0 : (current + 1) % (int)count;

    if (libvlc_video_set_spu(player, tracks[next].id) != 0) {
        iptv_log("[VLChannel] libVLC refused subtitle track %d\n",
                 tracks[next].id);
        return false;
    }

    if (tracks[next].id == -1) {
        snprintf(notice, notice_size, "%s: %s",
                 iptv_text(IPTV_TEXT_SUBTITLES),
                 iptv_text(IPTV_TEXT_SUBTITLES_OFF));
        iptv_log("[VLChannel] Subtitles off\n");
    } else {
        char scratch[16];
        snprintf(notice, notice_size, "%s: %s",
                 iptv_text(IPTV_TEXT_SUBTITLES),
                 label_of(&tracks[next], next, scratch, sizeof(scratch)));
        iptv_log("[VLChannel] Subtitle track %d of %zu\n", next + 1, count);
    }
    return true;
}

/* h:mm:ss, or m:ss under an hour. A film needs the hour and an episode does
 * not, and a leading "0:" on everything is a digit the viewer has to skip. */
static void format_position(char *out, size_t out_size, long long ms) {
    if (ms < 0) ms = 0;
    long long total = ms / 1000;
    long long hours = total / 3600;
    long long minutes = (total / 60) % 60;
    long long seconds = total % 60;

    if (hours > 0)
        snprintf(out, out_size, "%lld:%02lld:%02lld", hours, minutes, seconds);
    else
        snprintf(out, out_size, "%lld:%02lld", minutes, seconds);
}

bool iptv_transport_seek(libvlc_media_player_t *player, int seconds,
                         char *notice, size_t notice_size) {
    if (notice && notice_size)
        notice[0] = '\0';
    if (!player || !libvlc_media_player_set_time ||
        !libvlc_media_player_get_time)
        return false;

    if (libvlc_media_player_is_seekable &&
        !libvlc_media_player_is_seekable(player))
        return false;

    /*
     * Measured from the last position this reported, not from the player, when
     * the two are close.
     *
     * Holding the button gives presses faster than libVLC applies a seek, and
     * asking the player where it is between two of them answers with the
     * position it has not finished leaving. Three quick presses would then land
     * thirty seconds away only if every one of them happened to be read after
     * the previous had taken effect, and otherwise somewhere arbitrary between
     * ten and thirty. Accumulating our own target makes a held button
     * predictable.
     */
    long long now = (long long)libvlc_media_player_get_time(player);
    long long from = now;
    if (last_reported_ms >= 0) {
        long long drift = last_reported_ms - now;
        if (drift < 0) drift = -drift;
        if (drift < 5000)
            from = last_reported_ms;
    }

    long long target = from + (long long)seconds * 1000;
    if (target < 0)
        target = 0;

    /*
     * Kept just short of the end. Seeking exactly to the length is a request to
     * play nothing, and libVLC answers it by reporting the file ended - which
     * this core reads as "the video finished" and steps to the next entry. A
     * viewer who pressed forward once does not expect the next episode.
     */
    long long length = libvlc_media_player_get_length
                           ? (long long)libvlc_media_player_get_length(player)
                           : 0;
    if (length > 0 && target > length - 2000) {
        target = length - 2000;
        if (target < 0)
            target = 0;
    }

    char position[32];
    format_position(position, sizeof(position), target);

    /*
     * Held against an end of the file, the clamp above leaves the target where
     * it already was. Saying "forward 10s" there would be the one thing this
     * notice exists to prevent: a button that reports an action it did not
     * perform. The position alone is the honest answer, and seeing it stay put
     * is how the viewer learns they are at the end.
     *
     * False, too, so that the caller does not resync audio for a seek that
     * never happened - which would be an audible gap in exchange for nothing.
     */
    if (target == from) {
        snprintf(notice, notice_size, "%s", position);
        return false;
    }

    libvlc_media_player_set_time(player, (libvlc_time_t)target);
    last_reported_ms = target;

    /*
     * The arrows are not letters, so they say the same thing in both languages
     * and there is no string to translate. The number is what actually moved
     * rather than what was asked for, because a jump that ran into the start of
     * the file moved less than ten seconds and should say so.
     */
    long long moved = (target - from) / 1000;
    snprintf(notice, notice_size, "%s%llds   %s",
             moved < 0 ? "<< " : ">> ", moved < 0 ? -moved : moved, position);
    iptv_log("[VLChannel] Seek %+llds to %s\n", moved, position);
    return true;
}
