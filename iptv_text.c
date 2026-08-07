#include "iptv_text.h"

#include <stddef.h>

/*
 * One row per string, one column per language, and the id repeated in the row.
 *
 * The id is repeated so the table can be checked against itself at startup
 * instead of trusted. An enum used as an array index only works while the array
 * is in the same order as the enum, and nothing in C enforces that; the first
 * time someone adds a string in the middle, every row below it moves and the
 * guide starts confidently printing the wrong line. The check below catches that
 * on the first call rather than in a screenshot.
 */
typedef struct {
    iptv_text_id id;
    const char *english;
    const char *portuguese;
} text_row;

static const text_row rows[] = {
    { IPTV_TEXT_GUIDE_TITLE,
      "CHANNEL GUIDE",
      "GUIA DE CANAIS" },

    { IPTV_TEXT_CHANNEL_COUNT_ONE,
      "%zu channel",
      "%zu canal" },
    { IPTV_TEXT_CHANNEL_COUNT_MANY,
      "%zu channels",
      "%zu canais" },

    { IPTV_TEXT_VIDEO_GUIDE_TITLE,
      "VIDEO GUIDE",
      "GUIA DE VIDEOS" },
    { IPTV_TEXT_VIDEO_COUNT_ONE,
      "%zu video",
      "%zu video" },
    { IPTV_TEXT_VIDEO_COUNT_MANY,
      "%zu videos",
      "%zu videos" },

    /*
     * The English forms are shorter than the Portuguese ones, which is why the
     * fitting loop measures the string it is about to draw instead of assuming a
     * width. On a small picture English will often still fit the full hint where
     * Portuguese has already dropped to the short one.
     */
    { IPTV_TEXT_HINTS_FULL,
      "D-pad move   B watch   A close",
      "Direcional mover   B assistir   A fechar" },
    { IPTV_TEXT_HINTS_SHORT,
      "B watch   A close",
      "B assistir   A fechar" },
    { IPTV_TEXT_HINTS_MINIMAL,
      "B watch  A back",
      "B ver  A sair" },

    { IPTV_TEXT_NO_GROUP,
      "No group",
      "Sem grupo" },

    { IPTV_TEXT_UP_NEXT,
      "Next",
      "A seguir" },

    { IPTV_TEXT_LOADING,
      "Loading",
      "Carregando" },

    { IPTV_TEXT_NO_SIGNAL,
      "NO SIGNAL",
      "SEM SINAL" },

    { IPTV_TEXT_SCHEDULE_TITLE,
      "SCHEDULE",
      "PROGRAMACAO" },
    /*
     * Deliberately not "no listing for this channel": it is drawn in the column
     * where a programme title would be, on a screen the viewer opened to read
     * programme titles, and the shortest true thing is the one that does not
     * push the channel name off its own row.
     */
    { IPTV_TEXT_NO_LISTING,
      "No listing",
      "Sem programacao" },
};

static iptv_language current = IPTV_LANG_ENGLISH;

void iptv_text_set_language(iptv_language language) {
    if (language < 0 || language >= IPTV_LANG_COUNT)
        language = IPTV_LANG_ENGLISH;
    current = language;
}

iptv_language iptv_text_language(void) {
    return current;
}

/*
 * Verified once, on the first lookup: the table has one row per id and every row
 * sits at its own index. If it does not, every string is fetched from English so
 * the guide stays readable while being obviously untranslated - a visible
 * failure that points at the table, rather than an invisible one that points
 * nowhere.
 */
static int table_is_sane(void) {
    static int checked = 0;
    static int sane = 0;

    if (!checked) {
        checked = 1;
        sane = (sizeof(rows) / sizeof(*rows)) == (size_t)IPTV_TEXT_COUNT;
        for (size_t i = 0; sane && i < sizeof(rows) / sizeof(*rows); i++) {
            if (rows[i].id != (iptv_text_id)i || !rows[i].english)
                sane = 0;
        }
    }
    return sane;
}

const char *iptv_text(iptv_text_id id) {
    if (!table_is_sane() || id < 0 || id >= IPTV_TEXT_COUNT)
        return "";

    const text_row *row = &rows[id];
    const char *value = NULL;

    switch (current) {
        case IPTV_LANG_PORTUGUESE: value = row->portuguese; break;
        default:                   value = row->english;    break;
    }

    if (!value)
        value = row->english;
    return value ? value : "";
}
