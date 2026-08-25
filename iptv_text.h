#ifndef IPTV_TEXT_H
#define IPTV_TEXT_H

/*
 * The words the core puts on the screen, in every language it speaks.
 *
 * Only the on-screen display is translated. The core options and the log are
 * English and stay English, on purpose: a log is something a stranger sends to
 * whoever can read it, and a log whose wording depends on the sender's locale
 * is a log that has to be translated back before it can be compared with
 * another one. The banner and the guide are the opposite - they are read by one
 * person, on a television, in their own house.
 *
 * The two translations of a string live side by side in one row rather than in
 * two separate lists. Two lists drift: someone inserts a string in the middle of
 * one of them and every string after it silently shifts by one, which produces a
 * guide that is grammatical, confident and wrong. Here a missing translation is
 * a missing field, and a missing field is visible at the place where it is
 * missing.
 */

typedef enum {
    IPTV_LANG_ENGLISH = 0,
    IPTV_LANG_PORTUGUESE,
    IPTV_LANG_COUNT
} iptv_language;

typedef enum {
    /* Guide header. */
    IPTV_TEXT_GUIDE_TITLE,

    /*
     * Channel count. Both take one %zu. Portuguese and English happen to agree
     * on needing a separate singular, which is not something to rely on for the
     * next language added - a plural rule is a language's own business, and the
     * day one of them needs three forms this enum grows a third entry rather
     * than the drawing code growing a rule.
     */
    IPTV_TEXT_CHANNEL_COUNT_ONE,
    IPTV_TEXT_CHANNEL_COUNT_MANY,

    /*
     * The same guide, over a list where every entry is a video.
     *
     * Separate strings and not a word substituted into the channel ones: "guia
     * de canais" and "guia de vídeos" agree in gender in Portuguese and would
     * not survive a swap of one noun, and the next language added will have its
     * own reason. A whole string per case is the only form that lets a
     * translator write what their language actually needs.
     */
    IPTV_TEXT_VIDEO_GUIDE_TITLE,
    IPTV_TEXT_VIDEO_COUNT_ONE,
    IPTV_TEXT_VIDEO_COUNT_MANY,

    /*
     * Guide footer, longest first. The guide picks the longest one that fits the
     * space it actually measured, so these are three ways of saying the same
     * thing and not three different messages. Button letters are Xbox names,
     * which is what EmuVR models and what is printed on the pad in the reader's
     * hand.
     */
    IPTV_TEXT_HINTS_FULL,
    IPTV_TEXT_HINTS_SHORT,
    IPTV_TEXT_HINTS_MINIMAL,

    /* Shown as the group of a channel whose list gave it none. */
    IPTV_TEXT_NO_GROUP,

    /*
     * Introduces the programme that follows the one on air. Written to be read
     * mid-line, after the current title, so it carries its own spacing in the
     * drawing code rather than in the string - a translator should not have to
     * count spaces to keep the banner from looking broken.
     */
    IPTV_TEXT_UP_NEXT,

    /* Beside the spinner while a channel is opening. */
    IPTV_TEXT_LOADING,

    /* Full-screen test pattern shown after a live channel fails. */
    IPTV_TEXT_NO_SIGNAL,

    /* The schedule screen: its header, and what a row says when the listing
     * covers no programme for that channel right now. */
    IPTV_TEXT_SCHEDULE_TITLE,
    IPTV_TEXT_NO_LISTING,

    /*
     * The four notices a local file can put in the corner.
     *
     * These exist because of where this core is used. RetroArch shows its own
     * message when a track changes; EmuVR does not show RetroArch's overlay at
     * all, so inside a headset the button would be a button that does nothing
     * observable. The core therefore says it itself, which means saying it in
     * both languages like everything else on the screen.
     *
     * The track name is not translated and must not be: it comes out of the
     * file, it is what the person who made the file called it, and "Portugues
     * (Brasil)" is more use to the viewer than any word this table could put in
     * its place.
     */
    IPTV_TEXT_AUDIO,
    IPTV_TEXT_SUBTITLES,
    /* Feminine in Portuguese, agreeing with "legendas" - which is why this is a
     * whole string and not a shared word for "off". */
    IPTV_TEXT_SUBTITLES_OFF,
    IPTV_TEXT_NO_SUBTITLES,
    IPTV_TEXT_ONE_AUDIO,
    /* Held on screen for as long as a local video is paused. */
    IPTV_TEXT_PAUSED,

    IPTV_TEXT_COUNT
} iptv_text_id;

/*
 * Sets the language used by every later iptv_text() call. Out of range values
 * select English rather than being refused: a wrong option string is not worth
 * a blank guide.
 */
void iptv_text_set_language(iptv_language language);
iptv_language iptv_text_language(void);

/* Never returns NULL. Falls back to English, then to an empty string. */
const char *iptv_text(iptv_text_id id);

#endif
