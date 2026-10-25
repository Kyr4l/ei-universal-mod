# macOS: running Universal Mod with CrossOver

Universal Mod can run on Apple Silicon through CrossOver, without a Windows VM. This is a working configuration from one Mac, not a claim of full macOS compatibility or a native macOS port.

## Tested configuration and scope

| Component | Tested version |
| --- | --- |
| Mac | Apple M5 Pro, 64 GiB RAM |
| macOS | 26.6.2 |
| CrossOver | 26.2 (Wine 11.0 reported by the game) |
| Bottle | Windows 10, 64-bit, dedicated to Evil Islands |
| Base game | GOG Russian edition, installer `setup_evil_islands_russian_2.0.0.5.exe`, game 1.06 |
| Launcher | EIStarter-UM 0.2.6 |
| Mod | 0.8.7, build `f0cb39794` |
| Runtime plugins | um.dll 1.8.1, um-engine.dll 0.14.1 |
| Game renderer | Wine DirectDraw → wined3d → OpenGL; D7VK disabled |
| Display | 1920×1080; game at 1024×768, proportionally scaled to 1440×1080 |

Verified: the mod's main menu, readable Russian text, multiplayer character creation, a locally hosted session, entering the camp, moving the character, and loading the saved character after a restart. Polish base-game menu resources and Polish accented characters were also checked separately with the Polish locale described below.

Not verified: a complete mission or campaign, connecting another player, Internet hosting, sound playback, D7VK/Vulkan rendering, exclusive fullscreen, Intel Macs, or other CrossOver versions. Entering the multiplayer camp is not evidence that every gameplay screen works.

## 1. Install into a dedicated bottle

