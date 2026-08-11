#ifndef IPTV_OSD_H
#define IPTV_OSD_H

#include <stdbool.h>
#include <stdint.h>

#include "iptv_playlist.h"

/*
 * On-screen display: the channel banner and the full guide.
 *
 * Everything is drawn by hand into the XRGB8888 frame the core is about to
 * hand the frontend. There is no toolkit inside a libretro core and no font
 * either, so the primitives here are rectangles, alpha blending and a bitmap
 * font scaled by a whole number - which is what a set-top box menu looked like
 * anyway.
 *
 * The module owns no video state and never touches audio. It is told when a
 * channel changed, ticked once per frame, given input while the guide is open,
 * and asked to draw. Keeping it that way is deliberate: the audio path in this
 * core has been broken twice by changes that had nothing to do with audio.
 */

typedef enum {
    IPTV_OSD_UP,
    IPTV_OSD_DOWN,
    IPTV_OSD_LEFT,
    IPTV_OSD_RIGHT,
    IPTV_OSD_CONFIRM,
    IPTV_OSD_CANCEL
} iptv_osd_key;

/* Automatic channel-change overlay. Full-screen guide and schedule remain
 * available in both styles; loading has its own independent switch. */
typedef enum {
    IPTV_OSD_STYLE_TV = 0,       /* yellow channel number, upper right */
    IPTV_OSD_STYLE_CABLE_TV,     /* full receiver-style banner */
    IPTV_OSD_STYLE_CABLE_TV_PIP  /* guide with current picture and details */
} iptv_osd_style;

void iptv_osd_set_style(iptv_osd_style style);

/*
 * What the banner's second line carries when the channel has a programme
 * listing. With no listing - no tvg-id, no .epg file, or a channel the grid
 * does not cover - it falls back to the category in every case, which is what
 * it did before listings existed.
 */
typedef enum {
    /* "Imortais - A seguir 18:32 Encontro Explosivo" */
    IPTV_BANNER_SCHEDULE = 0,
    /* The same with the synopsis in the middle. Longer, so it spends more time
     * scrolling; worth it on a film channel, less so on news. */
    IPTV_BANNER_SCHEDULE_DESC
} iptv_banner_info;

void iptv_osd_set_banner_info(iptv_banner_info info);

/*
 * A channel is opening and there is nothing to show yet.
 *
 * The core knows this and the overlay does not, so it is told rather than
 * guessing from a frame that has not arrived. While it is set, the overlay
 * draws a turning ring and the word Loading over whatever is behind it.
 */
void iptv_osd_set_waiting(bool waiting);

void iptv_osd_channel_changed(void);          /* shows the banner */

/*
 * Shows the banner, or hides it if it is already up. Bound to the d-pad down
 * key outside the guide. While it is held open this way the banner does not
 * time out.
 */
void iptv_osd_toggle_banner(void);
bool iptv_osd_banner_visible(void);
void iptv_osd_tick(void);                     /* once per retro_run */

/*
 * True while any full-screen list is up - the guide or the schedule. The core
 * asks this to decide whether the pad belongs to the menu, and that answer is
 * the same for both, so it stayed one question with its original name.
 */
bool iptv_osd_guide_open(void);
void iptv_osd_open_guide(const iptv_playlist *playlist, int current_channel);
void iptv_osd_close_guide(void);           /* closes whichever is open */

/*
 * The schedule: every channel on one list, with what is on now and what
 * follows it beside each. Needs a programme listing to be interesting, but
 * opens without one - the rows fall back to the category and the header says
 * there is no listing, which is more use than a button that appears broken.
 */
bool iptv_osd_schedule_open(void);
void iptv_osd_open_schedule(const iptv_playlist *playlist, int current_channel);

/*
 * Handles one key while the guide is open. Returns the channel index to tune,
 * or -1 when the guide only moved its cursor.
 */
int iptv_osd_key_pressed(iptv_osd_key key, const iptv_playlist *playlist);

/* True when the next frame has to be composited rather than passed through. */
bool iptv_osd_visible(void);

void iptv_osd_draw(
    uint32_t *pixels,
    const uint32_t *video_pixels,
    unsigned width,
    unsigned height,
    unsigned pitch_bytes,
    const iptv_playlist *playlist,
    int current_channel
);

/* Replaces the picture with a broadcast-style test pattern and a localized
 * NO SIGNAL message. It is independent of the TV/Cable TV overlay style. */
void iptv_osd_draw_no_signal(
    uint32_t *pixels,
    unsigned width,
    unsigned height,
    unsigned pitch_bytes
);

#endif
