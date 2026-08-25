#define VLC_DYNAMIC_IMPLEMENTATION
#include "vlc_dynamic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ctype.h>

#ifdef _WIN32
#include <windows.h>

/* Present since Windows 8; defined here so older SDK headers still build. */
#ifndef LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR
#define LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR 0x00000100
#endif
#ifndef LOAD_LIBRARY_SEARCH_DEFAULT_DIRS
#define LOAD_LIBRARY_SEARCH_DEFAULT_DIRS 0x00001000
#endif

/* Windows paths compare case insensitively; avoids depending on _stricmp. */
static bool path_prefix_matches(const char *path, const char *prefix) {
    while (*prefix) {
        if (tolower((unsigned char)*path) != tolower((unsigned char)*prefix))
            return false;
        path++;
        prefix++;
    }
    return true;
}

static bool path_equals(const char *a, const char *b) {
    return path_prefix_matches(a, b) && strlen(a) == strlen(b);
}

/* True only for a real child of dir, so a similarly prefixed sibling directory
 * can never be mistaken for the selected runtime. */
static bool path_is_inside(const char *path, const char *dir) {
    size_t length = strlen(dir);
    if (length == 0 || !path_prefix_matches(path, dir))
        return false;
    return path[length] == '\\' || path[length] == '/';
}

/*
 * Reports where a loaded module actually lives, and whether that is inside the
 * runtime directory. VLChannel is a variant of an existing libVLC core that keeps
 * its DLLs in the RetroArch root, so anything resolved from outside its own
 * directory means the two installations are being mixed.
 */
static bool module_is_inside(
    HMODULE module,
    const char *dir,
    char *out,
    size_t out_size
) {
    if (out && out_size)
        out[0] = '\0';
    if (!module)
        return false;
    if (!GetModuleFileNameA(module, out, (DWORD)out_size))
        return false;
    return path_is_inside(out, dir);
}
#endif

__typeof__(&libvlc_new) vlchannel_libvlc_new = NULL;
__typeof__(&libvlc_clock) vlchannel_libvlc_clock = NULL;
__typeof__(&libvlc_get_version) vlchannel_libvlc_get_version = NULL;
__typeof__(&libvlc_release) vlchannel_libvlc_release = NULL;
__typeof__(&libvlc_log_set) vlchannel_libvlc_log_set = NULL;
__typeof__(&libvlc_media_new_location) vlchannel_libvlc_media_new_location = NULL;
__typeof__(&libvlc_media_new_path) vlchannel_libvlc_media_new_path = NULL;
__typeof__(&libvlc_media_add_option) vlchannel_libvlc_media_add_option = NULL;
__typeof__(&libvlc_media_release) vlchannel_libvlc_media_release = NULL;
__typeof__(&libvlc_media_player_new) vlchannel_libvlc_media_player_new = NULL;
__typeof__(&libvlc_media_player_release) vlchannel_libvlc_media_player_release = NULL;
__typeof__(&libvlc_media_player_set_media) vlchannel_libvlc_media_player_set_media = NULL;
__typeof__(&libvlc_media_player_play) vlchannel_libvlc_media_player_play = NULL;
__typeof__(&libvlc_media_player_stop) vlchannel_libvlc_media_player_stop = NULL;
__typeof__(&libvlc_media_player_set_pause) vlchannel_libvlc_media_player_set_pause = NULL;
__typeof__(&libvlc_media_player_get_state) vlchannel_libvlc_media_player_get_state = NULL;
__typeof__(&libvlc_media_player_get_time) vlchannel_libvlc_media_player_get_time = NULL;
__typeof__(&libvlc_media_player_get_length) vlchannel_libvlc_media_player_get_length = NULL;
__typeof__(&libvlc_media_player_set_time) vlchannel_libvlc_media_player_set_time = NULL;
__typeof__(&libvlc_media_player_is_seekable) vlchannel_libvlc_media_player_is_seekable = NULL;
__typeof__(&libvlc_audio_set_callbacks) vlchannel_libvlc_audio_set_callbacks = NULL;
__typeof__(&libvlc_audio_set_format) vlchannel_libvlc_audio_set_format = NULL;
__typeof__(&libvlc_audio_get_track) vlchannel_libvlc_audio_get_track = NULL;
__typeof__(&libvlc_audio_get_track_description) vlchannel_libvlc_audio_get_track_description = NULL;
__typeof__(&libvlc_audio_set_track) vlchannel_libvlc_audio_set_track = NULL;
__typeof__(&libvlc_audio_set_delay) vlchannel_libvlc_audio_set_delay = NULL;
__typeof__(&libvlc_track_description_list_release) vlchannel_libvlc_track_description_list_release = NULL;
__typeof__(&libvlc_video_set_callbacks) vlchannel_libvlc_video_set_callbacks = NULL;
__typeof__(&libvlc_video_set_format_callbacks) vlchannel_libvlc_video_set_format_callbacks = NULL;
__typeof__(&libvlc_video_set_deinterlace) vlchannel_libvlc_video_set_deinterlace = NULL;
__typeof__(&libvlc_video_get_spu) vlchannel_libvlc_video_get_spu = NULL;
__typeof__(&libvlc_video_set_spu) vlchannel_libvlc_video_set_spu = NULL;
__typeof__(&libvlc_video_get_spu_description) vlchannel_libvlc_video_get_spu_description = NULL;