1. In CrossOver, create a **Windows 10 64-bit** bottle, for example `Evil Islands`.
2. Run your GOG offline installer inside that bottle. The tested installation path was `C:\GOG Games\Evil Islands`.
3. Download the launcher and mod archives from [Releases](https://github.com/Kyr4l/ei-universal-mod/releases). The tested combination was `EIStarter-UM-0.2.6.zip` and `Universal-Mod-beta-0.8.7-f0cb39794.zip`.
4. Unpack the launcher into the bottle as `C:\EIStarter-UM`. Use a fresh launcher directory rather than mixing files from an older starter.
5. Unpack the mod into `C:\EIStarter-UM\mods`, so its `config.reg` is at `C:\EIStarter-UM\mods\Universal-Mod\config.reg`.

The corresponding macOS paths are:

```text
~/Library/Application Support/CrossOver/Bottles/Evil Islands/drive_c/GOG Games/Evil Islands/
~/Library/Application Support/CrossOver/Bottles/Evil Islands/drive_c/EIStarter-UM/
```

Keep the game, launcher and mod in the same bottle. No additional .NET installation, Winetricks fonts or manually configured DLL overrides were needed for this EIStarter-UM setup. The launcher configures its loader; the [old-starter instructions](manual-installation.md) are a different installation path.

The current [release/beta folder conventions](../README.md#release-or-beta) use `Universal-Mod` and `Universal-Mod-Beta` respectively. The paths below describe the manually unpacked build above; use your actual selected mod folder if it differs.

## 2. Use the tested OpenGL renderer

Before starting the game, create or edit `C:\EIStarter-UM\mods\Universal-Mod\um.cfg`. Merge these settings into the file, preserving unrelated options:

```ini
DDRAW_DLL=""
MSAA=false
```

An empty `DDRAW_DLL` disables the mod's D7VK wrapper. The tested launcher package did not contain an `Engine\ddraw.dll` override, and no CrossOver graphics-backend overrides were added.

This is an OpenGL configuration, not a report of D7VK working through Metal/MoltenVK. After launch, check the selected mod's `um.log` for:

```text
Renderer chain: Wine ddraw > wined3d > OpenGL
```

A `Vulkan GPU` inventory line elsewhere in the log does not mean the game is using Vulkan.

## 3. Use borderless scaling

In `C:\EIStarter-UM\EIStarter-UM.cfg`, merge the following into the existing sections; do not replace the whole configuration file:

```ini
[Launcher]
Borderless=true
BorderlessScale=true

[Addon]
PauseOnMinimize=false

[Game: General Settings]
FullScreen=dword:0
VideoResolution=string:1024x768
VideoColorDepthInBits=dword:32
```

The launcher also exposes **Borderless scaled** in its window-mode options. It writes the relevant settings to the mod when Play is pressed; editing only the registry is not persistent. See [launcher configuration](EIStarter-UM.md).

On the tested 1920×1080 display, the 4:3 game image was scaled to 1440×1080. Mouse clicks lined up with menu buttons and clicked destinations in the camp.

This mode has **no title bar**, so there is no normal title-bar drag handle. Selecting the launcher's ordinary Windowed mode does not add one: the game creates a frameless window there too. A movable, decorated window was not tested; this guide does not provide a title-bar workaround.

## 4. Match the locale to the game's text resources

The Russian installation initially displayed garbled text. Setting `Font CharSet` alone did not fix it; using the Russian locale produced Windows ANSI code page 1251 and readable text. Reinstalling fonts was not needed.

Close all programs in this bottle, back up its `cxbottle.conf`, then edit:

```text
~/Library/Application Support/CrossOver/Bottles/Evil Islands/cxbottle.conf
```

Under the existing `[EnvironmentVariables]` section, add or update:

```ini
[EnvironmentVariables]
"LANG" = "ru_RU.UTF-8"
"LC_ALL" = "ru_RU.UTF-8"
```

Preserve other bottle settings. Start the game again after changing the locale. If a custom launch script explicitly sets `LANG` or `LC_ALL`, update that script too rather than keeping conflicting locale settings.

For a separately installed Polish base-game localization, `pl_PL.UTF-8` for both values produced code page 1250 and correct Polish menu characters. This does **not** translate the mod's Russian strings or provide a Polish language pack. Mixing legacy text resources from different encodings can leave some text unreadable. Changing the launcher's own language also does not translate the game's graphical menu resources.

## 5. Launch and check

Use CrossOver's **Run Command** in the same bottle to run `C:\EIStarter-UM\EIStarter-UM.exe`, select the installed mod, and press **Play**. If prompted for the original game, select `C:\GOG Games\Evil Islands`.

Alternatively, this Terminal command starts the launcher with Play selected automatically and saves command output to `~/Library/Logs/evil-islands-crossover.log`:

```sh
CX_APP="/Applications/CrossOver.app"
if [ ! -d "$CX_APP" ]; then
    CX_APP="$HOME/Applications/CrossOver.app"
fi
BOTTLE="Evil Islands"
STARTER="$HOME/Library/Application Support/CrossOver/Bottles/$BOTTLE/drive_c/EIStarter-UM"

"$CX_APP/Contents/SharedSupport/CrossOver/bin/wine" \
    --bottle "$BOTTLE" --debugmsg=-all --no-wait --workdir "$STARTER" \
    'C:\EIStarter-UM\EIStarter-UM.exe' --play \
    > "$HOME/Library/Logs/evil-islands-crossover.log" 2>&1
```

Adjust the bottle name and paths if you installed elsewhere. Do not launch another instance while the game is already running.

Check that the main menu shows **Universal Mod** and its version, rather than just the base game. To reproduce the tested gameplay scope, create or load a multiplayer character, host a local session, enter the camp and click a destination. An empty Internet server list is expected; see the [multiplayer instructions](../README.md#multiplayer) before attempting remote play.

For troubleshooting, collect `EIStarter-UM.log` and `addon.log` from the launcher directory and `um.log` from the selected mod directory. Include the macOS, CrossOver, launcher and mod versions, the renderer-chain line, and the exact last working screen. Review logs for personal paths or network addresses before posting them.
