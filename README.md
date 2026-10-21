# EI-Universal-Mod

A multiplayer-friendly rebalance of **Evil Islands: Curse of the Lost Soul**, inspired by EI-Mod, HG-Mod and others.

[Download](https://github.com/Kyr4l/ei-universal-mod/releases) | [Discord](https://discord.gg/nGm2mwakQx) | [Documentation](docs) | [Report an issue](https://github.com/Kyr4l/ei-universal-mod/issues)

---

## Contents

- [What it changes](#what-it-changes)
- [Install](#install)
- [Release or beta](#release-or-beta)
- [Multiplayer](#multiplayer)
- [Tools](#tools)
- [Notes](#notes)
- [License and credits](#license-and-credits)

## What it changes

| Area | Highlights |
| :--- | :--- |
| Gameplay | Much lower stamina use while sprinting. No XP loss on death (money loss x10). XP is not split among players, and monsters give more of it. Cheaper perks. Simplified, rounded character stats. Realistic weight. |
| Combat and healing | Reworked melee. Reworked healing: potions are common and cheap. Most enemies drop items, with reworked loot tables. |
| Content and economy | More quests and maps. New materials, runes, monsters, blueprints and NPCs. Rebalanced and expanded merchants. Skill and perk rebalance. |
| Assets and text | HD Lands integration, updated assets, and localization fixes across dialogs and item names. |

And more. Bugs and inconsistencies of the original game are patched or worked around.

## Install

You need Evil Islands installed (the GOG version or the original CD, both work). The launcher does everything else.

1. Download `EIStarter-UM-<version>.zip` from [Releases](https://github.com/Kyr4l/ei-universal-mod/releases) and unpack it anywhere you can write to.
2. Run `EIStarter-UM.exe`. On Linux, run it with Wine in the game's prefix: `wine EIStarter-UM.exe`. The first time it asks for the game's folder if it cannot find it.
3. Click the **(i)** button next to the mod list, then **Install** on the line you want (see below). The mod can also be unpacked by hand into `mods`.
4. Pick the mod in the list and click **Play**.

EIStarter-UM replaces EIStarter: no registry file to import, no DLL override, no .NET. It also checks for new mod builds in the background and updates them in place. How it works: [docs/EIStarter-UM.md](docs/EIStarter-UM.md).

Prefer the original EIStarter? See the [manual installation guide](docs/manual-installation.md).

## Release or beta

Both can be installed side by side; each has its own folder and its own saves, so nothing is mixed. To carry saves over, copy them by hand.

| Line | Folder | Built from | For |
| :--- | :--- | :--- | :--- |
| Release (master) | `mods/Universal-Mod` | `master`, when a version is released | A stable game |
| Pre-Release (beta) | `mods/Universal-Mod-Beta` | `beta`, rebuilt after every commit | The latest fixes, balance changes and content (currently recommended) |

For development, link the repository's `Universal-Mod` directory into the launcher's `mods` folder.

## Multiplayer

The game's master servers are long gone, so the multiplayer menu is empty unless a host is on your local network. Join a game directly: in the Multiplayer menu, enter the host's IP address. A ping value means it works; 999 means the IP is wrong or the host's NAT or firewall blocks incoming connections.

To host outside a local network, open port `8888` / `UDP` on your router, and allow the game through the firewall when it asks.

## Tools

| Tool | What it is |
| :--- | :--- |
| [EIStarter-UM](docs/EIStarter-UM.md) (`tools/EIStarter-UM`) | The mod launcher and updater described above. |
| [um-multitool](tools/um-multitool/README.md) (`tools/um-multitool`) | The toolkit this mod is built with: file conversion and archives, a 3D viewer, a map editor and a live connection to the game. GUI and command line, Windows and Linux. |
| [um-bot](tools/um-bot/README.md) (`tools/um-bot`) | A companion player for multiplayer: joins your hosted game as another player, follows you, fights, heals itself and loots. |
| `um.dll`, `um-engine.dll` (`resources/universal-mod`) | The mod's runtime plugins: fixes, performance work, borderless window, the DLL server that um-multitool talks to. |

Everything else about the project lives in [docs](docs): file formats, the game's memory and protocol, scripting commands, shadows, and more.

## Notes

- **Assets.** Some community assets have unclear ownership; we use them only inside this project and will remove anything on request. Details in [docs/licensing.md](docs/licensing.md).
- **Compatibility.** The mod was made with the HD Lands texture pack. Large visual overhauls that change base game files (for example Evil Islands: Rebirth) are not supported and may break.
- **Use of AI.** Much of the tooling (um-multitool, um.dll, EIStarter-UM) was written with AI help; the code is reviewed, tested and maintained by humans. The file formats the tools handle were reverse-engineered with AI from the game's executable and files, then compared byte for byte with the game's own files and with older community tools.
- **Politics.** Western and Russian modders contributed. This is a passion project; we take no part in political discussions or conflicts.

We value feedback: open an issue or join the Discord.

## License and credits

um-multitool, um-bot, um.dll, um-engine.dll and EIStarter-UM are free software under the GPL-3.0-or-later ([LICENSE](LICENSE)), Copyright (C) 2026 Kyr4l. The mod's content, the game's files, and third-party tools and libraries keep their owners' terms: see [docs/licensing.md](docs/licensing.md).

Credits and thanks to the community tools, sources and people this work builds on: [CREDITS.md](CREDITS.md).
