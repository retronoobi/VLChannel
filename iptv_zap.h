#ifndef IPTV_ZAP_H
#define IPTV_ZAP_H

#include <stdbool.h>
#include <stdint.h>

/*
 * The channel change noise, and the one rule that matters about it.
 *
 * It is mixed into the block that is already on its way to the frontend, at the
 * very last moment, and it touches nothing else. Not the ring buffer, not the
 * read or write position, not the PTS, not sync_offset, not audio_sent_frames.
 *
 * That is not tidiness, it is the lesson of 0.1.8 and 0.2.0. The audio path in
 * this core has been broken twice by changes made with a theory in hand, and
 * both times the change looked local. The alignment between sound and picture
 * here is the *length of the audio queue*, so anything that adds or removes
 * samples from that queue moves the picture out of sync with the sound. A sound
 * effect has no business in that queue.
 *
 * So the noise is not queued. It is summed on top of whatever the core was
 * about to output, sample by sample, and when it finishes it stops. If the
 * volume is off, iptv_zap_mix does nothing at all and the audio path is byte
 * for byte the one that was already working.
 *
 * The timing is a gift rather than a problem: a channel change is followed by a
 * caching window of silence while the new channel prebuffers - a second and a
 * half by default - so a 110 ms noise lands in a gap that already existed. It
 * covers the silence instead of competing with anything.
 */

/* 0 disables. 100 plays the takes at the level they were recorded. */
void iptv_zap_set_volume(int percent);
int iptv_zap_volume(void);

/*
 * Starts the noise. Takes are used in turn rather than at random: zapping is
 * something people do several times in a row, and a random pick can repeat
 * itself twice, which is the exact thing that would sound wrong.
 *
 * Does nothing when the volume is 0. Restarts from the beginning if one is
 * already playing - holding a channel change button repeats the sound rather
 * than layering it into mush.
 */
void iptv_zap_trigger(void);

/* True while a noise still has samples left to play. */
bool iptv_zap_active(void);

/* Forgets any noise in progress, for a channel that is being torn down. */
void iptv_zap_reset(void);

/*
 * Mixes what is left of the noise into an interleaved stereo S16 block of
 * `frames` frames, saturating instead of wrapping. Safe to call every frame,
 * whether or not anything is playing.
 */
void iptv_zap_mix(int16_t *stereo, unsigned frames);

#endif
