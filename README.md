# EI-Universal-Mod

[Discord Server](https://discord.gg/nGm2mwakQx)

A Multiplayer-Friendly mod for Evil Islands : Curse of the Lost Soul. Inspired by EI-Mod, HG-Mod, and others.

This mod features a complete rebalance of the Multiplayer mode. Adds new materials, characters, etc...
Several bugs and inconsistencies have also been patched or worked around.

The mod includes (but is not limited to):

- Gameplay
  - Severely reduced stamina consumption while sprinting
  - No XP loss on death; monetary loss increased ×10
  - XP scaling: XP is not split among players
  - Increased XP from monsters
  - Reduced perk cost multiplier
  - Simplified, rounded character stats
  - Realistic weight management

- Combat & Healing
  - Reworked melee combat
  - Reworked healing: potions are more common and cheaper
  - Most enemies drop items on death
  - Reworked loot tables

- Content & Economy
  - More quests and maps
  - New materials, runes, monsters, and blueprints
  - New NPCs; merchant inventories rebalanced and expanded
  - Skill and perk rebalance

- Assets & Localization
  - HD Lands integration and updated assets
  - Localization fixes and minor text corrections across dialogs and item names

And more!

*Requires EIStarter & SpellAddon, available in the [Releases](https://github.com/Kyr4l/ei-universal-mod/releases) section*
*also provided in the `tools/` directory (with pre-configured .ini settings)*
*OR these can also be manually downloaded from these links below*

[Starter 2.0](https://allods.gipat.ru/files/ei/soft/eistarter_obt_1.7z)
[SpellAddon](https://evilislandsaddon.forumotion.com/t2-spelladdon)

~~[Starter 1.046](https://allods.gipat.ru/files/ei/soft/setup%20addon%20v.1.046.0.exe)~~ (Deprecated)

We value feedback! If you have any comments to make please open an Issue on this repo.

## Installation

*You need to pick which branch of the mod you want to play with, the `beta` branch is currently recommended as it contains the latest fixes, balance changes, and has overall more content.*

### Download the mod

- [Recommended – Stable] - Download the [Latest](https://github.com/Kyr4l/ei-universal-mod/releases/latest) archive of this mod
- [Testing – Beta] - Download the [Pre-Release](https://github.com/Kyr4l/ei-universal-mod/releases) archive of this mod (automatically built from the `beta` branch and updated after each new commit)
- [Advanced – Development] Download the entire repo and link the mod directory to EIStarter's `Mod` directory

### Install the mod

- Download & extract the Evil Islands Addon 2.0, located in `tools/eistarter_obt_1.7z` (skip if you already have it installed)
- Copy (or make a junction/symlink) the `Universal-Mod` directory into the  `<EIStarter Path>/Mods` directory
- Run EIStarter and select "Universal-Mod", then click "Play"

### Linux only : Extra steps to run the game with WINE

WINE requires a few DLL overrides to run EIStarter properly, otherwise the injection will fail and the game will launch in vanilla.

It is __strongly__ recommended to install the game with Lutris, as it provides a dedicated environment to run the game.

Here is how to set it up with Lutris:

- Open Lutris and right click on your game then open the __Properties__ menu
- Switch to the __Game options__ tab and select the starter executable (EIStarter.exe)
- Go to the __Runner options__ and add the following __DLL override__: `dinput` as the __Key__ and `n,b` as __Value__.
- Save and play!

If you use EIStarter 2.0 instead of the old version, you need to install `dotnet8` and `vcrun2022`.
Simply select your game in Lutris and click the WINE drop-down menu, then open Winetricks and install `dotnet8` & `vcrun2022` from there.

## Hosting & Joining Servers

*The master servers of this game have been shut down long ago, therefore server discovery is no longer possible unless you use a few community servers.*

Unless you use a master server, the multiplayer menu will be empty unless there is a host on your local network. Connecting to a game has to be done directly, which means by connecting to an IP manually, this can be done in the Multiplayer menu.
Simply enter the IP of the other player hosting the game, if everything goes right then you should see a ping value, if this value is 999 that means either the IP is invalid, or the host didn't setup their NAT/Firewall to allow incoming connections.

In order to host servers for this game outside of a local network (LAN), you need to open the port `8888`/`UDP`, this must be done on your router configuration panel.
Then your compouter should display a prompt asking to allow the game to access the firewall, this must be granted.

### Troubleshooting

If the game crashes or freezes after clicking on __Multiplayer__, edit the `ei_plugin.ini` config file and add a new line containing `NewMaster=0`. Then save and retry launching multiplayer.

## Assets ownership

Some community assets used in this mod have unclear ownership. We use them only within the Universal Mod project and do not claim any rights over them. If any rights holder requests removal, we will comply immediately.
If we accidentaly used assets that we do not have permission to use, then please feel free to submit an issue and point which assets we used without permission, mistakes can happen.
For every asset that we created, we require from modders using our assets to link Universal Mod in the credits, that includes the mod name, along with the repository link.

## Disclaimer

This mod was developed using the HD Lands texture pack. Large visual-overhaul mods (for example, "Evil Islands: Rebirth") that modify base game files are __not__ officially supported because they can cause visual inconsistencies and incompatibilities when running additional mods. Universal Mod *may* work with such variants, but full compatibility is not guaranteed.

- We __do not__ own any of the tools used *except* __um-multitool__, __um-bot__, __um.dll__ / __um-engine.dll__ (see [License](#license)) and the scripts in __ei-multitool__; the other binaries used are community tools.
- Some files included in `extra-assets/reference-assets` come from the vanilla game, some others come from other mods.
- This mod was developed with contributions from both Western and Russian modders. It is strictly a passion project, and we do not endorse or engage in any political discussions or conflicts. Our goal is solely to enhance and preserve an old game we love.

This repository contains a mix of original code, community-created assets, and files from *Evil Islands* mostly for reference purposes.

## Universal Mod Multitool

__um-multitool__ (`tools/um-multitool`) is the toolkit we use to build this mod, in a single program for Windows and Linux. It runs as a GUI or from the command line.

- __File Processing__: converts textures (`.dds` <-> `.mmp`) and configs (`.ini` <-> `.reg`), packs and unpacks `.res` / `.mq` archives, dumps `.mob` maps, edits the gameplay database (with checks), the language packs and multiplayer characters (`.mp`).
- __3D Viewer__: browses every item and unit of the database with its model and textures. Units are dressed like in the game, custom skins can be tried on them, and GIFs and UV maps can be exported.
- __Map Editor__: edits `.mob` maps and `.mpr` terrains, quests and their scripts, checks them like the game does, and rebuilds the navmesh like the game does (no more navmesh regeneration at load time).
- __UM DLL Connector__: talks to the running game through `um.dll`: stats, quests and scripts live, and a game console.

## Use of AI

A large part of the tools in this repository (um-multitool, um.dll) was written with the help of AI. The code remains reviewed, tested and maintained by humans.

The file formats the tools read and write (`.mp` characters, navmeshes, terrains, maps, databases, textures...) were reverse-engineered with AI, from the game's executable and its files. The results were then compared at the binary level with the game's own files and with the older community tools, to make sure the files we produce are exactly what the game engine expects.

## License

__um-multitool__ (`tools/um-multitool`), __um-bot__ (`tools/um-bot`) and __um.dll__ with __um-engine.dll__ (`resources/universal-mod/um-dll`, `resources/universal-mod/um-engine`) are free software: you can redistribute them and/or modify them under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version (`GPL-3.0-or-later`; the full text is in [LICENSE](LICENSE)). They are distributed in the hope that they will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. Their source files say so in their first lines (`SPDX-License-Identifier: GPL-3.0-or-later`).

Copyright (C) 2026 Kyr4l.

Not covered by that license, and kept under their owners' terms:

- the mod's content: `Universal-Mod/`, the rest of `resources/`, `extra-assets/` (see [Assets ownership](#assets-ownership)); the files that come from *Evil Islands* belong to its rights holders (Nival Interactive);
- the other tools in `tools/` (SpellAddon has its own license, `third-party-tools/` belong to their authors);
- the tools' logos, icons and splash screen (`tools/um-multitool/assets`, `tools/um-bot/assets` and the `icon_data.hpp` made from them), which are renders of the game's own models and textures;
- um-multitool's two alert sounds (`tools/um-multitool/sfx`);
- the EI ATD command descriptions in `tools/um-multitool/mapedit/atd_script_functions.hpp`, which come from the EI ATD modder's `scripts.htm`;
- the libraries bundled with um-multitool and um-bot, which keep their own licenses, all compatible with the GPL (see [Libraries](#libraries)): Dear ImGui (MIT, `tools/um-multitool/vendor/imgui/LICENSE.txt`), GLFW (zlib/libpng, built into the Windows executables), miniaudio (public domain / MIT-0) and stb_image (public domain / MIT).

## Credits

SpellAddon & EI ATD Developers:

- VeryGoodGirl
- PlayHard_GoPro

<https://evilislandsaddon.forumotion.com/>
<https://vk.com/evil_islands_addon>

### Community tools and sources our file-format work builds on

The formats of the game (`.res`, `.mob`, `.mpr`, `.fig` / `.mod` / `.lnk`, `.anm`, `.bon`, `.mmp`, the databases...) were documented by the Evil Islands community long before this project. Our readers, writers and editors (um-multitool, um.dll) were written with their sources and tools at hand, and compared against them byte by byte. Our thanks, and credit, go to:

- __konstvest__: [ei_maper](https://github.com/konstvest/ei_maper) (map and `.mob` editor, GPL-3.0) and [ei_figer](https://github.com/konstvest/ei_figer) (Blender import / export of models, animations and morphs, GPL-3.0). Their source is our main reference for `.mob`, `.mpr`, figures, animations and for how the editor behaves; the Map Editor follows ei_maper's rules for object placement, logic, patrol points and traps.
- __Demoth__: eipacker (the `.res` archive format and its hash table), MobExplorer and `eisc_con` (`.mob` files and the mission script checker), and the help given to many other community projects.
- __VeryGoodGirl__: EN_VGG_EDITOR, whose full description of every `.mob` field and every script command we used as a reference.
- __aspadm__: EI-HD-tiles (tile atlas generator and the documentation of how the terrain tiles are built from base materials and blend masks). Free to use under its own terms: no commercial use, credit the author.
- __The authors of DBEditor__ (the gameplay database format; its changelog thanks Robin and Sagrer), __ZoneView__ (zone viewer, source of the minimap look) and __MMPStudio__ (`.mmp` textures): the formats these tools read and write are documented in `docs/file-formats`.
- __Nival Interactive__: the game itself.
- __WinterSnowfall__ ([D7VK](https://github.com/WinterSnowfall/d7vk)) and the DXVK authors (Philip Rebohle, Joshua Ashton, Robin Kertels): studied while researching a modern graphics layer for the game's DirectDraw / Direct3D 7 renderer.

### Libraries

um-multitool bundles [Dear ImGui](https://github.com/ocornut/imgui) (Omar Cornut, MIT), [GLFW](https://www.glfw.org/) (Marcus Geelnard, Camilla Löwy, zlib/libpng), [miniaudio](https://miniaud.io) (David Reid, public domain / MIT-0) and [stb_image](https://github.com/nothings/stb) (Sean Barrett, public domain).

### Reverse engineering of the game engine

The game's executable was analysed with [Cutter](https://cutter.re) / [rizin](https://rizin.re) and the Ghidra decompiler (through rz-ghidra), alongside the community documentation above. Thanks to their authors.

Special thanks to :

- Atom (Atm)
- SunGuru
- As bestos
- HD Lands Team

Спасибо
