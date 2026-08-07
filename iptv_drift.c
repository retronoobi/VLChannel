#include "iptv_drift.h"

#include <stdbool.h>

/*
 * Deadband, in milliseconds of queue error.
 *
 * The queue is measured in blocks and always shows a small sawtooth around
 * wherever it sits - the RetroArch log wanders between 1181 and 1261 ms while
 * drifting +0.0 ms/s. Sixty milliseconds is wider than that sawtooth and far
 * narrower than the hundreds of milliseconds that are audible.
 */
#define DEADBAND_MS 60

/* Queue error that earns the full half percent from the proportional term. */
#define FULL_SCALE_MS 400

/*
 * How much slower the integral learns inside the deadband.
 *
 * It used to not learn there at all, so that a frontend without drift could sit
 * on exactly one. Two sessions of fifteen channels killed that idea twice over.
 * Under EmuVR, where there is real drift, freezing the integral inside the band
 * meant the queue fell out of the bottom, got pushed back in, froze, fell out
 * again - a slow cycle that parked the queue 65 ms below target on every good
 * channel and read as a residual +0.5 ms/s. And under RetroArch the ratio never
 * rested on exactly one anyway: it measured between +15 and -1037 ppm.
 *
 * So the freeze bought nothing and cost a standing offset. Inside the band the
 * integral now still learns, eight times slower, which is enough to close the
 * last few tens of milliseconds without chasing the sawtooth.
 */
#define DEADBAND_SLOWDOWN 8

/*
 * How fast the integral learns. The plant is extremely slow: half a percent of
 * ratio moves the queue by five milliseconds per second, so pulling the queue
 * back by 280 ms takes the better part of a minute no matter what this is set
 * to. The divisor is chosen so the integral takes roughly half a minute to
 * traverse its whole range, which is fast next to a drift measured in minutes
 * and slow next to anything that could ring.
 */
#define INTEGRAL_DIVISOR 1500

/* How much the ratio may move per block, so a correction is a slide in pitch
 * and never a step. */
#define RATIO_SLEW 4

static int32_t current_ratio = IPTV_DRIFT_ONE;
static int64_t integral_accumulator = 0;
static uint32_t phase = 0;

static int32_t clamp32(int32_t value, int32_t low, int32_t high) {
    if (value < low)  return low;
    if (value > high) return high;
    return value;
}

static int64_t clamp64(int64_t value, int64_t low, int64_t high) {
    if (value < low)  return low;
    if (value > high) return high;
    return value;
}

void iptv_drift_flush_ring(void) {
    /*
     * Only the phase. current_ratio and the integral describe the frontend and
     * outlive any one channel - see the header.
     */
    phase = 0;
}

void iptv_drift_reset(void) {
    current_ratio = IPTV_DRIFT_ONE;
    integral_accumulator = 0;
    phase = 0;
}

int32_t iptv_drift_ratio(void) {
    return current_ratio;
}

int32_t iptv_drift_next_ratio(int64_t queued_ms, int64_t target_ms) {
    int64_t error = queued_ms - target_ms;
    int32_t proportional = 0;

    bool outside = (error <= -DEADBAND_MS || error >= DEADBAND_MS);

    if (outside) {
        /*
         * Queue longer than wanted -> read faster than we write, so the ratio
         * goes above one and the surplus is spent. Shorter -> below one.
         *
         * The proportional term stays out here. Inside the band it would only
         * be reacting to the sawtooth of the queue filling and emptying, which
         * is noise and not error.
         */
        int64_t term = (error * IPTV_DRIFT_MAX_STEP) / FULL_SCALE_MS;
        proportional = clamp32((int32_t)clamp64(term, -IPTV_DRIFT_MAX_STEP,
                                                IPTV_DRIFT_MAX_STEP),
                               -IPTV_DRIFT_MAX_STEP, IPTV_DRIFT_MAX_STEP);
    }

    /* The integral learns everywhere, slower inside the band. */
    integral_accumulator += outside ? error : (error / DEADBAND_SLOWDOWN);
    integral_accumulator = clamp64(integral_accumulator,
                                   -(int64_t)IPTV_DRIFT_MAX_STEP * INTEGRAL_DIVISOR,
                                   (int64_t)IPTV_DRIFT_MAX_STEP * INTEGRAL_DIVISOR);

    int32_t integral = (int32_t)(integral_accumulator / INTEGRAL_DIVISOR);
    int32_t wanted = IPTV_DRIFT_ONE +
                     clamp32(proportional + integral,
                             -IPTV_DRIFT_MAX_STEP, IPTV_DRIFT_MAX_STEP);

    if (wanted > current_ratio)
        current_ratio = wanted < current_ratio + RATIO_SLEW ? wanted
                                                            : current_ratio + RATIO_SLEW;
    else if (wanted < current_ratio)
        current_ratio = wanted > current_ratio - RATIO_SLEW ? wanted
                                                            : current_ratio - RATIO_SLEW;
    return current_ratio;
}

size_t iptv_drift_frames_needed(size_t frames, int32_t ratio) {
    if (ratio <= 0)
        ratio = IPTV_DRIFT_ONE;

    /* Whole frames the phase will advance, plus the interpolation neighbour,
     * plus one for a fractional phase carried in from the previous block. */
    uint64_t advance = ((uint64_t)frames * (uint64_t)ratio) >> 16;
    return (size_t)advance + 2;
}

size_t iptv_drift_read(const int16_t *ring, size_t ring_len, size_t read_pos,
                       int16_t *out, size_t frames, int32_t ratio) {
    if (!ring || !out || frames == 0 || ring_len == 0)
        return 0;
    if (ratio <= 0)
        ratio = IPTV_DRIFT_ONE;

    uint64_t position = phase;                /* Q16, fractional part only */

    for (size_t i = 0; i < frames; i++) {
        uint64_t whole = position >> 16;
        uint32_t fraction = (uint32_t)(position & 0xFFFFu);

        size_t a = (read_pos + whole * 2) % ring_len;

        int32_t a_left  = ring[a];
        int32_t a_right = ring[(a + 1) % ring_len];

        if (fraction == 0) {
            /*
             * The identity path, spelled out rather than left to arithmetic.
             * With a ratio of one every frame lands here, and the block is the
             * same bytes the plain copy produced.
             */
            out[i * 2]     = (int16_t)a_left;
            out[i * 2 + 1] = (int16_t)a_right;
        } else {
            size_t b = (read_pos + (whole + 1) * 2) % ring_len;
            int32_t b_left  = ring[b];
            int32_t b_right = ring[(b + 1) % ring_len];

            out[i * 2] =
                (int16_t)(a_left + (((b_left - a_left) * (int32_t)fraction) >> 16));
            out[i * 2 + 1] =
                (int16_t)(a_right + (((b_right - a_right) * (int32_t)fraction) >> 16));
        }

        position += (uint32_t)ratio;
    }

    phase = (uint32_t)(position & 0xFFFFu);
    return (size_t)(position >> 16);
}
