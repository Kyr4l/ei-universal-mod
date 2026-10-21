#!/usr/bin/env bash
# package.sh: builds dist/EIStarter-UM-<version>.zip, the launcher as users get it: EIStarter-UM.exe next to the files EIStarter OBT-1 brings
# (taken from ../eistarter-obt-1.zip, which this repository already ships): addon.dll, Engine/ (dinput.dll loader, the game's exe and its
# helper DLLs, mp3dec.asi, SpellAddonX.asi; not EI_Plugin.asi / EI_Plugin.ini, EIStarter's docs or its whatsnew), design/ (skins), lang/ (texts), mods/.0 (the "no mods" entry), plus um-window.asi (borderless window for
# every mod). The mods (Universal-Mod ...) are unpacked into its mods/ folder by the user.
# Usage: ./package.sh        (needs 7z, unzip; EIStarter-UM.exe and window/um-window.asi built: make win; make window)
set -euo pipefail
cd "$(dirname "$0")"
VERSION=$(grep -o 'kVersion = "[^"]*"' main.cpp | head -1 | sed 's/.*"\(.*\)"/\1/')
STARTER_ZIP=../eistarter-obt-1.zip
[ -f EIStarter-UM.exe ] && [ -f window/um-window.asi ] || { echo "build first: make win window"; exit 1; }
[ -f "$STARTER_ZIP" ] || { echo "$STARTER_ZIP not found"; exit 1; }
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
unzip -q "$STARTER_ZIP" -d "$TMP"
SRC="$TMP/EIStarter OBT-1"
OUT="$TMP/EIStarter-UM"
mkdir -p "$OUT/mods" dist
cp -r "$SRC/design" "$SRC/lang" "$SRC/Engine" "$OUT/"
python3 fix_english.py "$OUT/lang/en" "$OUT/lang/ko" "$OUT/lang/zh" # the English copies of the texts: typos of EIStarter OBT-1 corrected
cp "$SRC/addon.dll" "$OUT/"
cp -r "$SRC/mods/.0" "$OUT/mods/"
mkdir -p "$OUT/docs"
cp "$SRC/docs/how to make translation.txt" "$OUT/docs/"
rm -f "$OUT/Engine/EI_Plugin.asi" "$OUT/Engine/EI_Plugin.ini" # its comfort features are not needed by Universal Mod
cp ../../docs/EIStarter-UM.md "$OUT/docs/EIStarter-UM.md"
cp EIStarter-UM.exe "$OUT/"
cp window/um-window.asi "$OUT/Engine/"
cat > "$OUT/README.txt" <<TXT
EIStarter-UM $VERSION - starts Evil Islands with a mod (a replacement of EIStarter).

1. Unpack this folder anywhere (it needs write access: EIStarter-UM.cfg is created here).
2. Unpack a mod (for example Universal-Mod) into its mods folder.
3. Run EIStarter-UM.exe (under Linux: with Wine, in the prefix of the game), choose the mod, click Play.
   The first time it asks for the folder of the original game if it cannot find it.
Nothing else to set up: no registry file, no DLL override. Details: docs/EIStarter-UM.md

Credits
- The look (design/), the texts (lang/), the loader (Engine/dinput.dll), addon.dll and the audio plugin mp3dec.asi of Engine/ come from EIStarter OBT-1 and the
  Evil Islands Addon engine by the EIStarter / Gipat.Ru authors (Sun, Atm, Demoth, Void Dragon, Lost Soul, Asp Adm; translations: Tyrianix, Fred,
  Peter plEI ...; addon.dll: Jet, Nivea, Holy Warrior, Sagrer, Barges, Demoth ...). Thank you.
- EIStarter-UM itself (the program, um-window.asi) is part of the Universal Mod project: https://github.com/Kyr4l/ei-universal-mod
- Engine/game.exe is the game's own executable (Nival Interactive / Fishtank Interactive).
TXT
rm -f "dist/EIStarter-UM-$VERSION.zip"
(cd "$TMP" && 7z a -tzip -mx=9 -bso0 -bsp0 "$OLDPWD/dist/EIStarter-UM-$VERSION.zip" EIStarter-UM)
ls -la "dist/EIStarter-UM-$VERSION.zip"
