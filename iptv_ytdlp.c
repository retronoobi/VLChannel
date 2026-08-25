#include "iptv_ytdlp.h"
#include "iptv_child.h"
#include "iptv_log.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define YT_URL_MAX  4096
#define YT_PATH_MAX 4096
#define YT_TIMEOUT_MS 30000

/* The directory plus a file name, so it cannot be the same size as the
 * directory. The compiler pointed this one out too. */
#define YT_EXE_MAX (YT_PATH_MAX + 32)

static char directory[YT_PATH_MAX] = {0};
static char executable[YT_EXE_MAX] = {0};
static char cookies_file[YT_EXE_MAX] = {0};
static bool executable_present = false;

/*
 * One job at a time, and a generation number.
 *
 * The viewer can change channel while a resolution is running. The thread
 * cannot be safely killed mid-process, so it is left to finish and its answer
 * is thrown away: the generation it was started under no longer matches, and a
 * correct answer about a video nobody is watching is worse than none, because
 * it would open the wrong thing.
 */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t worker;
static bool worker_running = false;

static unsigned generation = 0;
static iptv_yt_state state = IPTV_YT_IDLE;
static char result_video[YT_URL_MAX];
static char result_audio[YT_URL_MAX];

typedef struct {
    unsigned generation;
    bool live;                    /* ask for a broadcast, not for a file */
    char url[YT_URL_MAX];
} job;

void iptv_ytdlp_set_directory(const char *dir) {
    snprintf(directory, sizeof(directory), "%s", dir ? dir : "");
    executable_present = false;
    executable[0] = '\0';
    cookies_file[0] = '\0';
    if (!directory[0])
        return;

#ifdef _WIN32
    snprintf(executable, sizeof(executable), "%s\\yt-dlp.exe", directory);
    snprintf(cookies_file, sizeof(cookies_file), "%s\\cookies.txt", directory);
#else
    snprintf(executable, sizeof(executable), "%s/yt-dlp", directory);
    snprintf(cookies_file, sizeof(cookies_file), "%s/cookies.txt", directory);
#endif

    FILE *probe = fopen(executable, "rb");
    if (probe) {
        fclose(probe);
        executable_present = true;
        iptv_log("[VLChannel] yt-dlp found at %s\n", executable);
    } else {
        iptv_log("[VLChannel] yt-dlp not at %s; YouTube entries will use the "
                 "proxy instead\n", executable);
    }
}

bool iptv_ytdlp_available(void) {
    return executable_present;
}

/* Checked at retry time so cookies.txt can be added without restarting the core. */
static bool cookies_available(void) {
    if (!cookies_file[0])
        return false;
    FILE *probe = fopen(cookies_file, "rb");
    if (!probe)
        return false;
    fclose(probe);
    return true;
}

/*
 * One line out of the captured text, cut to fit.
 *
 * The cut is deliberate and is not a loss: what is wanted is a URL, and this
 * buffer is already four kilobytes. Anything that overflows it is not an
 * address yt-dlp meant to give, and the check further down - does it start with
 * "http" - throws it out. Writing the bound rather than relying on snprintf's
 * also lets the compiler see it, which is worth a warning nobody has to decide
 * whether to believe.
 */
static void take_line(char *out, size_t out_size, const char *text) {
    size_t length = strcspn(text, "\r\n");
    if (length >= out_size)
        length = out_size - 1;
    memcpy(out, text, length);
    out[length] = '\0';
}

static bool generation_is_current(unsigned value) {
    pthread_mutex_lock(&lock);
    bool current = value == generation;
    pthread_mutex_unlock(&lock);
    return current;
}

/* The shape iptv_child asks for. A channel change bumps the generation, and
 * that is what turns the change into a real cancellation rather than a
 * pthread_join() waiting on a resolver nobody is listening to. */
static bool generation_expired(void *cookie) {
    return !generation_is_current(*(const unsigned *)cookie);
}

/*
 * Runs yt-dlp and fills the two lines. Returns false when nothing came back.
 * Called on the worker thread only.
 *
 * The process itself - the pipe, the timeout, the cancellation - belongs to
 * iptv_child. What is left here is the part that is about yt-dlp: which
 * arguments it takes, and what its answer is supposed to look like.
 */
