#include "iptv_child.h"
#include "iptv_ytdlp_command.h"
#include "iptv_log.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

/*
 * Quoting can double backslashes and adds delimiters around every argument, so
 * the command line is sized for that worst case rather than for the length of
 * the strings that go into it.
 */
#define IPTV_CHILD_CMD_MAX 16384

#ifndef _WIN32
/*
 * Single quotes, with an embedded quote written the only way a POSIX shell
 * accepts one: close, escape, reopen. Ugly, and correct for every byte.
 */
static bool append_posix_argument(char *out, size_t out_size, size_t *used,
                                  const char *argument) {
    size_t need = *used;
    if (need > 0) need++;                                   /* the separator */

    for (const char *at = argument; *at; at++)
        need += (*at == '\'') ? 4 : 1;
    need += 2;                                              /* the quotes */

    if (need + 1 >= out_size)
        return false;

    if (*used > 0)
        out[(*used)++] = ' ';
    out[(*used)++] = '\'';
    for (const char *at = argument; *at; at++) {
        if (*at == '\'') {
            memcpy(out + *used, "'\\''", 4);
            *used += 4;
        } else {
            out[(*used)++] = *at;
        }
    }
    out[(*used)++] = '\'';
    out[*used] = '\0';
    return true;
}
#endif

iptv_child_result iptv_child_capture(const char *executable,
                                     const char *const *arguments,
                                     size_t count,
                                     unsigned timeout_ms,
                                     iptv_child_cancelled cancelled,
                                     void *cookie,
                                     char *out, size_t out_size) {
    if (!out || out_size == 0)
        return IPTV_CHILD_NOT_STARTED;
    out[0] = '\0';

    if (!executable || !executable[0] || !arguments || count == 0)
        return IPTV_CHILD_NOT_STARTED;

#ifdef _WIN32
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE pipe_rd = NULL, pipe_wr = NULL;
    if (!CreatePipe(&pipe_rd, &pipe_wr, &sa, 0))
        return IPTV_CHILD_NOT_STARTED;
    SetHandleInformation(pipe_rd, HANDLE_FLAG_INHERIT, 0);

    char cmd[IPTV_CHILD_CMD_MAX];
    if (!iptv_build_windows_command(cmd, sizeof(cmd), arguments, count)) {
        CloseHandle(pipe_wr);
        CloseHandle(pipe_rd);
        iptv_log("[VLChannel] Command line for %s is too long or invalid\n",
                 executable);
        return IPTV_CHILD_NOT_STARTED;
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
        iptv_log("[VLChannel] %s could not be started (error %lu)\n",
                 executable, (unsigned long)GetLastError());
        return IPTV_CHILD_NOT_STARTED;
    }

    size_t used = 0;
    ULONGLONG deadline = GetTickCount64() + timeout_ms;
    bool timed_out = false;
    bool was_cancelled = false;
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

            size_t room = out_size - 1 - used;
            size_t copy = got < room ? (size_t)got : room;
            if (copy > 0) {
                memcpy(out + used, chunk, copy);
                used += copy;
                out[used] = '\0';
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

        if (cancelled && cancelled(cookie)) {
            was_cancelled = true;
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

    if (timed_out || was_cancelled || pipe_failed) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 2000);
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(pipe_rd);

    if (was_cancelled) {
        out[0] = '\0';
        return IPTV_CHILD_CANCELLED;
    }
    if (timed_out) {
        out[0] = '\0';
        return IPTV_CHILD_TIMED_OUT;
    }
    if (pipe_failed && used == 0)
        return IPTV_CHILD_PIPE_FAILED;
    return IPTV_CHILD_OK;
#else
    /*
     * popen() gives no way to time out and no way to cancel, so neither is
     * offered here. This branch exists so the module builds and can be tested
     * away from Windows; the core ships on Windows, where the branch above is
     * the one that runs.
     */
    (void)timeout_ms;
    (void)cancelled;
    (void)cookie;

    char cmd[IPTV_CHILD_CMD_MAX];
    size_t used = 0;
    cmd[0] = '\0';
    for (size_t i = 0; i < count; i++) {
        const char *argument = (i == 0) ? executable : arguments[i];
        if (!append_posix_argument(cmd, sizeof(cmd), &used, argument)) {
            iptv_log("[VLChannel] Command line for %s is too long\n",
                     executable);
            return IPTV_CHILD_NOT_STARTED;
        }
    }
    if (used + 12 >= sizeof(cmd))
        return IPTV_CHILD_NOT_STARTED;
    memcpy(cmd + used, " 2>/dev/null", 13);

    FILE *pipe = popen(cmd, "r");
    if (!pipe)
        return IPTV_CHILD_NOT_STARTED;

    size_t got = fread(out, 1, out_size - 1, pipe);
    out[got] = '\0';
    pclose(pipe);
    return IPTV_CHILD_OK;
#endif
}