#ifdef _WIN32
static HMODULE libvlc_module = NULL;
static HMODULE libvlccore_module = NULL;
static char runtime_directory[VLCHANNEL_PATH_MAX] = {0};
static char runtime_version[64] = {0};
static bool runtime_legacy = false;
static char *previous_plugin_path = NULL;
static char *previous_dll_directory = NULL;
static bool process_paths_saved = false;

static void save_process_paths(void) {
    process_paths_saved = true;
    DWORD plugin_length = GetEnvironmentVariableA("VLC_PLUGIN_PATH", NULL, 0);
    if (plugin_length > 0) {
        previous_plugin_path = (char *)malloc(plugin_length);
        if (previous_plugin_path)
            GetEnvironmentVariableA(
                "VLC_PLUGIN_PATH",
                previous_plugin_path,
                plugin_length
            );
    }

    DWORD dll_length = GetDllDirectoryA(0, NULL);
    if (dll_length > 0) {
        previous_dll_directory = (char *)malloc(dll_length + 1);
        if (previous_dll_directory)
            GetDllDirectoryA(dll_length + 1, previous_dll_directory);
    }
}

static void restore_process_paths(void) {
    if (!process_paths_saved)
        return;

    SetEnvironmentVariableA("VLC_PLUGIN_PATH", previous_plugin_path);
    SetDllDirectoryA(previous_dll_directory);
    free(previous_plugin_path);
    free(previous_dll_directory);
    previous_plugin_path = NULL;
    previous_dll_directory = NULL;
    process_paths_saved = false;
}