static bool run_ytdlp(const char *url, const char *format, const char *cookies,
                      unsigned work_generation, char *video, char *audio) {
    video[0] = audio[0] = '\0';

    const char *arguments[10];
    size_t count = 0;
    arguments[count++] = executable;
    arguments[count++] = "-f";
    arguments[count++] = format;
    if (cookies && cookies[0]) {
        arguments[count++] = "--cookies";
        arguments[count++] = cookies;
    }
    arguments[count++] = "--get-url";
    arguments[count++] = "--no-playlist";
    arguments[count++] = "--no-warnings";
    /* Standalone, so a playlist's text can only be read as one URL argument
     * and never as a yt-dlp option. */
    arguments[count++] = "--";
    arguments[count++] = url;

    char output[YT_URL_MAX * 2];
    unsigned checked = work_generation;
    iptv_child_result ran =
        iptv_child_capture(executable, arguments, count, YT_TIMEOUT_MS,
                           generation_expired, &checked,
                           output, sizeof(output));

    switch (ran) {
    case IPTV_CHILD_OK:
        break;
    case IPTV_CHILD_CANCELLED:
        return false;
    case IPTV_CHILD_TIMED_OUT:
        iptv_log("[VLChannel] yt-dlp timed out after %u seconds\n",
                 YT_TIMEOUT_MS / 1000);
        return false;
    case IPTV_CHILD_PIPE_FAILED:
        iptv_log("[VLChannel] yt-dlp output pipe failed\n");
        return false;
    default:
        return false;
    }

    const char *first = output;
    const char *first_end = strchr(first, '\n');
    take_line(video, YT_URL_MAX, first);
    take_line(audio, YT_URL_MAX, first_end ? first_end + 1 : "");

    /*
     * Only an address counts. yt-dlp writes its diagnostics to stderr, which on
     * Windows shares this pipe, so a failure can arrive as a line of prose - and
     * a line of prose handed to libVLC becomes "unsupported protocol", which
     * sends whoever reads the log looking in the wrong place.
     */
    if (strncmp(video, "http", 4) != 0) {
        if (video[0])
            iptv_log("[VLChannel] yt-dlp said: %.200s\n", video);
        video[0] = audio[0] = '\0';
        return false;
    }
    if (strncmp(audio, "http", 4) != 0)
        audio[0] = '\0';
    return true;
}

static void *worker_main(void *argument) {
    job *work = (job *)argument;

    char video[YT_URL_MAX];
    char audio[YT_URL_MAX];
    /*
     * Chosen once, here, and used for both attempts. Asking for a file on the
     * first try and a broadcast on the second would make the cookies retry a
     * different question rather than the same question asked again, and the log
     * would then be describing two experiments as one.
     */
    const char *format = work->live ? IPTV_YT_LIVE_FORMAT : IPTV_YT_FORMAT;
    bool ok = run_ytdlp(work->url, format, NULL, work->generation, video, audio);

    if (!ok && generation_is_current(work->generation) &&
        cookies_available()) {
        iptv_log("[VLChannel] yt-dlp did not resolve without cookies; "
                 "retrying with system/vlchannel/cookies.txt\n");
        ok = run_ytdlp(work->url, format, cookies_file, work->generation,
                       video, audio);
        if (!ok && generation_is_current(work->generation))
            iptv_log("[VLChannel] yt-dlp also failed with cookies.txt\n");
    }

    pthread_mutex_lock(&lock);
    /* Only if the world has not moved on. */
    if (work->generation == generation) {
        if (ok) {
            memcpy(result_video, video, sizeof(result_video));
            memcpy(result_audio, audio, sizeof(result_audio));
            state = IPTV_YT_DONE;
        } else {
            state = IPTV_YT_FAILED;
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

void iptv_ytdlp_begin(const char *youtube_url, bool live) {
    iptv_ytdlp_cancel();
    join_previous();

    if (!executable_present || !youtube_url || !youtube_url[0]) {
        pthread_mutex_lock(&lock);
        state = IPTV_YT_FAILED;
        pthread_mutex_unlock(&lock);
        return;
    }

    job *work = (job *)calloc(1, sizeof(*work));
    if (!work) {
        pthread_mutex_lock(&lock);
        state = IPTV_YT_FAILED;
        pthread_mutex_unlock(&lock);
        return;
    }

    pthread_mutex_lock(&lock);
    generation++;
    work->generation = generation;
    work->live = live;
    snprintf(work->url, sizeof(work->url), "%s", youtube_url);
    result_video[0] = result_audio[0] = '\0';
    state = IPTV_YT_WORKING;
    pthread_mutex_unlock(&lock);

    if (pthread_create(&worker, NULL, worker_main, work) != 0) {
        free(work);
        pthread_mutex_lock(&lock);
        state = IPTV_YT_FAILED;
        pthread_mutex_unlock(&lock);
        return;
    }
    worker_running = true;
}

iptv_yt_state iptv_ytdlp_poll(void) {
    pthread_mutex_lock(&lock);
    iptv_yt_state now = state;
    pthread_mutex_unlock(&lock);
    return now;
}

const char *iptv_ytdlp_url(void) {
    return result_video;
}

const char *iptv_ytdlp_audio_url(void) {
    return result_audio;
}

void iptv_ytdlp_cancel(void) {
    pthread_mutex_lock(&lock);
    generation++;                 /* whatever is running answers about nothing */
    state = IPTV_YT_IDLE;
    result_video[0] = result_audio[0] = '\0';
    pthread_mutex_unlock(&lock);
}

void iptv_ytdlp_shutdown(void) {
    iptv_ytdlp_cancel();
    join_previous();
    directory[0] = executable[0] = cookies_file[0] = '\0';
    executable_present = false;
}
