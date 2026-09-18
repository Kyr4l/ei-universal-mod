#!/usr/bin/env bash
# Fetches and cross-compiles a static GLFW 3.5.1 (Win32 backend) for
# x86_64-w64-mingw32, installed locally under vendor/glfw-mingw/ (never
# installed system-wide). Windows has no X11/Wayland split to worry about;
# this is GLFW's native Win32 windowing backend.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PREFIX="$SCRIPT_DIR/glfw-mingw"
SRC_DIR="$SCRIPT_DIR/glfw-src"

if [[ -f "$PREFIX/lib/libglfw3.a" ]]; then
    echo "GLFW for mingw already built at $PREFIX (delete it to force a rebuild)."
    exit 0
fi

if ! command -v x86_64-w64-mingw32-gcc &>/dev/null; then
    echo "Error: x86_64-w64-mingw32-gcc not found. Install the mingw64 cross toolchain first." >&2
    exit 1
fi

MINGW_SYSROOT="$(x86_64-w64-mingw32-gcc -print-sysroot)/mingw"
if [[ ! -d "$MINGW_SYSROOT" ]]; then
    echo "Error: mingw sysroot not found at $MINGW_SYSROOT" >&2
    exit 1
fi

if [[ ! -d "$SRC_DIR" ]]; then
    git clone --branch 3.5.1 --depth 1 https://github.com/glfw/glfw.git "$SRC_DIR"
fi

cmake -S "$SRC_DIR" -B "$SRC_DIR/build-mingw" \
    -DCMAKE_SYSTEM_NAME=Windows \
    -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
    -DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres \
    -DCMAKE_FIND_ROOT_PATH="$MINGW_SYSROOT" \
    -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER \
    -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
    -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_BUILD_TYPE=Release \
    -DGLFW_BUILD_EXAMPLES=OFF \
    -DGLFW_BUILD_TESTS=OFF \
    -DGLFW_BUILD_DOCS=OFF \
    -DBUILD_SHARED_LIBS=OFF

cmake --build "$SRC_DIR/build-mingw" -j"$(nproc)"
cmake --install "$SRC_DIR/build-mingw"

echo "GLFW for mingw installed to $PREFIX"
