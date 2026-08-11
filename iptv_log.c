#include "iptv_log.h"

#include <stdio.h>
#include <stdarg.h>
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

void iptv_log(const char *format, ...) {
    va_list arguments;
    pthread_mutex_lock(&log_mutex);
    unsigned long long stamp = log_now_ms();

    fprintf(stderr, "[%8llu ms] ", stamp);
    va_start(arguments, format);
    vfprintf(stderr, format, arguments);
    va_end(arguments);

    if (!log_file) {
        pthread_mutex_unlock(&log_mutex);
        return;
    }

    fprintf(log_file, "[%8llu ms] ", stamp);
    va_start(arguments, format);
    vfprintf(log_file, format, arguments);
    va_end(arguments);

    /*
     * Flushed every so often rather than on every line. libVLC's echo runs to
     * hundreds of thousands of lines in a session, and flushing each one turns
     * the log into a disk-bound brake on the decoder threads that produce it.
     * The record lock also makes the flush cadence exact.
     */
    if (++since_flush >= 64) {
        since_flush = 0;
        fflush(log_file);
    }
    pthread_mutex_unlock(&log_mutex);
}
