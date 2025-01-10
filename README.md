# EI-Universal-Mod

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

*This mod includes HD Lands*

*Requires EIStarter, which can be downloaded from the following links, or from the `tools/` directory*

[Starter 2.0](https://allods.gipat.ru/files/ei/soft/eistarter_obt_1.7z) 

[Starter 1.046](https://allods.gipat.ru/files/ei/soft/setup%20addon%20v.1.046.0.exe) (Old, use Starter 2.0 if possible)

We value feedback! If you have any comments to make please open an Issue on this repo.


# Installation

*You need to pick which branch of the mod you want to play with, the `beta` branch is currently recommended as it contains the latest fixes, balance changes, and has overall more content.*

- Download the latest archive of this mod in the [Releases](https://github.com/Kyr4l/ei-universal-mod/releases) section __OR__ Download the entire repo for `beta` testing (using `git` is strongly recommended)
- Install the Evil Islands Addon, located in `tools/eistarter-1046-en.exe` (skip if you already have it installed)
- Copy (or make a junction/symlink) the `Universal-Mod` directory into the  `<EIStarter Path>/Mods` directory
- Run EIStarter and select "Universal-Mod", then click "Play""


### Linux only : Extra steps to run the game with WINE :

WINE requires a DLL override to run EIStarter properly, otherwise the injection will fail and the game will launch in vanilla.

It is **strongly** recommended to install the game with Lutris, as it provides a dedicated environment to run the game.

Here is how to set it up with Lutris:
- Open Lutris and right click on your game then open the **Properties** menu
- Switch to the **Game options** tab and select the starter executable (EIStarter.exe)
- Go to the **Runner options** and add the following **DLL override**: `dinput` as the **Key** and `n,b` as **Value**
- Save and play!

If you use EIStarter 2.0 instead of the old version, you need to install `dotnet8`. 
Simply select your game in Lutris and click the WINE drop-down menu, then open Winetricks and install `dotnet8` from there.


# Hosting & Joining Servers

*The master servers of this game have been shut down long ago, therefore server discovery is no longer possible unless you use a few community servers.*

Unless you use a master server, the multiplayer menu will be empty unless there is a host on your local network. Connecting to a game has to be done directly, which means by connecting to an IP manually, this can be done in the Multiplayer menu.
Simply enter the IP of the other player hosting the game, if everything goes right then you should see a ping value, if this value is 999 that means either the IP is invalid, or the host didn't setup their NAT/Firewall to allow incoming connections.

In order to host servers for this game outside of a local network (LAN), you need to open the port 8888/UDP, this must be done on your router configuration panel. 
Then your compouter should display a prompt asking to allow the game to access the firewall, this must be granted.


# Disclaimer

- We **do not** own any of the tools used *except* the scripts in **ei-multitool**, the binaries used are community tools.
- Some files included in `./resources/reference-assets` come from the vanilla game, some others come from other mods.


# Credits

Made with the precious help of the Russian modding community.

Special thanks to :
- Atom (Atm)
- SunGuru
- HD Lands Team

Спасибо

