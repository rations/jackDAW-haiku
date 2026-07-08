# Builds on Haiku only (uses the non-packaged JACK install and glib2 from pkgman).
# Engine/model code is C (gnu99, glib2 — no GTK); UI code is C++ (Interface Kit).
CC  = gcc
CXX = g++
NONPACKAGED = /boot/system/non-packaged

GLIB_CFLAGS := $(shell pkg-config --cflags glib-2.0 gobject-2.0)
GLIB_LIBS   := $(shell pkg-config --libs glib-2.0 gobject-2.0)
# Audio file I/O (libsndfile) + sample-rate conversion (libsamplerate), used by
# the clip peak tables and the playback feeder thread.
AUDIO_CFLAGS := $(shell pkg-config --cflags sndfile samplerate)
AUDIO_LIBS   := $(shell pkg-config --libs sndfile samplerate)

COMMON   = -Wall -Wextra -MMD -MP -I$(NONPACKAGED)/include -Isrc
CFLAGS   = -std=gnu99 $(COMMON) $(GLIB_CFLAGS) $(AUDIO_CFLAGS)
CXXFLAGS = -std=c++17 $(COMMON) $(GLIB_CFLAGS)
# -ltracker: BFilePanel (open/save panels) lives in libtracker, not libbe.
LDFLAGS  = -L$(NONPACKAGED)/lib -ljack -lbe -ltracker $(GLIB_LIBS) $(AUDIO_LIBS)

C_SOURCES   = src/engine/glib_check.c src/engine/message.c src/engine/settings.c \
              src/engine/track.c src/engine/project.c src/engine/jackdaw-engine.c \
              src/engine/tempomap.c src/engine/timeruler.c \
              src/engine/audio_clip.c src/engine/clipregion.c src/engine/undo.c
CXX_SOURCES = src/ui/main.cpp src/ui/JackDawApp.cpp src/ui/MainWindow.cpp \
              src/ui/TransportView.cpp src/ui/TimelineView.cpp src/ui/RulerView.cpp \
              src/ui/TrackAreaView.cpp src/ui/StepperControl.cpp \
              src/ui/MetronomeWindows.cpp src/ui/KnobView.cpp src/ui/FaderView.cpp \
              src/ui/VuView.cpp src/ui/TrackStripView.cpp src/ui/MixerStripView.cpp \
              src/ui/MixerView.cpp src/ui/MixerWindow.cpp src/ui/RegionGainWindow.cpp

OBJECTS = $(C_SOURCES:.c=.o) $(CXX_SOURCES:.cpp=.o)
DEPS    = $(OBJECTS:.o=.d)
TARGET  = JackDAW

$(TARGET): $(OBJECTS)
	$(CXX) $(OBJECTS) -o $(TARGET) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

-include $(DEPS)

clean:
	rm -f $(OBJECTS) $(DEPS) $(TARGET)

.PHONY: clean