#define LOAD_REQUIRED(name)                                                     \
    do {                                                                        \
        vlchannel_##name = (__typeof__(vlchannel_##name))GetProcAddress(              \
            libvlc_module, #name);                                               \
        if (!vlchannel_##name) {                                                    \
            fprintf(stderr, "[VLChannel] Missing symbol: %s\n", #name);             \
            vlchannel_unload_libvlc();                                              \
            return false;                                                        \
        }                                                                        \
    } while (0)

/*
 * Symbols the core can work without. Keeping them optional means a runtime
 * that predates one of these features still loads instead of failing outright,
 * which matters now that two libVLC generations are supported. Every call site
 * of an optional symbol checks it for NULL first.
 */
#define LOAD_OPTIONAL(name)                                                     \
    do {                                                                        \
        vlchannel_##name = (__typeof__(vlchannel_##name))GetProcAddress(              \
            libvlc_module, #name);                                               \
        if (!vlchannel_##name)                                                      \
            fprintf(stderr,                                                      \
                    "[VLChannel] Optional symbol unavailable: %s\n", #name);        \
    } while (0)

bool vlchannel_load_libvlc(const char *directory) {
    if (libvlc_module)
        return true;
    if (!directory || !directory[0])
        return false;

    snprintf(
        runtime_directory,
        sizeof(runtime_directory),
        "%s",
        directory
    );

    /* Sized above runtime_directory so appending the file name can never
     * truncate the path into one that silently fails to load. */
    char libvlc_path[VLCHANNEL_PATH_BUFFER];
    char libvlccore_path[VLCHANNEL_PATH_BUFFER];
    char plugin_path[VLCHANNEL_PATH_BUFFER];
    snprintf(libvlc_path, sizeof(libvlc_path), "%s\\libvlc.dll", runtime_directory);
    snprintf(libvlccore_path, sizeof(libvlccore_path), "%s\\libvlccore.dll", runtime_directory);
    snprintf(plugin_path, sizeof(plugin_path), "%s\\plugins", runtime_directory);

    /*
     * VLChannel must run only on the DLLs inside its own runtime directory.
     * A generic libVLC core keeps its runtime in the RetroArch root, and
     * Windows would happily bind to those: LoadLibrary returns an already
     * mapped module whenever one with the same base name exists, and a DLL's
     * dependencies are resolved from the executable's directory before any
     * directory this core sets. Loading a mixed runtime is worse than not
     * starting, because the failures that follow look like core bugs.
     *
     * So every step below verifies rather than hopes, and refuses instead of
     * warning.
     */
    HMODULE foreign = GetModuleHandleA("libvlc.dll");
    if (foreign) {
        char foreign_path[VLCHANNEL_PATH_BUFFER] = {0};
        GetModuleFileNameA(foreign, foreign_path, sizeof(foreign_path));
        if (!path_equals(foreign_path, libvlc_path)) {
            fprintf(stderr,
                    "[VLChannel] Refusing to start: this process already has "
                    "libvlc.dll loaded from %s. Windows would hand VLChannel that "
                    "library instead of its own runtime. Close the other "
                    "libVLC based core first.\n",
                    foreign_path[0] ? foreign_path : "an unknown location");
            runtime_directory[0] = '\0';
            return false;
        }
    }

    save_process_paths();
    SetDllDirectoryA(directory);
    SetEnvironmentVariableA("VLC_PLUGIN_PATH", plugin_path);

    /*
     * LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR resolves a DLL's dependencies from its
     * own folder. It needs Windows 8, or Windows 7 with KB2533623, so an older
     * mechanism is tried in turn. Plain LoadLibrary is deliberately not used
     * as a last resort: it would search the RetroArch root.
     */
    const DWORD isolated_flags =
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;

    /*
     * libvlccore is mapped first, from an explicit path. Once it is in the
     * process under that name, libvlc.dll cannot bind to a different copy.
     */
    libvlccore_module = LoadLibraryExA(libvlccore_path, NULL, isolated_flags);
    if (!libvlccore_module)
        libvlccore_module = LoadLibraryExA(
            libvlccore_path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!libvlccore_module) {
        fprintf(stderr,
                "[VLChannel] Could not load %s (Windows error %lu)\n",
                libvlccore_path, (unsigned long)GetLastError());
        vlchannel_unload_libvlc();
        return false;
    }

    libvlc_module = LoadLibraryExA(libvlc_path, NULL, isolated_flags);
    if (!libvlc_module)
        libvlc_module = LoadLibraryExA(
            libvlc_path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);

    if (!libvlc_module) {
        fprintf(
            stderr,
            "[VLChannel] Could not load %s (Windows error %lu)\n",
            libvlc_path,
            (unsigned long)GetLastError()
        );
        vlchannel_unload_libvlc();
        return false;
    }

    /* Confirm what is actually mapped, by name, not by what was requested. */
    char actual_libvlc[VLCHANNEL_PATH_BUFFER];
    char actual_core[VLCHANNEL_PATH_BUFFER];
    bool libvlc_ok = module_is_inside(
        GetModuleHandleA("libvlc.dll"),
        runtime_directory, actual_libvlc, sizeof(actual_libvlc));
    bool core_ok = module_is_inside(
        GetModuleHandleA("libvlccore.dll"),
        runtime_directory, actual_core, sizeof(actual_core));

    fprintf(stderr, "[VLChannel] libvlc.dll     -> %s\n",
            actual_libvlc[0] ? actual_libvlc : "unknown");
    fprintf(stderr, "[VLChannel] libvlccore.dll -> %s\n",
            actual_core[0] ? actual_core : "unknown");

    if (!libvlc_ok || !core_ok) {
        fprintf(stderr,
                "[VLChannel] Refusing to start: a library was resolved from "
                "outside %s. VLChannel keeps its own runtime precisely so it "
                "cannot mix with another libVLC installation.\n",
                runtime_directory);
        vlchannel_unload_libvlc();
        return false;
    }

    LOAD_REQUIRED(libvlc_new);
    LOAD_OPTIONAL(libvlc_clock);
    LOAD_REQUIRED(libvlc_get_version);
    LOAD_REQUIRED(libvlc_release);
    LOAD_REQUIRED(libvlc_log_set);
    LOAD_REQUIRED(libvlc_media_new_location);
    LOAD_REQUIRED(libvlc_media_new_path);
    LOAD_REQUIRED(libvlc_media_add_option);
    LOAD_REQUIRED(libvlc_media_release);
    LOAD_REQUIRED(libvlc_media_player_new);
    LOAD_REQUIRED(libvlc_media_player_release);
    LOAD_REQUIRED(libvlc_media_player_set_media);
    LOAD_REQUIRED(libvlc_media_player_play);
    LOAD_REQUIRED(libvlc_media_player_stop);
    LOAD_REQUIRED(libvlc_media_player_set_pause);
    LOAD_REQUIRED(libvlc_media_player_get_state);
    LOAD_REQUIRED(libvlc_media_player_get_time);
    LOAD_REQUIRED(libvlc_media_player_get_length);
    /* Optional, all four of them: they exist in every runtime this core ships
     * with, and a runtime that somehow lacks one should lose the ability to
     * seek a local file rather than refuse to play a television channel. Every
     * call site tests for NULL. */
    LOAD_OPTIONAL(libvlc_media_player_set_time);
    LOAD_OPTIONAL(libvlc_media_player_is_seekable);
    LOAD_REQUIRED(libvlc_audio_set_callbacks);
    LOAD_REQUIRED(libvlc_audio_set_format);
    LOAD_REQUIRED(libvlc_audio_get_track);
    LOAD_REQUIRED(libvlc_audio_get_track_description);
    LOAD_REQUIRED(libvlc_audio_set_track);
    LOAD_OPTIONAL(libvlc_audio_set_delay);
    LOAD_REQUIRED(libvlc_track_description_list_release);
    LOAD_REQUIRED(libvlc_video_set_callbacks);
    LOAD_REQUIRED(libvlc_video_set_format_callbacks);
    LOAD_OPTIONAL(libvlc_video_set_deinterlace);
    LOAD_OPTIONAL(libvlc_video_get_spu);
    LOAD_OPTIONAL(libvlc_video_set_spu);
    LOAD_OPTIONAL(libvlc_video_get_spu_description);

    const char *version = vlchannel_libvlc_get_version();
    snprintf(runtime_version, sizeof(runtime_version), "%s",
             version ? version : "unknown");
    runtime_legacy = (runtime_version[0] == '2');

    fprintf(stderr, "[VLChannel] Runtime: %s\n", runtime_directory);
    fprintf(stderr, "[VLChannel] libVLC %s (%s generation)\n",
            runtime_version, runtime_legacy ? "2.x" : "3.x");
    return true;
}

void vlchannel_unload_libvlc(void) {
    if (libvlc_module) {
        FreeLibrary(libvlc_module);
        libvlc_module = NULL;
    }
    if (libvlccore_module) {
        FreeLibrary(libvlccore_module);
        libvlccore_module = NULL;
    }
    restore_process_paths();
    runtime_directory[0] = '\0';
    runtime_version[0] = '\0';
    runtime_legacy = false;
}

const char *vlchannel_get_runtime_directory(void) {
    return runtime_directory;
}

const char *vlchannel_get_runtime_version(void) {
    return runtime_version;
}

bool vlchannel_runtime_is_legacy(void) {
    return runtime_legacy;
}

#else

bool vlchannel_load_libvlc(const char *directory) {
    (void)directory;
    return false;
}

void vlchannel_unload_libvlc(void) {
}

const char *vlchannel_get_runtime_directory(void) {
    return "";
}

const char *vlchannel_get_runtime_version(void) {
    return "";
}

bool vlchannel_runtime_is_legacy(void) {
    return false;
}

#endif
