# Makefile for um-multitool (C++17)
# Builds two binaries from this directory:
#   um-multitool      - CLI merging um-ddsmmp, um-inireg, um-mobdump, um-restool
#   um-multitool-gui  - FLTK GUI wrapper that spawns um-multitool in a console

CXX ?= g++
CXXFLAGS ?= -std=c++17 -O3 -Wall -Wextra -pthread
TARGET ?= um-multitool
GUI_TARGET ?= um-multitool-gui

# Cross-compilation variables for Windows (.exe)
WIN_CXX = x86_64-w64-mingw32-g++
WIN_FLAGS = -std=c++17 -O3 -Wall -Wextra -pthread -static -static-libgcc -static-libstdc++

ifeq ($(OS),Windows_NT)
    TARGET_BIN = $(TARGET).exe
    GUI_TARGET_BIN = $(GUI_TARGET).exe
else
    TARGET_BIN = $(TARGET)
    GUI_TARGET_BIN = $(GUI_TARGET)
endif

SRCS = main.cpp ddsmmp.cpp inireg.cpp mobdumper.cpp restool.cpp
GUI_SRCS = gui_main.cpp

# Native Linux GUI build uses the system FLTK (fltk-config).
# Note: -flto is intentionally dropped from fltk-config's suggested ldflags
# to avoid noisy cross-TU ODR warnings with no real benefit here.
FLTK_CXXFLAGS = $(shell fltk-config --cxxflags)
FLTK_LDFLAGS = -lfltk

# Windows GUI cross-compile uses the locally vendored static FLTK build.
# -isystem (not -I) marks the vendored headers as system headers so -Wextra
# doesn't flood the build with warnings from FLTK's own internals.
FLTK_MINGW_PREFIX = vendor/fltk-mingw
WIN_GUI_FLAGS = -std=c++17 -O3 -Wall -Wextra -isystem $(FLTK_MINGW_PREFIX)/include \
                -static -static-libgcc -static-libstdc++
WIN_GUI_LDFLAGS = -L$(FLTK_MINGW_PREFIX)/lib -lfltk_images -lfltk -lfltk_png -lfltk_jpeg -lfltk_z \
                  -lws2_32 -lcomctl32 -lgdi32 -lole32 -luuid -mwindows

all: $(TARGET_BIN) $(GUI_TARGET_BIN)

$(TARGET_BIN): $(SRCS) subtools.hpp
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS)

$(GUI_TARGET_BIN): $(GUI_SRCS)
	$(CXX) $(CXXFLAGS) $(FLTK_CXXFLAGS) -o $@ $(GUI_SRCS) $(FLTK_LDFLAGS)

# Cross-compile targets for Windows .exe from Linux
win: $(TARGET).exe $(GUI_TARGET).exe

$(TARGET).exe: $(SRCS) subtools.hpp
	$(WIN_CXX) $(WIN_FLAGS) -o $@ $(SRCS)

# Requires vendor/fltk-mingw to exist first: run vendor/build-fltk-mingw.sh
$(GUI_TARGET).exe: $(GUI_SRCS)
	@if [ ! -f "$(FLTK_MINGW_PREFIX)/lib/libfltk.a" ]; then \
		echo "Error: $(FLTK_MINGW_PREFIX) not found. Run vendor/build-fltk-mingw.sh first." >&2; \
		exit 1; \
	fi
	$(WIN_CXX) $(WIN_GUI_FLAGS) -o $@ $(GUI_SRCS) $(WIN_GUI_LDFLAGS)

clean:
	rm -f um-multitool um-multitool.exe um-multitool-gui um-multitool-gui.exe

.PHONY: all clean win

