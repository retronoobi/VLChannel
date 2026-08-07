#include "iptv_osd.h"
#include "iptv_font.h"
#include "iptv_log.h"
#include "iptv_text.h"
#include "iptv_epg.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* --------------------------------------------------------------- colours */

/*
 * The palette of a satellite receiver menu, because that is what this is
 * pretending to be. Blue panel, amber for the selected row, white text with a
 * dark shadow so it survives on top of a bright picture.
 */
#define COLOUR_PANEL      0x0A1E5Au
#define COLOUR_PANEL_EDGE 0x1E46A0u
#define COLOUR_HEADER     0x123A8Cu
#define COLOUR_ROW        0x14307Au
#define COLOUR_ROW_ALT    0x0E2464u
#define COLOUR_SELECTED   0xF0C020u
#define COLOUR_TV_NUMBER  0x11DB13u
#define COLOUR_TEXT       0xFFFFFFu
#define COLOUR_TEXT_DARK  0x08122Eu
#define COLOUR_DIM        0x000000u

#define BANNER_FRAMES 300              /* five seconds at the fixed 60 Hz */
#define MAX_GROUPS 128

/*
 * How far left and right jump in the schedule. A fixed number and not the count
 * of visible rows: the layout only settles at draw time, while the keys are
 * handled before anything is drawn, so a page tied to the geometry would change
 * size whenever a channel arrives at a different resolution.
 */
#define SCHEDULE_PAGE 10

/* ----------------------------------------------------------------- state */

/*
 * Which full-screen list is up, if any.
 *
 * One state and not two booleans. Two booleans can both be true, and "the guide
 * and the schedule are both open" is not a screen this code can draw - it is a
 * state that would exist only to be forgotten about by the next person to add a
 * third list. The compiler cannot check a rule like "these two are never both
 * set"; it can check that a variable holds one of three values.
 */
typedef enum {
    MENU_NONE = 0,
    MENU_GUIDE,          /* by category, then channel */
    MENU_SCHEDULE        /* by channel, with what is on */
} menu_screen;

static menu_screen menu = MENU_NONE;
static int banner_countdown = 0;
static iptv_osd_style osd_style = IPTV_OSD_STYLE_TV;

/*
 * The banner is held open while a label is still walking.
 *
 * Five seconds was chosen when everything fit; a long category needs longer
 * than that to be read once, and a banner that vanishes mid-word shows the
 * viewer a fragment and then takes it away. The drawing code sets this every
 * frame it still has something to finish, and the countdown simply refuses to
 * expire while it is set.
 */
static bool banner_text_busy = false;
static bool banner_text_was_busy = false;

/* Held open by the viewer instead of by a countdown - see the d-pad below. */
static bool banner_pinned = false;

/* Focus: 0 on the group column, 1 on the channel column. Only the guide has
 * two columns; the schedule is one list and leaves this alone. */
static int focus = 1;
static int group_row = 0;
static int group_scroll = 0;
static int channel_row = 0;
static int channel_scroll = 0;

/* The schedule's own cursor, kept apart from the guide's so that leaving one
 * list and coming back does not move the other. */
static int sched_row = 0;
static int sched_scroll = 0;

/*
 * Groups, resolved once when the guide opens.
 *
 * Doing this per frame would be O(channels) sixty times a second, which is
 * nothing on a list of eleven and unacceptable on a list of twenty thousand.
 */
static struct {
    const char *name;
    size_t count;
} groups[MAX_GROUPS];
static size_t group_count = 0;

/* ------------------------------------------------------------- utilities */

static uint32_t blend(uint32_t dst, uint32_t src, unsigned alpha) {
    if (alpha >= 255)
        return src;

    unsigned inverse = 255 - alpha;
    unsigned r = (((src >> 16) & 0xFF) * alpha + ((dst >> 16) & 0xFF) * inverse) / 255;
    unsigned g = (((src >>  8) & 0xFF) * alpha + ((dst >>  8) & 0xFF) * inverse) / 255;
    unsigned b = (((src      ) & 0xFF) * alpha + ((dst      ) & 0xFF) * inverse) / 255;
    return (r << 16) | (g << 8) | b;
}

typedef struct {
    uint32_t *pixels;
    unsigned width;
    unsigned height;
    unsigned stride;          /* in pixels, not bytes */
} surface;

static void fill_rect(
    const surface *s, int x, int y, int w, int h, uint32_t colour, unsigned alpha
) {
    if (w <= 0 || h <= 0 || alpha == 0)
        return;

    /* Clipped rather than trusted: the frame size changes with the channel,
     * from 640x386 to 1902x1080 on one ordinary list. */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)s->width)  w = (int)s->width - x;
    if (y + h > (int)s->height) h = (int)s->height - y;
    if (w <= 0 || h <= 0)
        return;

    for (int row = 0; row < h; row++) {
        uint32_t *line = s->pixels + (size_t)(y + row) * s->stride + x;
        if (alpha >= 255) {
            for (int col = 0; col < w; col++)
                line[col] = colour;
        } else {
            for (int col = 0; col < w; col++)
                line[col] = blend(line[col], colour, alpha);
        }
    }
}

/*
 * UTF-8 in, one drawable byte out.
 *
 * The font has eight rows and no space for accents, so an accented letter is
 * drawn as its base letter: "Educacao" is a better answer than a box, and far
 * better than the two mojibake characters a raw byte walk would produce.
 */
