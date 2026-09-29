# um-modelviewer2 (C++17): Dear ImGui + GLFW + OpenGL 2 item model viewer.
#   make / make linux   native Linux build (distro GLFW, pkg-config glfw3)
#   make win            Windows .exe cross-built with MinGW, using the static GLFW that
#                       um-multitool's vendor/build-glfw-mingw.sh builds
#   make all            both (win only when that GLFW build is present)

CXX ?= g++
TARGET = um-modelviewer2
WIN_CXX = x86_64-w64-mingw32-g++

IMGUI_DIR = vendor/imgui
IMGUI_SRCS = $(IMGUI_DIR)/imgui.cpp $(IMGUI_DIR)/imgui_draw.cpp $(IMGUI_DIR)/imgui_tables.cpp \
             $(IMGUI_DIR)/imgui_widgets.cpp $(IMGUI_DIR)/imgui_impl_glfw.cpp $(IMGUI_DIR)/imgui_impl_opengl2.cpp
SRCS = main.cpp $(IMGUI_SRCS)
HDRS = $(wildcard *.hpp)

CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -isystem $(IMGUI_DIR)
LINUX_FLAGS = $(shell pkg-config --cflags glfw3)
LINUX_LIBS = $(shell pkg-config --libs glfw3) -lGL

GLFW_MINGW = ../../um-multitool/vendor/glfw-mingw
WIN_FLAGS = -isystem $(GLFW_MINGW)/include -static -static-libgcc -static-libstdc++ -s
WIN_LIBS = -L$(GLFW_MINGW)/lib -lglfw3 -lopengl32 -lgdi32 -lcomdlg32 -lole32 -luuid -mwindows

linux: $(TARGET)

$(TARGET): $(SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) $(LINUX_FLAGS) -o $@ $(SRCS) $(LINUX_LIBS)

win: $(TARGET).exe

$(TARGET).exe: $(SRCS) $(HDRS)
	@if [ ! -f "$(GLFW_MINGW)/lib/libglfw3.a" ]; then \
		echo "No MinGW GLFW in $(GLFW_MINGW): run ../../um-multitool/vendor/build-glfw-mingw.sh first." >&2; exit 1; fi
	$(WIN_CXX) $(CXXFLAGS) $(WIN_FLAGS) -o $@ $(SRCS) $(WIN_LIBS)

all: linux
	@if [ -f "$(GLFW_MINGW)/lib/libglfw3.a" ] && command -v $(WIN_CXX) >/dev/null; then $(MAKE) win; \
	else echo "Skipping the Windows build (no MinGW or no MinGW GLFW)."; fi

clean:
	rm -f $(TARGET) $(TARGET).exe

.PHONY: linux win all clean
