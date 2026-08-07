#ifndef IPTV_DRIFT_H
#define IPTV_DRIFT_H

#include <stddef.h>
#include <stdint.h>

/*
 * Keeping the audio queue where it was put, against a frontend whose cadence is
 * not quite the frame rate the core declared.
 *
 * The core hands the frontend 48000/fps frames per retro_run and assumes
 * retro_run happens fps times a second. Measured over three sessions, EmuVR is
 * 0.35% away from that and RetroArch is not measurably away at all. Nothing in
 * the core noticed: the queue drained 3.5 ms every second, in a straight line,
 * until it hit bottom after four minutes and stayed there - a reserve of
 * 1200 ms can never refill when the reader is faster than the writer, so the
 * audio did not come back until the channel was changed.
 *
 * The correction deliberately does *not* change what the frontend receives. The
 * block is always exactly the number of frames it has always been. What varies
 * is how many frames are consumed from the ring to produce it, by resampling
 * with linear interpolation at a ratio that stays within half a percent of one.
 * Half a percent is under nine cents of pitch, which is inaudible here, and it
 * keeps the libretro side of the contract byte for byte identical to the
 * version that works.
 *
 * At a ratio of exactly one this is a copy. Not approximately: the phase lands
 * on whole samples, the interpolation weight is zero, and the output is the
 * same bytes the memcpy produced. There is a test for that, because "should be
 * identical" is how this core got broken twice.
 */

#define IPTV_DRIFT_ONE      65536          /* ratio 1.0, Q16 */
/*
 * How far the ratio may leave one. 655 is 1%, which is seventeen cents of
 * pitch - still inaudible on television audio, and deliberately about three
 * times the worst drift measured (0.35% under EmuVR).
 *
 * It started at half a percent. The simulation then showed a frontend drifting
 * a full 0.5% parking 148 ms away from the target instead of on it: with no
 * authority to spare, the loop can only balance, never correct. Headroom is
 * what turns "just barely keeps up" into "returns to where it should be".
 */
#define IPTV_DRIFT_MAX_STEP 655            /* 1.0% of IPTV_DRIFT_ONE */

/*
 * Called whenever the ring is flushed - a channel change, a resync.
 *
 * Forgets the fractional phase, because the samples it pointed into no longer
 * exist. Keeps the learned correction, because it does not belong to the
 * channel: it is the frontend calling retro_run at a slightly different rate
 * than the fps the core declared, and EmuVR does that at 60.21 Hz whatever
 * happens to be playing.
 *
 * This mattered more than it looks. The loop needs about 170 seconds to learn
 * the correction from zero, so throwing it away on every zap meant a viewer
 * changing channel every few minutes never left the transient at all - the
 * queue sat around 1157 ms instead of the 1193 it reaches when the correction
 * survives. The behaviour that has to be forgotten and the behaviour that has
 * to be kept were in the same function, which is why nobody noticed.
 */
void iptv_drift_flush_ring(void);

/* Forgets everything, learned correction included. For unloading content. */
void iptv_drift_reset(void);

/*
 * The next ratio, from the queue we have and the queue we want.
 *
 * Proportional plus integral, and the integral is the point rather than a
 * refinement. With proportional alone the loop needs a standing error to
 * produce the very correction that cancels the drift, so the queue settles
 * wherever that balance happens to be: simulated against the measured EmuVR
 * rate it parked at 920 ms instead of 1200, and against a hypothetical +0.5%
 * it parked at 1600. Stable, never empty, and 280 ms out of place - which is
 * precisely the quantity this whole exercise is about.
 *
 * So the integral carries the frontend's steady cadence error, and the
 * proportional term handles what moves. The integral only accumulates outside
 * the deadband, which both limits windup and lets a frontend that does not
 * drift sit at exactly one for ever.
 */
int32_t iptv_drift_next_ratio(int64_t queued_ms, int64_t target_ms);

/* The ratio in use, in Q16. For the log, and for tests. */
int32_t iptv_drift_ratio(void);

/*
 * How many frames must be available in the ring before a block of `frames` can
 * be produced at this ratio. Includes the neighbour that interpolation reads.
 */
size_t iptv_drift_frames_needed(size_t frames, int32_t ratio);

/*
 * Reads `frames` stereo frames into `out`, resampling from the ring at `ratio`.
 *
 * `ring_len` and `read_pos` are in int16 samples, not frames, matching the
 * core's ring. The fractional position is carried between calls inside this
 * module. Returns the number of whole frames consumed, which is what the caller
 * advances the read position by.
 */
size_t iptv_drift_read(const int16_t *ring, size_t ring_len, size_t read_pos,
                       int16_t *out, size_t frames, int32_t ratio);

#endif
