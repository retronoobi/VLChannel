#ifndef IPTV_TRANSPORT_H
#define IPTV_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>

#include "vlc_dynamic.h"

/*
 * The four controls a file on disk can have and a television channel cannot:
 * pick an audio track, pick a subtitle track, jump back, jump forward.
 *
 * This is a module of its own because of what it must not touch. The core has
 * two audio alignments already - the fixed reserve that live IPTV uses and the
 * PTS clock that a resolved YouTube stream uses - and both were arrived at
 * painfully, over sessions of measurement, for streams that arrive at the speed
 * the network sends them. A local file is a third case that needs neither, and
 * the way to keep the first two working is to add nothing to them. So nothing
 * here reaches into the sync path; the only thing it asks of the core is a
 * resync after an operation that moved the stream, which is the same call a
 * channel change already makes.
 *
 * Nothing here draws, either. libVLC renders subtitles into the picture it
 * already hands this core, so choosing a track is the whole of the work - what
 * the core draws is a line saying which track that was, and that lives in the
 * overlay where the rest of the drawing lives.
 *
 * Every entry point is a no-op unless the caller has decided the current entry
 * is a local file. The decision belongs to the caller because the caller is the
 * one holding the playlist.
 */

/*
 * How far a jump goes.
 *
 * One value for both directions, because an asymmetric jump is a control the
 * viewer has to remember rather than one they can feel. Ten seconds is long
 * enough to skip a missed line of dialogue and short enough that overshooting
 * costs one more press.
 */
#define IPTV_TRANSPORT_STEP_SECONDS 10

/*
 * The longest notice any of these produces, including the track name.
 *
 * Track names come out of the file - "Português (Brasil) [Forced]" is an
 * ordinary one - so this is sized for a name somebody actually wrote, not for
 * the two-letter codes the well-behaved files use.
 */
#define IPTV_TRANSPORT_NOTICE_MAX 128

/*
 * Forgets the last position it reported. Called when a channel opens, so that
 * the first jump on a new file is measured from that file.
 */
void iptv_transport_reset(void);

/*
 * Steps to the next audio track, wrapping at the end.
 *
 * "Disable" is skipped: libVLC offers it in the same list, and a viewer cycling
 * through languages who lands on silence has no way of knowing whether they
 * broke the file or the core. Subtitles are the opposite case - see below.
 *
 * Fills `notice` with the localised line to show and returns true when
 * something changed. False means there was nothing to change: one track, or no
 * player. `notice` is filled either way, because "only one audio track" is an
 * answer to the button press and a button that does nothing visible is a button
 * the viewer presses again harder.
 */
bool iptv_transport_next_audio(libvlc_media_player_t *player,
                               char *notice, size_t notice_size);

/*
 * Steps to the next subtitle track, wrapping - and the wrap goes through "off",
 * which is what makes one button enough. Turning subtitles off is a thing
 * people want; turning audio off is not.
 */
bool iptv_transport_next_subtitle(libvlc_media_player_t *player,
                                  char *notice, size_t notice_size);

/*
 * Jumps `seconds` from where playback is now, negative for backwards, clamped
 * to the file.
 *
 * Returns true when the position moved, and fills `notice` with the direction
 * and the position it landed on - a bare "10s" says what was asked for, and
 * where it ended up is what the viewer actually wanted to know.
 *
 * The caller must resync the audio afterwards: a jump leaves a second and a
 * half of the old position sitting in the ring, and playing it would be a
 * stutter that sounds like the seek failed.
 */
bool iptv_transport_seek(libvlc_media_player_t *player, int seconds,
                         char *notice, size_t notice_size);

#endif
