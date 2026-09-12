# game.exe static analysis notes

Findings from a **read-only, static** pass over an installed `game.exe` (no
disassembly/decompilation, no modification) using `objdump -x` (PE header/
import table dump) and `strings`. Goal: understand engine internals well
enough to inform `um.dll` diagnostics/modding work, and to record useful
built-in commands the game already exposes. This documents *facts observed
in the binary's metadata and text strings only* - no game code is reproduced.

Analyzed copy: 3,825,725 bytes, PE32, 4 sections, no debug build artifacts
beyond a stripped-symbol release binary. A `game.exe.bak` (the original,
unpatched vanilla binary, dated 2001-03-26) was also found beside it and
cross-checked - see the LAA note below.

## PE header facts

- `ImageBase=0x00400000`, `SizeOfImage=0x003D7000` (static range
  `0x00400000`-`0x007D7000`). **No `.reloc` section and relocations are
  stripped** - the image cannot be rebased, i.e. no ASLR is possible for this
  binary regardless of OS-level ASLR settings.
- `Characteristics=0x12F` on the installed `game.exe`, which **includes
  `IMAGE_FILE_LARGE_ADDRESS_AWARE` (0x0020)** - but this is because
  `_cpr/game-exe-laa-patch/apply_laa_patch.py` had already been applied to
  this install in an earlier session. Cross-checked against the vanilla
  backup, `game.exe.bak` (present beside it, 2001-03-26, untouched):
  `Characteristics=0x10F` there - **LAA is NOT set in the original binary**,
  confirming the earlier repo assumption ( "vanilla game.exe is not LAA" )
  was correct all along. `cmp -l` between the two files shows **exactly one
  byte differs** (the low byte of the Characteristics field, `0x0F` ->
  `0x2F`), confirming the patch script is minimal and touches nothing else.
- Linker: MSVC linker 6.0, timestamp `2001-03-26`. Debug directory retains a
  CodeView (NB10) reference to `D:\home\Allods3\Game\ReleaseRussian\Game.pdb`
  - confirms the engine's internal codename is **"Allods3"** and this is a
  Russian release build (matches Evil Islands' Nival Interactive / Allods
  engine lineage).
- Only 4 sections: `.text` (0x339670), `.rdata` (0x478b2), `.data` (0x19000),
  `.rsrc` (0x92c8). No `.tls`, no exception directory, no export directory
  (expected for a plain EXE).

## Imported DLLs (what the engine actually links against)

| DLL | Purpose |
| --- | --- |
| `DDRAW.dll` | Only `DirectDrawEnumerateExA` + `DirectDrawCreateEx` imported (not the plain `DirectDrawCreate`) - matches `um.dll`'s existing FPS-hook code, which patches both defensively but only `...Ex` will ever actually be found on this build. |
| `DINPUT.dll` | Only `DirectInputCreateEx` - keyboard/mouse/joystick input. |
| `DSOUND.dll` | DirectSound, single ordinal import (`#2`) - likely just device enumeration/creation, actual playback goes through Miles Sound System (see below). |
| `binkw32.dll` | Bink Video (`.bik`) playback - matches `tools/third-party-tools/rad-video-tools/`. |
| `mss32.dll` | Miles Sound System (`AIL_*` functions) - the real audio engine (samples, sequences, reverb, volume, DirectSound interop). |
| `WSOCK32.dll` | Winsock 1.1 - the multiplayer networking transport. |
| `WINMM.dll` | `mmioOpenA/Read/Seek/Close` (RIFF-style file I/O, likely for `.wav`) + `timeGetTime`. |
| `ADVAPI32.dll` | Registry access (`Reg*`) - config storage, matches `docs/reg-format.md`. |
| `SHELL32.dll`, `WINSPOOL.DRV`, `comdlg32.dll` | Installer/UI-adjacent (drag-drop, printing, file dialogs) - likely used by setup/launcher dialogs bundled in the same binary, not core gameplay. |
| `ole32.dll` | `CoCreateInstance`/`CoInitialize` - likely for DirectX COM object creation. |
| `USER32.dll` | Notably imports `SetWindowsHookExA`/`CallNextHookEx`/`UnhookWindowsHookEx` directly - **the game itself installs its own Windows hook(s)**, independent of `um.dll`'s keyboard hook. Worth remembering if hook-ordering issues ever come up. |

No `D3D8.dll`/`D3D9.dll` import and no export table (plain EXE). A single
string `D3D8.DLL` appears in `.rdata` near DirectDraw-related strings and
error text (`Couldn't create CLSID_DirectMusic`, `Couldn't CreateSurface`) -
inconclusive; could be an optional/probed capability check rather than an
active render path. **Not confirmed** whether a D3D8 code path actually
exists and is reachable; would need disassembly to confirm.

