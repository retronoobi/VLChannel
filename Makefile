CORE_NAME := vlchannel_libretro
CC = gcc

VLC_CFLAGS := $(shell pkg-config --cflags libvlc)

CFLAGS := -O2 -fPIC -Wall -MMD -MP -I. $(VLC_CFLAGS)
LDFLAGS := -shared -static-libgcc -static-libstdc++
LIBS := -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic

TARGET := $(CORE_NAME).dll
TEST_OUTPUT_RESOLUTION := test_output_resolution.exe
TEST_YTDLP_COMMAND := test_ytdlp_command.exe
TEST_THREAD_SAFETY := test_thread_safety.exe
TEST_OSD_PIP := test_osd_pip.exe

# Objects live in obj/ so that stale objects from another core in the same
# directory - VLCine leaves vlc_core.o and friends behind - can never be linked
# into this DLL by mistake. It is deliberately NOT build/: that is where
# package-vlchannel.ps1 assembles the release, and "make clean" would take the
# package with it.
BUILD_DIR := obj
SOURCES := iptv_core.c iptv_playlist.c iptv_log.c iptv_osd.c iptv_text.c iptv_zap.c iptv_drift.c iptv_epg.c iptv_sequence.c iptv_ytdlp.c iptv_ytdlp_command.c vlc_video.c vlc_audio.c vlc_dynamic.c
OBJS := $(addprefix $(BUILD_DIR)/,$(SOURCES:.c=.o))
DEPS := $(OBJS:.o=.d)

all: $(TARGET)

test-output-resolution: $(TARGET) $(TEST_OUTPUT_RESOLUTION)
	./$(TEST_OUTPUT_RESOLUTION)

test-ytdlp-command: $(TARGET) $(TEST_YTDLP_COMMAND)
	./$(TEST_YTDLP_COMMAND)

test-thread-safety: $(TEST_THREAD_SAFETY)
	./$(TEST_THREAD_SAFETY)

test-osd-pip: $(TEST_OSD_PIP)
	./$(TEST_OSD_PIP)

test: test-output-resolution test-ytdlp-command test-thread-safety test-osd-pip

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(TEST_OUTPUT_RESOLUTION): test_output_resolution.c libretro.h
	$(CC) -O2 -Wall -I. -o $@ $<

$(TEST_YTDLP_COMMAND): test_ytdlp_command.c iptv_ytdlp_command.c iptv_ytdlp_command.h
	$(CC) -O2 -Wall -I. -o $@ test_ytdlp_command.c iptv_ytdlp_command.c -lshell32

$(TEST_THREAD_SAFETY): test_thread_safety.c iptv_log.c iptv_log.h
	$(CC) -O2 -Wall -I. -o $@ test_thread_safety.c iptv_log.c $(LIBS)

$(TEST_OSD_PIP): test_osd_pip.c iptv_osd.c iptv_osd.h iptv_epg.c iptv_epg.h iptv_text.c iptv_text.h iptv_log.c iptv_log.h
	$(CC) -O2 -Wall -I. -o $@ test_osd_pip.c iptv_osd.c iptv_epg.c iptv_text.c iptv_log.c $(LIBS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR) *.o *.dll $(TEST_OUTPUT_RESOLUTION) $(TEST_YTDLP_COMMAND) $(TEST_THREAD_SAFETY) $(TEST_OSD_PIP) test_osd_pip_*.bmp test_osd_pip.epg

-include $(DEPS)

.PHONY: all clean test test-output-resolution test-ytdlp-command test-thread-safety test-osd-pip
