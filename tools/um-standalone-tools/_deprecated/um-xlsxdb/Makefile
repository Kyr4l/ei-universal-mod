# Makefile for um-xlsxdb (C++17)

CXX ?= g++
CXXFLAGS ?= -std=c++17 -O3 -Wall -Wextra
TARGET ?= um-xlsxdb

# Cross-compilation variables for Windows (.exe)
WIN_CXX = x86_64-w64-mingw32-g++
WIN_FLAGS = -std=c++17 -O3 -Wall -Wextra -static -static-libgcc -static-libstdc++

ifeq ($(OS),Windows_NT)
    TARGET_BIN = $(TARGET).exe
else
    TARGET_BIN = $(TARGET)
endif

SRCS = um-xlsxdb.cpp
HDRS = inflate.hpp zip_reader.hpp xlsx_reader.hpp cp1251.hpp db_schema_generated.hpp

all: $(TARGET_BIN)

$(TARGET_BIN): $(SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS)

# Cross-compile target for Windows .exe from Linux
win: $(TARGET).exe

$(TARGET).exe: $(SRCS) $(HDRS)
	$(WIN_CXX) $(WIN_FLAGS) -o $@ $(SRCS)

clean:
	rm -f um-xlsxdb um-xlsxdb.exe

.PHONY: all clean win
