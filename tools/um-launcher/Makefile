# Makefile for um-launcher (C++17): the mod launcher that replaces EIStarter (see docs/launcher.md).
#
#   make win    - um-launcher.exe: 32-bit, GLFW 3.3 (runs on Windows XP and newer, and under Wine), the build to ship
#   make linux  - um-launcher (native: shows the screens; starting the game needs the Windows registry, so use the .exe under Wine)
#   make window - window/um-window.asi (the borderless-window plugin)
#   make all    - all of them
#   ./package.sh - dist/um-launcher-<version>.zip, the package users get
#   make test   - the file-handling tests against a real EIStarter folder (make test STARTER=/path/to/EIStarter)
#
# Dear ImGui, GLFW and stb_image are um-multitool's vendored copies (../um-multitool/vendor).

CXX ?= g++
WIN_CXX = i686-w64-mingw32-g++
VENDOR = ../um-multitool/vendor
IMGUI_DIR = $(VENDOR)/imgui
IMGUI_SRCS = $(IMGUI_DIR)/imgui.cpp $(IMGUI_DIR)/imgui_draw.cpp $(IMGUI_DIR)/imgui_tables.cpp \
             $(IMGUI_DIR)/imgui_widgets.cpp $(IMGUI_DIR)/imgui_impl_glfw.cpp $(IMGUI_DIR)/imgui_impl_opengl2.cpp
# i18n.cpp: the translation hooks the vendored ImGui calls (the launcher's own texts come from lang.ini).
SRCS = main.cpp ../um-multitool/i18n.cpp $(VENDOR)/stb/stb_image_impl.cpp $(IMGUI_SRCS)
HDRS = $(wildcard *.hpp) ../um-multitool/i18n.hpp ../um-multitool/lang/ru.txt

COMMON_FLAGS = -std=c++17 -O2 -Wall -Wextra -pthread -I. -I../um-multitool -I$(VENDOR) -isystem $(IMGUI_DIR) -isystem $(VENDOR)/stb
LINUX_FLAGS = $(COMMON_FLAGS) $(shell pkg-config --cflags glfw3)
LINUX_LIBS = $(shell pkg-config --libs glfw3) -lGL -ldl -lm
WIN_FLAGS = $(COMMON_FLAGS) -D_WIN32_WINNT=0x0501 -DWINVER=0x0501 -isystem $(VENDOR)/glfw-mingw-xp32/include -static -static-libgcc -static-libstdc++ -s -mwindows
WIN_LIBS = -L$(VENDOR)/glfw-mingw-xp32/lib -lglfw3 -lopengl32 -lgdi32 -lcomdlg32 -lole32 -lshell32 -luuid -lwinmm -lws2_32

all: linux win window

linux: um-launcher
um-launcher: $(SRCS) $(HDRS)
	$(CXX) $(LINUX_FLAGS) -o $@ $(SRCS) $(LINUX_LIBS)

win: um-launcher.exe
um-launcher.exe: $(SRCS) $(HDRS)
	$(WIN_CXX) $(WIN_FLAGS) -o $@ $(SRCS) $(WIN_LIBS)

# um-window.asi: the borderless-window plugin (resources/universal-mod/um-dll/um_window.cpp built on its own); addon.dll loads it from Engine\.
window: window/um-window.asi
window/um-window.asi: window/um_window_plugin.cpp window/um_window_standalone.hpp ../../resources/universal-mod/um-dll/um_window.cpp
	$(WIN_CXX) -shared -DUM_WINDOW_STANDALONE -o $@ window/um_window_plugin.cpp ../../resources/universal-mod/um-dll/um_window.cpp -Iwindow -std=c++17 -O2 -static -static-libgcc -s -Wall -Wextra

STARTER ?= .
test: tests/launcher_tests
	./tests/launcher_tests "$(STARTER)"
tests/launcher_tests: tests/launcher_tests.cpp $(HDRS)
	$(CXX) -std=c++17 -O1 -Wall -Wextra -I. -o $@ $<

clean:
	rm -f um-launcher um-launcher.exe window/um-window.asi tests/launcher_tests

.PHONY: all linux win window test clean
