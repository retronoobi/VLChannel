#include "iptv_ytdlp_command.h"

static bool append_char(char *out, size_t out_size, size_t *used, char value) {
    if (*used + 1 >= out_size)
        return false;
    out[(*used)++] = value;
    out[*used] = '\0';
    return true;
}

static bool append_backslashes(char *out, size_t out_size, size_t *used,
                               size_t count) {
    for (size_t i = 0; i < count; i++)
        if (!append_char(out, out_size, used, '\\'))
            return false;
    return true;
}

/*
 * Always quoting keeps one rule for empty strings, spaces and ordinary text.
 *
 * The Windows C runtime treats backslashes specially only when they precede a
 * quote. A run before a literal quote is doubled and followed by one more
 * backslash; a run at the end is doubled so the closing quote stays structural.
 */
static bool append_argument(char *out, size_t out_size, size_t *used,
                            const char *argument) {
    if (*used > 0 && !append_char(out, out_size, used, ' '))
        return false;
    if (!append_char(out, out_size, used, '"'))
        return false;

    size_t backslashes = 0;
    for (const char *at = argument; ; at++) {
        if (*at == '\\') {
            backslashes++;
            continue;
        }

        if (*at == '"') {
            if (!append_backslashes(out, out_size, used, backslashes) ||
                !append_backslashes(out, out_size, used, backslashes) ||
                !append_char(out, out_size, used, '\\') ||
                !append_char(out, out_size, used, '"'))
                return false;
            backslashes = 0;
            continue;
        }

        if (*at == '\0') {
            if (!append_backslashes(out, out_size, used, backslashes) ||
                !append_backslashes(out, out_size, used, backslashes) ||
                !append_char(out, out_size, used, '"'))
                return false;
            return true;
        }

        if (!append_backslashes(out, out_size, used, backslashes) ||
            !append_char(out, out_size, used, *at))
            return false;
        backslashes = 0;
    }
}

bool iptv_build_windows_command(
    char *out,
    size_t out_size,
    const char *const *arguments,
    size_t count
) {
    if (!out || out_size == 0)
        return false;
    out[0] = '\0';

    if (!arguments || count == 0)
        return false;

    size_t used = 0;
    for (size_t i = 0; i < count; i++) {
        if (!arguments[i] ||
            !append_argument(out, out_size, &used, arguments[i])) {
            out[0] = '\0';
            return false;
        }
    }
    return true;
}

bool iptv_ytdlp_build_windows_command(
    char *out,
    size_t out_size,
    const char *executable,
    const char *format,
    const char *cookies,
    const char *url
) {
    if (!out || out_size == 0)
        return false;
    out[0] = '\0';

    if (!executable || !executable[0] || !format || !format[0] ||
        !url || !url[0])
        return false;

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
    arguments[count++] = "--";
    arguments[count++] = url;

    return iptv_build_windows_command(out, out_size, arguments, count);
}
