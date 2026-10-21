# Manual installation

The recommended way is [EIStarter-UM](EIStarter-UM.md), which sets everything up by itself (see the [README](../README.md)). This page is for the cases where you want to use the original EIStarter, or to understand what the launcher automates.

## What you need

- Evil Islands: Curse of the Lost Soul, installed (GOG or the original CD), version 1.06.
- The Evil Islands Addon 2.0 starter (EIStarter OBT-1): the Releases page has it as `eistarter-obt-1.zip` (also in `tools/` with pre-configured .ini settings), or download it from [allods.gipat.ru](https://allods.gipat.ru/files/ei/soft/eistarter_obt_1.7z).
- [SpellAddon](https://evilislandsaddon.forumotion.com/t2-spelladdon): included in the Releases archive of the starter (`Engine/SpellAddonX.asi`).
- The mod: the latest [Release](https://github.com/Kyr4l/ei-universal-mod/releases/latest), or a [Pre-Release](https://github.com/Kyr4l/ei-universal-mod/releases) (built from the `beta` branch after each commit). For development, link the repository's `Universal-Mod` directory into the starter's `Mods` directory.

The old Starter 1.046 is deprecated and not supported.

## Steps

1. Unpack the starter (skip if it is already installed).
2. On first use, run `Init Starter Registry.bat` as administrator: it copies the game's registry keys to `HKCU\Software\Gipat.Ru\EI_Starter`. Then add a `path` string value to that key holding the starter's folder (the loader reads it to find `addon.dll`; without it the game starts without the mod).
3. Copy (or make a junction or symlink) the `Universal-Mod` directory into `<EIStarter path>/Mods`.
4. Run `EIStarter.exe`, select "Universal Mod", click Play.

EIStarter 2.0 is a .NET program: under Wine it needs `dotnet8` and `vcrun2022` (Lutris: select the game, open the Wine menu, then Winetricks, and install them).

## Linux (Wine)

Wine needs a DLL override for the starter's loader, otherwise the injection fails and the game starts without the mod. Installing the game with Lutris is strongly recommended, as it provides a dedicated environment:

1. In Lutris, right-click the game and open Properties.
2. In Game options, select the starter executable (`EIStarter.exe`).
3. In Runner options, add the DLL override `dinput` = `n,b`.
4. Save and play.

## Troubleshooting

If the game crashes or freezes after clicking Multiplayer, edit `ei_plugin.ini` (in the starter's `Engine` folder) and add the line `NewMaster=0`, then retry.
