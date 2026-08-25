CORE_NAME := vlchannel_libretro
CC = gcc

VLC_CFLAGS := $(shell pkg-config --cflags libvlc)

CFLAGS := -O2 -fPIC -Wall -MMD -MP -I. $(VLC_CFLAGS)
LDFLAGS := -shared -static-libgcc -static-libstdc++
LIBS := -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic

TARGET := $(CORE_NAME).dll

# Objects live in obj/ so that stale objects from another core in the same
# directory - VLCine leaves vlc_core.o and friends behind - can never be linked
# into this DLL by mistake. It is deliberately NOT build/: that is where
# package-vlchannel.ps1 assembles the release, and "make clean" would take the
# package with it.
BUILD_DIR := obj
SOURCES := iptv_core.c iptv_playlist.c iptv_log.c iptv_osd.c iptv_text.c iptv_zap.c iptv_drift.c iptv_epg.c iptv_sequence.c iptv_ytdlp.c iptv_ytdlp_command.c iptv_streamlink.c iptv_child.c iptv_scrub.c iptv_transport.c vlc_video.c vlc_audio.c vlc_dynamic.c
OBJS := $(addprefix $(BUILD_DIR)/,$(SOURCES:.c=.o))
DEPS := $(OBJS:.o=.d)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR) *.o *.dll

-include $(DEPS)

.PHONY: all clean
