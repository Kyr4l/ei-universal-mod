# Makefile for um-modelviewer (C++17)
# Dear ImGui + GLFW + OpenGL2 3D viewer for Evil Islands .fig/.bon/.mod/.anm
# figures - see main.cpp's header comment for the toolkit rationale (shared
# with um-multitool-gui).

CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pthread
TARGET ?= um-modelviewer

WIN_CXX = x86_64-w64-mingw32-g++
WIN_FLAGS = -std=c++17 -O2 -Wall -Wextra -pthread -static -static-libgcc -static-libstdc++

ifeq ($(OS),Windows_NT)
    TARGET_BIN = $(TARGET).exe
else
    TARGET_BIN = $(TARGET)
endif

SRCS = main.cpp
HDRS = res_archive.hpp figure_format.hpp mmp_texture.hpp db_units.hpp asset_source.hpp \
       model_loader.hpp camera.hpp config.hpp inflate.hpp zip_reader.hpp xlsx_reader.hpp \
       db_schema_generated.hpp cp1251.hpp

IMGUI_DIR = vendor/imgui
IMGUI_SRCS = $(IMGUI_DIR)/imgui.cpp $(IMGUI_DIR)/imgui_draw.cpp $(IMGUI_DIR)/imgui_tables.cpp \
             $(IMGUI_DIR)/imgui_widgets.cpp $(IMGUI_DIR)/imgui_impl_glfw.cpp $(IMGUI_DIR)/imgui_impl_opengl2.cpp
ALL_SRCS = $(SRCS) $(IMGUI_SRCS)

# Native Linux build uses the distro's GLFW (libglfw-devel / glfw3 pkg-config).
GLFW_CXXFLAGS = $(shell pkg-config --cflags glfw3) -isystem $(IMGUI_DIR)
GLFW_LDFLAGS = $(shell pkg-config --libs glfw3) -lGL

# Windows cross-compile uses the locally vendored static GLFW build (see
# vendor/build-glfw-mingw.sh; shared with um-multitool's Windows GUI target).
GLFW_MINGW_PREFIX = vendor/glfw-mingw
WIN_GUI_FLAGS = -std=c++17 -O2 -Wall -Wextra -isystem $(GLFW_MINGW_PREFIX)/include -isystem $(IMGUI_DIR) \
                -static -static-libgcc -static-libstdc++
WIN_GUI_LDFLAGS = -L$(GLFW_MINGW_PREFIX)/lib -lglfw3 -lopengl32 -lgdi32 -lcomdlg32 -lole32 -luuid -mwindows

all: $(TARGET_BIN)

$(TARGET_BIN): $(ALL_SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) $(GLFW_CXXFLAGS) -o $@ $(ALL_SRCS) $(GLFW_LDFLAGS)

win: $(TARGET).exe

$(TARGET).exe: $(ALL_SRCS) $(HDRS)
	@if [ ! -f "$(GLFW_MINGW_PREFIX)/lib/libglfw3.a" ]; then \
		echo "Error: $(GLFW_MINGW_PREFIX) not found. Run vendor/build-glfw-mingw.sh first." >&2; \
		exit 1; \
	fi
	$(WIN_CXX) $(WIN_GUI_FLAGS) -o $@ $(ALL_SRCS) $(WIN_GUI_LDFLAGS)

clean:
	rm -f um-modelviewer um-modelviewer.exe

.PHONY: all clean win
