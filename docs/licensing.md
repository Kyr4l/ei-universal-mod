# Licensing and ownership

Back to the [README](README.md).

## License

__um-multitool__ (`tools/um-multitool`), __um-bot__ (`tools/um-bot`), __EIStarter-UM__ (`tools/EIStarter-UM`, with its `um-window.asi`) and __um.dll__ with __um-engine.dll__ (`resources/universal-mod/um-dll`, `resources/universal-mod/um-engine`) are free software: you can redistribute them and/or modify them under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version (`GPL-3.0-or-later`; the full text is in [LICENSE](LICENSE)). They are distributed in the hope that they will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. Their source files say so in their first lines (`SPDX-License-Identifier: GPL-3.0-or-later`).

Copyright (C) 2026 Kyr4l.

Not covered by that license, and kept under their owners' terms:

- the mod's content: `Universal-Mod/`, the rest of `resources/`, `extra-assets/` (see [Assets ownership](#assets-ownership)); the files that come from *Evil Islands* belong to its rights holders (Nival Interactive);
- the files EIStarter-UM's package takes from EIStarter OBT-1 (skins, texts, loader, `addon.dll`, `.asi` plugins), which belong to their authors, and `Engine/game.exe`, which belongs to the game's rights holders;
- the other tools in `tools/` (SpellAddon has its own license, `third-party-tools/` belong to their authors);
- the tools' logos, icons and splash screen (`tools/um-multitool/assets`, `tools/um-bot/assets` and the `icon_data.hpp` made from them), which are renders of the game's own models and textures;
- um-multitool's two alert sounds (`tools/um-multitool/sfx`);
- the EI ATD command descriptions in `tools/um-multitool/mapedit/atd_script_functions.hpp`, which come from the EI ATD modder's `scripts.htm`;
- the libraries bundled with um-multitool and um-bot, which keep their own licenses, all compatible with the GPL (see [Libraries](../CREDITS.md#libraries)): Dear ImGui (MIT, `tools/um-multitool/vendor/imgui/LICENSE.txt`), GLFW (zlib/libpng, built into the Windows executables), miniaudio (public domain / MIT-0) and stb_image (public domain / MIT).

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
