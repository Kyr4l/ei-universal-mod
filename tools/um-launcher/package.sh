#!/usr/bin/env bash
# package.sh: builds dist/um-launcher-<version>.zip, the launcher as users get it: um-launcher.exe next to the files EIStarter OBT-1 brings
# (taken from ../eistarter-obt-1.zip, which this repository already ships): addon.dll, Engine/ (dinput.dll loader, the game's exe and its
# helper DLLs, the .asi plugins), design/ (skins), lang/ (texts), mods/.0 (the "no mods" entry), plus um-window.asi (borderless window for
# every mod). The mods (Universal-Mod ...) are unpacked into its mods/ folder by the user.
# Usage: ./package.sh        (needs 7z, unzip; um-launcher.exe and window/um-window.asi built: make win; make window)
set -euo pipefail
cd "$(dirname "$0")"
VERSION=$(grep -o 'kVersion = "[^"]*"' main.cpp | head -1 | sed 's/.*"\(.*\)"/\1/')
STARTER_ZIP=../eistarter-obt-1.zip
[ -f um-launcher.exe ] && [ -f window/um-window.asi ] || { echo "build first: make win window"; exit 1; }
[ -f "$STARTER_ZIP" ] || { echo "$STARTER_ZIP not found"; exit 1; }
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
unzip -q "$STARTER_ZIP" -d "$TMP"
SRC="$TMP/EIStarter OBT-1"
OUT="$TMP/um-launcher"
mkdir -p "$OUT/mods" dist
cp -r "$SRC/design" "$SRC/lang" "$SRC/Engine" "$OUT/"
cp "$SRC/addon.dll" "$OUT/"
cp -r "$SRC/mods/.0" "$OUT/mods/"
mkdir -p "$OUT/docs"
cp "$SRC/docs/how to make translation.txt" "$SRC/docs/EIStarter info.rtf" "$SRC/docs/whatnews.htm" "$OUT/docs/"
cp ../../docs/launcher.md "$OUT/docs/um-launcher.md"
cp um-launcher.exe "$OUT/"
cp window/um-window.asi "$OUT/Engine/"
cat > "$OUT/README.txt" <<TXT
um-launcher $VERSION - starts Evil Islands with a mod (a replacement of EIStarter).

1. Unpack this folder anywhere (it needs write access: launcher.cfg is created here).
2. Unpack a mod (for example Universal-Mod) into its mods folder.
3. Run um-launcher.exe (under Linux: with Wine, in the prefix of the game), choose the mod, click Play.
   The first time it asks for the folder of the original game if it cannot find it.
Nothing else to set up: no registry file, no DLL override. Details: docs/um-launcher.md
TXT
rm -f "dist/um-launcher-$VERSION.zip"
(cd "$TMP" && 7z a -tzip -mx=9 -bso0 -bsp0 "$OLDPWD/dist/um-launcher-$VERSION.zip" um-launcher)
ls -la "dist/um-launcher-$VERSION.zip"
