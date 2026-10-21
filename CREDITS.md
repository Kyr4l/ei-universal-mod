# Credits

Back to the [README](README.md).

## EIStarter, the Addon engine and SpellAddon

EIStarter-UM reads the formats of EIStarter OBT-1 and ships its skins, texts, loader and `addon.dll`, by the EIStarter / Gipat.Ru authors: Sun, Atm, Demoth, Void Dragon, Lost Soul and Asp Adm (translations: Tyrianix, Fred, Peter plEI and others; the addon library: Jet, Nivea, Holy Warrior, Sagrer, Barges, Demoth and others). <https://allods.gipat.ru>

SpellAddon & EI ATD developers:

- VeryGoodGirl
- PlayHard_GoPro

<https://evilislandsaddon.forumotion.com/>
<https://vk.com/evil_islands_addon>

## Community tools and sources our file-format work builds on

The formats of the game (`.res`, `.mob`, `.mpr`, `.fig` / `.mod` / `.lnk`, `.anm`, `.bon`, `.mmp`, the databases...) were documented by the Evil Islands community long before this project. Our readers, writers and editors (um-multitool, um.dll) were written with their sources and tools at hand, and compared against them byte by byte. Our thanks, and credit, go to:

- __konstvest__: [ei_maper](https://github.com/konstvest/ei_maper) (map and `.mob` editor, GPL-3.0) and [ei_figer](https://github.com/konstvest/ei_figer) (Blender import / export of models, animations and morphs, GPL-3.0). Their source is our main reference for `.mob`, `.mpr`, figures, animations and for how the editor behaves; the Map Editor follows ei_maper's rules for object placement, logic, patrol points and traps.
- __Demoth__: eipacker (the `.res` archive format and its hash table), MobExplorer and `eisc_con` (`.mob` files and the mission script checker), and the help given to many other community projects.
- __VeryGoodGirl__: EN_VGG_EDITOR, whose full description of every `.mob` field and every script command we used as a reference.
- __aspadm__: EI-HD-tiles (tile atlas generator and the documentation of how the terrain tiles are built from base materials and blend masks). Free to use under its own terms: no commercial use, credit the author.
- __The authors of DBEditor__ (the gameplay database format; its changelog thanks Robin and Sagrer), __ZoneView__ (zone viewer, source of the minimap look) and __MMPStudio__ (`.mmp` textures): the formats these tools read and write are documented in `docs/file-formats`.
- __Nival Interactive__: the game itself.
- __WinterSnowfall__ ([D7VK](https://github.com/WinterSnowfall/d7vk)) and the DXVK authors (Philip Rebohle, Joshua Ashton, Robin Kertels): studied while researching a modern graphics layer for the game's DirectDraw / Direct3D 7 renderer.

## Libraries

um-multitool bundles [Dear ImGui](https://github.com/ocornut/imgui) (Omar Cornut, MIT), [GLFW](https://www.glfw.org/) (Marcus Geelnard, Camilla Löwy, zlib/libpng), [miniaudio](https://miniaud.io) (David Reid, public domain / MIT-0) and [stb_image](https://github.com/nothings/stb) (Sean Barrett, public domain).

## Reverse engineering of the game engine

The game's executable was analysed with [Cutter](https://cutter.re) / [rizin](https://rizin.re) and the Ghidra decompiler (through rz-ghidra), alongside the community documentation above. Thanks to their authors.

## Special thanks


- Atom (Atm)
- SunGuru
- As bestos
- HD Lands Team

Спасибо
