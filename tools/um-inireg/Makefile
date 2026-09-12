# Makefile for um-inireg (C++17)

CXX ?= g++
CXXFLAGS ?= -std=c++17 -O3 -Wall -Wextra -pthread
TARGET ?= um-inireg

# Cross-compilation variables for Windows (.exe)
WIN_CXX = x86_64-w64-mingw32-g++
WIN_FLAGS = -std=c++17 -O3 -Wall -Wextra -pthread -static -static-libgcc -static-libstdc++

ifeq ($(OS),Windows_NT)
    TARGET_BIN = $(TARGET).exe
else
    TARGET_BIN = $(TARGET)
endif

SRCS = inireg.cpp

all: $(TARGET_BIN)

$(TARGET_BIN): $(SRCS)
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS)

# Cross-compile target for Windows .exe from Linux
win: $(TARGET).exe

$(TARGET).exe: $(SRCS)
	$(WIN_CXX) $(WIN_FLAGS) -o $@ $(SRCS)

clean:
	rm -f um-inireg um-inireg.exe

.PHONY: all clean win
