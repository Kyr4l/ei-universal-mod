# EIStarter-UM: the mod launcher

`tools/EIStarter-UM` replaces EIStarter OBT-1 (a closed-source .NET program, slow to open under Wine, and a registry setup that many users get wrong).
C++17, Dear ImGui + GLFW like um-multitool; one 32-bit `EIStarter-UM.exe` (GLFW 3.3: runs on Windows XP and newer, and under Wine in the prefix of
the game). It starts `Engine\game.exe` with a mod and shows "Starting..." until the game window is up.

## Credits

The skins, texts, loader (`Engine\dinput.dll`), `addon.dll` and `mp3dec.asi` in the package are EIStarter OBT-1's, by the EIStarter / Gipat.Ru authors; EIStarter-UM only reads their formats, its code is new.

## The game executable

The package's `Engine\game.exe` is the game's own exe with the large address aware flag set (one bit of the PE header, byte-identical to the community's patched exe). With the stock exe the game ran out of address space under D7VK ("map failed", then "abnormal program termination" in the Multiplayer lobby). The launcher does not patch anything itself.

## Tested

In clean Wine 11.19 prefixes (hidden display, silent audio) with the package, an empty registry and Universal-Mod: the author's own install, the GOG installer (`setup_evil_islands_2.0.0.5.exe`, installed silently to `C:\GOG Games\Evil Islands`) and the 2001 CD edition (the ISO mounted as `D:`, label `EICD1`, installed by its InstallShield to `C:\Program Files (x86)\Evil Islands`): each reaches the main menu with "Universal Mod ver. 0.8.6" through the launcher. GOG writes a relative CD-ROM Path (`.\`) that makes the game ask for a disc; the launcher replaces such a path by the game folder, and keeps an existing one (the CD edition's `D:\`). Every Options control was checked: widget -> `EIStarter-UM.cfg` (OK keeps, Cancel discards), `.cfg` -> registry (the debug log lists every value written), and what the game does with it where it is visible (window size and borderless, movies off, addon.dll's draw-radius / MQ / second speed / console patches, the log level, the mod and its skin, the engine exe name and the failure message). Not exercised: real fullscreen (the hidden X display has no mode switching: the game dies on its exclusive-fullscreen mode there), the Website button (it would open a browser).

## For users

1. Unpack the launcher package (`tools/EIStarter-UM/package.sh` builds `dist/EIStarter-UM-<version>.zip`) anywhere you can write to.
2. Unpack a mod (Universal-Mod ...) into its `mods\` folder.
3. Run `EIStarter-UM.exe` (Linux: install the game in Lutris, unpack the launcher inside the game's prefix, set the game's executable in Lutris to `EIStarter-UM.exe` and start it from Lutris; there is no native Linux build, the game needs Wine anyway), choose the mod, **Play**.

The first start asks for the folder of the original game (the one with `game.exe` and `res`) when it cannot find it (`C:\Evil Islands`, the
installer's registry key, GOG's folders). There is no `Init Starter Registry.bat`, no `path` value to add by hand, no DLL override to set, and no
.NET / vcrun2022 to install (EIStarter 2 needed all of them).

The window is EIStarter's: the same skins (`design\<lang>\*.png|bmp`, `font.otf`, `click.wav`), languages (`lang\<code>\lang.ini`, UTF-16) and mod
folders (`mods\<mod>\config.reg`, or `mod.config` with the Utils option "Read mods from mod.config"), so what was made for EIStarter works.
Under the mod chooser a dot tells whether the selected mod uses um.dll: green "um.dll detected" (its `config.reg` [DEV] Plugin names it and the file is in the mod's folder), amber when only one of the two is true (addon.dll would not load it), grey "no um.dll". The (i) window's text is read-only but selectable (Ctrl+C), with Copy site / Open site buttons.
Buttons: Play, Options, Readme (`lang\<lang>\readme.txt`), Website, Uninstall (`uninstall.exe` when there is one), Exit, the mod chooser, the (i)
button (mod information, its readme / changelog, and the mod's own script button), Lang (cycles the languages). Keyboard, as in OBT-1: Play is selected at start; Tab / Shift+Tab move the selection (the selected button wears its pressed picture, the mouse does not change it), Up / Down move it through the menu or change the mod when the chooser is selected, Enter or Space press the selected button (Enter on the chooser = Play), Esc = Exit.

**Mod updates** (Universal Mod; another repository can be set with `*=owner/repo` under `[Update Repositories]` in the cfg): the release and the beta (pre-release) are kept as two mods, side by side, so their saves never mix: the newest release goes into `mods/Universal-Mod` and the newest pre-release into `mods/Universal-Mod-Beta`; both then show in the mod chooser as "Universal Mod (release)" and "(beta)". Saves can be copied from one to the other by hand. At start the launcher checks GitHub once (Utils: turn it off) and shows "update available" in the bottom right corner for the selected mod; it never downloads or installs anything by itself, and it does not keep running in the background. The (i) window has a row per line (Install / Update / Reinstall buttons, a progress bar with the megabytes while it downloads, then the unpacking): it downloads the release's `Universal-Mod*.zip`, checks every file's CRC, unpacks it beside the mod, then swaps the folders: everything is replaced except `mp` and `saves` (the saves); `um.cfg` and `um-engine.cfg` are kept when the new build has none (the release ships none) and the old ones are copied to `mods/.update/backup` when replaced. If anything fails the old folder is put back. Builds are told apart by commit: `makemod` writes `build.info` (`version=`, `commit=`) into the mod, the release zip's name ends with the same hash (`Universal-Mod-beta-0.8.6-<hash>.zip`), and the rows show "0.8.6 (b373a09) -> 0.8.6 (a1b2c3d)"; the same commit is "up to date". Without `build.info` (older builds) it falls back to what the updater recorded, then to the version number, and a name without a hash shows the build time instead. Needs HTTPS (TLS 1.2): not on Windows XP. `EISTARTER_UM_API` (a base URL) replaces `https://api.github.com` for tests. Play is disabled during an update; do not update while the game runs.

**Options** (the tabs of EIStarter's dialog): Graphics (adapter, resolution, mip-mapping, antialiasing, dithering, FPS-independent cursor (marked not recommended: it flickers with D7VK; off on a new install),
filtering, textures, shadows, terrain LOD, lighting / shadowing frequency, movies, audio), Addon (landscape / objects draw radius, pause when
minimized, addon.log levels), Multiplayer (console, "do not load .MQ from the host", second speed), Camera (every camera value, and the **Original camera** button), Paths (game folder,
resources, CD, the engine exe name), Utils (mod skins, mod.config, close when the game is up, debug log. New: **Window mode** = Fullscreen /
Windowed / **Borderless window** / **Borderless scaled**. In EIStarter's Windowed mode the game is a frameless window that minimizes and pauses when it loses the focus
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

### Borderless scaled (a lower resolution fills the screen)

The window mode **Borderless scaled** (um.cfg `BORDERLESS_SCALE=true`, `um-window.ini` `Scale=1`) lets a game resolution below the monitor's fill the screen instead of sitting in a small window in the corner: the game window gets
the game's picture scaled by the largest factor that fits the monitor (the aspect ratio is kept: 1024x768 on a 1920x1080 monitor becomes 1440x1080, 1280x720 becomes 1920x1080), centred, with black bars around it. Set the game resolution
in Video (lower than the screen's); a resolution as big as the monitor, or bigger, is left alone. How it works (`um_window.cpp`, the same code in um.dll and in the plugin):
- the window is made at its final size and place when the game creates it; two to four black windows around it are the bars; the window and the bars are topmost only while the game is active (the taskbar would cover the edge of the picture otherwise);
- D7VK stretches its frame to the window by itself; with any other DirectDraw (the game's own, Wine's) the primary surface's `Blt` / `BltFast` become one stretching `Blt` to the picture's rectangle (a stretch is the driver's job: 0.04 ms of the main thread per frame measured);
- the game reads the mouse as if its window sat at 0,0 at its own size: `GetCursorPos` / `SetCursorPos` and the position of the mouse messages the game's loop takes from `GetMessageA` / `PeekMessageA` (it never dispatches them to the window procedure) are mapped
  (`um_cursor_map.hpp`; the fraction of a pixel is carried over, so slow movement is not lost when the game re-centres the pointer every frame);
- exclusive full screen (`fullscreen=1`) has no window to scale: the plugin switches itself off and logs it.
Tested in the Wine rig on D7VK and on Wine's own ddraw: letterbox sizes, clicks and the hero's walk to the clicked point equal to the unscaled run, main thread 7.6 -> 6.8 ms (D7VK) and 7.0 -> 7.0 ms (native) per frame in the multiplayer base.

## Releases

`.github/workflows/beta-auto-build.yml` and `master-auto-build.yml` run `tools/EIStarter-UM/package.sh` and attach `EIStarter-UM-<version>.zip` to every beta pre-release and master release, next to the mod zip. The workflow packages the committed `EIStarter-UM.exe` and `window/um-window.asi`: rebuild them (`make win window`) and commit them with a change.

## Building and testing

`make win` (EIStarter-UM.exe, i686 MinGW, GLFW 3.3 from um-multitool's vendor folder), `make window` (um-window.asi), `make test STARTER=<an EIStarter folder>` (parsers against real
mods / lang.ini / config round trip / registry sync on a stand-in), `./package.sh` (the zip).

Test rig (not in the repository): a dedicated Wine prefix with the game, the starter folder and Universal-Mod, `launch.sh` runs `EIStarter-UM.exe --play`
(the launcher presses Play itself after 5 frames) on the hidden display with the audio on a null sink and collects `EIStarter-UM.log`, `addon.log`, `um.log`.
`--dir <folder>` makes the launcher use another folder than its own; `--screenshot <file.ppm>` saves frame 12 and exits.

## 0.2.1 (2026-10-23): path settings without their closing backslash

The game builds file names by appending to the path settings (`<Work Path>3DfpFPU.dll`, `<Work Path>textures\`). A "Work Path" typed or imported without its closing backslash (`D:\Evil Islands`) made the game look for `D:\Evil Islands3DfpFPU.dll`, fail to load its 3D library
("Can't load 3D transform library", an exception the game catches) and then read its missing renderer: an access violation at game.exe 0x456D8D (reading address 0xC), exit code -1073741819, with nothing shown (the crash dump has the string `D:\Evil Islandstextures\`). Reproduced in the Wine rig with the same address.
`NormalizePaths` now adds the backslash to CD-ROM Path, Resources Path and Work Path whenever one is missing (tests/launcher_tests.cpp).

## 0.2.2 (2026-10-23): offer to download Universal Mod at the first start

When the launcher starts for the first time (no `EIStarter-UM.cfg`) and no mod of `mods\` is a Universal Mod with its `um.dll` (config.reg names it and the file is there), it asks "Universal Mod is not installed: download it now?"
with the choice of the line: **Release (master)** (the newest stable build, the default) or **Pre-release (beta)**. "Download" runs the updater of Info > Updates (GitHub releases of Kyr4l/ei-universal-mod; the release goes to
`mods\Universal-Mod`, the beta to `mods\Universal-Mod-Beta`), shows the progress, then selects the new mod; a failure offers "Try again". "Not now" does not ask again (the cfg exists from then on; Info > Updates installs it later).
Tested under Wine against a local stand-in for GitHub's API (`EISTARTER_UM_API=http://127.0.0.1:<port>`): the question, the beta choice, the download, the unpacking, "um.dll detected" and the new mod chosen in the list.

## 0.2.3 (2026-10-23): no more silent failures at start

A program without a console shows nothing when it cannot start, which is what a virtual machine without 3D acceleration looked like. Now EIStarter-UM.log records each step (GLFW started, window created, the OpenGL renderer and
version, settings loaded, window shown) and every GLFW error; the window is requested three times (as wanted, then with no depth / stencil buffer, then with a frame and no alpha) before giving up; a failure to open a window, and a crash
of the launcher itself (address and code), show a message box that says what to do ("turn on 3D acceleration in the virtual machine, or update the graphics driver; send EIStarter-UM.log").

## 0.2.4 (2026-10-23): runs without 3D acceleration

When no OpenGL window can be opened (a virtual machine without 3D acceleration, a broken driver, a remote desktop), the launcher no longer gives up: it opens a plain window and draws itself on the CPU (`softrender.hpp`).
Dear ImGui's draw lists are rasterised into a 640 x 360 image (triangle filler with the top-left rule, vertex colour times texel, straight alpha blending, nearest-neighbour textures including the font atlas through ImGui's texture protocol),
which is copied to the window with GDI every frame. `--software` forces this mode; EIStarter-UM.log says "software rendering". The picture is the same as the OpenGL one (Wine rig: mean difference 0.04 / 255 over the whole window,
no pixel differing by more than 40), and one frame costs a few milliseconds. `make test` includes tests/softrender_test.cpp (a quad cut in two triangles covers every pixel once, also at fractional corners).

## 0.2.5 (2026-10-23): multiplayer second speed and console on by default

"Second speed support" (`MP Speed-control`) and "Console support" (`MP console`) were off in a new EIStarter-UM.cfg (the launcher's own default, not an EIStarter one: the first start still imports the values of an old
`EI_Starter\settings` key). Both are now on by default: that is the configuration the speed sync (um-engine `MpSpeed`) and the ghost players were tested with. A cfg that already holds a choice keeps it;
"Do not load .MQ files from the host" stays off.

## 0.2.6 (2026-10-23): Borderless scaled

New window mode **Borderless scaled** in Options > Video (see "Borderless scaled" under "Borderless window"): the launcher writes `BORDERLESS_SCALE` to the mod's `um.cfg` (the key is added after `BORDERLESS` when an older um.cfg lacks it) and `Scale` to
`Engine\um-window.ini`, which the plugin and um.dll 1.7.0 read. `um-window.asi` is rebuilt with the scaling code (the Makefile links gdi32 for it).

## 0.2.7 (2026-10-25): the camera of the original CD release

The GOG release of the game writes camera settings (`HKCU\Software\Nival Interactive\EvilIslands\Camera Settings`) that make a bad camera. The launcher knows the 62 camera values of the original CD release (`game_defaults.hpp`, read from a CD installation's registry):

* **At start** it compares the camera values it will give the game (its `[Game: Camera Settings]` section, imported from the registry at the first start) with them. When any differ, a window lists them (the value's name, yours and the original) and offers to replace them with the original ones (**Replace** / **Keep mine**, and a **Don't show again** checkbox that sets `CameraCheck=false` in `EIStarter-UM.cfg`). The old EIStarter changes some as well: its own `Gipat.Ru\EI_Starter\EvilIslands` key has the keyboard camera powers at 0.44 (original 0.46), the four camera shortcuts' axes turned (AxisX 1 / AxisZ 0 instead of 0 / 1) and `FarClipDistance` 200 (original 100), 13 values in all, which the launcher imports at its first start. The log (`EIStarter-UM.log`) lists the differing values too; in Options > Camera, hover "N values differ" for the same list.
* **Options > Camera > Original camera** does the same on demand (applied with OK; Cancel changes nothing); the tab says how many values differ.
* Replacing writes the original values into the launcher's settings and into the registry: Nival's key (what the game reads without the launcher) and the clone `HKCU\Software\Gipat.Ru\EI_Starter\EvilIslands` (what addon.dll points the game at). The values it replaced are appended to `EIStarter-UM-camera-backup.txt` (one block per replacement, `name=value` lines in the cfg's format, to paste back under `[Game: Camera Settings]`).
* Only the "Camera Settings" key is touched (including the clip distances `FarClipDistance` / `NearClipDistance`, 100 / 2 in the CD release); graphics and the other settings stay as they are.

Russian texts of the new window and button are built in (`lang.hpp`, used when the language is `ru` and the language's `lang.ini` has no such key). Tested in the launcher rig (a Wine prefix with GOG-like values in Nival's key): popup with the right count, Replace (registry, cfg and backup written), no second popup, Keep + Don't show again, the Options button with OK and with Cancel.
