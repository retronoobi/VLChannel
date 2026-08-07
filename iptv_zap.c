#include "iptv_zap.h"
#include "iptv_zap_data.h"

#include <stddef.h>

static int volume_percent = 0;
static unsigned next_take = 0;

/* Which take is playing and how far into it. Only ever touched from the
 * libretro thread: triggered where the channel changes, consumed where the
 * block is handed over, and both of those are retro_run. */
static const int16_t *playing = NULL;
static unsigned playing_length = 0;
static unsigned playing_position = 0;

void iptv_zap_set_volume(int percent) {
    if (percent < 0)   percent = 0;
    if (percent > 200) percent = 200;
    volume_percent = percent;
    if (percent == 0)
        iptv_zap_reset();
}

int iptv_zap_volume(void) {
    return volume_percent;
}

void iptv_zap_trigger(void) {
    if (volume_percent <= 0)
        return;

    playing = iptv_zap_takes[next_take];
    playing_length = iptv_zap_lengths[next_take];
    playing_position = 0;
    next_take = (next_take + 1) % IPTV_ZAP_TAKES;
}

bool iptv_zap_active(void) {
    return playing != NULL && playing_position < playing_length;
}

void iptv_zap_reset(void) {
    playing = NULL;
    playing_length = 0;
    playing_position = 0;
}

static int16_t saturate(int32_t value) {
    if (value >  32767) return  32767;
    if (value < -32768) return -32768;
    return (int16_t)value;
}

void iptv_zap_mix(int16_t *stereo, unsigned frames) {
    if (!iptv_zap_active() || !stereo || frames == 0)
        return;

    unsigned remaining = playing_length - playing_position;
    unsigned count = frames < remaining ? frames : remaining;

    for (unsigned i = 0; i < count; i++) {
        /*
         * The takes are mono. The same sample goes to both channels, which is
         * what the recording already was - the two channels in the file are
         * identical, so splitting them would have invented a difference that
         * was never there.
         */
        int32_t sample =
            ((int32_t)playing[playing_position + i] * volume_percent) / 100;

        stereo[i * 2]     = saturate(stereo[i * 2] + sample);
        stereo[i * 2 + 1] = saturate(stereo[i * 2 + 1] + sample);
    }

    playing_position += count;
    if (playing_position >= playing_length)
        iptv_zap_reset();
}
