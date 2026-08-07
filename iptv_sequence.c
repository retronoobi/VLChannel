#include "iptv_sequence.h"

static uint64_t started_ms = 0;      /* 0 means "never reached Playing" */
static int      chain = 0;
static bool     held = false;

void iptv_seq_opening(void) {
    /*
     * Cleared on every open, and this line is the guard.
     *
     * Left over from the entry before, the timestamp would say that the video
     * which has just failed instantly had been playing for however long the
     * previous one ran. The budget would clear on every step and the core would
     * walk the whole list at full speed - precisely the failure this module
     * exists to prevent, produced by the module meant to prevent it.
     */
    started_ms = 0;
}

void iptv_seq_playing(uint64_t now_ms) {
    /*
     * First arrival only. libVLC re-enters Playing after buffering, and taking
     * the later timestamp would keep resetting the clock on a stuttering
     * stream, so a video that struggled for a minute would look like one that
     * never played.
     *
     * now_ms of zero would be indistinguishable from "never played"; one
     * millisecond of error, once, at the very start of the process.
     */
    if (started_ms == 0)
        started_ms = now_ms ? now_ms : 1;
}

uint64_t iptv_seq_played_ms(uint64_t now_ms) {
    if (started_ms == 0 || now_ms < started_ms)
        return 0;
    return now_ms - started_ms;
}

iptv_seq_action iptv_seq_ended(uint64_t now_ms) {
    if (iptv_seq_played_ms(now_ms) >= IPTV_SEQ_WATCHED_MS) {
        chain = 0;
        held = false;
        return IPTV_SEQ_ADVANCE;
    }

    chain++;
    if (chain <= IPTV_SEQ_CHAIN_LIMIT)
        return IPTV_SEQ_ADVANCE;

    /*
     * Held, and said once. The state machine can produce several ends for one
     * dead entry - an error, then a stop from the picture timeout - and a core
     * that repeats the same paragraph for each of them buries the one line
     * that matters.
     */
    if (held)
        return IPTV_SEQ_QUIET;
    held = true;
    return IPTV_SEQ_HOLD;
}

int iptv_seq_chain(void) {
    return chain;
}

size_t iptv_seq_next(size_t current, size_t count) {
    if (count == 0)
        return 0;
    size_t next = current + 1;
    return next >= count ? 0 : next;
}
