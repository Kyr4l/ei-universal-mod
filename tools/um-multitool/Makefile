# Makefile for um-multitool (C++17): ONE binary holding the command-line tools
# (ddsmmp, inireg, mobdump, restool, xlsxdb, viewer, map) and the GUI (File Processing,
# 3D Viewer, Map Editor and Settings tabs). Double-clicked it opens the GUI; from a terminal without
# arguments it prints the usage; `um-multitool gui` opens the GUI from a terminal.
#
#   make linux  - um-multitool (native Linux)
#   make win    - um-multitool.exe, cross-compiled with MinGW
#   make all    - both (the Windows part is skipped when MinGW is not installed)
#   make xp     - um-multitool-xp32.exe for Windows XP (32-bit runs on XP x64 too; after vendor/build-glfw-mingw-xp.sh)
# Builds run on all cores; objects go to build/<variant>/.
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
SRCS = main.cpp i18n.cpp ddsmmp.cpp inireg.cpp mobdumper.cpp restool.cpp xlsxdb.cpp dbexport.cpp db_model.cpp db_editor.cpp text_editor.cpp mp_editor.cpp alerts.cpp log.cpp gui_main.cpp viewer/viewer_app.cpp mapedit/map_app.cpp dllconnect/connector_app.cpp texedit/texture_app.cpp texedit/quest_map.cpp vendor/stb/stb_image_impl.cpp $(IMGUI_SRCS)
# um.dll's mission script checker is shared with the Map Editor's checks (not copied).
UMDLL_DIR = ../../resources/universal-mod/um-dll
HDRS = $(wildcard *.hpp) lang/ru.txt $(wildcard viewer/*.hpp) $(wildcard mapedit/*.hpp) $(wildcard dllconnect/*.hpp) $(wildcard texedit/*.hpp) $(UMDLL_DIR)/mob_script_check.hpp $(UMDLL_DIR)/mob_script_functions.hpp $(wildcard sfx/*.mp3)

# -I. lets viewer/ share this directory's cp1251.hpp; -isystem keeps -Wextra off vendored code.
COMMON_FLAGS = -std=c++17 -O2 -Wall -Wextra -pthread -I. -I$(UMDLL_DIR) -isystem $(IMGUI_DIR) -isystem vendor/miniaudio

LINUX_FLAGS = $(COMMON_FLAGS) $(shell pkg-config --cflags glfw3)
LINUX_LIBS = $(shell pkg-config --libs glfw3) -lGL -ldl -lm

# Console subsystem on purpose (no -mwindows): command-line use then behaves normally in cmd and
# PowerShell, and a double-click only briefly shows the console window before the GUI replaces it.
GLFW_MINGW_PREFIX = vendor/glfw-mingw
WIN_FLAGS = $(COMMON_FLAGS) -isystem $(GLFW_MINGW_PREFIX)/include -static -static-libgcc -static-libstdc++ -s
WIN_LIBS = -L$(GLFW_MINGW_PREFIX)/lib -lglfw3 -lopengl32 -lgdi32 -lcomdlg32 -lole32 -lshell32 -luuid -lws2_32

WIN_BINS =
ifneq ($(OS),Windows_NT)
ifneq ($(shell command -v $(WIN_CXX) 2>/dev/null),)
WIN_BINS = win
endif
endif

# Parallel by default: one object file per source (build/<variant>/), all cores; headers tracked by -MMD.
MAKEFLAGS += -j$(shell nproc)
DEPFLAGS = -MMD -MP

all: linux $(WIN_BINS)

linux: $(TARGET)
win: $(TARGET).exe

# One object list per variant, and the rule to build them.
LINUX_OBJS = $(SRCS:%.cpp=build/linux/%.o)
WIN_OBJS = $(SRCS:%.cpp=build/win/%.o)
XP32_OBJS = $(SRCS:%.cpp=build/xp32/%.o)

build/linux/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(LINUX_FLAGS) $(DEPFLAGS) -c $< -o $@
build/win/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(WIN_CXX) $(WIN_FLAGS) $(DEPFLAGS) -c $< -o $@
build/xp32/%.o: %.cpp
	@mkdir -p $(dir $@)
	i686-w64-mingw32-g++ $(XP_FLAGS) -isystem vendor/glfw-mingw-xp32/include $(DEPFLAGS) -c $< -o $@
# The sounds are assembled into alerts.cpp (.incbin): -MMD does not see them.
build/linux/alerts.o build/win/alerts.o build/xp32/alerts.o: $(wildcard sfx/*.mp3)

$(TARGET): $(LINUX_OBJS)
	$(CXX) $(LINUX_FLAGS) -o $@ $^ $(LINUX_LIBS)

# Requires vendor/glfw-mingw to exist first: run vendor/build-glfw-mingw.sh
# The .exe's icon (assets/um-multitool.ico), compiled into a resource object.
WIN_RES = um-multitool-res.o
$(WIN_RES): um-multitool.rc assets/um-multitool.ico
	x86_64-w64-mingw32-windres $< -O coff -o $@

$(TARGET).exe: $(WIN_OBJS) $(WIN_RES)
	@if [ ! -f "$(GLFW_MINGW_PREFIX)/lib/libglfw3.a" ]; then \
		echo "Error: $(GLFW_MINGW_PREFIX) not found. Run vendor/build-glfw-mingw.sh first." >&2; \
		exit 1; \
	fi
	$(WIN_CXX) $(WIN_FLAGS) -o $@ $^ $(WIN_LIBS)

# Windows XP variant (make xp): um-multitool-xp32.exe (XP, 32-bit; it runs on XP x64 too), with
# the GLFW built by vendor/build-glfw-mingw-xp.sh (no Vista-only imports). Same code; C++17 runtime static.
XP_FLAGS = $(COMMON_FLAGS) -D_WIN32_WINNT=0x0501 -DWINVER=0x0501 -static -static-libgcc -static-libstdc++ -s
XP_LIBS = -lglfw3 -lopengl32 -lgdi32 -lcomdlg32 -lole32 -lshell32 -luuid -lws2_32
xp: $(TARGET)-xp32.exe

um-multitool-res32.o: um-multitool.rc assets/um-multitool.ico
	i686-w64-mingw32-windres $< -O coff -o $@

$(TARGET)-xp32.exe: $(XP32_OBJS) um-multitool-res32.o
	@if [ ! -f vendor/glfw-mingw-xp32/lib/libglfw3.a ]; then echo "Run vendor/build-glfw-mingw-xp.sh first." >&2; exit 1; fi
	i686-w64-mingw32-g++ $(XP_FLAGS) -o $@ $^ -Lvendor/glfw-mingw-xp32/lib $(XP_LIBS)

-include $(LINUX_OBJS:.o=.d) $(WIN_OBJS:.o=.d) $(XP32_OBJS:.o=.d)

clean:
	rm -rf build um-multitool um-multitool.exe um-multitool-xp32.exe $(WIN_RES) um-multitool-res32.o

.PHONY: all linux win xp clean
