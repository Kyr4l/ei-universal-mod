# Makefile for um-multitool (C++17)
# Builds two binaries from this directory:
#   um-multitool      - CLI merging um-ddsmmp, um-inireg, um-mobdump, um-restool, um-xlsxdb
#   um-multitool-gui  - Dear ImGui + GLFW GUI wrapper that spawns um-multitool in a console

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

SRCS = main.cpp ddsmmp.cpp inireg.cpp mobdumper.cpp restool.cpp xlsxdb.cpp
HDRS = subtools.hpp inflate.hpp zip_reader.hpp xlsx_reader.hpp cp1251.hpp db_schema_generated.hpp

# Dear ImGui is vendored as plain source (its normal distribution model: copied
# straight into the project tree, not built as a separate library). GLFW is
# the windowing/input backend underneath it - it natively supports X11 AND
# Wayland on Linux (auto-detected at runtime from the session, no code needed
# on our end) plus Win32, so the same GUI code covers all three targets.
# Rendering uses ImGui's OpenGL2 (legacy fixed-pipeline) backend: no GL loader
# library needed, and it is the most robust option under Wine (see below).
IMGUI_DIR = vendor/imgui
IMGUI_SRCS = $(IMGUI_DIR)/imgui.cpp $(IMGUI_DIR)/imgui_draw.cpp $(IMGUI_DIR)/imgui_tables.cpp \
             $(IMGUI_DIR)/imgui_widgets.cpp $(IMGUI_DIR)/imgui_impl_glfw.cpp $(IMGUI_DIR)/imgui_impl_opengl2.cpp
GUI_SRCS = gui_main.cpp $(IMGUI_SRCS)

# Native Linux GUI build uses the distro's GLFW (libglfw-devel / glfw3 pkg-config)
# and the system OpenGL library. -isystem marks the vendored ImGui headers as
# system headers so -Wextra doesn't flood the build with warnings from code we
# don't maintain.
GLFW_CXXFLAGS = $(shell pkg-config --cflags glfw3) -isystem $(IMGUI_DIR)
GLFW_LDFLAGS = $(shell pkg-config --libs glfw3) -lGL

# Windows GUI cross-compile uses the locally vendored static GLFW build.
GLFW_MINGW_PREFIX = vendor/glfw-mingw
WIN_GUI_FLAGS = -std=c++17 -O3 -Wall -Wextra -isystem $(GLFW_MINGW_PREFIX)/include -isystem $(IMGUI_DIR) \
                -static -static-libgcc -static-libstdc++
WIN_GUI_LDFLAGS = -L$(GLFW_MINGW_PREFIX)/lib -lglfw3 -lopengl32 -lgdi32 -lcomdlg32 -lole32 -luuid -mwindows

all: $(TARGET_BIN) $(GUI_TARGET_BIN)

$(TARGET_BIN): $(SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS)

$(GUI_TARGET_BIN): $(GUI_SRCS)
	$(CXX) $(CXXFLAGS) $(GLFW_CXXFLAGS) -o $@ $(GUI_SRCS) $(GLFW_LDFLAGS)

# Cross-compile targets for Windows .exe from Linux
win: $(TARGET).exe $(GUI_TARGET).exe

$(TARGET).exe: $(SRCS) $(HDRS)
	$(WIN_CXX) $(WIN_FLAGS) -o $@ $(SRCS)

# Requires vendor/glfw-mingw to exist first: run vendor/build-glfw-mingw.sh
$(GUI_TARGET).exe: $(GUI_SRCS)
	@if [ ! -f "$(GLFW_MINGW_PREFIX)/lib/libglfw3.a" ]; then \
		echo "Error: $(GLFW_MINGW_PREFIX) not found. Run vendor/build-glfw-mingw.sh first." >&2; \
		exit 1; \
	fi
	$(WIN_CXX) $(WIN_GUI_FLAGS) -o $@ $(GUI_SRCS) $(WIN_GUI_LDFLAGS)

clean:
	rm -f um-multitool um-multitool.exe um-multitool-gui um-multitool-gui.exe

.PHONY: all clean win
