#ifndef IPTV_YTDLP_COMMAND_H
#define IPTV_YTDLP_COMMAND_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Builds the mutable command line passed to CreateProcessA, out of an argument
 * list the caller has already decided on.
 *
 * Every argument is quoted according to the Windows C argv rules, including
 * runs of backslashes before quotes and before the closing quote. The quoting
 * has nothing to do with which program is being run, which is why this is the
 * general form: yt-dlp was the first caller and streamlink is the second, and
 * a second copy of these rules would be a second place for them to be subtly
 * wrong.
 *
 * `arguments[0]` is argv[0]. On failure `out` is left empty and false is
 * returned; the only failure is not fitting.
 */
bool iptv_build_windows_command(
    char *out,
    size_t out_size,
    const char *const *arguments,
    size_t count
);

/*
 * The yt-dlp command line, in terms of the above.
 *
 * Kept as a function of its own because the argument list is the interesting
 * part - it is what the core actually asks of yt-dlp - and because a test can
 * then assert on the whole line rather than on a list it assembled itself. The
 * URL is placed after a standalone `--`, so text supplied by a playlist can
 * only be read as one URL argument and never as a yt-dlp option.
 */
bool iptv_ytdlp_build_windows_command(
    char *out,
    size_t out_size,
    const char *executable,
    const char *format,
    const char *cookies,
    const char *url
);

#endif