## Rendering pipeline (confirms/extends earlier `um.dll` findings)

Class names recovered from assert/error strings confirm the two software
rasterizer DLLs already known to this repo (`2DintMMX.dll`, `3DfpFPU.dll`),
plus a DirectDraw abstraction:

- `CDXDirectDraw` / `CDXDirectDrawSurface` - wraps DirectDraw (`SetCooperativeLevel`,
  `CreateDDSurface`), consistent with `um.dll`'s vtable-hook approach for the
  FPS counter (this class is presumably what calls the DirectDraw COM methods
  `um.dll` hooks).
- `CD3DWindow` - has `LockRenderSurface`/`UnlockRenderSurface` methods mirroring
  the DirectDraw window class; possibly a parallel/legacy Direct3D abstraction,
  possibly dead code. Unconfirmed without disassembly.
- `CI3DFigure`, `CFigure`, `CFigureManager`, `CObject3D`, `CObject3DClientSpecific`,
  `CTexture`, `CTextureManager`, `CParticle`, `CLightManager`,
  `CInterface3D`, `CVisualizator` - the scene graph / 3D model / texture /
  particle / lighting subsystems ("I3D" = the engine's internal 3D interface
  name, seen throughout).

## Engine architecture: client/server split even in singleplayer

`CWorldClient` and `CWorldServer` both exist as distinct classes, alongside
`CTcpNetDriver` (networking) and `#say`/`lmp_*` (LAN Message Protocol?) event
name strings (`lmp_quest_completed`, `lmp_player_connected`,
`lmp_trade_requested`, etc.). This strongly suggests **the engine always runs
a client/server simulation split, even in singleplayer** (a common pattern
for RTS/RPG engines of this era, to keep multiplayer and singleplayer code
paths unified) - relevant context if a future session investigates
performance: gameplay simulation (`CWorldServer`) and rendering/input
(`CWorldClient`) may be profileable/throttleable somewhat independently.

Other architecturally significant classes:

- `CScript` + the string prefix `WorldScript(...)` - the game has an internal
  scripting language/VM ("WorldScript") used for quest/AI logic; `ConsoleString`/
  `ConsoleFloat` script functions can print directly to the built-in console
  (see below).
- `CAIMap` - a dedicated AI/pathfinding map class, separate from the render
  scene graph.
- `CFileRegistry`, `CResourceBase`, `CMappedFile`, `CMappedResource`,
  `CMMIOResource` - the `.res` archive resource-loading system that
  `tools/um-restool` already works with; `CMappedFile`/`CMappedResource`
  suggests memory-mapped file I/O (`MapViewOfFile` is imported) rather than
  buffered reads for resource files, which is consistent with why `um.dll`'s
  file-I/O hooks (`ReadFile`/`WriteFile`) don't see every resource load - some
  reads may go through `MapViewOfFile` directly instead.
- `CItemConstructor`, `CSpellConstructor` - data-driven construction of items/
  spells from the `.res` databases (`items database`, `spells database`, etc.
  referenced directly in error strings).
- `CWinRegistry` - registry read/write wrapper; config is stored at
  `HKEY_.../Software\Nival Interactive\EvilIslands` (exact hive not
  determined from strings alone, but this is the key path), which matches the
  existing `docs/reg-format.md`/`config.reg` conventions in this repo.
- `CMusicSystem`, `CBinkVideo` - audio/video playback wrappers around Miles
  Sound System and Bink respectively.

Multiplayer uses a master-server domain string `a3master.nival.com` (likely
long dead; "a3" is presumably short for the internal "Allods3" codename).

## Built-in developer console (previously undocumented in this repo)

The game ships a full in-engine debug console with a documented command set,
recovered verbatim from `.rdata` help strings. This is a **major finding**:
several `um.dll` features built this session (and one removed earlier this
session) may be redundant with functionality the game already has built in.

Recovered commands (name, then its help text where available):

