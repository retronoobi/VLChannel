# VLChannel

VLChannel is a Windows libretro core that turns RetroArch into a channel-based
IPTV player. It loads M3U and M3U8 playlists through libVLC and provides quick
channel switching, a television-style on-screen display, an optional channel
guide and schedule, stream reconnection, audio synchronization controls, and a
clear no-signal screen for unavailable channels.

The core always presents a stable 1920x1080 frame to RetroArch, independently
of the resolution of the current stream. This keeps the picture geometry and
OSD layout consistent when changing between channels with different source
resolutions. Optional preprocessed `.epg` files placed beside a playlist add
programme information to the Cable TV guide and banner.

## Installing in RetroArch

VLChannel requires a 64-bit Windows build of RetroArch and a matching 64-bit
libVLC runtime.

1. Copy `vlchannel_libretro.dll` to:

   ```text
   RetroArch\cores\vlchannel_libretro.dll
   ```

2. Copy `vlchannel_libretro.info` to:

   ```text
   RetroArch\info\vlchannel_libretro.info
   ```

3. Create the following directory inside RetroArch's configured System/BIOS
   directory:

   ```text
   RetroArch\system\vlchannel\
   ```

4. Copy the 64-bit VLC runtime into that directory. It must contain at least:

   ```text
   RetroArch\system\vlchannel\libvlc.dll
   RetroArch\system\vlchannel\libvlccore.dll
   RetroArch\system\vlchannel\plugins\
   ```

   VLChannel loads libVLC exclusively from `system\vlchannel`; VLC installations
   or DLLs in other directories are not used.

5. Start RetroArch, choose **Load Core > VLChannel**, then load an `.m3u` or
   `.m3u8` playlist with **Load Content**.

A basic playlist looks like this:

```m3u
#EXTM3U
#EXTINF:-1 tvg-chno="1" group-title="News",Example Channel
https://example.com/live/index.m3u8
```

If an optional programme listing is available, give it the same base name as
the playlist:

```text
My Channels.m3u8
My Channels.epg
```

## Controls (Xbox layout)

The names below refer to the physical Xbox button layout. Libretro's internal
face-button names use a different convention.

| Xbox control | While watching | In the Cable TV guide or schedule |
| --- | --- | --- |
| D-pad Left/Right or Left Stick Left/Right | Previous/next channel | Change guide column or page through the schedule |
| D-pad Up/Down or Left Stick Up/Down | Down toggles the channel number/banner | Move the selection |
| **A** | Return to the first channel | Cancel and close |
| **B** | Reload/reconnect the current channel | Tune to the selected channel |
| **X** | Open or close the schedule | Switch to or close the schedule |
| **Y** | Open or close the channel guide | Switch to or close the guide |

The full guide and schedule are available only with the **Cable TV** OSD. The
minimal **TV** OSD uses the same channel and reconnection controls but displays
only the channel number.

## Core options

Core options are available under **Quick Menu > Core Options** while VLChannel
is running. The first value listed by RetroArch is the default.

| Option | Default | Description |
| --- | --- | --- |
| **Picture width** | `100%` | Shrinks the active picture horizontally to compensate for display or converter overscan. Changing it reopens the channel. |
| **Picture height** | `100%` | Shrinks the active picture vertically while keeping the core's 1920x1080 output. Changing it reopens the channel. |
| **Horizontal position** | `0%` | Moves a reduced picture left or right within the output canvas. |
| **Vertical position** | `0%` | Moves a reduced picture up or down within the output canvas. |
| **Network and live caching (ms)** | `1500` | Sets libVLC's network and live-stream buffer. Higher values may tolerate unstable connections better, at the cost of additional latency. Changing it reopens the channel. |
| **Audio reserve (ms)** | `1200` | Sets the audio queue target used to keep sound synchronized with the picture. `match caching` follows the caching option; `80 ms (legacy)` keeps the older low-reserve behavior. |
| **Global audio offset (ms)** | `0` | Adds a fixed audio timing offset to every channel. Use it when all channels are consistently early or late. |
| **Give up on a channel after (s)** | `20` | Sets how long the core waits for a channel to become ready before treating it as unavailable. |
| **Return to live after a frontend pause** | `enabled` | Reloads the current channel after RetroArch or EmuVR pauses the core, returning playback to the live point. |
| **Aspect ratio** | `default` | Uses the stream's normal aspect ratio or forces one of the listed ratios through libVLC. |
| **Deinterlace** | `auto` | Selects automatic deinterlacing, disables it, or forces a specific libVLC deinterlacing mode. |
| **Audio track** | `0` | Selects an audio track. `0` leaves track selection at libVLC's default. |
| **On-screen language** | `English` | Sets the language of the guide and Cable TV banner. `auto` follows the frontend language when supported. |
| **Channel change noise** | `50%` | Controls the volume of the short channel-change sound. This is an audio effect and is separate from Picture noise. |
| **Cable TV banner second line** | `now and next` | Shows the current and next programme, or adds the current programme description when listing data is available. |
| **OSD** | `TV` | `TV` displays a minimal green channel number. `Cable TV` enables the full information banner, guide, and schedule. |
| **Loading screen** | `disabled` | Shows a loading message while a channel is being opened. |
| **Picture noise** | `off` | Adds animated analogue-style grain to the channel picture and no-signal screen. The core OSD is drawn afterward and remains clean. |
| **Write the full core log** | `disabled` | Writes detailed diagnostics to `system\vlchannel-core.log`. Restart the core after changing this option. |

## Using VLChannel in EmuVR

Install the core and VLC runtime inside EmuVR's bundled RetroArch directory by
following the same layout described above:

```text
EmuVR\RetroArch\cores\vlchannel_libretro.dll
EmuVR\RetroArch\info\vlchannel_libretro.info
EmuVR\RetroArch\system\vlchannel\
```

Open `EmuVR\Game Scanner\custom_media.txt` and add
`vlchannel_libretro` to the media type that will contain your channel lists. For
example, a dedicated TV media type can be declared as:

```ini
TV = "vlchannel_libretro"
```

If that media line already contains another entry, append the core with a pipe:

```ini
TV = "existing_entry|vlchannel_libretro"
```

Place the playlists in the corresponding folder under `EmuVR\Games`, open Game
Scanner, assign the media and `vlchannel_libretro` core to that folder, save the
changes, and scan the files. The playlists can then be inserted into an EmuVR
television like other compatible media.

## Credits

VideoLAN:
[libVLC](https://images.videolan.org/vlc/libvlc.html)
Original VLC libretro core source:
[krisretro/vlc-libretro-core-source](https://github.com/krisretro/vlc-libretro-core-source).

