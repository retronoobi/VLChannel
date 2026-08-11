#ifndef IPTV_YTDLP_COMMAND_H
#define IPTV_YTDLP_COMMAND_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Builds the mutable command line passed to CreateProcessA.
 *
 * Every argument is quoted according to the Windows C argv rules, including
 * runs of backslashes before quotes and before the closing quote. The URL is
 * placed after a standalone `--`, so text supplied by a playlist can only be
 * interpreted as one URL argument and never as a yt-dlp option.
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
