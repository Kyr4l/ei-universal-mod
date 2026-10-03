# Makefile for um-bot (C++17): the Evil Islands companion player, standalone.
#
#   make linux  - um-bot (native Linux, X11 or Wayland through GLFW)
#   make win    - um-bot.exe, cross-compiled with MinGW
#   make all    - both (the Windows part is skipped when MinGW is not installed)
#
# Dear ImGui and the MinGW GLFW build are um-multitool's vendored copies (../um-multitool/vendor).

CXX ?= g++
TARGET ?= um-bot
WIN_CXX = x86_64-w64-mingw32-g++

VENDOR = ../um-multitool/vendor
IMGUI_DIR = $(VENDOR)/imgui
IMGUI_SRCS = $(IMGUI_DIR)/imgui.cpp $(IMGUI_DIR)/imgui_draw.cpp $(IMGUI_DIR)/imgui_tables.cpp \
             $(IMGUI_DIR)/imgui_widgets.cpp $(IMGUI_DIR)/imgui_impl_glfw.cpp $(IMGUI_DIR)/imgui_impl_opengl2.cpp
SRCS = main.cpp $(IMGUI_SRCS)
HDRS = $(wildcard *.hpp) assets/um-bot.ico ../um-multitool/mp_file.hpp ../um-multitool/viewer/item_db.hpp

COMMON_FLAGS = -std=c++17 -O2 -Wall -Wextra -pthread -I. -I../um-multitool -isystem $(IMGUI_DIR)
LINUX_FLAGS = $(COMMON_FLAGS) $(shell pkg-config --cflags glfw3)
LINUX_LIBS = $(shell pkg-config --libs glfw3) -lGL -ldl -lm

GLFW_MINGW_PREFIX = $(VENDOR)/glfw-mingw
WIN_FLAGS = $(COMMON_FLAGS) -isystem $(GLFW_MINGW_PREFIX)/include -static -static-libgcc -static-libstdc++ -s -mwindows
WIN_LIBS = -L$(GLFW_MINGW_PREFIX)/lib -lglfw3 -lopengl32 -lgdi32 -lcomdlg32 -lole32 -lshell32 -luuid -lws2_32

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

# The .exe's icon (assets/um-bot.ico), compiled into a resource object.
WIN_RES = um-bot-res.o
$(WIN_RES): um-bot.rc assets/um-bot.ico
	x86_64-w64-mingw32-windres $< -O coff -o $@

$(TARGET).exe: $(SRCS) $(HDRS) $(WIN_RES)
	$(WIN_CXX) $(WIN_FLAGS) -o $@ $(SRCS) $(WIN_RES) $(WIN_LIBS)

clean:
	rm -f $(TARGET) $(TARGET).exe $(WIN_RES)

.PHONY: all linux win clean
