#!/usr/bin/env python3
# fix_english.py <lang folder>...: corrects the English of EIStarter OBT-1's lang\<code>\lang.ini files that are English copies (en, ko, zh): typos such as
# "independed" / "thrilinear" / "Videodriver", inconsistent capitals. Run by package.sh on the copies it ships; the key names are untouched.
import sys

REPLACE = [
    ("FPS independed cursor", "FPS-independent cursor"), ("thrilinear", "Trilinear"), ("=point", "=Point"), ("=bilinear", "=Bilinear"),
    ("Primary Videodriver", "Primary video driver"), ("cbFullscreen=FullScreen", "cbFullscreen=Fullscreen"),
    ("Pause when minimized game", "Pause the game when minimized"), ("Textures quality", "Texture quality"), ("Shadows quality", "Shadow quality"),
    ("Do not load .MQ (Quest) files from Host", "Do not load .MQ (quest) files from the host"),
    ("Game Workdir Path", "Game folder"), ("Game Resources Path", "Game resources folder"), ("lbPathCD=CD Path", "lbPathCD=CD folder"),
    ("Game Engine Filename", "Game engine file name"), ("ModInfo=ModInfo", "ModInfo=Mod info"), ("Singleplayer", "Single player"),
    ("cbbTextures__0=high", "cbbTextures__0=High"), ("cbbTextures__1=normal", "cbbTextures__1=Normal"), ("cbbTextures__2=low", "cbbTextures__2=Low"),
    ("cbbTerrain__1=normal", "cbbTerrain__1=Normal"),
]
HELP = 'tbPathsHelpDesc=[] - the part you edit in the text box<rn>{} - a "variable":<rn>    install path - where the original game.exe is<rn>    name - the file name<rn>    extension - the file extension (e.g. "exe")<rn><rn>'
for folder in sys.argv[1:]:
    path = folder.rstrip('/') + '/lang.ini'
    raw = open(path, 'rb').read()
    if raw[:2] in (b'\xff\xfe', b'\xfe\xff'):
        continue  # a UTF-16 translation: not ours
    lines = raw.decode('latin-1').split('\n')
    out = []
    for line in lines:
        for a, b in REPLACE:
            line = line.replace(a, b)
        if line.startswith('tbPathsHelpDesc='):
            line = HELP + ('\r' if line.endswith('\r') else '')
        out.append(line)
    open(path, 'wb').write('\n'.join(out).encode('latin-1'))
    print('fixed', path)
