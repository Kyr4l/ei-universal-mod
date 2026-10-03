#!/usr/bin/env bash
# Static GLFW 3.5.1 builds for the Windows XP variants of um-multitool (make xp):
#   vendor/glfw-mingw-xp32  i686   (Windows XP, 32-bit)
#   vendor/glfw-mingw-xp64  x86_64 (Windows XP x64)
# GLFW calls SetProcessDPIAware (Vista and later) directly: XP refuses to start a program that imports it,
# even if the call is never made there. The copy built here loads it with GetProcAddress instead (patched
# source in vendor/glfw-src-xp, never installed system-wide).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$SCRIPT_DIR/glfw-src-xp"

if [[ ! -d "$SRC_DIR" ]]; then
    git clone --branch 3.5.1 --depth 1 https://github.com/glfw/glfw.git "$SRC_DIR"
fi
# The patch: SetProcessDPIAware through GetProcAddress (idempotent).
if ! grep -q "UM_XP_PATCH" "$SRC_DIR/src/win32_init.c"; then
    python3 - "$SRC_DIR/src/win32_init.c" <<'EOF'
import sys, re
p = sys.argv[1]; s = open(p).read()
new = ('{ /* UM_XP_PATCH: Vista+ only, loaded at run time so XP can start the program */ '
       'BOOL (WINAPI* f)(void) = (BOOL (WINAPI*)(void)) GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDPIAware"); '
       'if (f) f(); }')
s2, n = re.subn(r'SetProcessDPIAware\(\);', new, s)
if n == 0: sys.exit("SetProcessDPIAware() call not found in " + p)
open(p, 'w').write(s2)
print("patched", n, "call(s)")
EOF
fi

# ChangeWindowMessageFilterEx (Windows 7 and later): the same, in win32_window.c.
if ! grep -q "UM_XP_PATCH" "$SRC_DIR/src/win32_window.c"; then
    python3 - "$SRC_DIR/src/win32_window.c" <<'EOF2'
import sys, re
p = sys.argv[1]; s = open(p).read()
s2, n = re.subn(r'ChangeWindowMessageFilterEx\(', 'umChangeWindowMessageFilterEx(', s)
if n == 0: sys.exit("ChangeWindowMessageFilterEx not found in " + p)
helper = ('/* UM_XP_PATCH: Windows 7+ only, loaded at run time so XP can start the program */\n'
          'static BOOL umChangeWindowMessageFilterEx(HWND w, UINT m, DWORD a, void* p)\n'
          '{ BOOL (WINAPI* f)(HWND, UINT, DWORD, void*) = (BOOL (WINAPI*)(HWND, UINT, DWORD, void*))\n'
          '  GetProcAddress(GetModuleHandleW(L"user32.dll"), "ChangeWindowMessageFilterEx"); return f ? f(w, m, a, p) : FALSE; }\n')
i = s2.index('\n', s2.index('#include "internal.h"')) + 1
s2 = s2[:i] + helper + s2[i:]
open(p, 'w').write(s2)
print("patched", n, "call(s)")
EOF2
fi

build() { # <triplet> <prefix dir>
    local triplet=$1 prefix=$2
    if [[ -f "$prefix/lib/libglfw3.a" ]]; then echo "$prefix already built (delete it to rebuild)"; return; fi
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
echo "GLFW for the XP builds installed"
