#include "iptv_streamlink.h"
#include "iptv_child.h"
#include "iptv_log.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SL_URL_MAX  4096
#define SL_PATH_MAX 4096
#define SL_TIMEOUT_MS 30000

/* The directory plus the rest of the path to the program inside it. */
#define SL_EXE_MAX (SL_PATH_MAX + 64)

static char executable[SL_EXE_MAX] = {0};
static bool executable_present = false;

/*
 * One job at a time, and a generation number - the same arrangement as
 * iptv_ytdlp, and for the same reason. A resolution that is no longer wanted
 * cannot be killed from outside, so it is left to finish and its answer is
 * discarded because the generation it was started under has moved on.
 */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t worker;
static bool worker_running = false;

static unsigned generation = 0;
static iptv_sl_state state = IPTV_SL_IDLE;
static char result_url[SL_URL_MAX];

typedef struct {
    unsigned generation;
    char url[SL_URL_MAX];
} job;

void iptv_streamlink_set_directory(const char *vlchannel_directory) {
    executable_present = false;
    executable[0] = '\0';
    if (!vlchannel_directory || !vlchannel_directory[0])
        return;

#ifdef _WIN32
    snprintf(executable, sizeof(executable), "%s\\streamlink\\bin\\streamlink.exe",
             vlchannel_directory);
#else
    snprintf(executable, sizeof(executable), "%s/streamlink/bin/streamlink",
             vlchannel_directory);
#endif

    FILE *probe = fopen(executable, "rb");
    if (probe) {
        fclose(probe);
        executable_present = true;
        iptv_log("[VLChannel] streamlink found at %s\n", executable);
    } else {
        iptv_log("[VLChannel] streamlink not at %s; platform entries will have "
                 "only yt-dlp to fall back on\n", executable);
    }
}

bool iptv_streamlink_available(void) {
    return executable_present;
}

static size_t strip_newline(char *s) {
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r'))
        s[--len] = '\0';
    return len;
}

static bool generation_is_current(unsigned value) {
    pthread_mutex_lock(&lock);
    bool current = value == generation;
    pthread_mutex_unlock(&lock);
    return current;
}

static bool generation_expired(void *cookie) {
    return !generation_is_current(*(const unsigned *)cookie);
}

/*
 * Runs streamlink and fills one address. Called on the worker thread only.
 *
 * `--url` and `--default-stream` rather than the two positional arguments they
 * replace. A positional URL that happened to begin with a dash would be read as
 * an option, and a playlist is text somebody else wrote.
 *
 * No `--quiet`: it reads as the obvious way to keep the log out of the answer,
 * and it is the one flag that must not be passed. It silences the console
 * itself, and `--stream-url` prints through that console - so the run would
 * succeed and return nothing at all. It is also unnecessary: streamlink already
 * turns its logger off whenever --stream-url is given.
 */
static bool run_streamlink(const char *url, unsigned work_generation,
                           char *out) {
    out[0] = '\0';

    const char *arguments[8];
    size_t count = 0;
    arguments[count++] = executable;
    arguments[count++] = "--stream-url";
    arguments[count++] = "--default-stream";
    arguments[count++] = IPTV_SL_STREAMS;
    arguments[count++] = "--url";
    arguments[count++] = url;

    char output[SL_URL_MAX * 2];
    unsigned checked = work_generation;
    iptv_child_result ran =
        iptv_child_capture(executable, arguments, count, SL_TIMEOUT_MS,
                           generation_expired, &checked,
                           output, sizeof(output));

    switch (ran) {
    case IPTV_CHILD_OK:
        break;
    case IPTV_CHILD_CANCELLED:
        return false;
    case IPTV_CHILD_TIMED_OUT:
        iptv_log("[VLChannel] streamlink timed out after %u seconds\n",
                 SL_TIMEOUT_MS / 1000);
        return false;
    case IPTV_CHILD_PIPE_FAILED:
        iptv_log("[VLChannel] streamlink output pipe failed\n");
        return false;
    default:
        return false;
    }

    /*
     * The answer is one line. Anything else on the pipe is a complaint - on
     * Windows stderr shares it - so the first line that looks like an address
     * is taken and the rest is reported rather than passed on. libVLC given a
     * line of prose says "unsupported protocol", which sends whoever reads the
     * log looking in the wrong place entirely.
     */
    char *line = output;
    while (line && *line) {
        char *end = strchr(line, '\n');
        if (end)
            *end = '\0';
        strip_newline(line);
        if (strncmp(line, "http", 4) == 0) {
            snprintf(out, SL_URL_MAX, "%s", line);
            return true;
        }
        if (line[0])
            iptv_log("[VLChannel] streamlink said: %.200s\n", line);
        line = end ? end + 1 : NULL;
    }

    out[0] = '\0';
    return false;
}

static void *worker_main(void *argument) {
    job *work = (job *)argument;

    char url[SL_URL_MAX];
    bool ok = run_streamlink(work->url, work->generation, url);

    pthread_mutex_lock(&lock);
    if (work->generation == generation) {
        if (ok) {
            memcpy(result_url, url, sizeof(result_url));
            state = IPTV_SL_DONE;
        } else {
            state = IPTV_SL_FAILED;
        }
    }
    pthread_mutex_unlock(&lock);

    free(work);
    return NULL;
}

static void join_previous(void) {
    if (worker_running) {
        pthread_join(worker, NULL);
        worker_running = false;
    }
}

void iptv_streamlink_begin(const char *url) {
    iptv_streamlink_cancel();
    join_previous();

    if (!executable_present || !url || !url[0]) {
        pthread_mutex_lock(&lock);
        state = IPTV_SL_FAILED;
        pthread_mutex_unlock(&lock);
        return;
    }

    job *work = (job *)calloc(1, sizeof(*work));
    if (!work) {
        pthread_mutex_lock(&lock);
        state = IPTV_SL_FAILED;
        pthread_mutex_unlock(&lock);
        return;
    }

    pthread_mutex_lock(&lock);
    generation++;
    work->generation = generation;
    snprintf(work->url, sizeof(work->url), "%s", url);
    result_url[0] = '\0';
    state = IPTV_SL_WORKING;
    pthread_mutex_unlock(&lock);

    if (pthread_create(&worker, NULL, worker_main, work) != 0) {
        free(work);
        pthread_mutex_lock(&lock);
        state = IPTV_SL_FAILED;
        pthread_mutex_unlock(&lock);
        return;
    }
    worker_running = true;
}

iptv_sl_state iptv_streamlink_poll(void) {
    pthread_mutex_lock(&lock);
    iptv_sl_state now = state;
    pthread_mutex_unlock(&lock);
    return now;
}

const char *iptv_streamlink_url(void) {
    return result_url;
}

void iptv_streamlink_cancel(void) {
    pthread_mutex_lock(&lock);
    generation++;                 /* whatever is running answers about nothing */
    state = IPTV_SL_IDLE;
    result_url[0] = '\0';
    pthread_mutex_unlock(&lock);
}

void iptv_streamlink_shutdown(void) {
    iptv_streamlink_cancel();
    join_previous();
    executable[0] = '\0';
    executable_present = false;
}
