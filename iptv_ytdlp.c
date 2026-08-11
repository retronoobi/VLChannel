#include "iptv_ytdlp.h"
#include "iptv_ytdlp_command.h"
#include "iptv_log.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

#define YT_URL_MAX  4096
#define YT_PATH_MAX 4096
#define YT_TIMEOUT_MS 30000

/* The directory plus a file name, so it cannot be the same size as the
 * directory. The compiler pointed this one out too. */
#define YT_EXE_MAX (YT_PATH_MAX + 32)

/*
 * Quoting can double backslashes and adds delimiters around every argument.
 * Size for that worst case, not just for the unescaped input strings.
 */
#define YT_CMD_MAX (YT_EXE_MAX * 4u + YT_URL_MAX * 2u + 512u)

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

/*
 * Runs yt-dlp and fills the two lines. Returns false when nothing came back.
 * Called on the worker thread only.
 */
static bool run_ytdlp(const char *url, const char *cookies,
                      unsigned work_generation, char *video, char *audio) {
    video[0] = audio[0] = '\0';

#ifdef _WIN32
    /*
     * CreateProcess with an anonymous pipe, and CREATE_NO_WINDOW.
     *
     * Never popen(): on MinGW it calls AllocConsole, Windows serialises console
     * creation across the whole process, and the render loop freezes even
     * though this runs on a background thread. See iptv_ytdlp.h.
     */
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE pipe_rd = NULL, pipe_wr = NULL;
    if (!CreatePipe(&pipe_rd, &pipe_wr, &sa, 0))
        return false;
    SetHandleInformation(pipe_rd, HANDLE_FLAG_INHERIT, 0);

    char cmd[YT_CMD_MAX];
    if (!iptv_ytdlp_build_windows_command(
            cmd, sizeof(cmd), executable, IPTV_YT_FORMAT, cookies, url)) {
        CloseHandle(pipe_wr);
        CloseHandle(pipe_rd);
        iptv_log("[VLChannel] yt-dlp command line is too long or invalid\n");
        return false;
    }

    STARTUPINFOA si = {0};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = pipe_wr;
    si.hStdError = pipe_wr;
    si.hStdInput = NULL;

    PROCESS_INFORMATION pi = {0};
    /* Supplying lpApplicationName separately removes executable-path parsing
     * from the command line. argv[0] remains present in cmd for the child. */
    BOOL ok = CreateProcessA(executable, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                             NULL, NULL, &si, &pi);
    CloseHandle(pipe_wr);
    if (!ok) {
        CloseHandle(pipe_rd);
        iptv_log("[VLChannel] yt-dlp could not be started (error %lu)\n",
                 (unsigned long)GetLastError());
        return false;
    }

    /*
     * ReadFile on an anonymous pipe is blocking. The old implementation called
     * it before WaitForSingleObject(), so the advertised timeout was never
     * reached when yt-dlp produced no output. Poll the pipe and process instead:
     * every read is limited to bytes PeekNamedPipe already proved are there.
     * The generation check also turns a channel change into a real cancellation
     * rather than a pthread_join() waiting for a stale resolver forever.
     */
    char output[YT_URL_MAX * 2] = {0};
    size_t output_used = 0;
    ULONGLONG deadline = GetTickCount64() + YT_TIMEOUT_MS;
    bool timed_out = false;
    bool cancelled = false;
    bool pipe_failed = false;

    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe_rd, NULL, 0, NULL, &available, NULL)) {
            DWORD error = GetLastError();
            if (error != ERROR_BROKEN_PIPE)
                pipe_failed = true;
            available = 0;
        }

        while (available > 0) {
            char chunk[512];
            DWORD want = available < sizeof(chunk)
                             ? available : (DWORD)sizeof(chunk);
            DWORD got = 0;
            if (!ReadFile(pipe_rd, chunk, want, &got, NULL) || got == 0) {
                pipe_failed = true;
                break;
            }

            size_t room = sizeof(output) - 1 - output_used;
            size_t copy = got < room ? (size_t)got : room;
            if (copy > 0) {
                memcpy(output + output_used, chunk, copy);
                output_used += copy;
                output[output_used] = '\0';
            }
            available -= got;
        }

        DWORD process_state = WaitForSingleObject(pi.hProcess, 0);
        if (process_state == WAIT_OBJECT_0) {
            DWORD remaining = 0;
            if (!PeekNamedPipe(pipe_rd, NULL, 0, NULL, &remaining, NULL) ||
                remaining == 0)
                break;
        } else if (process_state == WAIT_FAILED) {
            pipe_failed = true;
            break;
        }

        if (!generation_is_current(work_generation)) {
            cancelled = true;
            break;
        }
        if (GetTickCount64() >= deadline) {
            timed_out = true;
            break;
        }
        if (pipe_failed)
            break;

        Sleep(10);
    }

    if (timed_out || cancelled || pipe_failed) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 2000);
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(pipe_rd);

    if (cancelled)
        return false;
    if (timed_out) {
        iptv_log("[VLChannel] yt-dlp timed out after %u seconds\n",
                 YT_TIMEOUT_MS / 1000);
        return false;
    }
    if (pipe_failed && output_used == 0) {
        iptv_log("[VLChannel] yt-dlp output pipe failed\n");
        return false;
    }

    char *first_end = strchr(output, '\n');
    if (first_end) {
        *first_end = '\0';
        snprintf(video, YT_URL_MAX, "%s", output);
        char *second = first_end + 1;
        char *second_end = strchr(second, '\n');
        if (second_end)
            *second_end = '\0';
        snprintf(audio, YT_URL_MAX, "%s", second);
    } else {
        snprintf(video, YT_URL_MAX, "%s", output);
    }
#else
    (void)work_generation;
    char cmd[YT_CMD_MAX];
    if (cookies) {
        snprintf(cmd, sizeof(cmd),
                 "\"%s\" -f '%s' --cookies \"%s\" --get-url --no-playlist "
                 "--no-warnings \"%s\" 2>/dev/null",
                 executable, IPTV_YT_FORMAT, cookies, url);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "\"%s\" -f '%s' --get-url --no-playlist --no-warnings "
                 "\"%s\" 2>/dev/null",
                 executable, IPTV_YT_FORMAT, url);
    }

    FILE *pipe = popen(cmd, "r");
    if (!pipe)
        return false;
    if (!fgets(video, YT_URL_MAX, pipe)) video[0] = '\0';
    if (!fgets(audio, YT_URL_MAX, pipe)) audio[0] = '\0';
    pclose(pipe);
#endif

    strip_newline(video);
    strip_newline(audio);

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
    bool ok = run_ytdlp(work->url, NULL, work->generation, video, audio);

    if (!ok && generation_is_current(work->generation) &&
        cookies_available()) {
        iptv_log("[VLChannel] yt-dlp did not resolve without cookies; "
                 "retrying with system/vlchannel/cookies.txt\n");
        ok = run_ytdlp(work->url, cookies_file, work->generation, video, audio);
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

void iptv_ytdlp_begin(const char *youtube_url) {
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
