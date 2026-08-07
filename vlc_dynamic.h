#ifndef VLC_DYNAMIC_H
#define VLC_DYNAMIC_H

#include <stdbool.h>
#include <vlc/vlc.h>

/*
 * Path buffers.
 *
 * VLCHANNEL_PATH_MAX holds a path; VLCHANNEL_PATH_BUFFER is for anything built by
 * appending to one, such as "<dir>\libvlc.dll" or an MRL assembled from a
 * playlist entry. Sizing both the same produced repeated -Wformat-truncation
 * warnings in VLCine and, worse, silent truncation into a path that cannot be
 * opened. The suffix allowance is generous on purpose: it costs a few bytes of
 * stack, and getting it wrong is a bug that only surfaces on long paths.
 *
 * They live here, in the lowest-level header, so both the core and the loader
 * see them without the loader having to pull in the core's state.
 */
#define VLCHANNEL_PATH_MAX 4096
#define VLCHANNEL_PATH_BUFFER (VLCHANNEL_PATH_MAX + 64)

bool vlchannel_load_libvlc(const char *runtime_directory);
void vlchannel_unload_libvlc(void);
const char *vlchannel_get_runtime_directory(void);

/*
 * VLChannel ships two runtimes side by side, inheriting VLCine's layout, and the
 * default is the opposite of VLCine's: 3.0.x rather than 2.2.8.
 *
 * Both generations play HLS, by different means. 3.0.x uses demux/adaptive,
 * with real bitrate adaptation; 2.2.x uses stream_filter/httplive, which takes
 * a variant and plays it the way ffmpeg based players do. Neither is strictly
 * better - there are channels that only work on each - so the choice is a core
 * option and both trees are shipped.
 *
 * Both generations expose the same public API surface used by this core, so the
 * difference is handled by loading from a different directory rather than by
 * compiling twice.
 */
const char *vlchannel_get_runtime_version(void);   /* "" when not loaded */
bool vlchannel_runtime_is_legacy(void);            /* true for libVLC 2.x */

extern __typeof__(&libvlc_new) vlchannel_libvlc_new;
extern __typeof__(&libvlc_clock) vlchannel_libvlc_clock;
extern __typeof__(&libvlc_get_version) vlchannel_libvlc_get_version;
extern __typeof__(&libvlc_release) vlchannel_libvlc_release;
extern __typeof__(&libvlc_log_set) vlchannel_libvlc_log_set;
extern __typeof__(&libvlc_media_new_location) vlchannel_libvlc_media_new_location;
extern __typeof__(&libvlc_media_new_path) vlchannel_libvlc_media_new_path;
extern __typeof__(&libvlc_media_add_option) vlchannel_libvlc_media_add_option;
extern __typeof__(&libvlc_media_release) vlchannel_libvlc_media_release;
extern __typeof__(&libvlc_media_player_new) vlchannel_libvlc_media_player_new;
extern __typeof__(&libvlc_media_player_release) vlchannel_libvlc_media_player_release;
extern __typeof__(&libvlc_media_player_set_media) vlchannel_libvlc_media_player_set_media;
extern __typeof__(&libvlc_media_player_play) vlchannel_libvlc_media_player_play;
extern __typeof__(&libvlc_media_player_stop) vlchannel_libvlc_media_player_stop;
extern __typeof__(&libvlc_media_player_set_pause) vlchannel_libvlc_media_player_set_pause;
extern __typeof__(&libvlc_media_player_get_state) vlchannel_libvlc_media_player_get_state;
extern __typeof__(&libvlc_media_player_get_time) vlchannel_libvlc_media_player_get_time;
extern __typeof__(&libvlc_media_player_get_length) vlchannel_libvlc_media_player_get_length;
extern __typeof__(&libvlc_audio_set_callbacks) vlchannel_libvlc_audio_set_callbacks;
extern __typeof__(&libvlc_audio_set_format) vlchannel_libvlc_audio_set_format;
extern __typeof__(&libvlc_audio_get_track) vlchannel_libvlc_audio_get_track;
extern __typeof__(&libvlc_audio_get_track_description) vlchannel_libvlc_audio_get_track_description;
extern __typeof__(&libvlc_audio_set_track) vlchannel_libvlc_audio_set_track;
extern __typeof__(&libvlc_audio_set_delay) vlchannel_libvlc_audio_set_delay;
extern __typeof__(&libvlc_track_description_list_release) vlchannel_libvlc_track_description_list_release;
extern __typeof__(&libvlc_video_set_callbacks) vlchannel_libvlc_video_set_callbacks;
extern __typeof__(&libvlc_video_set_format_callbacks) vlchannel_libvlc_video_set_format_callbacks;
extern __typeof__(&libvlc_video_set_deinterlace) vlchannel_libvlc_video_set_deinterlace;

#ifndef VLC_DYNAMIC_IMPLEMENTATION
#define libvlc_new vlchannel_libvlc_new
#define libvlc_clock vlchannel_libvlc_clock
#define libvlc_get_version vlchannel_libvlc_get_version
#define libvlc_release vlchannel_libvlc_release
#define libvlc_log_set vlchannel_libvlc_log_set
#define libvlc_media_new_location vlchannel_libvlc_media_new_location
#define libvlc_media_new_path vlchannel_libvlc_media_new_path
#define libvlc_media_add_option vlchannel_libvlc_media_add_option
#define libvlc_media_release vlchannel_libvlc_media_release
#define libvlc_media_player_new vlchannel_libvlc_media_player_new
#define libvlc_media_player_release vlchannel_libvlc_media_player_release
#define libvlc_media_player_set_media vlchannel_libvlc_media_player_set_media
#define libvlc_media_player_play vlchannel_libvlc_media_player_play
#define libvlc_media_player_stop vlchannel_libvlc_media_player_stop
#define libvlc_media_player_set_pause vlchannel_libvlc_media_player_set_pause
#define libvlc_media_player_get_state vlchannel_libvlc_media_player_get_state
#define libvlc_media_player_get_time vlchannel_libvlc_media_player_get_time
#define libvlc_media_player_get_length vlchannel_libvlc_media_player_get_length
#define libvlc_audio_set_callbacks vlchannel_libvlc_audio_set_callbacks
#define libvlc_audio_set_format vlchannel_libvlc_audio_set_format
#define libvlc_audio_get_track vlchannel_libvlc_audio_get_track
#define libvlc_audio_get_track_description vlchannel_libvlc_audio_get_track_description
#define libvlc_audio_set_track vlchannel_libvlc_audio_set_track
#define libvlc_audio_set_delay vlchannel_libvlc_audio_set_delay
#define libvlc_track_description_list_release vlchannel_libvlc_track_description_list_release
#define libvlc_video_set_callbacks vlchannel_libvlc_video_set_callbacks
#define libvlc_video_set_format_callbacks vlchannel_libvlc_video_set_format_callbacks
#define libvlc_video_set_deinterlace vlchannel_libvlc_video_set_deinterlace
#endif

#endif
