#ifndef IPTV_CHILD_H
#define IPTV_CHILD_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Running a program and reading what it printed.
 *
 * This exists because there are now two of them - yt-dlp and streamlink - and
 * the hard parts are identical for both while having nothing to do with either.
 * Everything below was written for yt-dlp first and moved here unchanged when
 * the second tool arrived; the comments came with it, because the reasons are
 * still the reasons.
 *
 * Three things here are not style choices:
 *
 * 1. On Windows it uses CreateProcess and never popen(). The comment in the
 *    original VLC libretro core - Krisreto's, which this follows - records why:
 *    popen() on MinGW calls AllocConsole, Windows serialises console creation
 *    per process, and the render loop freezes even though the call was made on a
 *    background thread. A bug that only appears on the target platform, found by
 *    someone else, and free to avoid.
 *
 * 2. The pipe is polled, never blocked on. ReadFile on an anonymous pipe waits
 *    forever, so an earlier version's advertised timeout was never reached when
 *    the child printed nothing at all. Every read here is limited to bytes that
 *    PeekNamedPipe has already proved are there.
 *
 * 3. It can be cancelled. A viewer who changes channel is not waiting for an
 *    answer about the previous one, and a resolver that cannot be abandoned is a
 *    resolver the shutdown path has to wait for.
 *
 * Nothing in here knows what a URL is. It runs a program, it collects bytes,
 * and it says what happened.
 */

/*
 * Asked between polls, on the calling thread. Returning true abandons the run:
 * the child is terminated and IPTV_CHILD_CANCELLED comes back.
 *
 * May be NULL, which means the run cannot be cancelled.
 */
typedef bool (*iptv_child_cancelled)(void *cookie);

typedef enum {
    IPTV_CHILD_OK = 0,       /* it ran; `out` holds what it printed */
    IPTV_CHILD_NOT_STARTED,  /* the program could not be started at all */
    IPTV_CHILD_TIMED_OUT,
    IPTV_CHILD_CANCELLED,    /* the caller asked for it to be abandoned */
    IPTV_CHILD_PIPE_FAILED   /* the pipe broke before anything was captured */
} iptv_child_result;

/*
 * Runs `executable` with `arguments`, and writes what it printed into `out`.
 *
 * `arguments[0]` is argv[0] and is conventionally the executable itself; the
 * path is passed separately as well, so the child's command line never has to
 * be parsed to find out what to run.
 *
 * Output longer than `out_size` is kept from the beginning and the rest is
 * dropped. Every caller here wants the first line or two, and a tool that
 * answers with a megabyte has already failed to answer the question.
 *
 * On Windows the child's stderr shares the pipe with its stdout, so a
 * diagnostic arrives mixed in with the answer. That is deliberate: a tool's
 * complaint is the most useful thing it produces on a bad day, and the caller
 * is expected to check that what it got looks like an answer rather than to
 * assume it. Elsewhere stderr is discarded, because popen() gives only one.
 *
 * `out` is always terminated, including on failure, where it is left empty.
 */
iptv_child_result iptv_child_capture(const char *executable,
                                     const char *const *arguments,
                                     size_t count,
                                     unsigned timeout_ms,
                                     iptv_child_cancelled cancelled,
                                     void *cookie,
                                     char *out, size_t out_size);

#endif
