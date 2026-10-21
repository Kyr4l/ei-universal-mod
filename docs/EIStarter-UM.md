# EIStarter-UM: the mod launcher

`tools/EIStarter-UM` replaces EIStarter OBT-1 (a closed-source .NET program, slow to open under Wine, and a registry setup that many users get wrong).
C++17, Dear ImGui + GLFW like um-multitool; one 32-bit `EIStarter-UM.exe` (GLFW 3.3: runs on Windows XP and newer, and under Wine in the prefix of
the game). It starts `Engine\game.exe` with a mod and shows "Starting..." until the game window is up.

## Credits

The skins, texts, loader (`Engine\dinput.dll`), `addon.dll` and `mp3dec.asi` in the package are EIStarter OBT-1's, by the EIStarter / Gipat.Ru authors; EIStarter-UM only reads their formats, its code is new.

## The game executable

The package's `Engine\\game.exe` is the game's own exe with the large address aware flag set (one bit of the PE header, byte-identical to the community's patched exe). With the stock exe the game ran out of address space under D7VK ("map failed", then "abnormal program termination" in the Multiplayer lobby). The launcher does not patch anything itself.

## Tested

In clean Wine 11.19 prefixes (hidden display, silent audio) with the package, an empty registry and Universal-Mod: the author's own install, the GOG installer (`setup_evil_islands_2.0.0.5.exe`, installed silently to `C:\GOG Games\Evil Islands`) and the 2001 CD edition (the ISO mounted as `D:`, label `EICD1`, installed by its InstallShield to `C:\Program Files (x86)\Evil Islands`): each reaches the main menu with "Universal Mod ver. 0.8.6" through the launcher. GOG writes a relative CD-ROM Path (`.\`) that makes the game ask for a disc; the launcher replaces such a path by the game folder, and keeps an existing one (the CD edition's `D:\`). Every Options control was checked: widget -> `EIStarter-UM.cfg` (OK keeps, Cancel discards), `.cfg` -> registry (the debug log lists every value written), and what the game does with it where it is visible (window size and borderless, movies off, addon.dll's draw-radius / MQ / second speed / console patches, the log level, the mod and its skin, the engine exe name and the failure message). Not exercised: real fullscreen (the hidden X display has no mode switching: the game dies on its exclusive-fullscreen mode there), the Website button (it would open a browser).

## For users

1. Unpack the launcher package (`tools/EIStarter-UM/package.sh` builds `dist/EIStarter-UM-<version>.zip`) anywhere you can write to.
2. Unpack a mod (Universal-Mod ...) into its `mods\` folder.
3. Run `EIStarter-UM.exe` (Linux: `wine EIStarter-UM.exe` in the game's prefix; there is no native Linux build, the game needs Wine anyway), choose the mod, **Play**.

The first start asks for the folder of the original game (the one with `game.exe` and `res`) when it cannot find it (`C:\Evil Islands`, the
installer's registry key, GOG's folders). There is no `Init Starter Registry.bat`, no `path` value to add by hand, no DLL override to set, and no
.NET / vcrun2022 to install (EIStarter 2 needed all of them).

The window is EIStarter's: the same skins (`design\<lang>\*.png|bmp`, `font.otf`, `click.wav`), languages (`lang\<code>\lang.ini`, UTF-16) and mod
folders (`mods\<mod>\config.reg`, or `mod.config` with the Utils option "Read mods from mod.config"), so what was made for EIStarter works.
Under the mod chooser a dot tells whether the selected mod uses um.dll: green "um.dll detected" (its `config.reg` [DEV] Plugin names it and the file is in the mod's folder), amber when only one of the two is true (addon.dll would not load it), grey "no um.dll". The (i) window's text is read-only but selectable (Ctrl+C), with Copy site / Open site buttons.
Buttons: Play, Options, Readme (`lang\<lang>\readme.txt`), Website, Uninstall (`uninstall.exe` when there is one), Exit, the mod chooser, the (i)
button (mod information, its readme / changelog, and the mod's own script button), Lang (cycles the languages). Enter = Play, Esc = Exit.

**Mod updates** (Universal Mod; another repository can be set with `*=owner/repo` under `[Update Repositories]` in the cfg): the release and the beta (pre-release) are kept as two mods, side by side, so their saves never mix: the newest release goes into `mods/Universal-Mod` and the newest pre-release into `mods/Universal-Mod-Beta`; both then show in the mod chooser as "Universal Mod (release)" and "(beta)". Saves can be copied from one to the other by hand. The launcher asks GitHub in the background at start (Utils: turn it off) and shows "update available" in the bottom right corner for the selected mod. The (i) window has a row per line (Install / Update / Reinstall buttons, a progress bar with the megabytes while it downloads, then the unpacking): it downloads the release's `Universal-Mod*.zip`, checks every file's CRC, unpacks it beside the mod, then swaps the folders: everything is replaced except `mp` and `saves` (the saves); `um.cfg` and `um-engine.cfg` are kept when the new build has none (the release ships none) and the old ones are copied to `mods/.update/backup` when replaced. If anything fails the old folder is put back. A folder the updater did not install is only offered a newer version (its own version compared with the release tag). Needs HTTPS (TLS 1.2): not on Windows XP. `EISTARTER_UM_API` (a base URL) replaces `https://api.github.com` for tests. Play is disabled during an update; do not update while the game runs.

**Options** (the tabs of EIStarter's dialog): Graphics (adapter, resolution, mip-mapping, antialiasing, dithering, FPS-independent cursor (marked not recommended: it flickers with D7VK; off on a new install),
filtering, textures, shadows, terrain LOD, lighting / shadowing frequency, movies, audio), Addon (landscape / objects draw radius, pause when
minimized, addon.log levels), Multiplayer (console, "do not load .MQ from the host", second speed), Camera (every camera value), Paths (game folder,
resources, CD, the engine exe name), Utils (mod skins, mod.config, close when the game is up, debug log. New: **Window mode** = Fullscreen /
Windowed / **Borderless window**. In EIStarter's Windowed mode the game is a frameless window that minimizes and pauses when it loses the focus
(a multiplayer game then desyncs); Borderless keeps it running and keeps the mouse inside. EIStarter's presets (disabled there) and its "Russian
charset fix for WinXP" checkbox (it did nothing) are not carried over.

Files beside the exe: `EIStarter-UM.cfg` (all settings), `EIStarter-UM.log` (what the launcher did, the replacement of `EIStarter With Debug.bat`),
`addon.log` (written by addon.dll). `starter.config` of EIStarter is imported on the first start, and so are its registry settings.

## How the game gets its mod (verified by decompiling EIStarter and by tests in a fresh Wine prefix)

Play = start `Engine\<ExeName>` (`game.exe`) with `Engine\` as the working directory; nothing is injected by the starter. The rest is the loader chain:

- `Engine\dinput.dll` ("Addon Loader") is an override of DirectInput. It reads `HKCU\Software\Gipat.Ru\EI_Starter\path` (the starter's folder) and
  loads `addon.dll` from there. Without `path` addon.dll is not loaded and the game runs vanilla, silently.
- `addon.dll` (0.12, closed source) reads `EI_Starter\settings\AddonPath` (the mod's folder; none = no mod), hooks
  `WinMain`, applies its patches, loads the mod's `config.reg` ([RES] resources, [DEV] Plugin = um.dll, [MPZone], [CONFIG]), the plugins and the
  `Engine\*.asi` files, and points game.exe's registry root from `Software\Nival Interactive\EvilIslands` to `Software\Gipat.Ru\EI_Starter\EvilIslands`.
  It only READS the registry. Its patches (MP console, second speed, MQ client, draw radii, pause on minimize) are switched by values of `EI_Starter\settings`.
- Without the `EvilIslands` clone game.exe stops right after choosing the renderer.
- game.exe does not write the registry while it runs.

Registry written by the launcher at Play from `EIStarter-UM.cfg` (HKCU, `regsync.hpp`):

| Key | Values |
| --- | --- |
| `Software\Gipat.Ru\EI_Starter` | `path` = the launcher's folder |
| `...\EI_Starter\settings` | `AddonPath`, `LandscapeDrawRadius`, `ObjectsDrawRadius` (only when set), `PauseOnMinimize`, `MP Speed-control`, `MQ client`, `MP console`, `LogLevel`, `ConsoleLogLevel`, `Xoptions Active=1`, `ModesFiltration=0` |
| `...\EI_Starter\EvilIslands\*` | the game's own settings: made from Nival's key (`Software\Nival Interactive\EvilIslands`) when missing, else from the built-in defaults of a fresh installation (`game_defaults.hpp`), then every value of the `[Game: ...]` sections of `EIStarter-UM.cfg` |
| `Software\Wine\DllOverrides` | only under Wine: `dinput` (and `ddraw` when `Engine\ddraw.dll` exists) = `native,builtin` (current Wine prefers a DLL beside the exe anyway; older versions do not) |

`EIStarter-UM.cfg` is the source of truth: the registry is rewritten from it at every Play, so a deleted Wine prefix loses nothing. The first start fills
it from what EIStarter left (the clone, then Nival's key). The `[Game: <key>]` sections hold `name=dword:N`, `name=float:X`, `name=string:TEXT` or `name=hex:..`.

## Borderless window

`Engine\um-window.asi` (`window/`, built from `resources/universal-mod/um-dll/um_window.cpp`, the code of um.dll's `BORDERLESS`) is loaded by addon.dll like
any `*.asi`; the launcher switches it with `Engine\um-window.ini` (`[Window] Borderless=1`). It subclasses the game window: a deactivation never reaches the
game, the cursor is confined to the window while it has the focus. Needs the game's `fullscreen=0` (the launcher's window mode takes care of both). When
a mod loads um.dll, um.dll's own `BORDERLESS` does it (the launcher sets it in the mod's `um.cfg`) and the plugin stays out of the way.

## Releases

`.github/workflows/beta-auto-build.yml` and `master-auto-build.yml` run `tools/EIStarter-UM/package.sh` and attach `EIStarter-UM-<version>.zip` to every beta pre-release and master release, next to the mod zip. The workflow packages the committed `EIStarter-UM.exe` and `window/um-window.asi`: rebuild them (`make win window`) and commit them with a change.

## Building and testing

`make win` (EIStarter-UM.exe, i686 MinGW, GLFW 3.3 from um-multitool's vendor folder), `make window` (um-window.asi), `make test STARTER=<an EIStarter folder>` (parsers against real
mods / lang.ini / config round trip / registry sync on a stand-in), `./package.sh` (the zip).

Test rig (not in the repository): a dedicated Wine prefix with the game, the starter folder and Universal-Mod, `launch.sh` runs `EIStarter-UM.exe --play`
(the launcher presses Play itself after 5 frames) on the hidden display with the audio on a null sink and collects `EIStarter-UM.log`, `addon.log`, `um.log`.
`--dir <folder>` makes the launcher use another folder than its own; `--screenshot <file.ppm>` saves frame 12 and exits.
