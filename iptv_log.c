#include "iptv_log.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

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
static unsigned long long log_now_ms(void) {
#ifdef _WIN32
    static unsigned long long origin = 0;
    unsigned long long now = (unsigned long long)GetTickCount64();
    if (origin == 0) origin = now;
    return now - origin;
#else
    static unsigned long long origin = 0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    unsigned long long now =
        (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)ts.tv_nsec / 1000000ull;
    if (origin == 0) origin = now;
    return now - origin;
#endif
}

static FILE *log_file = NULL;
static char log_file_path[4096] = {0};

void iptv_log_open(const char *path) {
    iptv_log_close();
    if (!path || !path[0])
        return;

    /*
     * Truncated on every start, not appended. A session log that answers "what
     * happened this time" is worth more than an archive nobody reads, and the
     * [VLC-LOG] echo makes this file grow by hundreds of kilobytes per session.
     */
    log_file = fopen(path, "w");
    if (!log_file) {
        fprintf(stderr, "[VLChannel] Cannot open the log file %s\n", path);
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
}

void iptv_log_close(void) {
    if (log_file) {
        fclose(log_file);
        log_file = NULL;
    }
    log_file_path[0] = '\0';
}

const char *iptv_log_path(void) {
    return log_file_path;
}

void iptv_log(const char *format, ...) {
    va_list arguments;
    unsigned long long stamp = log_now_ms();

    fprintf(stderr, "[%8llu ms] ", stamp);
    va_start(arguments, format);
    vfprintf(stderr, format, arguments);
    va_end(arguments);

    if (!log_file)
        return;

    fprintf(log_file, "[%8llu ms] ", stamp);
    va_start(arguments, format);
    vfprintf(log_file, format, arguments);
    va_end(arguments);

    /*
     * Flushed every so often rather than on every line. libVLC's echo runs to
     * hundreds of thousands of lines in a session, and flushing each one turns
     * the log into a disk-bound brake on the decoder threads that produce it.
     * The counter is written from several threads without a lock: the only
     * consequence of a lost increment is a flush landing a few lines late.
     */
    static unsigned since_flush = 0;
    if (++since_flush >= 64) {
        since_flush = 0;
        fflush(log_file);
    }
}
