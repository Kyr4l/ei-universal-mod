# EI-Universal-Mod

[Discord Server](https://discord.gg/nGm2mwakQx)

A complete, Multiplayer-Friendly mod for Evil Islands : Curse of the Lost Soul. Inspired by EI-Mod, HG-Mod, and others.

This mod features a complete rebalance of the Multiplayer mode. Adds new materials, characters, etc...

The mod includes (but is not limited to) :

- Severely reduced stamina consumption while sprinting
- No XP loss on death, but 10x higher money loss
- No XP splitting when playing with other people
- Increased XP gain from monsters
- Reduced perk cost multiplier
- Reworked healing, potions are more common but cheaper
- More Quests/Maps
- New materials/runes/potions/monsters/blueprints
- Reworked melee combat
- Better looting

And more!

This mod also includes HD Lands

*Requires EIStarter, which can be downloaded from the following links, or from the `tools/` directory*

[Starter 2.0](https://allods.gipat.ru/files/ei/soft/eistarter_obt_1.7z)

[Starter 1.046](https://allods.gipat.ru/files/ei/soft/setup%20addon%20v.1.046.0.exe) (DEPRECATED / SOME FEATURES AREN'T SUPPORTED)

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

WINE requires a DLL override to run EIStarter properly, otherwise the injection will fail and the game will launch in vanilla.

It is __strongly__ recommended to install the game with Lutris, as it provides a dedicated environment to run the game.

Here is how to set it up with Lutris:

- Open Lutris and right click on your game then open the __Properties__ menu
- Switch to the __Game options__ tab and select the starter executable (EIStarter.exe)
- Go to the __Runner options__ and add the following __DLL override__: `dinput` as the __Key__ and `n,b` as __Value__
- Save and play!

If you use EIStarter 2.0 instead of the old version, you need to install `dotnet8` and `vcrun2022`.
Simply select your game in Lutris and click the WINE drop-down menu, then open Winetricks and install `dotnet8` & `vcrun2022` from there.

## Hosting & Joining Servers

*The master servers of this game have been shut down long ago, therefore server discovery is no longer possible unless you use a few community servers.*

Unless you use a master server, the multiplayer menu will be empty unless there is a host on your local network. Connecting to a game has to be done directly, which means by connecting to an IP manually, this can be done in the Multiplayer menu.
Simply enter the IP of the other player hosting the game, if everything goes right then you should see a ping value, if this value is 999 that means either the IP is invalid, or the host didn't setup their NAT/Firewall to allow incoming connections.

In order to host servers for this game outside of a local network (LAN), you need to open the port `8888`/`UDP`, this must be done on your router configuration panel.
Then your compouter should display a prompt asking to allow the game to access the firewall, this must be granted.

## Assets ownership

This mod is open-source, however open source doesn't mean «free to steal».

Some community assets used in this mod have unclear ownership. We use them only within the Universal Mod project and do not claim any rights over them. If any rights holder requests removal, we will comply immediately.
If we accidentaly used assets that we do not have permission to use, then please feel free to submit an issue and point which assets we used without permission, mistakes can happen.
For every asset that we created, we require from modders using our assets to link Universal Mod in the credits, that includes the mod name, along with the repository link.

## Disclaimer

- We __do not__ own any of the tools used *except* the scripts in __ei-multitool__, the binaries used are community tools.
- Some files included in `extra-assets/reference-assets` come from the vanilla game, some others come from other mods.
- This mod was developed with contributions from both Western and Russian modders. It is strictly a passion project, and we do not endorse or engage in any political discussions or conflicts. Our goal is solely to enhance and preserve an old game we love.

### License Notice

This repository contains a mix of original code, community-created assets, and files from *Evil Islands* mostly for reference purposes.

#### Code 

All scripts and configuration files in the `assets/universal-mod/` directory along with their packed versions in `Universal-Mod/` are licensed under **GPL-3.0**, allowing modification and redistribution under the terms of that license. 
The `makemod.sh` script in `tools/ei-multitool/` directory is also licensed under that same license.

#### Assets & Modding Tools

The **GPL-3.0 license does not apply** to the following:
- **HD Lands** included in `resources/hd-lands-1.5/`.
- **Modding tools** included in `tools/`, which remain the property of their respective authors.
- **Extra-assets** stored in `extra-assets/`, which contain a mix of files from the vanilla game and community-created resources. These assets are **mostly provided for reference and internal modification only**.  

These assets **cannot be redistributed separately or used outside of this project**.  
If any rights holder requests removal, we will comply immediately.  

## Credits

Made with the precious help of the Russian modding community.

Special thanks to :

- Atom (Atm)
- SunGuru
- HD Lands Team

Спасибо
