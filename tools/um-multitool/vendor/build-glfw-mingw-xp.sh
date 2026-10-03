#!/usr/bin/env bash
# Static GLFW builds for the Windows XP variants of um-multitool (make xp):
#   vendor/glfw-mingw-xp32  i686   (Windows XP, 32-bit)
#   vendor/glfw-mingw-xp64  x86_64 (Windows XP x64)
# GLFW 3.3.10: the last GLFW that supports Windows XP and Vista (3.4 and later require Windows 7: an .exe built
# with them starts on XP, the command line works, but no window ever shows). 3.3 loads the Vista+ functions it
# can use (SetProcessDPIAware...) at run time, so it needs no patch. Never installed system-wide.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$SCRIPT_DIR/glfw-src-xp33"
VERSION=3.3.10

if [[ ! -d "$SRC_DIR" ]]; then
    git clone --branch "$VERSION" --depth 1 https://github.com/glfw/glfw.git "$SRC_DIR"
fi

build() { # <triplet> <prefix dir>
    local triplet=$1 prefix=$2
    if [[ -f "$prefix/lib/libglfw3.a" ]] && grep -q "GLFW_VERSION_MINOR *3$" "$prefix/include/GLFW/glfw3.h" 2>/dev/null; then
        echo "$prefix already built (GLFW $VERSION; delete it to rebuild)"; return
    fi
    rm -rf "$prefix" "$SRC_DIR/build-$triplet"
    command -v "$triplet-gcc" &>/dev/null || { echo "Error: $triplet-gcc not found" >&2; exit 1; }
    local sysroot; sysroot="$("$triplet-gcc" -print-sysroot)/mingw"
    cmake -S "$SRC_DIR" -B "$SRC_DIR/build-$triplet" \
        -DCMAKE_SYSTEM_NAME=Windows \
        -DCMAKE_C_COMPILER="$triplet-gcc" \
        -DCMAKE_RC_COMPILER="$triplet-windres" \
        -DCMAKE_C_FLAGS="-D_WIN32_WINNT=0x0501 -DWINVER=0x0501" \
        -DCMAKE_FIND_ROOT_PATH="$sysroot" \
        -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER \
        -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
        -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY \
        -DCMAKE_INSTALL_PREFIX="$prefix" \
        -DCMAKE_BUILD_TYPE=Release \
        -DGLFW_BUILD_EXAMPLES=OFF -DGLFW_BUILD_TESTS=OFF -DGLFW_BUILD_DOCS=OFF -DBUILD_SHARED_LIBS=OFF
    cmake --build "$SRC_DIR/build-$triplet" -j"$(nproc)"
    cmake --install "$SRC_DIR/build-$triplet"
}
build i686-w64-mingw32 "$SCRIPT_DIR/glfw-mingw-xp32"
build x86_64-w64-mingw32 "$SCRIPT_DIR/glfw-mingw-xp64"
echo "GLFW $VERSION for the XP builds installed"
