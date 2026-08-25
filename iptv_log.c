#include "iptv_log.h"
#include "iptv_scrub.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

/*
 * Every line carries the milliseconds since the core started, and this is not
 * cosmetic.
 *
 * Without it, the only way to ask "how long after the picture did the sound
 * start" was to count the distance between two line numbers - and line numbers
 * measure how talkative libVLC was, not time. A whole diagnosis was built on
 * that, comparing queue lengths across two sessions, when the question that
 * actually mattered was the order and the spacing of three events inside one
 * session. A log that cannot be measured invites being interpreted.
 */
/* Guards the clock origin, file state, complete records and flush cadence. */
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned long long log_origin = 0;
static unsigned since_flush = 0;

/* Called with log_mutex held. */
static unsigned long long log_now_ms(void) {
#ifdef _WIN32
    unsigned long long now = (unsigned long long)GetTickCount64();
    if (log_origin == 0) log_origin = now;
    return now - log_origin;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    unsigned long long now =
        (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)ts.tv_nsec / 1000000ull;
    if (log_origin == 0) log_origin = now;
    return now - log_origin;
#endif
}

static FILE *log_file = NULL;
static char log_file_path[4096] = {0};

/* Called with log_mutex held. */
static void close_log_file(void) {
    if (log_file) {
        fclose(log_file);
        log_file = NULL;
    }
    log_file_path[0] = '\0';
    since_flush = 0;
}

void iptv_log_open(const char *path) {
    pthread_mutex_lock(&log_mutex);
    close_log_file();
    if (!path || !path[0]) {
        pthread_mutex_unlock(&log_mutex);
        return;
    }

    /*
     * Truncated on every start, not appended. A session log that answers "what
     * happened this time" is worth more than an archive nobody reads, and the
     * [VLC-LOG] echo makes this file grow by hundreds of kilobytes per session.
     */
    log_file = fopen(path, "w");
    if (!log_file) {
        fprintf(stderr, "[VLChannel] Cannot open the log file %s\n", path);
        pthread_mutex_unlock(&log_mutex);
        return;
    }

    snprintf(log_file_path, sizeof(log_file_path), "%s", path);

    /*
     * No setvbuf here, and the reason is worth writing down.
     *
     * This started as setvbuf(log_file, NULL, _IOLBF, 0), to keep the file line
     * buffered so a crash would preserve the lines that led to it. On Windows
     * the CRT requires a size of at least 2, and an out-of-range argument does
     * not return an error: it calls the invalid parameter handler, which by
     * default terminates the process. Enabling the log option therefore killed
     * RetroArch outright - a diagnostic feature that crashed the thing it was
     * meant to diagnose.
     *
     * Ordinary buffering plus a periodic flush gives the same guarantee without
     * the trap.
     */
    pthread_mutex_unlock(&log_mutex);
}

void iptv_log_close(void) {
    pthread_mutex_lock(&log_mutex);
    close_log_file();
    pthread_mutex_unlock(&log_mutex);
}

const char *iptv_log_path(void) {
    return log_file_path;
}

/*
 * Long enough for the sentences this core writes and for anything libVLC says,
 * which it caps at a kilobyte before handing it over. What overflows is a URL,
 * and a URL is exactly the thing that must not be written half-scrubbed.
 */
#define LOG_LINE_MAX 2048

void iptv_log(const char *format, ...) {
    /*
     * Formatted once, here, instead of twice into two streams further down.
     *
     * The reason is iptv_scrub: the line has to exist as a string before the
     * viewer's address can be taken out of it, and a line written straight to a
     * stream has never been a string. Doing it this way also removes one of the
     * two formatting passes the old version did, so the logger is not slower
     * for having become private.
     */
    char stack[LOG_LINE_MAX];
    char *line = stack;
    char *heap = NULL;

    va_list arguments;
    va_start(arguments, format);
    int needed = vsnprintf(stack, sizeof(stack), format, arguments);
    va_end(arguments);

    /*
     * A line that does not fit gets one allocation. It is rare - a PlutoTV URL
     * is three thousand characters and nothing else comes close - and the
     * alternative is truncating precisely the lines that carry secrets, which
     * would leave the tail of a URL in the file with no field ever scrubbed.
     *
     * If the allocation fails the truncated copy is used. A short log line is
     * worth more than a logger that can fail, and it is still scrubbed.
     */
    if (needed >= (int)sizeof(stack)) {
        heap = (char *)malloc((size_t)needed + 1);
        if (heap) {
            va_start(arguments, format);
            vsnprintf(heap, (size_t)needed + 1, format, arguments);
            va_end(arguments);
            line = heap;
        }
    }

    iptv_scrub(line);

    pthread_mutex_lock(&log_mutex);
    unsigned long long stamp = log_now_ms();

    /* "%s" and not the line itself: it is content now, and a URL with a per
     * cent sign in it must not be read as a conversion. */
    fprintf(stderr, "[%8llu ms] %s", stamp, line);

    if (log_file) {
        fprintf(log_file, "[%8llu ms] %s", stamp, line);

        /*
         * Flushed every so often rather than on every line. libVLC's echo runs
         * to hundreds of thousands of lines in a session, and flushing each one
         * turns the log into a disk-bound brake on the decoder threads that
         * produce it. The record lock also makes the flush cadence exact.
         */
        if (++since_flush >= 64) {
            since_flush = 0;
            fflush(log_file);
        }
    }
    pthread_mutex_unlock(&log_mutex);

    free(heap);
}