static const char *decode_char(const char *text, unsigned char *out) {
    unsigned char c = (unsigned char)text[0];

    if (c < 0x80) {
        *out = c;
        return text + 1;
    }

    unsigned codepoint = 0;
    int extra = 0;
    if ((c & 0xE0) == 0xC0) { codepoint = c & 0x1F; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { codepoint = c & 0x0F; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { codepoint = c & 0x07; extra = 3; }
    else { *out = '?'; return text + 1; }

    for (int i = 1; i <= extra; i++) {
        if ((text[i] & 0xC0) != 0x80) { *out = '?'; return text + 1; }
        codepoint = (codepoint << 6) | (text[i] & 0x3F);
    }

    static const struct { unsigned code; char base; } folded[] = {
        { 0xC0, 'A' }, { 0xC1, 'A' }, { 0xC2, 'A' }, { 0xC3, 'A' }, { 0xC4, 'A' },
        { 0xC7, 'C' }, { 0xC8, 'E' }, { 0xC9, 'E' }, { 0xCA, 'E' }, { 0xCB, 'E' },
        { 0xCC, 'I' }, { 0xCD, 'I' }, { 0xCE, 'I' }, { 0xCF, 'I' }, { 0xD1, 'N' },
        { 0xD2, 'O' }, { 0xD3, 'O' }, { 0xD4, 'O' }, { 0xD5, 'O' }, { 0xD6, 'O' },
        { 0xD9, 'U' }, { 0xDA, 'U' }, { 0xDB, 'U' }, { 0xDC, 'U' },
        { 0xE0, 'a' }, { 0xE1, 'a' }, { 0xE2, 'a' }, { 0xE3, 'a' }, { 0xE4, 'a' },
        { 0xE7, 'c' }, { 0xE8, 'e' }, { 0xE9, 'e' }, { 0xEA, 'e' }, { 0xEB, 'e' },
        { 0xEC, 'i' }, { 0xED, 'i' }, { 0xEE, 'i' }, { 0xEF, 'i' }, { 0xF1, 'n' },
        { 0xF2, 'o' }, { 0xF3, 'o' }, { 0xF4, 'o' }, { 0xF5, 'o' }, { 0xF6, 'o' },
        { 0xF9, 'u' }, { 0xFA, 'u' }, { 0xFB, 'u' }, { 0xFC, 'u' },
        /*
         * Punctuation, added when programme synopses started arriving. Titles
         * are short and typed plainly; a synopsis is prose off a broadcaster's
         * feed, and it is full of curly quotes and en dashes. Without these
         * every one of them drew a question mark, so a sentence came out as
         * "Ela nao sabe ? mas o pai dela ? sim ?" - which reads as a decoding
         * fault in the core rather than as a typographic character it cannot
         * draw. The ellipsis becomes one dot for the same reason the accents
         * become bare letters: one glyph out, and the wrong-looking answer is
         * still the readable one.
         */
        { 0x2018, '\'' }, { 0x2019, '\'' }, { 0x201A, '\'' },
        { 0x201C, '"' },  { 0x201D, '"' },  { 0x201E, '"' },
        { 0x2013, '-' },  { 0x2014, '-' },  { 0x2015, '-' },
        { 0x2026, '.' },  { 0x00A0, ' ' },  { 0x00B7, '-' },
        { 0x2022, '-' },  { 0x00AB, '"' },  { 0x00BB, '"' },
    };
    for (size_t i = 0; i < sizeof(folded) / sizeof(*folded); i++)
        if (folded[i].code == codepoint) {
            *out = (unsigned char)folded[i].base;
            return text + 1 + extra;
        }

    *out = '?';
    return text + 1 + extra;
}

/*
 * Advance, not cell width.
 *
 * Every glyph in the font leaves column zero empty, so stepping seven pixels
 * instead of eight lets each glyph's last column sit where the next one's blank
 * first column would be. Nothing collides and the text stops looking spaced
 * out, which at three or four times scale is the difference between a menu and
 * a ransom note.
 */
#define FONT_ADVANCE 7

static int text_width(const char *text, int scale) {
    int cells = 0;
    unsigned char c;
    while (*text) {
        text = decode_char(text, &c);
        cells++;
    }
    return cells * FONT_ADVANCE * scale;
}

static void draw_text(
    const surface *s, int x, int y, const char *text, int scale,
    uint32_t colour, int max_width
) {
    int start_x = x;
    unsigned char c;

    while (*text) {
        text = decode_char(text, &c);
        if (max_width > 0 && x - start_x + FONT_ADVANCE * scale > max_width)
            return;                                   /* clipped, not wrapped */
        if (c < IPTV_FONT_FIRST || c > IPTV_FONT_LAST)
            c = '?';

        const uint8_t *glyph = iptv_font8x8[c - IPTV_FONT_FIRST];
        for (int row = 0; row < IPTV_FONT_HEIGHT; row++) {
            uint8_t bits = glyph[row];
            if (!bits)
                continue;
            for (int col = 0; col < IPTV_FONT_WIDTH; col++) {
                if (!((bits >> (7 - col)) & 1))
                    continue;
                fill_rect(s, x + col * scale, y + row * scale,
                          scale, scale, colour, 255);
            }
        }
        x += FONT_ADVANCE * scale;
    }
}

static void draw_text_shadowed(
    const surface *s, int x, int y, const char *text, int scale,
    uint32_t colour, int max_width
);

/*
 * The same drawing, but clipped to a box on both sides.
 *
 * draw_text() above stops when it runs past max_width, which is all a static
 * label needs. Scrolling text has to be able to start at a negative offset, and
 * then the glyphs on the left would spill outside the box - fill_rect only
 * clips against the picture, not against the row. So this one clamps every
 * pixel block to the box, and skips whole glyphs that fall outside it.
 */
static void draw_text_boxed(
    const surface *s, int box_x, int box_w, int x, int y, const char *text,
    int scale, uint32_t colour
) {
    unsigned char c;
    int advance = FONT_ADVANCE * scale;

    while (*text) {
        text = decode_char(text, &c);

        if (x + advance <= box_x) { x += advance; continue; }   /* left of box */
        if (x >= box_x + box_w)                                  /* right of box */
            return;

        if (c < IPTV_FONT_FIRST || c > IPTV_FONT_LAST)
            c = '?';

        const uint8_t *glyph = iptv_font8x8[c - IPTV_FONT_FIRST];
        for (int row = 0; row < IPTV_FONT_HEIGHT; row++) {
            uint8_t bits = glyph[row];
            if (!bits)
                continue;
            for (int col = 0; col < IPTV_FONT_WIDTH; col++) {
                if (!((bits >> (7 - col)) & 1))
                    continue;
                int px = x + col * scale;
                int pw = scale;
                if (px < box_x) { pw -= box_x - px; px = box_x; }
                if (px + pw > box_x + box_w) pw = box_x + box_w - px;
                if (pw > 0)
                    fill_rect(s, px, y + row * scale, pw, scale, colour, 255);
            }
        }
        x += advance;
    }
}

/*
 * Text that walks sideways when it does not fit.
 *
 * Four of these can be on screen at once - the name and the group on the
 * banner, and the selected row of each column in the guide - and each needs its
 * own position, so they are slots rather than one shared state.
 *
 * It walks left, waits, and then jumps back to the beginning rather than
 * walking backwards. Reading text that slides the wrong way is work, and the
 * useful resting state is the start - a channel name is identified by its
 * beginning far more than by its middle.
 *
 * Nothing here limits how long the text may be. The change detection is a hash
 * rather than a copy, because this overlay is meant to carry more than a
 * category eventually - programme information, if that ever arrives - and a
 * scroller with a buffer in it would quietly become the new ceiling, the way
 * the parser's 48 byte group already did once.
 */
typedef enum {
    SCROLL_BANNER_NAME,
    SCROLL_BANNER_GROUP,
    SCROLL_GUIDE_GROUP,
    SCROLL_GUIDE_CHANNEL,
    /* The schedule's highlighted row: the channel on the left, what is on to
     * the right of it. Two slots because they are two boxes on one line and
     * each walks its own text at its own length. */
    SCROLL_SCHED_CHANNEL,
    SCROLL_SCHED_PROGRAMME,
    SCROLL_SLOTS
} scroll_slot;

#define SCROLL_HOLD_FRAMES 90     /* a second and a half at each end */
#define SCROLL_SPEED_Q4    6      /* 0.375 px per frame per unit of scale */

static struct {
    uint32_t seen;        /* hash of the text this position belongs to */
    int  offset_q4;       /* how far left it has walked, in 1/16 px */
    int  hold;            /* frames still to wait before moving again */
    bool finished;        /* reached the end and waiting to snap back */
    bool shown_once;      /* the whole text has been past the box at least once */
} scroller[SCROLL_SLOTS];

/* FNV-1a, and any hash would do: this only has to notice that the text is not
 * the one the position belongs to. A collision costs a label that keeps its
 * scroll position across a change, not a wrong string on screen. */
static uint32_t text_hash(const char *text) {
    uint32_t h = 2166136261u;
    while (*text) {
        h ^= (unsigned char)*text++;
        h *= 16777619u;
    }
    return h;
}

static void reset_scroll(void) {
    memset(scroller, 0, sizeof(scroller));
}

/*
 * Draws `text` inside the box, scrolling it if it does not fit, and advances
 * that slot by one frame. Called at most once per slot per frame, which is what
 * makes "advance here" the same thing as "advance once per frame".
 */
/*
 * True only until the text has been shown end to end once.
 *
 * The banner waits on this, and the distinction matters: the label keeps
 * cycling for as long as it is on screen, so "is it moving" is true forever and
 * a banner that waited on that would never leave. What the viewer needs is to
 * see the whole thing once - after that the countdown is free to run and the
 * label finishes its cycle on the way out.
 */
static bool scroll_slot_busy(scroll_slot slot) {
    return !scroller[slot].shown_once;
}

static void draw_text_scrolling(
    const surface *s, scroll_slot slot, int box_x, int box_w, int y,
    const char *text, int scale, uint32_t colour, bool shadowed
) {
    int full = text_width(text, scale);

    if (full <= box_w) {
        /* Fits: nothing to animate, and the slot forgets where it was so the
         * next long name starts from the beginning. */
        scroller[slot].seen = 0;
        scroller[slot].offset_q4 = 0;
        scroller[slot].hold = 0;
        scroller[slot].finished = true;      /* nothing to wait for */
        scroller[slot].shown_once = true;    /* it is all visible already */
        if (shadowed)
            draw_text_shadowed(s, box_x, y, text, scale, colour, box_w);
        else
            draw_text(s, box_x, y, text, scale, colour, box_w);
        return;
    }

    uint32_t hash = text_hash(text);
    if (scroller[slot].seen != hash) {
        scroller[slot].seen = hash;
        scroller[slot].offset_q4 = 0;
        scroller[slot].hold = SCROLL_HOLD_FRAMES;
        scroller[slot].finished = false;
        scroller[slot].shown_once = false;
    }

    int limit_q4 = (full - box_w) * 16;

    if (scroller[slot].hold > 0) {
        scroller[slot].hold--;
        if (scroller[slot].hold == 0 && scroller[slot].finished) {
            /* Back to the beginning in one step, not by walking backwards. */
            scroller[slot].offset_q4 = 0;
            scroller[slot].finished = false;
            scroller[slot].hold = SCROLL_HOLD_FRAMES;
        }
    } else if (!scroller[slot].finished) {
        scroller[slot].offset_q4 += SCROLL_SPEED_Q4 * scale;
        if (scroller[slot].offset_q4 >= limit_q4) {
            scroller[slot].offset_q4 = limit_q4;
            scroller[slot].finished = true;
            scroller[slot].shown_once = true;
            scroller[slot].hold = SCROLL_HOLD_FRAMES;
        }
    }

    int x = box_x - scroller[slot].offset_q4 / 16;
    if (shadowed)
        draw_text_boxed(s, box_x, box_w, x + scale, y + scale, text, scale,
                        COLOUR_TEXT_DARK);
    draw_text_boxed(s, box_x, box_w, x, y, text, scale, colour);
}

/* A one pixel dark offset. Cheap, and it is the difference between readable
 * and not when the picture behind is bright. */
static void draw_text_shadowed(
    const surface *s, int x, int y, const char *text, int scale,
    uint32_t colour, int max_width
) {
    draw_text(s, x + scale, y + scale, text, scale, COLOUR_TEXT_DARK, max_width);
    draw_text(s, x, y, text, scale, colour, max_width);
}

/*
 * A starting scale derived from the picture, to be reduced until it fits.
 *
 * The floor is one, not two. A retro channel arrives at 352x288 and gets
 * stretched across the whole virtual television anyway, so eight pixels of font
 * in a small picture ends up the same apparent size as forty in a large one -
 * what matters is the proportion, not the pixel count.
 *
 * What matters just as much is *which* proportion, and the first version got it
 * wrong in a way that only showed on a widescreen channel. It divided the height
 * by 200 and truncated, so every picture from 200 to 399 lines tall drew at
 * scale one. A 352x288 channel and a 640x386 one - both real, both in the same
 * list - therefore used the same eight pixel font, but the second is 1.8 times
 * wider, so the text covered half as much of the screen. On a monitor that is
 * merely small; on the virtual television in EmuVR it is unreadable.
 *
 * So the scale is rounded to the nearest whole multiple rather than truncated,
 * and rounded *geometrically*: the step from k to k+1 happens at the geometric
 * mean sqrt(k*(k+1)), not at the arithmetic midpoint. Text size is a ratio, and
 * halfway between one and two font sizes is 1.41, not 1.5. Written as a squared
 * comparison to keep it in integers:
 *
 *     height >= TARGET_ROW * sqrt(k * (k - 1))   <=>   height^2 >= TARGET_ROW^2 * k * (k - 1)
 *
 * TARGET_ROW is the picture height a single scale step should be worth, taken
 * from the sizes that were checked by eye and looked right: 352x288 at scale 1,
 * 640x480 at 2, 1280x720 at 3. Those all put a row of text at about one
 * twenty-second of the picture height, which is 242 pixels per scale step.
 *
 * The result: 288 -> 1, 386 -> 2, 480 -> 2, 720 -> 3, 1080 -> 4. Only the 386
 * case changes from before, which is the one that was wrong.
 */
#define GUIDE_TARGET_ROW 242u

static int scale_for(unsigned height) {
    int scale = 1;
    for (int k = 2; k <= 6; k++) {
        unsigned threshold = GUIDE_TARGET_ROW * GUIDE_TARGET_ROW *
                             (unsigned)(k * (k - 1));
        if ((unsigned long long)height * height < threshold)
            break;
        scale = k;
    }
    return scale;
}

/* ------------------------------------------------------------- the groups */

static void collect_groups(const iptv_playlist *playlist) {
    group_count = 0;

    for (size_t i = 0; i < playlist->count; i++) {
        const char *name = playlist->channels[i].group;
        if (!name[0])
            name = iptv_text(IPTV_TEXT_NO_GROUP);

        size_t found = group_count;
        for (size_t j = 0; j < group_count; j++)
            if (strcmp(groups[j].name, name) == 0) { found = j; break; }

        if (found == group_count) {
            if (group_count >= MAX_GROUPS)
                continue;
            groups[group_count].name = name;
            groups[group_count].count = 0;
            group_count++;
        }
        groups[found].count++;
    }
}

static const char *group_of(const iptv_channel *channel) {
    return channel->group[0] ? channel->group
                             : iptv_text(IPTV_TEXT_NO_GROUP);
}

/* ------------------------------------------------------- the loading ring
 *
 * Twelve blocks on a circle, one bright and the rest fading behind it, turning
 * once every second and a bit. It is the shape every console has used for this,
 * and it is drawn out of the only primitive this overlay has - a filled
 * rectangle - because adding a circle rasteriser to draw twelve squares would
 * be a strange way to spend an afternoon.
 *
 * The positions are a fixed table rather than sin and cos at draw time. Twelve
 * angles never change, the table is exact where floating point would have to be
 * rounded to whole pixels anyway, and it keeps <math.h> out of a file that
 * otherwise needs nothing.
 *
 * Values are a unit circle scaled by 1024: x = round(1024*sin(k*30 degrees)),
 * y = round(-1024*cos(...)), starting at the top and going clockwise.
 */
#define SPIN_BLOCKS 12
static const int spin_x[SPIN_BLOCKS] = {
       0,  512,  887, 1024,  887,  512,
       0, -512, -887,-1024, -887, -512
};
static const int spin_y[SPIN_BLOCKS] = {
   -1024, -887, -512,    0,  512,  887,
    1024,  887,  512,    0, -512, -887
};

/* Frames per step. At 60 Hz, twelve steps of five frames is one turn a second -
 * fast enough to read as motion, slow enough not to strobe. */
#define SPIN_FRAMES_PER_STEP 5

static bool waiting = false;
static unsigned spin_tick = 0;

void iptv_osd_set_waiting(bool now) {
    if (now && !waiting)
        spin_tick = 0;            /* every wait starts at the top */
    waiting = now;
}

static void draw_waiting(const surface *s) {
    int scale = scale_for(s->height);
    const char *label = iptv_text(IPTV_TEXT_LOADING);

    int block = 3 * scale;                 /* one block of the ring */
    int radius = 14 * scale;
    int line = IPTV_FONT_HEIGHT * scale;
    int gap = 6 * scale;

    /*
     * The ring and the label are centred as one object, not each on its own.
     * Centring them separately puts the ring in the middle of the screen and
     * the text in the middle of the screen, which means the pair sits off to
     * one side by half the text.
     */
    int text_w = text_width(label, scale);
    int total = radius * 2 + gap + text_w;
    int left = ((int)s->width - total) / 2;
    int centre_x = left + radius;
    int centre_y = (int)s->height / 2;

    unsigned step = (spin_tick / SPIN_FRAMES_PER_STEP) % SPIN_BLOCKS;

    for (int i = 0; i < SPIN_BLOCKS; i++) {
        int x = centre_x + spin_x[i] * radius / 1024 - block / 2;
        int y = centre_y + spin_y[i] * radius / 1024 - block / 2;

        /*
         * How far behind the leading block this one is. The head is opaque and
         * the tail nearly gone, which is what makes a ring of identical squares
         * read as turning in a particular direction rather than just
         * flickering.
         *
         * The first version of this faded from 255 to 79 and the direction was
         * unreadable in a still frame - an integer division of 200 by twelve had
         * quietly eaten most of the range. Spelling the two ends out and
         * interpolating between them is both wider and harder to get wrong.
         */
        int behind = ((int)step - i + SPIN_BLOCKS) % SPIN_BLOCKS;
        const unsigned head = 255, tail = 35;
        unsigned alpha = tail + (head - tail) *
                         (unsigned)(SPIN_BLOCKS - 1 - behind) /
                         (SPIN_BLOCKS - 1);

        /* The leading block is also a shade larger. On a small picture the
         * alpha alone is a couple of pixels of difference; the size is what
         * still reads at 640x360. */
        int size = (behind == 0) ? block + 2 * scale : block;
        int nudge = (size - block) / 2;

        fill_rect(s, x - nudge, y - nudge, size, size, COLOUR_SELECTED, alpha);
    }

    draw_text_shadowed(s, centre_x + radius + gap, centre_y - line / 2,
                       label, scale, COLOUR_TEXT, 0);
}

void iptv_osd_draw_no_signal(
    uint32_t *pixels, unsigned width, unsigned height, unsigned pitch_bytes
) {
    if (!pixels || width == 0 || height == 0 || pitch_bytes < width * 4)
        return;

    surface s = { pixels, width, height, pitch_bytes / 4 };
    static const uint32_t upper_bars[] = {
        0xE8E8E8u, 0xFFFF00u, 0x00E5E5u, 0x00D800u,
        0xE500E5u, 0xE00000u, 0x0000D8u,
    };
    static const uint32_t middle_bars[] = {
        0x181818u, 0x383838u, 0x585858u, 0x787878u,
        0x989898u, 0xB8B8B8u, 0xD8D8D8u, 0xF4F4F4u,
    };
    static const uint32_t lower_bars[] = {
        0x0000D8u, 0x181818u, 0xE500E5u, 0x181818u,
        0x00E5E5u, 0x181818u, 0xE8E8E8u,
    };
    static const uint32_t bottom_bars[] = {
        0xF4F4F4u, 0xD0D0D0u, 0xA8A8A8u, 0x808080u,
        0x606060u, 0x383838u, 0x181818u,
    };

    fill_rect(&s, 0, 0, (int)width, (int)height, 0x000000u, 255);

    int upper_h = (int)(height * 67u / 100u);
    int middle_h = (int)(height * 8u / 100u);
    int lower_h = (int)(height * 12u / 100u);
    int bottom_y = upper_h + middle_h + lower_h;

    for (int i = 0; i < 7; i++) {
        int x0 = (int)((uint64_t)width * (unsigned)i / 7u);
        int x1 = (int)((uint64_t)width * (unsigned)(i + 1) / 7u);
        fill_rect(&s, x0, 0, x1 - x0, upper_h, upper_bars[i], 255);
    }
    for (int i = 0; i < 8; i++) {
        int x0 = (int)((uint64_t)width * (unsigned)i / 8u);
        int x1 = (int)((uint64_t)width * (unsigned)(i + 1) / 8u);
        fill_rect(&s, x0, upper_h, x1 - x0, middle_h,
                  middle_bars[i], 255);
    }
    for (int i = 0; i < 7; i++) {
        int x0 = (int)((uint64_t)width * (unsigned)i / 7u);
        int x1 = (int)((uint64_t)width * (unsigned)(i + 1) / 7u);
        fill_rect(&s, x0, upper_h + middle_h, x1 - x0, lower_h,
                  lower_bars[i], 255);
        fill_rect(&s, x0, bottom_y, x1 - x0, (int)height - bottom_y,
                  bottom_bars[i], 255);
    }

    const char *label = iptv_text(IPTV_TEXT_NO_SIGNAL);
    int scale = (int)height / 90;
    if (scale < 2) scale = 2;
    if (scale > 12) scale = 12;
    while (scale > 1 && text_width(label, scale) > (int)width * 3 / 5)
        scale--;

    int line_h = IPTV_FONT_HEIGHT * scale;
    int pad_x = 9 * scale;
    int pad_y = 4 * scale;
    int panel_w = text_width(label, scale) + pad_x * 2;
    int panel_h = line_h + pad_y * 2;
    int panel_x = ((int)width - panel_w) / 2;
    int panel_y = ((int)height - panel_h) / 2 - (int)height / 20;

    fill_rect(&s, panel_x, panel_y, panel_w, panel_h, 0x000000u, 255);
    draw_text_shadowed(&s,
                       panel_x + (panel_w - text_width(label, scale)) / 2,
                       panel_y + pad_y, label, scale, COLOUR_TEXT, 0);
}

static iptv_banner_info banner_info = IPTV_BANNER_SCHEDULE;

void iptv_osd_set_style(iptv_osd_style style) {
    osd_style = style;
    if (style == IPTV_OSD_STYLE_TV)
        menu = MENU_NONE;
    banner_text_busy = false;
    banner_text_was_busy = false;
    reset_scroll();
}

void iptv_osd_set_banner_info(iptv_banner_info info) {
    banner_info = info;
}

/* Local clock, "18:32". Whatever the listing was written in, the viewer reads
 * the time on their own wall. */
static void clock_time(long long when, char *out, size_t out_size) {
    time_t t = (time_t)when;
    struct tm parts;
#ifdef _WIN32
    struct tm *p = localtime(&t);
    if (!p) { snprintf(out, out_size, "--:--"); return; }
    parts = *p;
#else
    if (!localtime_r(&t, &parts)) { snprintf(out, out_size, "--:--"); return; }
#endif
    snprintf(out, out_size, "%02d:%02d", parts.tm_hour, parts.tm_min);
}

/*
 * The second line of the banner, assembled once per draw.
 *
 * One line, one scroller, one string - the scroller has a single position and
 * does not need to know how many pieces went into what it is walking across.
 * That is also why the length limit had to go first: a title comes from a
 * broadcaster and a synopsis from their scheduling feed, and neither was
 * written with a banner in mind.
 *
 * The separator is " - " and not a middle dot, because the font has eight rows
 * and no dot; the folding table would have drawn a question mark between every
 * two fields, which looks like a fault in the core.
 *
 * With no listing this returns the category, exactly as before listings
 * existed. That fallback is the reason a list without an .epg still says
 * something useful instead of showing an empty strip.
 */
static const char *second_line(const iptv_channel *channel, bool *ordered) {
    /* Static because it is handed to the scroller and read again next frame.
     * Unlike a URL, this has an honest ceiling: it is text meant to be read off
     * a strip on a television, and the producer caps the synopsis at 200
     * characters before it ever reaches here. */
    static char line[1024];
    *ordered = false;

    iptv_epg_programme now;
    if (!iptv_epg_now_at(channel->tvg_id, channel->epg_index, &now))
        return group_of(channel);
    *ordered = now.ordered;

    size_t at = 0;
    int written = snprintf(line, sizeof(line), "%s", now.title);
    if (written > 0)
        at = (size_t)written < sizeof(line) ? (size_t)written : sizeof(line) - 1;

    if (banner_info == IPTV_BANNER_SCHEDULE_DESC && now.desc[0]) {
        written = snprintf(line + at, sizeof(line) - at, " - %s", now.desc);
        if (written > 0 && (size_t)written < sizeof(line) - at)
            at += (size_t)written;
    }

    iptv_epg_programme next;
    if (iptv_epg_next_at(channel->tvg_id, channel->epg_index, &next)) {
        if (next.ordered) {
            snprintf(line + at, sizeof(line) - at, " - %s %s",
                     iptv_text(IPTV_TEXT_UP_NEXT), next.title);
        } else {
            char when[8];
            clock_time(next.start, when, sizeof(when));
            snprintf(line + at, sizeof(line) - at, " - %s %s %s",
                     iptv_text(IPTV_TEXT_UP_NEXT), when, next.title);
        }
    }

    return line;
}

/* Nth channel of the selected group, in list order. */
static int channel_at(const iptv_playlist *playlist, size_t group, int row) {
    if (group >= group_count)
        return -1;

    int seen = 0;
    for (size_t i = 0; i < playlist->count; i++) {
        if (strcmp(group_of(&playlist->channels[i]), groups[group].name) != 0)
            continue;
        if (seen == row)
            return (int)i;
        seen++;
    }
    return -1;
}

/* ----------------------------------------------------------------- public */

void iptv_osd_channel_changed(void) {
    banner_countdown = BANNER_FRAMES;
    reset_scroll();
}

void iptv_osd_tick(void) {
    if (waiting)
        spin_tick++;

    if (banner_pinned)
        return;

    if (banner_countdown > 0 && !banner_text_busy) {
        /*
         * The countdown was frozen while the text finished its first pass, so
         * whatever is left of it would now play out on top of the time already
         * spent reading. A second and a half after the last word arrives is a
         * banner that leaves when it is done, instead of one that lingers.
         */
        if (banner_text_was_busy && banner_countdown > SCROLL_HOLD_FRAMES)
            banner_countdown = SCROLL_HOLD_FRAMES;
        banner_countdown--;
    }
    banner_text_was_busy = banner_text_busy;
}

/*
 * The d-pad down key, outside the guide: shows the banner, and hides it again.
 *
 * Worth having on its own - a viewer who missed the name should not have to
 * change channel and come back to see it - and worth having for what comes
 * next, since this overlay is where programme information would go.
 */
void iptv_osd_toggle_banner(void) {
    if (banner_pinned || banner_countdown > 0) {
        banner_pinned = false;
        banner_countdown = 0;
        banner_text_busy = false;
    } else {
        banner_pinned = true;
        reset_scroll();
    }
}

bool iptv_osd_banner_visible(void) {
    return banner_pinned || banner_countdown > 0;
}

bool iptv_osd_guide_open(void) {
    return menu != MENU_NONE;
}

bool iptv_osd_schedule_open(void) {
    return menu == MENU_SCHEDULE;
}

void iptv_osd_open_schedule(const iptv_playlist *playlist, int current_channel) {
    if (osd_style != IPTV_OSD_STYLE_CABLE_TV ||
        !playlist || playlist->count == 0)
        return;

    menu = MENU_SCHEDULE;
    banner_countdown = 0;
    reset_scroll();

    /* Opens on the channel being watched, like the guide: a list of two hundred
     * channels that always opens at the top is a list the viewer has to travel
     * before it tells them anything. */
    sched_row = 0;
    if (current_channel >= 0 && (size_t)current_channel < playlist->count)
        sched_row = current_channel;
    sched_scroll = 0;
}

void iptv_osd_open_guide(const iptv_playlist *playlist, int current_channel) {
    if (osd_style != IPTV_OSD_STYLE_CABLE_TV ||
        !playlist || playlist->count == 0)
        return;

    collect_groups(playlist);
    if (group_count == 0)
        return;

    menu = MENU_GUIDE;
    banner_countdown = 0;             /* the guide replaces the banner */
    focus = 1;

    /* Opens where the viewer already is, not at the top of the list. */
    group_row = 0;
    channel_row = 0;
    if (current_channel >= 0 && (size_t)current_channel < playlist->count) {
        const char *name = group_of(&playlist->channels[current_channel]);
        for (size_t i = 0; i < group_count; i++)
            if (strcmp(groups[i].name, name) == 0) { group_row = (int)i; break; }

        int seen = 0;
        for (size_t i = 0; i < playlist->count; i++) {
            if (strcmp(group_of(&playlist->channels[i]), name) != 0)
                continue;
            if ((int)i == current_channel) { channel_row = seen; break; }
            seen++;
        }
    }
    /* Both start at zero and the draw pulls them to wherever the cursor
     * landed, which is the one place that knows how many rows fit. */
    channel_scroll = 0;
    group_scroll = 0;
}

void iptv_osd_close_guide(void) {
    menu = MENU_NONE;
}

/* The schedule: one list, one cursor, and nothing to focus sideways into. */
static int schedule_key(iptv_osd_key key, const iptv_playlist *playlist) {
    int rows = (int)playlist->count;
    if (rows <= 0)
        return -1;

    switch (key) {
    case IPTV_OSD_UP:
        if (--sched_row < 0) sched_row = rows - 1;
        break;
    case IPTV_OSD_DOWN:
        if (++sched_row >= rows) sched_row = 0;
        break;

    /*
     * Left and right page through, rather than doing nothing. A two hundred
     * channel schedule one row at a time is a lot of button, and the two keys
     * are free here - in the guide they move between columns this list has not
     * got.
     */
    case IPTV_OSD_LEFT:
        sched_row -= SCHEDULE_PAGE;
        if (sched_row < 0) sched_row = 0;
        break;
    case IPTV_OSD_RIGHT:
        sched_row += SCHEDULE_PAGE;
        if (sched_row >= rows) sched_row = rows - 1;
        break;

    case IPTV_OSD_CONFIRM:
        menu = MENU_NONE;
        banner_countdown = BANNER_FRAMES;
        return sched_row;

    case IPTV_OSD_CANCEL:
        menu = MENU_NONE;
        break;
    }
    return -1;
}

int iptv_osd_key_pressed(iptv_osd_key key, const iptv_playlist *playlist) {
    if (menu == MENU_NONE)
        return -1;

    if (menu == MENU_SCHEDULE) {
        int chosen = schedule_key(key, playlist);
        if (key == IPTV_OSD_UP || key == IPTV_OSD_DOWN ||
            key == IPTV_OSD_LEFT || key == IPTV_OSD_RIGHT)
            reset_scroll();      /* the walking label belongs to the old row */
        return chosen;
    }

    if (group_count == 0)
        return -1;

    int rows = (int)groups[group_row].count;

    switch (key) {
    case IPTV_OSD_UP:
        if (focus == 0) {
            if (--group_row < 0) group_row = (int)group_count - 1;
            channel_row = 0;
            channel_scroll = 0;
        } else if (rows > 0) {
            if (--channel_row < 0) channel_row = rows - 1;
        }
        break;

    case IPTV_OSD_DOWN:
        if (focus == 0) {
            if (++group_row >= (int)group_count) group_row = 0;
            channel_row = 0;
            channel_scroll = 0;
        } else if (rows > 0) {
            if (++channel_row >= rows) channel_row = 0;
        }
        break;

    case IPTV_OSD_LEFT:
        focus = 0;
        break;

    case IPTV_OSD_RIGHT:
        focus = 1;
        break;

    case IPTV_OSD_CONFIRM:
        if (focus == 0) {
            focus = 1;                       /* into the channels of the group */
            channel_row = 0;
            channel_scroll = 0;
            break;
        }
        {
            int channel = channel_at(playlist, (size_t)group_row, channel_row);
            if (channel >= 0) {
                menu = MENU_NONE;
                banner_countdown = BANNER_FRAMES;
                return channel;
            }
        }
        break;

    case IPTV_OSD_CANCEL:
        menu = MENU_NONE;
        break;
    }

    return -1;
}

bool iptv_osd_visible(void) {
    return menu != MENU_NONE || waiting || iptv_osd_banner_visible();
}

/* ---------------------------------------------------------------- drawing */

static void draw_banner(
    const surface *s, const iptv_playlist *playlist, int current_channel
) {
    if (current_channel < 0 || (size_t)current_channel >= playlist->count)
        return;

    const iptv_channel *channel = &playlist->channels[current_channel];
    int scale = scale_for(s->height);

    /* Recomputed from scratch every frame: the labels below set it if they
     * still have somewhere to go. */
    banner_text_busy = false;
    int pad = 4 * scale;
    int line = IPTV_FONT_HEIGHT * scale;
    int box_h = line + pad;

    /*
     * Laid out from the top down with the gaps written out, because the first
     * version computed the second row from a height that happened to equal the
     * bottom of the first: the number badge and the group name ended up
     * touching, which on screen reads as one sitting on the other.
     */
    int height = pad + box_h + pad + line + pad;
    int y = (int)s->height - height - pad * 2;
    int x = pad * 2;
    int width = (int)s->width - pad * 4;

    /* Fading out instead of vanishing: the last second is spent on its way
     * out, which reads as a television and not as a dropped frame. */
    unsigned alpha = 225;
    if (!banner_pinned && banner_countdown < 60)
        alpha = (unsigned)(225 * banner_countdown / 60);

    fill_rect(s, x, y, width, height, COLOUR_PANEL, alpha);
    fill_rect(s, x, y, width, scale, COLOUR_PANEL_EDGE, alpha);
    fill_rect(s, x, y + height - scale, width, scale, COLOUR_PANEL_EDGE, alpha);

    char number[16];
    int shown = channel->number > 0 ? channel->number : current_channel + 1;
    snprintf(number, sizeof(number), "%d", shown);

    int box_w = text_width("000", scale) + pad * 2;
    int box_y = y + pad;
    fill_rect(s, x + pad, box_y, box_w, box_h, COLOUR_SELECTED, alpha);
    draw_text(s, x + pad + (box_w - text_width(number, scale)) / 2,
              box_y + (box_h - line) / 2, number, scale, COLOUR_TEXT_DARK, 0);

    bool ordered = false;
    const char *second = second_line(channel, &ordered);
    const char *channel_name = ordered && channel->tvg_name[0]
                             ? channel->tvg_name : channel->name;

    /* Name beside the badge, vertically centred against it. There is nothing
     * selected on a banner, so both lines scroll whenever they need to. */
    draw_text_scrolling(s, SCROLL_BANNER_NAME,
                        x + pad * 2 + box_w, width - box_w - pad * 4,
                        box_y + (box_h - line) / 2,
                        channel_name, scale, COLOUR_TEXT, true);
    if (scroll_slot_busy(SCROLL_BANNER_NAME))
        banner_text_busy = true;

    /*
     * The second line is what is on now, and the group only when there is no
     * listing for this channel.
     *
     * Same slot either way: it is one line with one position, and which text
     * it holds is not something the scroller needs to know. That is also why
     * the length limit had to go first - a programme title is written by a
     * broadcaster, not by whoever typed the category.
     */
    draw_text_scrolling(s, SCROLL_BANNER_GROUP,
                        x + pad, width - pad * 2, box_y + box_h + pad,
                        second, scale, COLOUR_SELECTED, true);
    if (scroll_slot_busy(SCROLL_BANNER_GROUP))
        banner_text_busy = true;
}

/* The general font advances seven cells, while a few digit strokes use the
 * eighth. That is compact for prose, but repeated TV digits then touch: 5 and
 * 7 along the top, 2 along the bottom, and the curved edge of 3 reads as one
 * shape. The large channel number gets the glyph's full eight-cell advance;
 * menus and guide text keep their deliberately tighter spacing. */
#define TV_NUMBER_ADVANCE 8

static int tv_number_width(const char *number, int scale) {
    return (int)strlen(number) * TV_NUMBER_ADVANCE * scale;
}

static void draw_tv_number_text(
    const surface *s, int x, int y, const char *number, int scale,
    uint32_t colour
) {
    for (const char *p = number; *p; p++) {
        char digit[2] = { *p, '\0' };
        draw_text_shadowed(s, x, y, digit, scale, colour, 0);
        x += TV_NUMBER_ADVANCE * scale;
    }
}

/*
 * The small overlay of an ordinary television: no panel, name, category or
 * programme text. The list's explicit tvg-chno wins; its position in the list
 * is the fallback, matching the number shown by the Cable TV banner.
 */
static void draw_tv_number(
    const surface *s, const iptv_playlist *playlist, int current_channel
) {
    if (current_channel < 0 || (size_t)current_channel >= playlist->count)
        return;

    const iptv_channel *channel = &playlist->channels[current_channel];
    int shown = channel->number > 0 ? channel->number : current_channel + 1;
    char number[16];
    snprintf(number, sizeof(number), "%d", shown);

    /*
     * Before the fixed canvas, a 4:3 480-line channel used scale 3. Once that
     * picture was expanded by the frontend, the number occupied roughly the
     * same apparent height as scale 7 does on the new 1080-line canvas. The TV
     * style now uses one further step so the number is a little easier to read.
     */
    int scale = scale_for(s->height) + 4;
    if (scale > 9)
        scale = 9;
    int margin = 6 * scale;
    int x = (int)s->width - margin - tv_number_width(number, scale);
    int y = margin;

    banner_text_busy = false;
    draw_tv_number_text(s, x, y, number, scale, COLOUR_TV_NUMBER);
}

/*
 * The guide sizes itself to the space instead of assuming the space is enough.
 *
 * The first version picked a scale from the frame height and drew. On a 43
 * channel list arriving at low resolution the result was a header where "GUIA DE
 * CANAIS" and "43 canais" printed on top of each other, and seven visible rows.
 * Both are the same mistake: computing a layout and never checking it against
 * the width and height it has to live in.
 *
 * So the scale starts from the picture and comes down until the header fits
 * across and a useful number of rows fits down. At the smallest scale the
 * channel count is dropped rather than allowed to collide - a title that is
 * readable beats a count that is not.
 */
#define GUIDE_ROWS_WANTED 10

/*
 * How far a list must be scrolled for the cursor to be on screen.
 *
 * One function because there are three lists - the guide's two columns and the
 * schedule - and this was three copies of the same three lines, of which one
 * was simply never written. The group column drew the first `visible` groups
 * and nothing else: on a list with 21 categories in a screen that fits 17, four
 * were unreachable, and moving the cursor onto them made the highlight vanish,
 * because the selected row was being drawn below the panel.
 *
 * It hid for as long as it did because the channel column is the one that gets
 * long, so the missing half of a pair looked like a column that never needed
 * the feature. A fourth list can now only get this wrong by not calling this.
 */
static int scroll_to_show(int cursor, int scroll, int visible) {
    if (visible < 1)
        return 0;
    if (cursor < scroll)
        scroll = cursor;
    if (cursor >= scroll + visible)
        scroll = cursor - visible + 1;
    return scroll < 0 ? 0 : scroll;
}

static void draw_guide(
    const surface *s, const iptv_playlist *playlist, int current_channel
) {
    /*
     * A list of nothing but videos calls itself a video guide. Everything below
     * this - the columns, the cursor, the buttons, the fitting loop - is the
     * same code doing the same thing; only the two strings differ, because the
     * only thing that is different is what the list contains.
     */
    bool videos = playlist->all_videos;
    const char *title = iptv_text(videos ? IPTV_TEXT_VIDEO_GUIDE_TITLE
                                         : IPTV_TEXT_GUIDE_TITLE);

    char summary[48];
    snprintf(summary, sizeof(summary),
             iptv_text(playlist->count == 1
                           ? (videos ? IPTV_TEXT_VIDEO_COUNT_ONE
                                     : IPTV_TEXT_CHANNEL_COUNT_ONE)
                           : (videos ? IPTV_TEXT_VIDEO_COUNT_MANY
                                     : IPTV_TEXT_CHANNEL_COUNT_MANY)),
             playlist->count);

    int scale = scale_for(s->height);
    int pad, line, row_height, panel_x, panel_y, panel_w, panel_h;
    int body_y, footer_h, body_h, visible;
    bool show_summary = true;

    for (;;) {
        pad = 3 * scale;
        line = IPTV_FONT_HEIGHT * scale;
        row_height = line + pad;

        int margin = pad * 3;
        panel_x = margin;
        panel_y = margin;
        panel_w = (int)s->width - margin * 2;
        panel_h = (int)s->height - margin * 2;

        body_y = panel_y + row_height + pad * 2;
        footer_h = row_height + pad;
        body_h = panel_h - (body_y - panel_y) - footer_h - pad;
        visible = body_h > 0 ? body_h / row_height : 0;

        int header_needed = text_width(title, scale) +
                            text_width(summary, scale) + pad * 6;

        if (scale == 1) {
            /* Nothing left to shrink: drop the count if it still does not fit. */
            show_summary = header_needed <= panel_w;
            break;
        }
        if (header_needed <= panel_w && visible >= GUIDE_ROWS_WANTED)
            break;
        scale--;
    }
    if (visible < 1)
        visible = 1;

    fill_rect(s, 0, 0, (int)s->width, (int)s->height, COLOUR_DIM, 150);
    fill_rect(s, panel_x, panel_y, panel_w, panel_h, COLOUR_PANEL, 235);
    fill_rect(s, panel_x, panel_y, panel_w, row_height + pad, COLOUR_HEADER, 255);
    draw_text(s, panel_x + pad * 2, panel_y + pad, title, scale,
              COLOUR_TEXT, panel_w - pad * 4);
    if (show_summary)
        draw_text(s, panel_x + panel_w - text_width(summary, scale) - pad * 2,
                  panel_y + pad, summary, scale, COLOUR_SELECTED, 0);

    /*
     * The group column is as wide as the widest group name, not a third of the
     * panel. One list has a single group called "RetroTV" and 43 channels: a
     * third of the width spent on one short word is a third stolen from the
     * names that matter.
     */
    int widest = 0;
    for (size_t i = 0; i < group_count; i++) {
        int w = text_width(groups[i].name, scale);
        if (w > widest) widest = w;
    }
    int groups_w = widest + pad * 4;
    if (groups_w > panel_w / 2) groups_w = panel_w / 2;
    if (groups_w < panel_w / 6) groups_w = panel_w / 6;

    int channels_x = panel_x + groups_w + pad;
    int channels_w = panel_w - groups_w - pad * 2;

    /*
     * Groups, scrolled to keep the cursor visible - the same three lines the
     * channel column has had since it was written.
     *
     * They were missing here, and the column simply drew the first `visible`
     * groups: on a list with 21 categories in a screen that fits 17, four
     * categories could not be seen and could not be reached, and moving the
     * cursor onto them made the highlight disappear entirely, because the
     * selected row was being drawn off the bottom of the panel.
     *
     * It hid for as long as it did because the channel column is the one that
     * gets long, so the missing half of a pair looked like a column that never
     * needed the feature. The schedule screen, written later from the same
     * pattern, has it.
     */
    group_scroll = scroll_to_show(group_row, group_scroll, visible);

    for (int i = 0; i < visible && group_scroll + i < (int)group_count; i++) {
        int row = group_scroll + i;
        int y = body_y + i * row_height;
        bool selected = (row == group_row);
        fill_rect(s, panel_x + pad, y, groups_w - pad, line + pad / 2,
                  selected ? (focus == 0 ? COLOUR_SELECTED : COLOUR_PANEL_EDGE)
                           : (i % 2 ? COLOUR_ROW_ALT : COLOUR_ROW), 255);
        uint32_t group_colour =
            (selected && focus == 0) ? COLOUR_TEXT_DARK : COLOUR_TEXT;

        /*
         * Only the row under the cursor walks. One of the measured lists has
         * nine truncated group names visible at once; nine of them moving is a
         * screen nobody can read, and a set-top box scrolls the highlight and
         * nothing else.
         */
        if (selected)
            draw_text_scrolling(s, SCROLL_GUIDE_GROUP, panel_x + pad * 2,
                                groups_w - pad * 3, y + pad / 4,
                                groups[row].name, scale, group_colour, false);
        else
            draw_text(s, panel_x + pad * 2, y + pad / 4, groups[row].name, scale,
                      group_colour, groups_w - pad * 3);
    }

    /* Channels of the selected group, scrolled to keep the cursor visible. */
    int rows = (int)groups[group_row].count;
    channel_scroll = scroll_to_show(channel_row, channel_scroll, visible);

    for (int i = 0; i < visible && channel_scroll + i < rows; i++) {
        int row = channel_scroll + i;
        int index = channel_at(playlist, (size_t)group_row, row);
        if (index < 0)
            break;

        const iptv_channel *channel = &playlist->channels[index];
        int y = body_y + i * row_height;
        bool selected = (row == channel_row);
        bool playing = (index == current_channel);

        fill_rect(s, channels_x, y, channels_w, line + pad / 2,
                  (selected && focus == 1) ? COLOUR_SELECTED
                                           : (i % 2 ? COLOUR_ROW_ALT : COLOUR_ROW),
                  255);

        uint32_t text_colour =
            (selected && focus == 1) ? COLOUR_TEXT_DARK : COLOUR_TEXT;

        char number[16];
        int shown = channel->number > 0 ? channel->number : index + 1;
        snprintf(number, sizeof(number), "%d", shown);
        int number_w = text_width("0000", scale);
        draw_text(s, channels_x + pad + (number_w - text_width(number, scale)),
                  y + pad / 4, number, scale,
                  (selected && focus == 1) ? COLOUR_TEXT_DARK : COLOUR_SELECTED, 0);

        if (selected)
            draw_text_scrolling(s, SCROLL_GUIDE_CHANNEL,
                                channels_x + pad * 2 + number_w,
                                channels_w - number_w - pad * 5, y + pad / 4,
                                channel->name, scale, text_colour, false);
        else
            draw_text(s, channels_x + pad * 2 + number_w, y + pad / 4,
                      channel->name, scale, text_colour,
                      channels_w - number_w - pad * 5);

        /* A dot for the channel currently playing, so the guide says where you
         * are as well as where you could go. */
        if (playing)
            fill_rect(s, channels_x + channels_w - pad * 2, y + line / 3,
                      scale * 2, scale * 2, text_colour, 255);
    }

    /* Footer, in the spirit of the coloured buttons on a receiver. Named after
     * an Xbox pad, which is what EmuVR models and what is printed on the pad in
     * the viewer's hands - libretro's own names for these buttons disagree. */
    int footer_y = panel_y + panel_h - footer_h;
    fill_rect(s, panel_x, footer_y, panel_w, footer_h, COLOUR_HEADER, 255);
    /*
     * Position in the list, measured first so the hints can be told where to
     * stop.
     *
     * Two attempts got this wrong in ways only a rendered frame showed: the
     * first drew it before the footer bar, which painted over it; the second
     * drew the hints to the panel edge, so the two texts collided. Reserving
     * the space is the fix, and it is the same fix as the header's.
     */
    /*
     * The counter belongs to the column the cursor is in.
     *
     * It always counted channels, which is why the group column being stuck was
     * invisible from the outside: with 21 categories on a screen that fits 17,
     * nothing anywhere said 21. A count that follows the focus is also the only
     * hint that a column has more below it, since there is no scrollbar.
     */
    int here = (focus == 0) ? group_row : channel_row;
    int total = (focus == 0) ? (int)group_count : rows;

    char position[32];
    int position_w = 0;
    if (total > visible) {
        snprintf(position, sizeof(position), "%d/%d", here + 1, total);
        position_w = text_width(position, scale) + pad * 2;
    }

    /*
     * Shorter wording when the space is shorter, rather than a sentence cut
     * off mid word. "A fe" is not a hint.
     */
    static const iptv_text_id hint_forms[] = {
        IPTV_TEXT_HINTS_FULL,
        IPTV_TEXT_HINTS_SHORT,
        IPTV_TEXT_HINTS_MINIMAL,
    };
    const size_t hint_count = sizeof(hint_forms) / sizeof(*hint_forms);

    /*
     * The width is measured from the translated string, not from the longest
     * one across languages. English is shorter than Portuguese here, so on a
     * small picture English keeps the full hint where Portuguese has already
     * fallen back to the short one - which is the point of measuring.
     */
    int hint_room = panel_w - pad * 4 - position_w;
    const char *hints = iptv_text(hint_forms[hint_count - 1]);
    for (size_t i = 0; i < hint_count; i++) {
        const char *form = iptv_text(hint_forms[i]);
        if (text_width(form, scale) <= hint_room) {
            hints = form;
            break;
        }
    }

    draw_text(s, panel_x + pad * 2, footer_y + pad / 2, hints, scale,
              COLOUR_TEXT, hint_room);

    if (position_w > 0)
        draw_text(s, panel_x + panel_w - position_w, footer_y + pad / 2,
                  position, scale, COLOUR_SELECTED, 0);
}

/*
 * What a schedule row says to the right of the channel name.
 *
 * Same shape as the banner's second line and deliberately not the same code:
 * the banner has one line and all the width there is, and can afford the
 * synopsis. A row here is one of ten or fifteen on screen and gets a fraction
 * of the width, so it carries only what is on and what follows. Sharing one
 * function would have meant a parameter deciding which of two layouts to
 * produce, which is two functions with extra steps.
 */
static const char *schedule_row_text(const iptv_channel *channel,
                                     char *out, size_t out_size,
                                     bool *ordered) {
    iptv_epg_programme now, next;
    *ordered = false;

    if (!iptv_epg_now_at(channel->tvg_id, channel->epg_index, &now)) {
        /* No programme covering this instant. The category still says something
         * about the channel, which beats an empty column; only when the list
         * gave no category either does the row admit it has nothing. */
        return channel->group[0] ? channel->group
                                 : iptv_text(IPTV_TEXT_NO_LISTING);
    }
    *ordered = now.ordered;

    size_t at = 0;
    int written = snprintf(out, out_size, "%s", now.title);
    if (written > 0)
        at = (size_t)written < out_size ? (size_t)written : out_size - 1;

    if (iptv_epg_next_at(channel->tvg_id, channel->epg_index, &next)) {
        if (next.ordered) {
            snprintf(out + at, out_size - at, " - %s %s",
                     iptv_text(IPTV_TEXT_UP_NEXT), next.title);
        } else {
            char when[8];
            clock_time(next.start, when, sizeof(when));
            snprintf(out + at, out_size - at, " - %s %s %s",
                     iptv_text(IPTV_TEXT_UP_NEXT), when, next.title);
        }
    }
    return out;
}

/*
 * The schedule: every channel on one list, with what is on beside it.
 *
 * The guide answers "what else is there", by category. This answers "what is
 * on", by channel, which is a different question and wants a different shape -
 * no group column, one row per channel in list order, and the programme given
 * most of the width because that is what the viewer opened this to read.
 *
 * The fitting loop is the guide's, for the guide's reason: a layout that is
 * computed and never checked against the width it has to live in is how "GUIA
 * DE CANAIS" once printed on top of "43 canais".
 */
static void draw_schedule(
    const surface *s, const iptv_playlist *playlist, int current_channel
) {
    const char *title = iptv_text(IPTV_TEXT_SCHEDULE_TITLE);

    char summary[48];
    if (iptv_epg_available())
        snprintf(summary, sizeof(summary),
                 iptv_text(playlist->count == 1 ? IPTV_TEXT_CHANNEL_COUNT_ONE
                                                : IPTV_TEXT_CHANNEL_COUNT_MANY),
                 playlist->count);
    else
        /* Says why every row is showing a category instead of a programme. A
         * screen full of categories under the word SCHEDULE looks like a fault;
         * the same screen with this in the corner looks like an answer. */
        snprintf(summary, sizeof(summary), "%s", iptv_text(IPTV_TEXT_NO_LISTING));

    int scale = scale_for(s->height);
    int pad, line, row_height, panel_x, panel_y, panel_w, panel_h;
    int body_y, footer_h, body_h, visible;
    bool show_summary = true;

    for (;;) {
        pad = 3 * scale;
        line = IPTV_FONT_HEIGHT * scale;
        row_height = line + pad;

        int margin = pad * 3;
        panel_x = margin;
        panel_y = margin;
        panel_w = (int)s->width - margin * 2;
        panel_h = (int)s->height - margin * 2;

        body_y = panel_y + row_height + pad * 2;
        footer_h = row_height + pad;
        body_h = panel_h - (body_y - panel_y) - footer_h - pad;
        visible = body_h > 0 ? body_h / row_height : 0;

        int header_needed = text_width(title, scale) +
                            text_width(summary, scale) + pad * 6;

        if (scale == 1) {
            show_summary = header_needed <= panel_w;
            break;
        }
        if (header_needed <= panel_w && visible >= GUIDE_ROWS_WANTED)
            break;
        scale--;
    }
    if (visible < 1)
        visible = 1;

    fill_rect(s, 0, 0, (int)s->width, (int)s->height, COLOUR_DIM, 150);
    fill_rect(s, panel_x, panel_y, panel_w, panel_h, COLOUR_PANEL, 235);
    fill_rect(s, panel_x, panel_y, panel_w, row_height + pad, COLOUR_HEADER, 255);
    draw_text(s, panel_x + pad * 2, panel_y + pad, title, scale,
              COLOUR_TEXT, panel_w - pad * 4);
    if (show_summary)
        draw_text(s, panel_x + panel_w - text_width(summary, scale) - pad * 2,
                  panel_y + pad, summary, scale, COLOUR_SELECTED, 0);

    /*
     * Three columns across one row: number, channel, programme.
     *
     * The channel column is a fixed share and not the widest name, which is what
     * the guide does with its groups. Group names repeat across a list and there
     * are a handful of them; channel names are all different and one list has a
     * fifty character one, so sizing to the widest would hand most of the row to
     * a single channel and leave the programmes - the reason this screen exists
     * - with nothing. A share that is too small only makes one name walk.
     */
    int number_w = text_width("0000", scale);
    int rows_x = panel_x + pad;
    int rows_w = panel_w - pad * 2;

    int channel_w = rows_w * 7 / 20;
    int channel_min = text_width("MMMMMMMMMMMM", scale);
    if (channel_w < channel_min) channel_w = channel_min;
    if (channel_w > rows_w / 2) channel_w = rows_w / 2;

    /*
     * The gaps are written out rather than left to fall where they may, because
     * the first rendered frame showed both of them closed: the playing dot sat
     * against the first letter of the name ("41.MTV Biggest"), and on the
     * highlighted row - the only one whose name walks to the edge of its box -
     * the name ran straight into the programme beside it. Neither is wrong by a
     * pixel count; both are unreadable, which is the only test that matters
     * here.
     *
     * So the dot gets a column of its own between the number and the name, and
     * the two text columns are separated by twice the padding instead of once.
     */
    int dot_x = rows_x + pad + number_w + pad;
    int channel_x = dot_x + pad * 2;
    int programme_x = rows_x + channel_w + pad * 3;
    int programme_w = rows_x + rows_w - programme_x - pad;

    int rows = (int)playlist->count;
    sched_scroll = scroll_to_show(sched_row, sched_scroll, visible);

    for (int i = 0; i < visible && sched_scroll + i < rows; i++) {
        int index = sched_scroll + i;
        const iptv_channel *channel = &playlist->channels[index];
        int y = body_y + i * row_height;
        bool selected = (index == sched_row);
        bool playing = (index == current_channel);

        fill_rect(s, rows_x, y, rows_w, line + pad / 2,
                  selected ? COLOUR_SELECTED
                           : (i % 2 ? COLOUR_ROW_ALT : COLOUR_ROW), 255);

        uint32_t text_colour = selected ? COLOUR_TEXT_DARK : COLOUR_TEXT;

        char number[16];
        int shown = channel->number > 0 ? channel->number : index + 1;
        snprintf(number, sizeof(number), "%d", shown);
        draw_text(s, rows_x + pad + (number_w - text_width(number, scale)),
                  y + pad / 4, number, scale,
                  selected ? COLOUR_TEXT_DARK : COLOUR_SELECTED, 0);

        int name_w = programme_x - channel_x - pad * 2;

        char buffer[512];
        bool ordered = false;
        const char *programme =
            schedule_row_text(channel, buffer, sizeof(buffer), &ordered);
        const char *channel_name = ordered && channel->tvg_name[0]
                                 ? channel->tvg_name : channel->name;

        /* Only the row under the cursor walks, and on that row both halves do.
         * Fifteen rows moving at once is a screen nobody can read. */
        if (selected) {
            draw_text_scrolling(s, SCROLL_SCHED_CHANNEL, channel_x, name_w,
                                y + pad / 4, channel_name, scale, text_colour,
                                false);
            draw_text_scrolling(s, SCROLL_SCHED_PROGRAMME, programme_x,
                                programme_w, y + pad / 4, programme, scale,
                                text_colour, false);
        } else {
            draw_text(s, channel_x, y + pad / 4, channel_name, scale,
                      text_colour, name_w);
            draw_text(s, programme_x, y + pad / 4, programme, scale,
                      selected ? text_colour : COLOUR_SELECTED, programme_w);
        }

        if (playing)
            fill_rect(s, dot_x, y + line / 3, scale * 2, scale * 2,
                      text_colour, 255);
    }

    int footer_y = panel_y + panel_h - footer_h;
    fill_rect(s, panel_x, footer_y, panel_w, footer_h, COLOUR_HEADER, 255);

    char position[32];
    int position_w = 0;
    if (rows > visible) {
        snprintf(position, sizeof(position), "%d/%d", sched_row + 1, rows);
        position_w = text_width(position, scale) + pad * 2;
    }

    static const iptv_text_id hint_forms[] = {
        IPTV_TEXT_HINTS_FULL,
        IPTV_TEXT_HINTS_SHORT,
        IPTV_TEXT_HINTS_MINIMAL,
    };
    const size_t hint_count = sizeof(hint_forms) / sizeof(*hint_forms);

    int hint_room = panel_w - pad * 4 - position_w;
    const char *hints = iptv_text(hint_forms[hint_count - 1]);
    for (size_t i = 0; i < hint_count; i++) {
        const char *form = iptv_text(hint_forms[i]);
        if (text_width(form, scale) <= hint_room) {
            hints = form;
            break;
        }
    }

    draw_text(s, panel_x + pad * 2, footer_y + pad / 2, hints, scale,
              COLOUR_TEXT, hint_room);
    if (position_w > 0)
        draw_text(s, panel_x + panel_w - position_w, footer_y + pad / 2,
                  position, scale, COLOUR_SELECTED, 0);
}

void iptv_osd_draw(
    uint32_t *pixels,
    unsigned width,
    unsigned height,
    unsigned pitch_bytes,
    const iptv_playlist *playlist,
    int current_channel
) {
    if (!pixels || width == 0 || height == 0 || !playlist || playlist->count == 0)
        return;

    surface s = { pixels, width, height, pitch_bytes / 4 };

    if (menu == MENU_GUIDE) {
        draw_guide(&s, playlist, current_channel);
        return;
    }
    if (menu == MENU_SCHEDULE) {
        draw_schedule(&s, playlist, current_channel);
        return;
    }

    /* The banner first, then the ring on top of it: the ring sits in the middle
     * of the picture and the banner along the bottom, so they do not overlap -
     * but if a small picture ever brings them together, the thing that says the
     * core is still working should be the one that survives. */
    if (iptv_osd_banner_visible()) {
        if (osd_style == IPTV_OSD_STYLE_TV)
            draw_tv_number(&s, playlist, current_channel);
        else
            draw_banner(&s, playlist, current_channel);
    }
    if (waiting)
        draw_waiting(&s);
}
