#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Kyr4l
# Packs um-multitool as an AppImage (make appimage): one portable file for Linux desktops, started by a
# double click or from a terminal with the usual arguments (AppRun is the binary itself).
#   build-appimage.sh <linked binary> <output .AppImage>
# AppDir layout: usr/bin/um-multitool with the files it reads beside it (lang/ru.txt, materials/,
# assets/logo-256.png for install-desktop) and usr/lib/libglfw.so.3 taken from this machine (found
# through the binary's rpath, see the Makefile). libGL, X11 and Wayland come from the user's system, as
# for any AppImage. The mount is read-only: the settings, the log and script_commands.txt go to
# ~/.config/um-multitool when $APPIMAGE is set (viewer/config.hpp: UserDir).
# appimagetool and the AppImage runtime (the small static loader the squashfs is appended to) are fetched
# once into vendor/ (gitignored): appimagetool would download the runtime itself at every run, and hangs
# when that download stalls.
set -euo pipefail

BIN="$1"
OUT="$2"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOOL="$HERE/vendor/appimagetool"
APPDIR="$HERE/build/appimage/AppDir"
TOOL_URL="https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage"
RUNTIME="$HERE/vendor/appimage-runtime-x86_64"
RUNTIME_URL="https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64"

fetch() { # fetch <url> <file>
    echo "Fetching $2"
    curl -fL --retry 3 -m 300 -o "$2.part" "$1"
    chmod +x "$2.part"
    mv "$2.part" "$2"
}
[[ -x "$TOOL" ]] || fetch "$TOOL_URL" "$TOOL"
[[ -f "$RUNTIME" ]] || fetch "$RUNTIME_URL" "$RUNTIME"

GLFW="$(ldd "$BIN" | awk '/libglfw\.so\.3/ { print $3 }')"
if [[ -z "$GLFW" || ! -f "$GLFW" ]]; then
    echo "Error: libglfw.so.3 not found by ldd $BIN" >&2
    exit 1
fi

rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin/lang" "$APPDIR/usr/bin/assets" "$APPDIR/usr/bin/materials" "$APPDIR/usr/lib"
cp "$BIN" "$APPDIR/usr/bin/um-multitool"
cp "$HERE/lang/ru.txt" "$APPDIR/usr/bin/lang/"
cp "$HERE/assets/logo-256.png" "$APPDIR/usr/bin/assets/"
shopt -s nullglob
for f in "$HERE"/materials/*.tsv; do cp "$f" "$APPDIR/usr/bin/materials/"; done
shopt -u nullglob
cp -L "$GLFW" "$APPDIR/usr/lib/libglfw.so.3"
# The menu entry (without the xdg-open shebang line) and the icon, where appimagetool wants them.
grep -v '^#!' "$HERE/um-multitool.desktop" > "$APPDIR/um-multitool.desktop"
cp "$HERE/assets/logo-256.png" "$APPDIR/um-multitool.png"
ln -s um-multitool.png "$APPDIR/.DirIcon"
ln -s usr/bin/um-multitool "$APPDIR/AppRun"

rm -f "$OUT"
# --appimage-extract-and-run: appimagetool is an AppImage itself and this way needs no FUSE.
ARCH=x86_64 "$TOOL" --appimage-extract-and-run --runtime-file "$RUNTIME" "$APPDIR" "$OUT"
chmod +x "$OUT"
ls -la "$OUT"
