#!/usr/bin/env bash
# Fetches and cross-compiles a static FLTK 1.3.11 for x86_64-w64-mingw32,
# installed locally under vendor/fltk-mingw/ (never installed system-wide).
# Native Linux builds use the distro's fltk-devel package instead.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PREFIX="$SCRIPT_DIR/fltk-mingw"
SRC_DIR="$SCRIPT_DIR/fltk-src"

if [[ -f "$PREFIX/lib/libfltk.a" ]]; then
    echo "FLTK for mingw already built at $PREFIX (delete it to force a rebuild)."
    exit 0
fi

if ! command -v x86_64-w64-mingw32-g++ &>/dev/null; then
    echo "Error: x86_64-w64-mingw32-g++ not found. Install the mingw64 cross toolchain first." >&2
    exit 1
fi

MINGW_SYSROOT="$(x86_64-w64-mingw32-g++ -print-sysroot)/mingw"
if [[ ! -d "$MINGW_SYSROOT" ]]; then
    echo "Error: mingw sysroot not found at $MINGW_SYSROOT" >&2
    exit 1
fi

rm -rf "$SRC_DIR"
git clone --branch release-1.3.11 --depth 1 https://github.com/fltk/fltk.git "$SRC_DIR"

cmake -S "$SRC_DIR" -B "$SRC_DIR/build-mingw" \
    -DCMAKE_SYSTEM_NAME=Windows \
    -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
    -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++ \
    -DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres \
    -DCMAKE_FIND_ROOT_PATH="$MINGW_SYSROOT" \
    -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER \
    -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
    -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_BUILD_TYPE=Release \
    -DFLTK_BUILD_TEST=OFF \
    -DFLTK_BUILD_EXAMPLES=OFF \
    -DOPTION_BUILD_SHARED_LIBS=OFF

cmake --build "$SRC_DIR/build-mingw" -j"$(nproc)"
cmake --install "$SRC_DIR/build-mingw"

echo "FLTK for mingw installed to $PREFIX"
