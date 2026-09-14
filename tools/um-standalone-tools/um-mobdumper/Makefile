# Makefile for um-mobdump (C++17)

CXX ?= g++
CXXFLAGS ?= -std=c++17 -O3 -Wall -Wextra -pthread
TARGET ?= um-mobdump

# Cross-compilation variables
WIN_CXX = x86_64-w64-mingw32-g++
WIN_FLAGS = -std=c++17 -O3 -Wall -Wextra -pthread -static -static-libgcc -static-libstdc++

# Check if compiling for Windows natively
ifeq ($(OS),Windows_NT)
    TARGET_BIN = $(TARGET).exe
else
    TARGET_BIN = $(TARGET)
endif

SRCS = mobdumper.cpp

all: $(TARGET_BIN)

$(TARGET_BIN): $(SRCS)
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS)

# Cross-compile target for Windows .exe from Linux
win: $(TARGET).exe

$(TARGET).exe: $(SRCS)
	$(WIN_CXX) $(WIN_FLAGS) -o $@ $(SRCS)

clean:
	rm -f um-mobdump um-mobdump.exe

.PHONY: all clean win
