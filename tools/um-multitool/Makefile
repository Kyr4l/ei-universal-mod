# Makefile for um-multitool (C++17): ONE binary holding the command-line tools
# (ddsmmp, inireg, mobdump, restool, xlsxdb, viewer) and the GUI (File Processing
# and 3D Viewer tabs). Double-clicked it opens the GUI; from a terminal without
# arguments it prints the usage; `um-multitool gui` opens the GUI from a terminal.
#
#   make linux  - um-multitool (native Linux)
#   make win    - um-multitool.exe, cross-compiled with MinGW
#   make all    - both (the Windows part is skipped when MinGW is not installed)
#
# GUI toolkit: Dear ImGui (vendored as source in vendor/imgui) on GLFW with the
# OpenGL2 backend - no GL loader needed, the most robust path under Wine. GLFW
# picks X11 or Wayland on Linux by itself. Linux uses the distro's GLFW
# (pkg-config glfw3); Windows the static build made by vendor/build-glfw-mingw.sh.

CXX ?= g++
TARGET ?= um-multitool
WIN_CXX = x86_64-w64-mingw32-g++

IMGUI_DIR = vendor/imgui
IMGUI_SRCS = $(IMGUI_DIR)/imgui.cpp $(IMGUI_DIR)/imgui_draw.cpp $(IMGUI_DIR)/imgui_tables.cpp \
             $(IMGUI_DIR)/imgui_widgets.cpp $(IMGUI_DIR)/imgui_impl_glfw.cpp $(IMGUI_DIR)/imgui_impl_opengl2.cpp
SRCS = main.cpp ddsmmp.cpp inireg.cpp mobdumper.cpp restool.cpp xlsxdb.cpp gui_main.cpp viewer/viewer_app.cpp $(IMGUI_SRCS)
HDRS = $(wildcard *.hpp) $(wildcard viewer/*.hpp)

# -I. lets viewer/ share this directory's cp1251.hpp; -isystem keeps -Wextra off vendored code.
COMMON_FLAGS = -std=c++17 -O2 -Wall -Wextra -pthread -I. -isystem $(IMGUI_DIR)

LINUX_FLAGS = $(COMMON_FLAGS) $(shell pkg-config --cflags glfw3)
LINUX_LIBS = $(shell pkg-config --libs glfw3) -lGL

# Console subsystem on purpose (no -mwindows): command-line use then behaves normally in cmd and
# PowerShell, and a double-click only briefly shows the console window before the GUI replaces it.
GLFW_MINGW_PREFIX = vendor/glfw-mingw
WIN_FLAGS = $(COMMON_FLAGS) -isystem $(GLFW_MINGW_PREFIX)/include -static -static-libgcc -static-libstdc++ -s
WIN_LIBS = -L$(GLFW_MINGW_PREFIX)/lib -lglfw3 -lopengl32 -lgdi32 -lcomdlg32 -lole32 -lshell32 -luuid

WIN_BINS =
ifneq ($(OS),Windows_NT)
ifneq ($(shell command -v $(WIN_CXX) 2>/dev/null),)
WIN_BINS = win
endif
endif

all: linux $(WIN_BINS)

linux: $(TARGET)

$(TARGET): $(SRCS) $(HDRS)
	$(CXX) $(LINUX_FLAGS) -o $@ $(SRCS) $(LINUX_LIBS)

win: $(TARGET).exe

# Requires vendor/glfw-mingw to exist first: run vendor/build-glfw-mingw.sh
$(TARGET).exe: $(SRCS) $(HDRS)
	@if [ ! -f "$(GLFW_MINGW_PREFIX)/lib/libglfw3.a" ]; then \
		echo "Error: $(GLFW_MINGW_PREFIX) not found. Run vendor/build-glfw-mingw.sh first." >&2; \
		exit 1; \
	fi
	$(WIN_CXX) $(WIN_FLAGS) -o $@ $(SRCS) $(WIN_LIBS)

clean:
	rm -f um-multitool um-multitool.exe

.PHONY: all linux win clean
