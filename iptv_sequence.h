#ifndef IPTV_SEQUENCE_H
#define IPTV_SEQUENCE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/*
 * When a video ends, what should happen next.
 *
 * A YouTube entry in a channel list is a video: it ends, and the end is not a
 * fault. The list is meant to keep running, so the next entry starts and the
 * bottom wraps round to the top - which is what makes a list of videos behave
 * like a channel that is always on.
 *
 * The danger is the failure case, and it is why this is a module of its own
 * rather than four lines inside the state machine.
 *
 * A proxy that is down answers immediately: the stream "ends" at once, the
 * advance fires, the next one ends at once too. A list of two hundred videos
 * would be walked end to end in a couple of seconds, tearing down and rebuilding
 * libVLC's input two hundred times, and the log would show two hundred channel
 * changes when the truth is one dead host. Nothing in that sequence is
 * obviously wrong while reading it; it is only wrong at speed.
 *
 * So the rule is: a video that ends without having played counts against a
 * budget, and a video that actually played spends long enough to clear it. This
 * is testable arithmetic over a clock, and ferramentas/teste-sequencia.c drives
 * it through the sequences that would otherwise need a broken server to
 * reproduce.
 */

typedef enum {
    IPTV_SEQ_ADVANCE,   /* start the next entry */
    IPTV_SEQ_HOLD,      /* budget spent: stay put, and say so once */
    IPTV_SEQ_QUIET      /* budget spent and already said */
} iptv_seq_action;

/*
 * How many videos may end in a row without having played.
 *
 * Three steps over a video that has genuinely been removed, and stops far short
 * of a list of any size.
 */
#define IPTV_SEQ_CHAIN_LIMIT 3

/*
 * How long a video must play to count as watched.
 *
 * Long enough that no failure reaches it - a refused connection ends in
 * milliseconds - and short enough that every real video passes it.
 */
#define IPTV_SEQ_WATCHED_MS 5000

/* A channel is being opened: nothing has played yet. */
void iptv_seq_opening(void);

/* This channel reached Playing. */
void iptv_seq_playing(uint64_t now_ms);

/* This channel's video ended, errored, or gave up. */
iptv_seq_action iptv_seq_ended(uint64_t now_ms);

/* How long the current entry played before ending, in ms. For the log. */
uint64_t iptv_seq_played_ms(uint64_t now_ms);

/* How many have ended in a row without playing. For the log and the tests. */
int iptv_seq_chain(void);

/* The next index, wrapping. Separate from the rest because "what follows the
 * last entry" is a decision and not an increment. */
size_t iptv_seq_next(size_t current, size_t count);

#endif
