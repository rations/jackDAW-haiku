# Builds on Haiku only (uses the non-packaged JACK install and glib2 from pkgman).
# Engine/model code is C (gnu99, glib2 — no GTK); UI code is C++ (Interface Kit).
# VST3 hosting builds against the sibling VST3-haiku checkout (Haiku-patched
# Steinberg SDK sources + its cmake-built static libs).
CC  = gcc
CXX = g++
NONPACKAGED = /boot/system/non-packaged
VST3_SDK ?= $(HOME)/VST3-haiku/vst3sdk
VST3_LIB ?= $(HOME)/VST3-haiku/build/lib/Release

# gio-2.0: project save/load copies bundle audio via GFile (g_file_copy).
GLIB_CFLAGS := $(shell pkg-config --cflags glib-2.0 gobject-2.0 gio-2.0)
GLIB_LIBS   := $(shell pkg-config --libs glib-2.0 gobject-2.0 gio-2.0)
# Audio file I/O (libsndfile) + sample-rate conversion (libsamplerate), used by
# the clip peak tables and the playback feeder thread.
AUDIO_CFLAGS := $(shell pkg-config --cflags sndfile samplerate)
AUDIO_LIBS   := $(shell pkg-config --libs sndfile samplerate)

COMMON   = -Wall -Wextra -MMD -MP -I$(NONPACKAGED)/include -Isrc
CFLAGS   = -std=gnu99 $(COMMON) $(GLIB_CFLAGS) $(AUDIO_CFLAGS)
CXXFLAGS = -std=c++17 $(COMMON) $(GLIB_CFLAGS)
# SDK translation units (ours in src/host/ + the three SDK sources below) need
# the SDK include root and a build-mode define (fdebug.h insists on one).
HOST_CXXFLAGS = $(CXXFLAGS) -I$(VST3_SDK) -DRELEASE=1
# -ltracker: BFilePanel (open/save panels) lives in libtracker, not libbe.
LDFLAGS  = -L$(NONPACKAGED)/lib -L$(VST3_LIB) -ljack -lbe -ltracker \
           -lsdk_hosting -lsdk_common -lbase -lpluginterfaces \
           $(GLIB_LIBS) $(AUDIO_LIBS)

C_SOURCES   = src/engine/glib_check.c src/engine/message.c src/engine/settings.c \
              src/engine/track.c src/engine/project.c src/engine/jackdaw-engine.c \
              src/engine/tempomap.c src/engine/timeruler.c \
              src/engine/audio_clip.c src/engine/clipregion.c src/engine/undo.c \
              src/engine/midiclip.c src/engine/render.c
CXX_SOURCES = src/ui/main.cpp src/ui/JackDawApp.cpp src/ui/MainWindow.cpp \
              src/ui/TransportView.cpp src/ui/TimelineView.cpp src/ui/RulerView.cpp \
              src/ui/TrackAreaView.cpp src/ui/StepperControl.cpp \
              src/ui/MetronomeWindows.cpp src/ui/KnobView.cpp src/ui/FaderView.cpp \
              src/ui/VuView.cpp src/ui/TrackStripView.cpp src/ui/MixerStripView.cpp \
              src/ui/MixerView.cpp src/ui/MixerWindow.cpp src/ui/RegionGainWindow.cpp \
              src/ui/MidiWindow.cpp src/ui/FxWindow.cpp src/ui/RenderWindow.cpp
HOST_SOURCES = src/host/pluginhost.cpp

# SDK sources compiled directly (they are not part of the SDK's static libs;
# same set the VST3-haiku hosts compile): the Haiku module loader, the
# component/controller provider, and the IBStream-on-memory used for state.
SDK_OBJECTS = src/host/sdk_module_haiku.o src/host/sdk_plugprovider.o \
              src/host/sdk_memorystream.o

OBJECTS = $(C_SOURCES:.c=.o) $(CXX_SOURCES:.cpp=.o) $(HOST_SOURCES:.cpp=.o) $(SDK_OBJECTS)
DEPS    = $(OBJECTS:.o=.d)
TARGET  = JackDAW

$(TARGET): $(OBJECTS)
	$(CXX) $(OBJECTS) -o $(TARGET) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

src/host/%.o: src/host/%.cpp
	$(CXX) $(HOST_CXXFLAGS) -c $< -o $@

# UI files that talk to the plugin host only need pluginhost.h (plain C API),
# so they build with the normal CXXFLAGS — no SDK include path leaks into the
# Interface Kit code.

src/host/sdk_module_haiku.o: $(VST3_SDK)/public.sdk/source/vst/hosting/module_haiku.cpp
	$(CXX) $(HOST_CXXFLAGS) -w -c $< -o $@

src/host/sdk_plugprovider.o: $(VST3_SDK)/public.sdk/source/vst/hosting/plugprovider.cpp
	$(CXX) $(HOST_CXXFLAGS) -w -c $< -o $@

src/host/sdk_memorystream.o: $(VST3_SDK)/public.sdk/source/common/memorystream.cpp
	$(CXX) $(HOST_CXXFLAGS) -w -c $< -o $@

-include $(DEPS)

clean:
	rm -f $(OBJECTS) $(DEPS) $(TARGET)

.PHONY: clean
