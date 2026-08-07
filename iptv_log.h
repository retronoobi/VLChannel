#ifndef IPTV_LOG_H
#define IPTV_LOG_H

/*
 * One log, two destinations.
 *
 * The core's diagnosis has always gone to stderr. That works under RetroArch
 * started from a terminal and is useless under EmuVR, which passes --log-file
 * and has no console: there, stderr goes nowhere at all, and three separate
 * investigations in this project were slowed down by having no way to see what
 * libVLC was saying.
 *
 * So every message also goes to a file when one is open. Same text, no
 * filtering: the [VLC-LOG] echo is exactly what is needed when a channel does
 * not open, and it is the part that never reaches the frontend's own log.
 */
void iptv_log_open(const char *path);   /* NULL or "" leaves the file closed */
void iptv_log_close(void);
const char *iptv_log_path(void);        /* "" when no file is open */

void iptv_log(const char *format, ...);

#endif