| Command | Effect |
| --- | --- |
| `QUIT` / `EXIT` | Exit the game. |
| `EXEC<UTE> [file]` | Execute a text file of console commands. |
| `LASTFPS` | Show last FPS. |
| `LOADVAR [Nplayer] [file]` | Load a previously dumped list of global script variables. |
| `LISTVAR [Nplayer] <[file]>` | Dump the list of global script variables to console and/or file. |
| `FILTER <[type] [1\|on\|0\|off]>` | Filter system log messages by subsystem: `none, ai, event, graphics, rpg, net, unitacks, all`. **This is a built-in, per-subsystem message-category filter already in the engine** - directly relevant to the "which part of the engine is busy" diagnostics idea discussed this session, though it filters *log messages*, not CPU time, so it's a lead rather than a full answer. |
| `HISTORY [N lines]` | Change console scrollback depth. |
| `CONSOLE [pos]` | Move the console window: `left, right, top, bottom, center, fullscreen`. |
| `DEBUGINFO [pos]` | Move/position a built-in debug info overlay: `left, right, rt (righttop), lt (lefttop)`. **The game already has its own debug-info overlay positioning system**, independent of `um.dll`'s overlay. |
| `FADEOUT nMillSec` | Screen fade out. |
| `net time 0/1` | Show client time-counting info. |
| `net server 0/1` | Show server traffic. |
| `net client 0/1` | Show client traffic. |
| `Rate <bytes/sec>` / `LocalRate <bytes/sec>` | Set connection rate. |
| `memusage` | Present as a command name; exact output format not recovered from strings alone. |
| `give [Nplayer] money [amount]` | Grants money - **the same effect the earlier removed money-overlay feature was reading via raw memory address; this console command is the game's own supported way to do it.** |
| `give [Nplayer] exp [amount]` | Grants XP, same note as above. |
| `give items` / `give quests` | Give items/quests (parameters not fully recovered). |
| `GodMode` | Toggle invulnerability. |
| `KillUnit` | Kill a unit. |
| `Teleport` | Teleport (parameters not recovered). |
| `KillScript` / `KillScript()` | Stops the running WorldScript ("not allowed here" in some contexts). |
| `show world_data` / `show render_info` / `show managers` | Additional built-in diagnostic dumps - **very likely the most direct built-in answer to "which part of the engine is using resources"**, since `render_info`/`managers` sound like exactly that. Exact output not recovered from strings alone. |
| `days [ddmmyyyy] [DDMMYYYY]` | An Easter egg/utility computing age between two dates ("Your age: N days"). |

The console is almost certainly opened with the backtick/tilde key given
the console command strings sit right next to `WorldScript`/script-related
text, and this lines up with **why `um.dll`'s original, oldest feature exists
at all**: rewriting backtick/number-row input as US-QWERTY scan codes so
that non-US keyboard layouts can still reliably open this console and use
its number-row-driven features. This wasn't previously written down as the
"why" anywhere in this repo - worth remembering.

## Practical implications for `um.dll` / future work

1. **The `show render_info`/`show managers`/`memusage` console commands are
   the most promising lead for real per-subsystem resource diagnostics** (the
   "which part of the engine takes the most resources" ask). If a future
   session can find a safe way to invoke these programmatically (e.g. by
   locating and calling the internal command-dispatch function via
   disassembly, or by simulating console keystrokes+`ReadFile`-free stdout
   capture), that would be far more informative than OS-level CPU/memory
   sampling. This requires actual disassembly (Ghidra/IDA) to find the
   dispatcher function address - out of scope for this strings-only pass.
2. **The `FILTER ai/event/graphics/rpg/net/unitacks` command** confirms the
   engine already classifies its own log output by subsystem - if `um.dll`'s
   file-I/O/log hooks ever see this output (e.g. if the game logs to a file
   that could be tailed), that's a zero-effort way to get subsystem-tagged
   diagnostics without touching engine code at all.
3. **`give money`/`give exp` console commands make raw-memory-address reading
   unnecessary** for that use case (the money/XP overlay feature removed
   earlier this session could be reimplemented as "send console keystrokes"
   instead of memory reads, avoiding the address-instability problem
   documented in `/memories/repo/um-overlay-fps-hook.md` entirely).
4. **The vanilla binary is confirmed NOT LAA** (verified against
   `game.exe.bak`) - `_cpr/game-exe-laa-patch/apply_laa_patch.py` remains a
   correct, minimal (single-byte), useful patch for any fresh install.
5. **The engine's own `DEBUGINFO`/`CONSOLE` positioning commands** mean any
   future overlay work should keep in mind there may be two independent
   overlay-like systems on screen at once (the game's built-in one and
   `um.dll`'s) - worth checking for visual overlap.
6. This was a **static, read-only, strings/PE-header-only pass** - no
   disassembly was performed, no function addresses were identified, and
   nothing here should be treated as precise enough to hook blindly. Next
   step for deeper RE would be loading the binary in Ghidra/IDA (not done
   here) to locate: the console command dispatcher, the DirectDraw/CDXDirectDraw
   vtable-populating code (to double check `um.dll`'s slot-index assumptions
   against the actual compiled layout), and the main game-loop/tick function.
