# The UI framework, input, camera and the application shell in game.exe

Written 2026-10-31 from the typed Ghidra decompiles of the Russian `EIStarter OBT-1/Engine/game.exe` (modules 0x456000-0x461000
application, 0x461000-0x463000 key bindings, 0x40C000-0x411000 CCamera, 0x5E2000-0x602000 message log / console / cursor /
camera controllers / CInterface / widgets). The screens themselves (0x602000-0x65F000: dialog, camp, main menu, save / load,
lobby, options, tutorial, battle HUD, scroll bars, edit controls) are read in their own round and only named here where the
framework calls them. Earlier notes this page corrects: `findings.md` (sections 0x5E2000 .. 0x602000), `flows.md`,
`game-memory.md` ("Camera", "Console"), `game-exe-analysis.md` (console). The batch rows live in
`_cpr/claude-re/full/batch-ui.tsv` and `batch-ui-structs.tsv` (merged into `functions.tsv` / `structs.tsv`).

Tags: **[V]** read directly in the decompile, **[I]** inferred from several functions, **[?]** guess / not resolved.
Addresses are VAs in game.exe; offsets are bytes; "UI units" = the virtual 800x600 space every screen is laid out in.

## How to re-implement it (short version)

1. **One window, one loop.** `main` (0x456440) registers class `EIGAME`, creates a `WS_POPUP | WS_VISIBLE` window of the
   configured resolution at (0, 0) (no border even in windowed mode), then runs a PeekMessage loop: when no message is waiting
   it runs a *frame* (net tick, fixed 55 ms server / client ticks with the accumulators `g_0x79BC64` / `g_0x79BC68`, camera,
   pending screen changes, render + present); every message goes to `App_HandleInputMessage` (0x457970) -> `CInterface`.
2. **Settings live in the registry** (`HKCU\Software\Nival Interactive\EvilIslands`, read by `AppSettings::ReadFromRegistry`
   0x45B210 into the 0x26C-byte global `g_0x79BC70`, written back at exit). The ini reader / writer exist but have no callers.
3. **Input is Win32 messages**, not DirectInput: keys arrive as `WM_KEYDOWN` with the *scancode* (`lParam >> 16 & 0x1FF`) looked up
   in `keyboard.ini`'s scancode -> action table `g_0x79BF70[0x200]`; the mouse position comes from the messages / `GetCursorPos`.
   DirectInput is only used for the *relative* mouse delta of the drag-look (`DInputMouse::GetState` 0x65ECB0, `g_0x7C7C60/64`).
4. **The cursor is drawn in software** (`CSoftCursor` 0x5E4E00-0x5E67B0, global `g_0x7C7BD8`): 25 `.mmp` frame strips, 125 ms
   animation, composed with an AND mask into the back buffer in `App_RenderFrame`, plus (FPSIndependentCursor) a 41 ms thread
   that repaints it on the *front* buffer.
5. **The UI is 3D geometry inside the scene.** Every window is a `CObject3D` (`CUIWidget` 0x148 bytes) owning `CI3DFigure` quads
   positioned at depth z by `x -> (x / 400 - 1) * tan(fov/2) * z` (`CObject3D::SetScreenPos2D` 0x5E6FA0, fov = pi/3.5), plus GDI
   text surfaces (`CUIBlitSurface`) blitted after the 3D pass, hit regions (`UIButton`) and animated figures. Screens are
   `CUIWindow`s (child windows + event handlers) on a stack owned by `CInterface` (`g_0x7C7D50`).
6. **Events**: `CInterface` sends every mouse / key event to the capture handler (`+0xC`) or the top screen's `IUIEvents`
   sub-object at `this + 0x94` (13 slots). `CUIWindow::Dispatch_*` (0x601110..) offers it to the event handlers first (camera
   controllers, click handlers), then to every enabled child whose rect contains the point, stopping at the first that sets
   its `handled` byte (`+0x9C`).
7. **The camera** (`CCamera` 0x40C8B0, global `g_0x79B2A8`) is a target + yaw quaternion + pitch + distance model eased every
   tick (`CCamera::Update` 0x40FB30, dt in 55 ms ticks); the controllers only set velocities / inputs. `.cam` files are
   0x24-byte keyframes `{time, pos, quat}` played by `PlayCamFile` (0x40EFB0).

## 1. Conventions

| Thing | Value |
|---|---|
| UI space | 800 x 600 virtual pixels; `UiScale::ScaleRect` 0x5F7CA0 `x * screenW / 800`, `y * screenH / 600` (integer); `UnscalePoint` 0x5F7E00 the inverse; screen size = `CInterface +0 / +4` (primary surface description, `Init` 0x5F4990) [V] |
| UI -> 3D | `CObject3D::SetScreenPos2D(x, y, z)` 0x5E6FA0: `pos = ((x/400 - 1) * k * z, (y/400 - 0.75) * k * z, z)`, `k = 0.48157462 = tan(pi/7)` = tan(fov/2); a quad built by `UIQuadMesh::InitRect` gets the uniform scale `z * k * scale / 400` so `scale` UI units = one vertex unit; y grows downwards on screen because the UI view has y down [I]; 800 UI units span the whole view at any depth [V] |
| depths used | 7.8 / 7.9 / 8.0 for the camp frame layers (`UIFrameWidget::BuildFrameMesh` 0x5F2A90), 10 for most quads; smaller z = in front [V] |
| texture uv | pixel coordinates of a 256 px texture: `u = px / 256`, `v = 1 - py / 256`, both inset by 1/512 towards the centre (`InitRect` 0x5E70E0) [V] |
| colours | GDI `COLORREF` BGR for text (`0xA7D7E4` = light yellow, `0xFFFFFF` white, `0x00FF00` green); `'^N'` in a message log line = colour escape, N bit0 blue, bit1 green, bit2 red (`^7` white, `^4` red, `^2` green, `^1` blue, `^6` yellow) (`CUIMessageLog::AddMessage` 0x5E2A30) [V] |
| fonts | `CInterface +0x70 / +0x74 / +0x78` = Times New Roman at `size * 0x68 / 0x70 / 0x90` (size from a 75 pt probe, i.e. scaled with the resolution); `MeasureText` 0x5F8340 returns UI units; the console uses Courier New 9 pt [V] |
| time | `timeGetTime()` ms everywhere in the UI; game ticks `g_0x78FF34` ms (55 normal, 27 at x2); `CCamera::Update` dt = ms * 0.0181818 = ticks [V] |
| widget rects | `CUIWidget +0xD8` int[4] in UI units; `PointInside` 0x5FFD30 unscales the screen point first [V] |
| key events | `OnKeyDown(vk, repeat, flags)`: `flags & 0x1FF` = scancode (bit 8 extended), `flags & 0x4000` = autorepeat [V] |

## 2. The application shell (0x456000-0x461000)

### 2.1 `main` (WinMain, 0x456440) - startup in order [V]

1. `res\sysmsg.txt`: three lines (DirectX 7 missing, low disk space, insert CD 2 prompts; English defaults when the file is missing).
2. `App_ProbeDirectX` 0x45E310 -> level code (0x100 IDirectDraw2 .. 0x700 IDirectDraw7, 0x800 D3D8 present); `< 0x700` -> `MessageBox` "CRITICAL ERROR" + exit.
3. `FactoryMap_Register` 0x458570: save-object factories 1 = `CGSPlayer_New` 0x457850, 2 = `NamedParty_New` 0x457740.
4. Command line (space separated): `-moviepath_<dir>`, `-nocam` (clears flag 8 of `g_0x7AF5C0`), `*.cam` -> `g_0x79BA1C`, `*.scr` -> `g_0x79BA18`, `*.mob` -> `g_0x79BA14`, `*.mpr` -> `g_0x79BA10`, `-benchmark_<n>` -> `g_0x79BD60 / 5C`, `-devicetarget_<n>` -> `AppSettings +0x2C`.
5. `LoadString` 0x65 = title "Evil Islands" (`g_0x79BB24`), 0x66 = class "EIGAME" (`g_0x79BA24`); `RegisterClassEx` (style `CS_DBLCLKS`, icon 0x69, **no cursor, no background brush**, wndproc `App_WndProc` 0x457D40). `FindWindow(class, title)` -> an existing instance is brought to front and this one `_exit(-1)`.
6. `g_0x7AF450 = "Software\Nival Interactive\EvilIslands"`; `World_InitWorkDir` 0x55EA10 (work dir = current dir + `\`); `AiInfo_Load config\ai.reg`; `AppSettings::ReadFromRegistry`; `CWorldClient::Init` / `CWorldServer::Init`; `CCamera::InitFromSettings` 0x40D0A0 (near / far / fov pi/3.5 from the settings).
7. Free disk space < 300 MB -> sysmsg line 2 warning; CD 2 volume label check (line 3) when the resources path is not the work path; `saves\`, `mp\`, `textures\` created under the work dir.
8. `DInputMouse` object -> `g_0x79BA08`; **`CreateWindowExA(0, "EIGAME", title, WS_POPUP | WS_VISIBLE (0x84000000), 0, 0, VideoResolution w, h)`** -> `g_0x79BC28`; `ShowWindow(nCmdShow)`; `SetCursor(0)`. The window has no frame in either mode: "fullscreen" only decides whether the DirectDraw device takes exclusive mode (`CDXDirectDraw +0x20 & 1`), so a borderless launcher only needs `FullScreen = 0` and a resolution equal to the desktop (um.dll BORDERLESS subclasses this window, `um-dll/um_window.cpp`).
9. Renderer `vt[2](hwnd, settings)` (`g_0x7AF080` DirectDraw / D3D7 device); video-memory probes -> `g_0x784753/754/755` (< 8 MB flags; `g_0x784753` makes the transition screenshots 400x300); `g_0x784756 = DrawingTarget != 0` (software device -> 16 px cursors); `SetGamma(brightness, contrast, gamma)`.
10. Audio (`CSoundManager::Open(hwnd, 1, -1, 44100, 16)`, `streamsn.reg`, `sfx.res`, `res\speech.res`, volumes `+0x58 / +0x5C / +0x54`, Reverse_Stereo); `Movies_PlayIniList("Start")` 0x458270 (`config\movie.ini` section key `movies`, comma separated, `Movies\<name>` through `Movie_PlayFile` 0x5DB3F0); `Progres.bik` loading movie; databases (perks, quests, units, spells, items, levers, `database.res`).
11. `DInput_Create` 0x65EAD0; `CWorldClient::InitClientResources`; `Cursors_Register` 0x5DAEA0 (FPSIndependentCursor -> kind 2, small set when `g_0x784756`); **`CInterface::Init(g_0x7C7D50, device)`**; `MapInfo_LoadBoth` 0x45EA00 (`map.txt`, `map_LMP.txt`); `App_LoadMapAndScript` 0x456140 (the `-mpr/-mob/-scr` arguments; without a `.mob` the game starts in the menu flow through `Game_SessionTick`).
12. The message loop (2.3). On `WM_QUIT`: `CTextureManager::DeleteCachedFiles` (`textures\*.mmp`), `CInterface::Destroy` 0x5F4F90 (writes `config\tutorial.ini`, `KeyBindings_Save`), `CSoftCursor::Stop`, release the mouse, `SetGamma(0, 0, 0)`, renderer `vt[3]`, `AppSettings::WriteToRegistry` 0x45C560, `CWorldServer::Done`, `CWorldClient::Done`; returns `wParam`.

### 2.2 `App_WndProc` (0x457D40) [V]

| Message | Action |
|---|---|
| `WM_PAINT` | `BeginPaint` / `EndPaint` only when `GetUpdateRect` says so (nothing is painted by GDI) |
| `WM_DESTROY` | `PostQuitMessage` when quitting is allowed (`g_0x784751`, `App_SetQuitAllowed` 0x455F10) |
| `WM_ACTIVATE` | minimized -> `g_0x79BC5D = 1` (activation changed), `g_0x79BC5C = 0`; `WA_INACTIVE` -> inactive, `CSoftCursor::SetMode(0)`, `CDXDirectDraw::RestoreDisplayMode` when exclusive, `DInputMouse::Acquire(0)`, `CSoundManager::PauseAllSamples`, **`ShowWindow(SW_MINIMIZE)`** (also in windowed mode: the reason um.dll swallows it); `WA_ACTIVE` -> `g_0x79BC5C = 1`, `SW_RESTORE`, `Acquire(1)`, `I3DFigureImpl::RestoreSurfaces` + `Cursors_StopAndRegister` when exclusive, `SetMode(1)`, `ResumeAllSamples`, `CInterface::ResetInputState` |
| `WM_SETCURSOR` | `SetCursor(0)` - the system cursor is never shown |
| `WM_SYSCOMMAND` | `SC_SCREENSAVE` (0xF140) / `SC_MONITORPOWER` (0xF170) return 0 (blocked); task list / minimize / close log "switch task" |
| `QueryCancelAutoPlay` | returns 1 (no autoplay for the CD) |

### 2.3 The message loop and one frame (`main` 0x456440, lines after the gamma call) [V]

```
loop:
  while PeekMessage(no remove) finds nothing:
    activation changed (g_0x79BC5D)? inactive: render-globals flag &= ~1, InvalidateRect; active: (device flag 2: re-query the
        primary surface under g_0x7AEE78) flag |= 1, SetCursor(0)
    dt = timeGetTime() - g_0x79B9F8 (ms since the last frame; first frame: 0)
    render-globals inactive  -> World_SetStatScramble(0); frameRendered = 0           (messages then go to App_WndProc only)
    else:
      World_SetStatScramble(1)
      CWorldClient::NetObjectIdle -> Lobby_IsAlive ? Lobby_Create : LobbySession_ReleaseActive2
      Net_TickAll 0x4122D0                                             (1st call: um-engine mp_speed hooks it)
      dt = min(dt, 5 * g_0x78FF34);  movie capture (g_0x79BC54): dt = 1000 / MovieCapturingFPS
      serverAccum g_0x79BC64 += dt;  clientAccum g_0x79BC68 = ftol(clientAccum + dt * g_0x784758) (speed factor, 1.0 when not in state 2) [I: the multiply]
      while serverAccum >= tick: CWorldServer::v6 Tick (0 -> stop everything), serverAccum -= tick, Net_TickAll
      while clientAccum >= tick and (TickNetAndQuery6 or not connection state 2):
            CWorldClient::v6 Tick, clientAccum -= tick, CCamera::Update(tick / 55), Net_TickAll   (2nd call: mp_ghost hook 0x4573AE)
      Session_ProcessRequest(g_0x7AFE20) 0x57C700 (handles one pending SessionRequest: zone change, save, menu flow; world-server.md 1)
      g_0x7AFCA8 = clientAccum / tick (the render interpolation fraction) unless paused (g_0x7AF624)
      client speed balance: lag = TickNetAndQuery7 - fraction; speed = lag < 0 ? 2^(0.1 lag) : lag < 1 ? 1 : 0.9 + 0.1 sqrt(lag);
            g_0x784758 = clamp(speed, 0.1, 10); 'Client speed ... balance' log when g_0x79BC60
      CCamera::Update once per whole tick of dt, then once with the remainder (um-engine camera_smooth redirects these 3 call sites)
      not paused: recording flag 0x10 -> CCamera::RecordKeyframe; playback flag 0x20 -> CCamera::PlayCamFile(tick g_0x7AF61C + fraction)
    frameRendered = 1
    CInterface::ProcessPendingScreens 0x5F57C0      (pops / replaces screens requested during the frame, ticks the top screen)
    App_RenderFrame(g_0x784752 present, 1) 0x458010  (BeginFrame, Render3D, EndFrame, Draw2D, cursor, flip, screenshot capture)
  GetMessage -> 0 = WM_QUIT -> shutdown (2.1 step 12)
  TranslateMessage; frameRendered ? App_HandleInputMessage : App_WndProc
```

`App_RenderFrame` (0x458010): only when the window is active (`g_0x79BC5C`) and a screen is open (`g_0x784750`, else `Sleep(50)`):
`I3DFigureImpl::BeginFrame` -> `CInterface::Render3D` 0x5F5220 (frozen background blit, top screen `vt[20] Draw` = the world + the
screen's quads, `ScreenFade::Draw`) -> `EndFrame`; `CInterface::Draw2D` 0x5F5320 (blit surfaces, hover / tooltips, message bar,
transition capture, FPS bar, console); `CSoftCursor::Lock / Draw / Unlock`; `CDDSurface::Blit` (the flip; "Can't flip render
surface"); movie capture writes `%sshot%.4d.bmp` of the back buffer.

### 2.4 `App_HandleInputMessage` (0x457970) [V]

`CInterface::FlushCloseRequest` before and after. `WM_MOUSEMOVE` -> `CInterface::OnMouseMove(keys, x, y)`; `0x201/0x202` L
down / up, `0x203` L dblclk, `0x204/0x205` R down / up, `0x206` R dblclk; `WM_MOUSEWHEEL` (delta = HIWORD) and the private
message `0xCC54` (wheel with delta 0) -> `OnMouseWheel`; `WM_KEYDOWN / WM_SYSKEYDOWN` -> `OnKeyDown(vk, repeat, flags)` except
Alt+F4 (`vk 0x73` with bit 29) -> `PostQuitMessage` when allowed; `WM_KEYUP / WM_SYSKEYUP` -> `OnKeyUp` (Alt+Tab re-sends
`WM_ACTIVATE`); `WM_CHAR` -> `OnChar`; everything else `App_WndProc`. Mouse coordinates are the message's client pixels; the
screens unscale them to 800x600.

### 2.5 Settings - `AppSettings` (`g_0x79BC70`, 0x26C bytes; every field in `batch-ui-structs.tsv`) [V]

Read by `ReadFromRegistry` 0x45B210 from `HKCU\Software\Nival Interactive\EvilIslands` (string / DWORD / 4-byte binary float
values, the compiled-in default when missing), written by `WriteToRegistry` 0x45C560 at exit (so every value exists after the
first run). `ReadFromIni` 0x459100 / `WriteToIni` 0x45A380 read the same keys from ini sections (`Path Settings`, `General
Settings`, `Terrain Settings`, `Statistics Output Settings`, `Camera Settings`, `Game Specific Settings`, `Shadow Settings`,
`Network Settings`) but **have no callers**. `SetDefaults` 0x458DA0 is an older default set (640x480, master server
`a3master.nival.com`), also unused.

| Group | Keys (offset in AppSettings) |
|---|---|
| paths | `Work path` +0x20, `Resources path` +0x24, `CD-ROM Path` +0x28, `CD1_Label`..`CD3_Label` +0x218.. (EICD1..3), `DefaultMapName` +0x258, `DefaultStreamingSound` +0x25C, `Master_Server_Name` +0x268 |
| video | `DrawingTarget` +0x2C (0 = hardware), `VideoResolution` "WxH" -> +0x30/+0x34 (800x600), `VideoColorDepthInBits` +0x38 (16), `VideoZBufferDepthInBits` +0x3C, `VideoRefreshRate` +0x40, `FullScreen` +0x44 (1), `Video` +0x4C, `Dithering` +0x60, `Specular` +0x64, `Antialiasing` +0x68, `TextureQuality` +0x6C, `Mipmapping` +0x70, `Filtering` +0x74, `PerspectiveCorrection` +0x78, `ShowParticles` +0x7C, `AutomaticQuantization` +0x84, `MovieCapturingFPS` +0x88 (15), `Brightness` +0x8C, `Contrast` +0x90, `Gamma` +0x94 (floats) |
| audio | `Audio` +0x50, `AudioStreaming` +0x54 (music), `AudioSFX` +0x58 (master), `AudioSpeech` +0x5C (volumes 100), `Reverse_Stereo` +0x98 |
| shadows / lighting | `ShadowQuality` +0x9C, `HeroShadow` +0xA0, `UnitShadow`, `BuildingShadow`, `FloraShadow`, `OtherShadow` +0xB0, `LightingFreq` +0xB4 (10000), `FloraSpecialLight` +0xB8, `EnableSelfShadowing` +0xCC, `EnableDynamicLights` +0xD0, `ShadowingFreq` +0xD8, `ObjectsLightingCoeff` +0x22C, `Hero_Light_R/G/B/Radius` +0x230.., `Quest_Light_*` +0x240.. |
| terrain / effects | `BorderFogDistance` +0xC4, fog 40 / 75 / 32 m, `FogNightStartDistance` +0x260 (50), `FogDayStartDistance` +0x264 (90), `EnableWaterWaves` +0xC8, `EnableFootprints` +0xDC, `EnableBloodprints` +0xE0, `EnableFireprints` +0xE4, `EnableBloodprintsRelight` +0xE8, `DefaultWindForce` +0x1E8 (0.4), `DefaultTimeScale` +0x1EC (60) |
| camera | `CameraFlipXAxis` +0xF4, `CameraFlipYAxis` +0xF8, `CameraMinHeight` +0xFC (4), `CameraMaxHeight` +0x100 (100), `CameraDefaultHeight` +0x104 (30), `CameraDefaultXDeploy / YDeploy` +0x108 / +0x10C, `CameraMin/Max/MinLimit/MaxLimit/DefaultDistanceToCarrier` +0x110..+0x120 (4 / 100 / 3 / 80 / 30), `CameraBriefing*` +0x124..+0x138, `CameraBorderScrollArea` +0x13C (5 px), `CameraFrameSelectionSensetiveArea` +0x140 (20), `CameraTerrainSensetiveArea` +0x144 (10), `CameraDefault{Scroll,Rotate,Pitch,Zoom}{Speed,PowerMouse,PowerKeyboard}` +0x148..+0x174 (1 / 1 / 0.5), `CameraShortcut_%d_{AxisX,AxisY,AxisZ,Angle,Distance,Pitch}` x4 +0x178 (0x18 each), `CameraDefaultMinPitch / MaxPitch` +0x1D8 / +0x1DC (20 / 80 deg), `NearClipDistance` +0x1E0 (2), `FarClipDistance` +0x1E4 (100) |
| game | `Joystick` +0x48 (0), `FPSIndependentCursor` +0x80 (1 = cursor thread), `UseSWCursors` +0x1F0, `SelectionType` +0x1F4, `CornerSize` +0x1F8, `DrawVisibleOnly` +0x1FC, `DrawPath` +0x200, `DrawFlyingHP` +0x204, `ShowTutorial` +0x208 (`g_0x79BE78`), `Autosave` +0x20C, `ToolTip_Time` +0x210 (0.5 s, `g_0x79BE80`), `SwitchFilters` +0x214, `Font_CharSet` +0x224 ("english"), `Keyboard_Layout` +0x228 (0x409; `CInterface::Init` activates that HKL), `Difficulty_Level` +0x254, `CreateOutputInfo` +0xEC, `NumberOfFrames` +0xF0 |

### 2.6 `map.txt` (`MapInfo_Load` 0x45EA20) [V]

`<resources>\map.txt` and `map_LMP.txt` are parsed with `CTextManager::ReadLines` into the global-map data (`MapInfo`: `+4`
allod records 0x2C bytes, `+0x18` zone records 0x5C bytes): `#allod <n>` (5 lines each), `#zone <n>` (name, kind word `brief` /
camp / base, then per zone `#camera`, `#maps` (2 names -> +0x20 / +0x24), `#figure`, `#weather` (`tornado` ...), `#cage`,
`#restrict` (3 ints -> +0x38..), `#exit <n>` (0x3C-byte exit records at +0x4C), `#deploy`, `#deployangle`, `#passtime`,
`#remove`, `#view`). `QuestFiles_Scan` re-reads it for `#quest`. The world-map screen (0x740B08) and the quest log draw from it.

## 3. Input

### 3.1 `keyboard.ini` (`KeyBindings_Load` 0x462C10, `Save` 0x462DF0, `FindKeyByAction` 0x462F70) [V]

`<saves root>\config\keyboard.ini`, one `<keyname> <actionname>` per line. 88 key records `g_0x7857B0` (0x48 each: scancode,
static name, `GetKeyNameText` display text): `1`..`0`, `-`, `+`, `TAB`, `Q`..`]`, `A`..`'`, `\`, `Z`..`/`, `KP_STAR`, `SPACE`,
`CAPSLOCK`, `F1`..`F12`, `PAUSE`, `KP_*`, `HOME`, `UP`, `PGUP`, `LEFT`, `RIGHT`, `END`, `DOWN`, `PGDN`, `INS`, `DEL`. The table
`g_0x79BF70[0x200]` maps a scancode (bit 8 = extended) to an action code; a key becomes an action in the handler that reads
`g_0x79BF70[flags & 0x1FF]` (`CamCtlFree::OnKeyDown` 0x5EADD0, `CInterface::OnKeyDown` for 0x3B, the battle HUD, the camp
screens). 60 actions `g_0x787070` (0x10 each: code, name, options group, order):

| Codes | Actions (group) |
|---|---|
| 1..4 | `pause`, `decel`, `accel`, `obj` (game) |
| 5..0x10 | `camera_up`, `camera_down`, `camera_left`, `camera_right`, `camera_zoom_in`, `camera_zoom_out`, `camera_track`, `camera_norm`, `camera1`..`camera4` (camera) |
| 0x11..0x16 | `cs_head`, `cs_body`, `cs_rleg`, `cs_lleg`, `cs_rhand`, `cs_lhand` (called shots) |
| 0x17..0x1D | `crawl`, `sneak`, `walk`, `run`, `swarm`, `follow`, `use_science` (movement) |
| 0x1E..0x25 | `spell1`..`spell8` (spells) |
| 0x26..0x2D | `weapon1`..`weapon4`, `item1`..`item4` (weapons / items) |
| 0x2E..0x34 | `w_info1`..`w_info4`, `w_text1`, `w_text2`, `w_minimap` (windows) |
| 0x35, 0x36 | `quicksave`, `quickload` (game) |
| 0x37..0x3A | `select1`..`select3`, `select_all` (selection) |
| 0x3B | `tutorial_script` (shows the top screen's tutorial page: `CInterface::OnKeyDown`) |

Options-screen groups: 3 game, 4 selection, 5 movement, 6 camera, 7 weapons / items, 8 spells, 9 called shots, 0xA windows.
`CTooltipControl` appends "(key)" from `FindKeyByAction`.

### 3.2 Mouse [V]

Position: the `WM_MOUSE*` client coordinates (and `GetCursorPos` in `CSoftCursor::UpdateState`); the HUD stores the last
position in `g_0x7C7C50 / 54` (copied to `CInterface +0x68 / +0x6C` once per frame for the hover tests). Relative motion: the
DirectInput mouse (`DInputMouse` 0x20 bytes, `IDirectInputDevice7` `GUID_SysMouse`, cooperative level `NONEXCLUSIVE |
FOREGROUND`, `GetState` 0x65ECB0 -> `+0x10 dx`, `+0x14 dy`, `+0x18 wheel`, `+0x1C buttons`) accumulated into `g_0x7C7C60 /
64` and consumed only by `CamCtlFree::OnMouseMove` during the right-button drag-look (the cursor is frozen with
`CSoftCursor::SetSoftwareMode(1)` and put back with `SetCursorPos` on release). Buttons are never read from DirectInput.
The mouse is acquired / unacquired with the window activation.

### 3.3 The software cursor `CSoftCursor` (`g_0x7C7BD8`, 0xB8 bytes) [V]

- `Init` 0x5E60B0 (device, kind, mouse, small): cursor size `g_0x7C7C90 / 94` = 32 (16 for the small set), screen size from the
  primary surface, scratch surfaces 32x32 (`+0x60`) and 64x64 (`+0x64`), event `+0x1C`. `kind` 1 = composed only in
  `App_RenderFrame`; 2 (`FPSIndependentCursor`) = plus `ThreadProc` 0x5E5690: every 41 ms, while the window is active, restores
  the saved background on the **front** buffer (`device +0x14`) and draws the cursor there, so it moves between frames.
- Frames: `Cursors_Register` 0x5DAEA0 adds 25 `cursor_*.mmp` (or `sm_cursor_*`) strips with `AddFrame` 0x5E63F0 ->
  `CSoftCursorFrame::CreateFromMmp` 0x5E4EB0 (0x20-byte frame: surface strip of 0x7C-px frames, count = width / 0x7C, hot spot,
  AND-mask buffer). `SetFrame(i)` 0x5E5200 indices: 0 default, 1 attack, 2 spell, 3 wait, 4 use, 5..12 scroll
  u / ur / r / dr / d / dl / l / ul, 13 cancel, 14 talk, 15 move, 16..21 attack_hd / bd / lh / rh / ll / rl, 22 camera, 23
  spellcancel, 24 steal.
- `UpdateState` 0x5E52E0: animation column advances every 125 ms, position from `GetCursorPos` (or `+0x80/+0x84` in software
  mode), clamped to the screen, `dstRect +0x30` = position - hot spot clipped, matching `srcRect +0x40`.
- `Draw` 0x5E54A0 (from `App_RenderFrame`): `Blt` the back-buffer pixels under the cursor into the scratch, `ComposeFrame`
  0x5E5C40 (`scratch = scratch & mask | frame`, 16 / 32 bpp), `Blt` back; kind 2 only restores the background on the front
  buffer. um.dll `um_ddraw.cpp` replaces the read-back with a colour-keyed quad (the GPU stall on D7VK).
- `SetMode(0)` hides it (window inactive, movies); `RestoreBackground` 0x5E6630 is called by the Bink player around frames.

### 3.4 Modifiers and routing in `CInterface` [V]

`OnKeyDown` 0x5F78C0: scancode `0x29` (`` ` ``) toggles the console (`CUIConsole::SetVisible`); `0x2A / 0x36` -> `+0x7D shift`,
`0x1D / 0x11D` -> `+0x7C ctrl`, `0x38 / 0x138` -> `+0x7E alt` (not on autorepeat); `0x46` (Scroll Lock) -> screenshot
`shot%.4d.bmp` of the primary surface (counter `g_0x7C7E24`); console visible -> `CUIConsole::OnKeyDown`; lobby chat input mode
-> message log slot 9; else top screen sub-object slot 9; afterwards, when the screen did not handle it (`+0x9C`) and the key's
action is 0x3B -> `ShowTutorialForTopScreen(1)`. `OnKeyUp` 0x5F7BE0 clears the modifiers by *virtual key* (0x10 / 0x11 / 0x12),
`OnChar` 0x5F7B40 skips scancode 0x29. Mouse: `OnLButtonDown` 0x5F76A0 .. `OnMouseWheel` 0x5F7880 forward slot 2..8 / 12 to the
**capture handler `+0xC`** (an `IUIEvents*` set by a screen that wants every event, cleared on every push / pop) or else to the top
screen's sub-object; `+0x7F` = a double click is pending (set by slot 7, cleared by the next L down / up), `+0x80` = right button
held. `ResetInputState` 0x5F8C50 clears the modifiers and sends `Deactivate` (slot 1) to the top screen (window activation,
screen switches).

### 3.5 The camera controllers (`IUIEvents` handlers added with `CUIWindow::AddEventHandler`) [V]

Settings globals used below (AppSettings fields): scroll power mouse `g_0x79BDBC` / keyboard `g_0x79BDC0`, scroll speed
`g_0x79BDB8`; rotate `g_0x79BDC8` / `C` / `C4`; pitch `g_0x79BDD4` / `D8` / `D0`; zoom `g_0x79BDE0` / `E4` / `DC`; flip X / Y
`g_0x79BD64` / `68`; border scroll area `g_0x79BDAC`.

| Controller | Created by | Keys | Mouse |
|---|---|---|---|
| `CamCtlFree` 0x5EACB0 (vt 0x740208, 0x2C) - the in-game camera | battle HUD `Build`, world view (id 7) `OnOpen`, `CDemoScreen::OnOpen`, `DialogScreen::OnOpen` | keyboard.ini actions: `camera_up/down` forward axis `+0x20`, `camera_left/right` side `+0x24`, `zoom_in/out` zoom `+0x28`, `camera1..4` -> `CCamera::LoadView(0..3)` (`SaveView` with Ctrl / Alt). Without Ctrl / Alt: scroll velocity `(side, -forward) * kbdScrollPower * scrollSpeed`, zoom input `zoom * kbdZoomPower * zoomSpeed`; with Ctrl / Alt: rotate `side * kbdRotatePower * rotateSpeed`, pitch `forward * kbdPitchPower * 50`, zoom | not dragging: edge scrolling when `g_0x79BDAC > 0` (cursor within the margin -> direction mask 1 left, 0x10 right, 0x100 up, 0x1000 down -> scroll velocity +-1 per axis + keyboard axes, cursor frames 11 l / 7 r / 5 u / 9 d / 12 ul / 6 ur / 10 dr / 8 dl); right button held (`OnRButtonDown` 0x5EAD00 freezes the cursor): DirectInput delta: rotate `dx / 400 * 10 * mouseRotatePower * speed * flipX`, pitch `dy / 300 * 500 * mousePitchPower * speed * flipY` (only when `|dy| > |dx|` or already pitching); with Ctrl / Alt: scroll `(dx / 400, dy / 300) * mouseScrollPower * speed`; cursor frame 22 (camera); wheel -> `CCamera::OnWheelZoom` 0x4107A0; `Deactivate` stops everything (`CCamera::StopMotion`) |
| `CamCtlClassic` 0x5EA810 (vt 0x74019C, 0x24) | nothing references vt 0x74019C in the decompiles: dead code of an older scheme [V] | virtual keys: arrows = forward / side axes, PgUp / PgDn = zoom axis; Ctrl: scroll + zoom; else rotate / zoom / pitch | right drag in UI units `(x / 400 - 1, y / 300 - 1)`: Ctrl -> rotate / pitch with the flips, else scroll |
| `CamCtlSimple` 0x5EB600 (vt 0x740244, 0x14) | nothing references vt 0x740244 either: dead code [V] | arrows = rotate / pitch axes, PgUp / PgDn zoom input directly; Ctrl: rotate `axis * kbdRotatePower * speed`, pitch `* 30`; else scroll `(rotate, pitch) * kbdScrollPower * speed` | none |
| `UIOrderClickHandler` 0x5EB8C0 (vt 0x740278, 0x20) | world view screen (id 7) | Ctrl -> cursor 1 (attack), Space -> `Game_SetServerPaused` toggle | left click: unit -> added to the id array `+0x10` (and hidden), terrain -> `PickTerrainPoint2` + `CPlayerClient::SendMsg30_IdList` (move); Ctrl + unit -> `SendMsg31_IdList(ids, unit, 6)` (attack); right click clears. A debug order tool, not the battle HUD |
| `UIKeyHandler741154` (12 bytes) | world view / dialog `OnOpen` | empty slots 9 / 11 (a placeholder) | |

### 3.6 `CCamera` (`g_0x79B2A8`, 0x308 bytes, vt_CCamera; fields in `batch-ui-structs.tsv`) [V]

Model: target `+0xE0` (x, y, z = terrain height + `+0xEC` height offset), yaw quaternion `+0xF4` (about z), pitch `+0x104`
(degrees), distance `+0xF0`; `UpdateFromTarget` 0x40F8B0 derives the view quaternion `+0x34 = yaw * rot_x(-(90 + pitch))` and
the position `+0x1C = target - up(view) * distance`, then `AvoidObstacles` 0x40E4C0 lifts the camera above the terrain box
(`Terrain_SampleHeightBox` 0x40D9B0, +-2 m) and above objects within 16 m, by at least `CameraMinHeight` and at most
`CameraMaxHeight` above the ground. `Update(dt)` 0x40FB30 (dt in ticks): sound listener; flags `0x60` (recording / playback)
return; follow the carrier unit `+0x11C` (flag 8 = snap onto the unit, else the target eases 20 % per call); scroll velocity
`+0x98` eases to `+0xA4` with `+0xB0` (0.1333), moves the target by `v * dt * (0.2 + (dist - min) / (1.125 (max - min)))` and
drops the carrier; leash flag `0x100` keeps the target within `+0x118` of `+0x10C` (`SetLeash` 0x411360, dialogs); height eases
10 % to the terrain box; pitch / yaw / zoom speeds ease with `+0xC8 / +0xBC / +0xD4` (0.333) towards the inputs set by the
controllers (`SetRotateInput` 0x40F360, `SetPitchInput` 0x40F3C0, `SetZoomInput` 0x40F420, `SetScrollVelocity` 0x40F230); pitch
clamped to `CameraDefaultMinPitch..MaxPitch` (briefing pair with flag 0x200), distance between the hard limits with a spring
back to the soft limits (`1.1 * dt` of the excess); shake list `+0x108` (`AddShake` 0x4113A0: within 20 m of the source, `amp *
exp(-f t) * cos(g_0x79B2A4 * 0.2667 t)` added to z for `strength * 30` ticks). The memory-scanned addresses in
`game-memory.md` are these fields: `0x79B2C4 = +0x1C`, `0x79B2DC = +0x34`, `0x79B388 = +0xE0`, `0x79B39C / 0x79B3A8 = +0xF4 / +0x100`
(the yaw quaternion's z and w = sin / cos of the half angle).

Other entry points: `InitFromSettings` 0x40D0A0, `FollowUnitId` 0x40D420 (-1 stops; um-engine um_rig), `SetTarget` 0x40F5C0
(terrain height re-sampled over `CameraTerrainSensetiveArea`), `SetYaw` 0x4109E0, `LookFromTo` 0x411060 (script camera:
flag 0x40 free, `SetDirection` 0x4110D0 from a direction vector), `SetFreeFlag` 0x410E50, `SaveView / LoadView` 0x410860 /
0x4108D0 (8 presets `+0x120`, the first 4 persisted as `CameraShortcut_*`), `ApplyPreset` 0x411470, `Read / Write` 0x410AB0 /
0x410C10 (save games; the carrier is forced to 4 = the hero on load), `GetNearFarFov` 0x40D380 (`+0x2E0..+0x2E8`, the
projection), `GetGroundPoint` 0x40E810 (view ray with z = 0, `CWorldClient::Render`).

`.cam` files (`<resources>\camera\<name>`, `LoadCamFile` 0x40ECD0): `size / 0x24` keyframes `{float time, float pos[3], float
quat[4]}` into `+0x2F0`; `cutIndex > 1` cuts the file at the first gap > 500 time units (the demo cuts). `PlayCamFile(time)`
0x40EFB0: segment search from `+0x304`, lerp position / slerp quaternion, flag 0x20 cleared after the last key.
`StartRecording` 0x40E910 renames `camera\camera.cam` to `camera%.4d.cam` and opens a new one (flag 0x10; `RecordKeyframe`
0x40EB60 every frame from `main`). `SampleSmoothHeight` 0x40DB50 (bicubic B-spline terrain height, `+0x208..+0x2DC`) is dead code.

## 4. The screen manager `CInterface` (`g_0x7C7D50`, 0xB0 bytes; 49 fields in `batch-ui-structs.tsv`)

### 4.1 Lifetime [V]

`Init` 0x5F4990 (device): screen size `+0 / +4`, fonts `+0x70..`, keyboard layout (`Keyboard_Layout` HKL or US), `KeyBindings_Load`,
console `+0x14` (`g_0x7C7D64`), `DebugText::Init(g_0x79B930)`, message log `+0x1C`, fade `+0x18`, tutorial pages from
`config\tutorial.ini` into the map `+0x98`, `+0x30 = 1` (drawing enabled). `Destroy` 0x5F4F90 pops everything, writes the
tutorial pages, saves the key bindings. `ReadSaveState` 0x5F8420 / `WriteSaveState` 0x5F85C0 persist `+0x89..+0x94`, the queued
tutorial name `+0xA4` / page `+0xA8` and `+0xAC` (the third dot-part of the last `OnNamedValue` name with value 1.0) in the save
file (versions 0x47 / 0x56 / 0x5D / 0x5F gates); `ResetSaveState` 0x5F51F0 zeroes them for a new game.

### 4.2 Screen ids - `CreateScreenById` 0x5F5BA0 (called by `CPlayerClient::OnMsg07_GameMode`: the **server** picks the screen) [V]

| id | Class (vtable / sub-object vtable, size) | Role |
|---|---|---|
| 0 | - | nothing |
| 1 | `CUIScreen740764` (0x740764 / 0x740730, 0x230) | battle HUD (selection array, orders; `CamCtlFree` + order input in `Build`) |
| 2 | `CMainMenu` (0x740688 / 0x740654, 0x17C) | main menu |
| 3 | `DialogScreen` (0x7405AC / 0x740578, 0x2A4) | dialogue (speakers, topics, `DialogCamera`) |
| 4 | `CUIScreen740B3C` (0x740B3C / 0x740B08, 0x294) | menu background host: save / load slots (3 string-pair arrays +0x18C / +0x1A0 / +0x1B4), world map |
| 5 | `CCampScreen` (0x74091C / 0x7408E8, 0x260; `CCampScreen(this, 0)`) | camp (tabs weapons / skills / spells / items / constructors) |
| 7 | `CUIScreen740840` (0x740840 / 0x74080C, 0x168) | world view: free camera + `UIOrderClickHandler` (debug / spectator) |
| 8 | `CCreditsScreen` (0x740C3C / 0x740C08, 0x1FC; resource "Crdt") | credits (Bink) |
| 9 | `CUIScreen740D18` (0x740D18 / 0x740CE4, 0x1B8) | item exchange / trade |
| other | taken as a `CUIWindow*` | |

Each is constructed with `CInterface` as the argument and handed to `ReplaceScreens`. Screens pushed on top (not by id):
`CMessageBox` 0x634650, `CTutorialScreen` (0x740E28, 0x194), `CEscMenu`, options, save / load, lobby, `CDemoScreen` (0x73F7F8,
`SetPendingScreen` 0x5F5B60) - see the screens page.

### 4.3 Stack operations [V]

- `ReplaceScreens(screen)` 0x5F71C0: drops transition surfaces, pops every screen, `ResetInputState`, pushes the one screen
  ("Cannot activate NULL interface screen"), sets the UI render states (alpha blend on, alpha test on with ref 0x80, src
  `SRCALPHA`, dest `INVSRCALPHA`), `g_0x784750 = 1` (rendering allowed), `vt[25] OnOpen`, `App_PumpMessages`, capture handler = 0,
  cursor frame 0.
- `PushScreen(screen, fade, keepWorld)` 0x5F7350 (34 callers): cursor 3 (wait); `fade && !keepWorld` -> transition 3 + one
  `App_RenderFrame(0, 1)` (parks the current screenshot); the first screen pushed over the world saves the server pause state in
  `+0x60` and pauses the server (not in a lobby); `fade && keepWorld` -> transition 1 (greyed frozen background); previous top
  gets sub `Deactivate` and `vt[19] Activate(0)` (its blit surfaces are suspended to the heap); push; render states; `OnOpen`.
- `RequestPopScreen(code)` 0x5F74E0 (42 callers): `+0x4C = 1`, `+0x48 = code`; the pop happens in `ProcessPendingScreens`:
  `Deactivate`, `vt[26] OnClose`, delete, transition 4 restores the parked screenshot, the last screen left un-pauses the server
  (`Game_SetServerPaused(+0x60)`), the new top gets `Activate(1)` and `vt[27] OnChildClosed(code)` (0x990001 = disconnect confirmed
  -> close-all + lobby release).
- `ProcessPendingScreens` 0x5F57C0 (once per frame) also: autosave request `+0x81` -> top `vt[35] TryAutosave` until it returns 1;
  close-all `+0x44` (`FlushCloseRequest` 0x5F5AB0 runs it before / after every input message too: `g_0x784750 = 0`, nothing is
  rendered until the next push); pending screen `+0x40` -> `ReplaceScreens`; cursor copy `+0x68 / +0x6C`; disconnect flag `+0x88`
  in a lobby -> `CMessageBox("disconnect", "disconnect_msg", 0x990000)`; else top `vt[24] Tick` and the message log `Tick`.
- `CloseAllScreens` 0x5F74F0, `GetTopScreen` 0x5F7590, `SetPendingScreen` 0x5F5B60.

### 4.4 Draw order per frame [V]

1. `Render3D` 0x5F5220 (inside `BeginFrame / EndFrame`): transition states 2 / 3 / 4 blit the frozen screenshot `+0x54`
   (`CUIBlitSurface::BlitToBack`) first; top screen `vt[20] Draw(renderGlobals)` - a screen's `Draw` typically renders the world
   (`CWorldClient::Render`) and then `CUIWindow::Draw` draws its quads and children; `ScreenFade::Draw` 0x5E69C0 (letterbox bars
   `+0x10`, fade-to-black value `+4`; `fadeout <ms>` console command / `StartFade` 0x5E6B90).
2. `Draw2D` 0x5F5320 (after `EndFrame`): `DebugText` lines; top screen `vt[22] BlitSurfaces` then `vt[23] UpdateHoverAndButtons`
   (tooltips); message bar `+0x64`; transition 1 / 3 grab the back buffer into a new 800x600 (400x300 when `g_0x784753`)
   `CUIBlitSurface` `+0x54` + a 256x192 copy `+0x5C` and grey it (`ToGrayscale` 0x5FE440); in a lobby with one screen the
   message log's lines; `DebugText::Tick`; FPS bar `+0x20` (every 400 ms, `ShowFpsBar` 0x5F5700); console `Blit`.
3. `CSoftCursor::Draw`, flip.

Only the top screen draws; screens below are frozen as the screenshot and their surfaces are parked (`Activate(0)`).

### 4.5 How scripts and the server drive the UI [V]

| Entry | From | Does |
|---|---|---|
| `HandleScreenCommand(line)` 0x5F8070 | `CPlayerClient::OnMsg13_ScreenCommand` 0x667E60 (server message 0x13 = script `BroadcastMsgString`) | first word upper-cased: `say` / `say_block <id> [textkey]` -> top `vt[32] OnSay(id, String_HashCI(key))`; `tutorial <name>` -> pushed on the queue `+0x84` (shown by `ShowQueuedTutorial` 0x5F8A80 when the HUD is idle); `briefing ...` -> `vt[34] OnBriefing`; `autosave` -> `+0x81`; `endofgame` -> `vt[31]` |
| `CreateScreenById` | `OnMsg07_GameMode` | the game mode = the screen |
| `OnNamedValue(name, value)` 0x5F7FC0 | `OnMsg81_NamedValue` / `OnMsg82` | top `vt[33]`; remembers the third dot-part in `+0xAC` when value == 1.0 |
| `OnItemsChanged` 0x5F7F30, `OnUnitAdded29 / 30` 0x5F7F60 / 80, `OnEndOfGame` 0x5F7FA0, `OnSay` 0x5F82A0, `OnLobbyListChanged` 0x5F82D0, `OnLobbyRef` 0x5F8310, `OnSaveToSlot` 0x5F86D0 (bottom screen `vt[40]`) | `CPlayerClient`, `LobbyClient`, `Session_SaveToSlot` | forwards to the top screen's slots 28..40 |
| `ShowTutorialForTopScreen(force)` 0x5F86F0 | the screens' `Build` / `OnOpen`, key 0x3B | `vt[41] GetTutorialName` -> page map `+0x98` (`config\tutorial.ini` `<screen> <page>`, -1 never shown): pushes `CTutorialScreen(flag 0, name, page)` with a fade when forced or `ShowTutorial` is on and the page is -1 |
| `ShowMessageBar(text)` 0x5F75B0 | loading / saving | synchronous: an 800x40 green centred text at y = 280 rendered once |
| `AddChatLine(text, colour)` 0x5F82F0 | `LobbyClient::OnAddressLine` (um-engine mp_ghost hooks this call at 0x41E656) | `CUIMessageLog::AddMessage` |
| `EnableChatInput` 0x5F7C60 / `ClearChatLines` 0x5F7C80 | Enter in the battle / camp / dialog / trade screens | lobby chat line |
| `ConsoleAddText` 0x5F7EB0 | `LogStream::sync` (every game log line) | `CUIConsole::AddText` |

### 4.6 The message log `CUIMessageLog` (`CInterface +0x1C`, vt 0x7400DC / 0x7400A8, `CUIWidget` + 0x10) [V]

`AddMessage(text, colour)` 0x5E2A30 wraps at 0x30C px (font 1, `'^N'` colour escapes kept per segment), keeps at most 12
lines (0x18-byte nodes `{text, colour codes, time, surface}`), each line a colour-keyed `CUIBlitSurface` 17 px high placed at
`(10, 10 + 16 i)` by `BlitSurfaces` 0x5E29E0; `Tick` 0x5E2990 drops lines older than 20 s. The chat input line (surface
`(10, 573)-(790, 590)`, text `+0x150`, caret `+0x154`, mode `+0x14C`): `OnKeyDown` 0x5E3230 (Enter sends a lobby chat message
through `LobbyClient`, Esc cancels), `OnChar` 0x5E3550 (200 chars max), `RedrawInput` 0x5E3700.

### 4.7 The console `CUIConsole` (`g_0x7C7D64`, 0x7C bytes; `Console_ExecuteLine` 0x5DC620) [V]

Toggled by scancode 0x29; default rect `(0, 0, 800, 150)` (`SetRect` 0x5E3E90 clamps to >= 76 x 69 px, `visibleLines = (h - 24)
/ 15 - 1`), Courier New 9 pt, history band `0x282828`, output lines grey `0xB4B4B4` bottom-up (`RenderHistory` 0x5E4800), input
band 31 px with `'> '` (`RedrawInput` 0x5E4A10), 30 typed commands (Up / Down), 0x100 output lines (`history <n>`), PgUp / PgDn
scroll, Ctrl+Home / End, Shift+arrows select, `game-memory.md`'s "+0x2C input length" is the selection anchor. Enter echoes the
line and runs `Console_ExecuteLine`: `@expr` -> `WorldScript(expr)`, `#expr` -> `ConsoleFloat`, `$expr` -> `ConsoleString`
(cheat mode only, `g_0x7C7B64`; never in a lobby); words upper-cased; the word XOR 0x66 compared with `g_0x740028`
("thingamabob") enables cheat mode ("Activated!").

| Table | Commands -> handler |
|---|---|
| always (`g_0x7C7B28`) | `console [left/right/lefttop/lt/righttop/rt/top/bottom/center/fullscreen]` -> `CInterface::ConsoleSetRect` 0x5F7ED0 (handler 0x5DDBE0); `lastfps` (0x5DEEE0); `exec` / `execute <file>` -> `Console_Execute` 0x5DEF50 (`CStdioFile` lines re-fed); `kick` (0x5E0590) / `ban` (0x5E06E0) / `rate <bytes/s>` (0x5E0990) / `localrate` (0x5E0A10) / `net time|server|client 0/1` (0x5DF9B0) -> the lobby server / client; `quit` / `exit` -> `App_PostQuit` 0x455F20 |
| cheat mode (`g_0x7C7B38`) | `help` (0x5DF5E0), `debuginfo [pos]` -> `DebugText_SetRect` 0x5F7F10 (0x5DD920), `history [n]` -> `ConsoleSetMaxLines` 0x5F7EF0 (0x5DDF80), `filter <none/ai/event/graphics/rpg/net/unitacks/all> [on/off]` (0x5DDFE0, the log subsystem mask), `show world_data|render_info|managers` (0x5DE460), `listvar [Nplayer] [file]` -> `Console_ListVar` 0x5DEA00, `loadvar` (0x5DEC90), `give [Nplayer] money|exp <n>` -> `Console_Give` 0x5DF0F0 (global map only), `fadeout <ms>` -> `Console_Fadeout` 0x5DF8E0 -> `ScreenFade::StartFade`, `join` (0x5DF3A0), `server` (0x5DF490), `memusage` (0x5DF5A0), `lootall` (0x5DF5B0), `disconnect` (0x5DFC10), `generateacks`, `days [ddmmyyyy] [DDMMYYYY]` (0x5E00D0), `partyresend` (0x5E0570), `fps` -> `CInterface::ShowFpsBar` (0x5E0830) |

The handler bodies 0x5DD920-0x5E0A10 were not decompiled as functions in this build's listing (undefined code); the names above
come from the strings they use and from `Console_*` where Ghidra did define them.

## 5. The widget framework (0x5FC000-0x602000)

### 5.1 `CUIWidget` (0x148 bytes, vt 0x740F60, ctor 0x5FE700) and `CUIWindow` (0x164, vt 0x741008, ctor 0x600B20) [V]

A `CObject3D` (position `+0x1C`, quaternions `+0x34 / +0x44`, colour from `g_0x7C7E58`) with: `+0x94` the `IUIEvents` sub-object
vtable (events are delivered to `this + 0x94`), `+0x98` `CInterface`, `+0x9C` handled byte, `+0xD8` rect (UI units), `+0xE8`
enabled, `+0xEC` black / `+0xF0` white `CMaterial` (kind 0x16; every quad uses `+0xEC`), and five `CArray`s `{data, count, cap,
grow}`: `+0xF8` textures (`AddTextureByName` 0x5FEF20: `CTextureManager::FindOrCreateNamed`; `AddRenderTargetTexture` 0x5FF0B0 for
the unit previews; textures[0] is bound to every new quad), `+0x108` quad figures (`AddQuadMesh(name)` 0x5FF3C0: a stand-alone
`.fig` from `CFigureManager`, material `+0x27C` = widget `+0xEC`, texture `+0x280`; 260 callers), `+0x118` blit surfaces
(`AddBlitSurface` 0x5FFA90), `+0x128` buttons (`AddButton` 0x5FF830, 0x34 bytes each), `+0x138` figure holders (`AddFigureHolder`
0x5FFD80). `CUIWindow` adds `+0x148` child windows (`AddChildWindow` 0x600D30, drawn / ticked / activated with the parent when
enabled) and `+0x158` the `std::vector<IUIEvents*>` of event handlers (`AddEventHandler` 0x600CC0; owned). `SetEnabled(on,
activate)` 0x6002A0; `Activate(on)` 0x6002C0 suspends / restores the blit surfaces.

### 5.2 Vtable slot conventions [V]

`CUIWidget` primary (25 slots): 0 deleting dtor, 1..17 `CObject3D`, **18 Build** (empty; `UIFrameWidget::BuildFrameMesh`),
**19 Activate(on)**, **20 Draw(renderGlobals)** (3D pass: quads + figure holders), **21 UpdateHover** (tooltip states from the
cursor), **22 BlitSurfaces** (2D pass), **23 UpdateHoverAndButtons** (hover + tooltip blits), **24 Tick** (per frame logic).
`CUIWindow` (42 slots) recurses 19..24 into the enabled children and adds: **25 OnOpen** (pure; called by Push / Replace),
**26 OnClose** (pure; before delete), 27 `OnChildClosed(code)`, 28 `OnItemsChanged(a, b, c)`, 29 / 30 `OnUnitAdded`, 31
`OnEndOfGame`, 32 `OnSay(id, hash, 0)`, 33 `OnNamedValue(name, value)`, 34 `OnBriefing`, 35 `TryAutosave -> bool`, 36
`OnLobbyListChanged`, 37 `OnLobbyRef`, 38 / 39 screen-specific setters (`SetField22C` / `SetField2A0`), 40 `OnSaveToSlot`, 41
`GetTutorialName -> char*` (defaults 0x5795A0..0x579680: empty / 0). The 55-slot screens (0x74091C, 0x740A2C, 0x740D18,
0x74214C) add 42..54 (45 `GetHelpRect`, 50 `GetHeroIdAt`, 53 returns 0.0; the rest empty defaults 0x5F6370..).
`IUIEvents` sub-object (13 slots, base 0x7401D0; defaults 0x5E2800..0x5E28B0, 0x5EA800 / 0x5EA840): 0 deleting dtor thunk
(`this - 0x94`), **1 Deactivate**, **2 OnLButtonDown(keys, x, y)**, 3 OnLButtonUp, 4 OnMouseMove, 5 OnRButtonDown, 6
OnRButtonUp, 7 OnLButtonDblClk (default -> 2), 8 OnRButtonDblClk (default -> 5), **9 OnKeyDown(vk, repeat, flags)**, 10
OnChar, 11 OnKeyUp, 12 OnMouseWheel(keys, delta, x, y) -> int. The earlier "18 Build?, 24 Update, 27 OnCommand" guesses in
`findings.md` are these.

### 5.3 Event dispatch (`CUIWindow::Dispatch_*`, sub-object slots 1..12, 0x601110..0x601A60) [V]

`this->handled (+0x9C) = 0`; for every handler of `+0x158`: its handled byte (`+8`) cleared, the slot called, stop when it set
it; then for every **enabled** child window (mouse events: only those whose rect contains the point, `PointInside` 0x5FFD30 with
the point unscaled to UI units; key events: all) its sub-object slot; the first child that reports handled ends the loop and
the flag propagates up. `Deactivate` (slot 1) goes to every handler and child. A screen's own `OnLButtonDown` usually first
calls the dispatch, then tests its buttons with `ButtonAtPoint(x, y)` 0x5FFCC0 (`UIButton::HitTest` 0x5FC8F0: rect or polygon
in UI units, origin `+0x28/+0x2C` subtracted for scrolled lists) and plays `UI_PlaySound` 0x5F7E60 (`buttons\*.wav`).

### 5.4 The draw path [V]

- 3D pass (`CUIWindow::Draw` 0x600EF0, only when `CInterface +0x30`): render target = the renderer's current, `CI3DFigure::Draw`
  0x5E6D90 for every quad of `+0x108` (material `+0x27C`, texture `+0x280`, constant diffuse `0xFF000000`, scale matrix `+0x23C`
  on top of the model matrix), `CI3DFigureHolder::RenderAnimated` 0x5FC2F0 for `+0x138` (animated with `timeGetTime / 67`), then
  the enabled children. `CUIWindow::Update` 0x600F70 wraps the hover pass with alpha-ref 1.
- 2D pass: `CUIWidget::BlitSurfaces` 0x5FECC0 -> `CUIBlitSurface::Blt` 0x5FDA80 for every visible surface (`DDBLT_WAIT`, or
  `DDBLT_KEYSRCOVERRIDE` with `+0x34` when flag 1), clipped to the screen.
- Text: `CUIBlitSurface::DrawText(text, rect UI units or empty, DT flags, colour BGR, shadow, font 0..2)` 0x5FDE20 (GDI
  `DrawText` on the surface's DC, transparent background, black shadow at (1, 1) first; 253 callers), `DrawWrappedText(text,
  rect, maxLines, flags, colour, lineHeight, shadow)` 0x5FDFA0 (word wrap by `CInterface::MeasureText`; returns the height),
  `DrawFrameRect` 0x5FDCB0, `FillColor` 0x5FE370 / `FillBackground` 0x5FE2F0, `GetPixel` 0x5FD240 (region picking on the world map),
  `GetDC / ReleaseDC` 0x5FDC00 / 0x5FDC60 (under the DirectDraw critical section; "Can't get surface's DC.").

### 5.5 `UIQuadMesh` - the quad builders (a `CI3DFigure`; own vertices `+0x1C0` SoA blocks of 4, normals `+0x1D0`, `{vertex, 0, uv}` triplets `+0x1E0`, indices `+0x200`, uvs `+0x210`, scale `+0x23C`) [V]

| Builder | Arguments | Geometry |
|---|---|---|
| `InitRect` 0x5E70E0 (202 callers) | z, scale, x, y, x2, y2, u0, v0, u1, v1 | 4 vertices +-(w / 2) / scale around the centre, 2 triangles, uv rect; `u1 < u0` mirrors (both shifted by -1) |
| `InitGrid` 0x5E7680 | + cols, rows | `(cols + 1)(rows + 1)` vertices from the **top-left** corner, the same uv rect on every cell (tiled) |
| `InitBorderStrips` 0x5E7D60 (was InitFrame16) | z, scale, l, t, r, b, border, uv | 4 strips between the outer rect and the rect inset by `border`, each with the whole uv rect |
| `InitBorderRing` 0x5E8450 (was InitFrame8) | same | 8 vertices, 8 triangles; inner uv inset by `border * (u1 - u0) / (r - l)` |
| `InitDisc` 0x5E8C60 (was InitN) | z, scale, cx, cy, radius, n, uv | n points on a circle, fan `(0, i + 1, i)` |
| `InitRing` 0x5E91F0 (was InitStrip) | + thickness | n quads between `radius` and `radius - thickness` |
| `InitQuadUV` 0x5E99D0 | corners + 3 uv pairs | one quad with explicit per-corner uv (rotated / mirrored art): `(z, scale, x0, y0, x1, y1, x2, y2, u0, v0, u1, v1, u2, v2)`, 4th uv (0, 0) (section 6) [V] |
| helpers | `SetUniformScale` 0x5E7060, `SetScaleY` 0x5E7090, `BindOwnArrays` 0x5EA150, `CloneQuadMesh` 0x5FF460 | |

Every builder ends with the 1/512 uv inset and `BindOwnArrays` (own arrays -> draw pointers `+0x9C..+0xC8`, colour arrays sized).

### 5.6 `CUIBlitSurface` (0x38 bytes, vt 0x740F28) [V]

`CUIBlitSurface(ui, l, t, r, b, flags, scale)` 0x5FCC00 (49 callers), `_rect` 0x5FCDD0, `_texture(name)` 0x5FCF80 (picture
backgrounds): a **system-memory** `CDXDirectDrawSurface` of the rect's pixel size (`+0x10`; cleared with `DDBLT_COLORFILL` to
`+0x34`), rect `+0x24` in screen pixels (scaled from UI units), flags `+0xC` 1 colour-keyed, 2 no source rect, 4 visible.
`SuspendOrRestore` 0x5FD7A0 copies the pixels to a heap backup `+0x1C` and releases the DirectDraw surface (counters
`g_0x7C7E74` bytes stored / `g_0x7C7E78` surfaces) - every screen below the top one lives like that. `SetVisible` 0x5FD760,
`MoveTo` 0x5FE400, `SetRect` 0x5FD3A0, `StretchTo(w, h)` 0x5FD440, `BlitToBack` 0x5FD3D0, `ToGrayscale` 0x5FE440.

### 5.7 Buttons and tooltips [V]

`UIButton` (0x34): `SetRect(l, t, r, b)` 0x5FC9E0 (157 callers) or a polygon (`AddPoint` 0x5FCA70, enabled from 3 points), `Clear`
0x5FCA40, origin `+0x28 / +0x2C`, tooltip `+0x30` = a `CTooltipControl` (0x184; `SetTooltipById(key)` 0x5FC730 reads `tip %d`
from the text manager, 95 callers; `SetTooltipText` 0x5FC690; state `+0x180` 0 idle / 1 hover begun / 3 left). `CUIWidget::
UpdateHover` 0x5FEBF0 feeds the cursor (`CInterface +0x68 / +0x6C`) to every button's tooltip each frame; the tooltip shows after
`ToolTip_Time`. `IsTooltipActive` 0x5FC660.

### 5.8 Figures, unit previews and portraits [V]

`CI3DFigureHolder` (4 bytes = one `CObject3D*`): `Load(model, textures)` 0x5FC1F0 creates a world object of kind 0x33 with flag
`0x4000000` (UI object) at (0, 0, 3), loads the model / materials / animation; `SetAnimationByName` 0x5FC3D0 / `ByIndex`
0x5FC3B0, `ShowPart` 0x5FC4C0 / `ShowAllParts` 0x5FC4F0 (flag 0x80 on the parts), `SetScreenPos2D` 0x5FC450, `RenderAnimated`
0x5FC2F0 (the camp / lobby heroes). `UIUnitPreview::Render` 0x5EC300 draws a unit into a render-target texture
(`AddRenderTargetTexture`) for the inventory / trade screens; `GetOrCreatePortrait` 0x5ECA10 keeps a `std::map<name, {time,
figure}>` at screen `+0x170` trimmed by `TrimPortraitCache` 0x5F2F20; `PickFaceFigure` 0x5ED7B0 picks `infa%sme%dface` /
`face%s%02d` by prototype; `LoadPictureTo256Target` 0x5EC020 decodes a `.mmp` into a 256x256 A1R5G5B5 render target and
`CUIWidget::BlitPictureIntoTexture` 0x5FFFB0 writes one into a region of an existing texture (slot art, speaker portraits).

### 5.9 The item / spell / perk info panels (`UIItemInfoWin`, 0x5EE440-0x5F2A00) [V]

`Create(x, y)` 0x5EE440: a 200x300 colour-keyed surface, 45 `infoitem_%d` labels (`+0x1C8`) and 25 `infoskill_%d` labels
(`+0x1DC`). The item shown is the only entry of the display `CParty` at `+0x150` (`SetItem` 0x5EE6C0). `Refresh` 0x5EE760 picks the
panel by item kind (`+0x10`) / loot category (`+0x4C`) / subtype (`+0x50`): 0x3007 cat 1 -> `DrawMaterialInfo` 0x5EFD90 (record
`g_0x7C86D8[+0x54]`, `mattype_<NAME>` text, price / weight, resist lists over `+0x2C[7]`); cat 2 sub 0 / 1 / 2 ->
`DrawWeaponArmorInfo` 0x5F0370 (`WeaponProto g_0x7C86F8` 0xB4 / `ArmorProto g_0x7C8718` 200 / `g_0x7C8738` 0x8C; `damage_%d`
typed damage `%s %d-%d %s` from `+0x98[7]` for weapons, resist lists from `+0x70[7] * +0x6C * 10` for armor); sub 3 ->
`DrawSpellInfo` 0x5F0B90 (`SpellProto g_0x7C8510[+0x54]`: `school_%d`, mana / range / area / duration); sub 4 -> `DrawPerkInfo`
0x5F1130 (`g_0x7C8530[+0x54]`); 0x3006 sub 8 -> `DrawItemOrSpellInfo` 0x5EF300; 0x3008 or Ctrl on a spell -> `DrawSpellItemInfo`
0x5EF5D0 (`spell <code>` text section, rune `modifier` lines); else the generic panel (name, `matshort`, count, price, weight,
durability for 0x3004 / 0x3005, `loot_item` text). Layout: `DrawWrappedText` name in white at `(15, 10)-(185, 25)` (17 px lines),
then 15 px lines in `0xA7D7E4`; `FormatResistLists` 0x5F13A0 turns 7 floats into `{value, "dmg_%d" above, "dmg_%d" below}`.
`DrawPerkDetail(record)` 0x5F1590 is the skills window's 26-case detail text. Ctrl toggles the spell view
(`UIInventoryWin::OnKeyDown / OnKeyUp` 0x5EE640 / 0x5EE680 -> `Refresh`).

## 6. What is not known

- ~~InitQuadUV argument order~~ settled [V] (0x5E99D0): `(z, scale, x0, y0, x1, y1, x2, y2, u0, v0, u1, v1, u2, v2)`: corner 0 is
  the origin, corners 1 and 2 give the two edge vectors (`(x1 - x0) / scale`, `(x2 - x0) / scale`, stored at the mesh +0x1C0); the
  uv table (+0x210) gets `u / 256, 1 - v / 256` for corners 0..2 in the same order, and the 4th corner's uv is set to (0, 0), not
  derived; then every uv gets the half-texel inset (+1/512, -1/512).
- `InitBorderStrips`: the 3rd / 4th strips are read as left / right by symmetry (only the first two vertex blocks were
  decoded) [I].
- ~~Console handler bodies~~ settled [V] (2026-11-03): the 21 handlers 0x5DD920..0x5E0A10 are now Ghidra functions, named
  `Console_*` in functions.tsv (`Console_DebugInfo`, `Console_Console`, `Console_History`, `Console_LogMask`, `Console_Show`,
  `Console_LoadVar`, `Console_LastFps`, `Console_Join`, `Console_Host`, `Console_MemUsage_Nop` (empty in this build),
  `Console_LootAll`, `Console_Help`, `Console_Net`, `Console_Disconnect`, `Console_Days` (a date easter egg), `Console_PartyResend`,
  `Console_Kick` / `Console_Ban` (the top screen's slot 38 with 1 / 2), `Console_FpsBar`, `Console_Rate`, `Console_LocalRate`);
  the argument syntax above matches their bodies.
- `CamCtlClassic` and `CamCtlSimple`: no creator, their vtables are referenced only by their own methods [V]: dead code, the
  live game uses only `CamCtlFree` [untestable: no path reaches them].
- ~~GetGroundPoint / ShowPart / ItemLink::Clone fall-throughs~~ settled [V] (2026-11-03): the typed re-exports have proper returns
  and `ShowPart` 0x5FC4C0 / `ItemLink::Clone` 0x5ED710 match their machine code exactly. `GetGroundPoint` 0x40E810 = the camera's
  forward ray hits height 0 at `t = -z / dirZ` (if |dirZ| > 1e-5 and t > 0), else the camera position. `ShowPart`: show clears
  part flag 0x80, hide sets it. `Clone` writes the refcount before its null check (a harmless quirk of the original).
- ~~Static initialiser globals~~ settled [V] (generic sweep 2026-11-01): 0x5EA7C0 / 0x5EBF60.. / 0x5F4660 / 0x5FC1A0 / 0x5FC5B0
  fill per-TU math-header constants (g_0x7C7CF8 / 7C7D28 / 7C7E00 / 7C7E28 / 7C7E50 = 1.0f, g_0x7C7D20 = 2 pi, g_0x7C7D24 = pi/2),
  unreferenced; `g_0x7C7E58` is the default widget colour.
- The `MapZoneRecord` (0x5C) string fields beyond `+0x20 / +0x24` and the allod record's embedded `CObject` at `+0x18` [?].
- `CSoftCursorFrame::CreateFromMmp`: settled as far as needed [I]: the per-pixel copy loops are compiler-eliminated in the
  decompile; the surface comes from `CDXDirectDrawSurface::CreateFromPicture`.

## 7. Corrections to earlier notes

- `findings.md` 0x5FD000: "`+0xB4/+0xB8` enabled elements, `+0xC4..+0xC8` child windows, `+0xEC` layer, `+0xF8` element array
  (figures from the device)" - wrong: `+0xA0..+0xD4` are zero in the ctor, `+0xEC / +0xF0 / +0xF4` are materials, `+0xF8` is the
  **texture** array, `+0x148` the child windows, `+0x158` the handler vector; there is no layer field. "`DispatchSlotN` forward to
  children first, then to elements whose rect contains the point" - it is **handlers first, then children by rect**.
- `findings.md` slot guesses "18 Build?, 24 Update, 25 Build/OnOpen, 27 OnCommand": 18 is a build hook only `UIFrameWidget`
  uses, 24 is `Tick`, 25 `OnOpen`, 27 `OnChildClosed(result code)`; script commands go through `HandleScreenCommand` -> slots
  32 / 34 / 31, not 27.
- `findings.md` 0x5EE000: "`UIHitRegion`" = `UIButton` (the 0x34-byte record at `+0x128[]`); "`CUIBlitSurface` = widget
  background" = any GDI text / picture surface.
- `flows.md`: `DDraw_ProbeDevice` is `App_ProbeDirectX` 0x45E310 (the DX level, not a device probe); `CSoftCursor::Reset`
  0x5E6260 is `Start` (it starts the cursor thread); `CInterface::Render3D` 0x5F5220 is `Render3D`; `CCamera::ClampPitch`
  0x40D710 is `LevelRoll` (it removes roll; the pitch clamp is in `Update`); `CCamera::SaveKeyframe` = `RecordKeyframe`,
  `UpdateFlyPath` = `PlayCamFile`; `CSoftCursor::AddFrame` has one caller family (`Cursors_Register`), not 49.
- `game-memory.md` console: `+0x2C` is the selection anchor, not the input length; `+0x04` is the surface pointer (set while
  open) - consistent. Camera: the four scanned addresses are `CCamera` fields (3.6).
- `game-exe-analysis.md` console: `GodMode`, `KillUnit`, `Teleport`, `KillScript`, `give items / quests` are **script** functions
  reachable through `@expr`, not console commands (the command tables are listed in 4.7); the console key is scancode 0x29.
- `functions.tsv` names fixed here: `CCamera::GetPositionXYZ` -> `GetNearFarFov`; `CCamera::FreeLists` -> a deque dtor;
  `TextureDecode1 / 2` -> picture-to-surface loaders; `UIQuadMesh::InitN / InitStrip / InitFrame8 / InitFrame16` ->
  `InitDisc / InitRing / InitBorderRing / InitBorderStrips`; `Array84_GetAt / Array36_GetAt` -> `SpellProtoTable_GetAt /
  PerkProtoTable_GetAt`; `UIHandler7403F8` -> `ItemLink`; `MapData / Record2C / 5C / 3C` -> `MapInfo / MapAllodRecord /
  MapZoneRecord / MapExitRecord`; `StringFloatMap_*` -> `NamedFloatMap_*`.

## 8. Function index (one line each; the full purposes are in `batch-ui.tsv`)

Application: `main` 0x456440 WinMain; `App_MessageBox` 0x4560D0; `App_LoadMapAndScript` 0x456140; `App_SetQuitAllowed` 0x455F10;
`App_PostQuit` 0x455F20; `App_HandleInputMessage` 0x457970; `App_WndProc` 0x457D40; `App_RenderFrame` 0x458010; `App_PumpMessages`
0x458220; `Movies_PlayIniList` 0x458270; `FactoryMap_Register` 0x458570; `NamedParty_New` 0x457740; `CGSPlayer_New` 0x457850;
`AppSettings::AppSettings` 0x458870 / `SetDefaults` 0x458DA0 / `ReadFromIni` 0x459100 / `WriteToIni` 0x45A380 / `ReadFromRegistry`
0x45B210 / `WriteToRegistry` 0x45C560 / `~AppSettings` 0x458730; `App_ProbeDirectX` 0x45E310; `MapInfo_Load` 0x45EA20 /
`LoadBoth` 0x45EA00 / `dtor` 0x45E6F0; `NamedFloatMap_Get / Set / Remove / Read` 0x460AF0 / 0x460BC0 / 0x460DB0 / 0x460FE0;
`IsHeroId` 0x4613A0; `Net_TickAll` 0x4122D0; `GSWorld::GetLastError` 0x456360.

Key bindings: `KeyBindings_Load` 0x462C10, `Save` 0x462DF0, `FindKeyByAction` 0x462F70; `DInput_Create` 0x65EAD0, `DInputMouse::
Acquire` 0x65EC70, `GetState` 0x65ECB0.

Camera: `StaticInit_CCamera` 0x40C880; `CCamera::CCamera` 0x40C8B0; `dtorBody` 0x40CE60; `Init` 0x40D190; `Done` 0x40D1D0;
`InitFromSettings` 0x40D0A0; `GetNearFarFov` 0x40D380; `FollowObject` 0x40D3B0; `FollowUnitId` 0x40D420; `TurnTowardsCarrier`
0x40D470; `LevelRoll` 0x40D710; `Terrain_SampleHeightBox` 0x40D9B0; `SampleSmoothHeight` 0x40DB50; `CubicBSpline_SetCoeffs`
0x40E3D0; `Poly3_Eval` 0x40E440; `CubicBSpline_Eval3` 0x40E460; `AvoidObstacles` 0x40E4C0; `GetGroundPoint` 0x40E810;
`StartRecording` 0x40E910; `StopRecording` 0x40ECB0; `RecordKeyframe` 0x40EB60; `LoadCamFile` 0x40ECD0; `PlayCamFile` 0x40EFB0;
`SeekCamFileStart` 0x40F1B0; `SetScrollVelocity` 0x40F230; `SetRotateInput` 0x40F360; `SetPitchInput` 0x40F3C0; `SetZoomInput`
0x40F420; `StopMotion` 0x40F430; `SetTarget` 0x40F5C0; `MoveTarget3` 0x40F640; `MoveTarget` 0x40F7B0; `UpdateFromTarget` 0x40F8B0;
`Update` 0x40FB30; `OnWheelZoom` 0x4107A0; `SaveView` 0x410860; `LoadView` 0x4108D0; `SetYaw` 0x4109E0; `Read` 0x410AB0; `Write`
0x410C10; `LoadPresetsFromSettings` 0x410D10; `StorePresetsToSettings` 0x410D70; `SetFreeFlag` 0x410E50; `LookFromTo` 0x411060;
`SetDirection` 0x4110D0; `SetLeash` 0x411360; `AddShake` 0x4113A0; `ApplyPreset` 0x411470.

Message log / console / fade / cursor: `IUIEvents::*_Nop` 0x5E2800..0x5E28B0, 0x5EA800, 0x5EA840; `CUIMessageLog::CUIMessageLog`
0x5E26F0, `~body` 0x5E28E0, `Tick` 0x5E2990, `BlitSurfaces` 0x5E29E0, `AddMessage` 0x5E2A30, `OnKeyDown` 0x5E3230, `OnChar`
0x5E3550, `SetInputMode` 0x5E36C0, `RedrawInput` 0x5E3700; `CUIConsole::CUIConsole` 0x5E3CE0, `~body` 0x5E3DF0, `SetRect` 0x5E3E90,
`SetMaxLines` 0x5E3F90, `Blit` 0x5E3FC0, `SetVisible` 0x5E3FD0, `AddText` 0x5E4090, `OnKeyDown` 0x5E4180, `OnChar` 0x5E44A0, `OnKeyUp`
0x5E47E0, `RenderHistory` 0x5E4800, `RedrawInput` 0x5E4A10; `Console_ExecuteLine` 0x5DC620, `Console_Execute` 0x5DEF50, `ListVar`
0x5DEA00, `Give` 0x5DF0F0, `Fadeout` 0x5DF8E0; `StaticInit_CSoftCursor` 0x5E4DD0; `CSoftCursor::Destroy` 0x5E4E00, `CSoftCursorFrame::
CreateFromMmp` 0x5E4EB0, `Destroy` 0x5E5160, `CSoftCursor::CSoftCursor` 0x5E51C0, `SetFrame` 0x5E5200, `SetMode` 0x5E5270,
`AccumulateMouseDelta` 0x5E52B0, `UpdateState` 0x5E52E0, `Draw` 0x5E54A0, `DDrawSurface_Blt` 0x5E5640, `ThreadProc` 0x5E5690,
`ComposeFrame` 0x5E5C40, `Init` 0x5E60B0, `Start` 0x5E6260, `Stop` 0x5E62E0, `AddFrame` 0x5E63F0, `Lock` 0x5E65F0, `Unlock` 0x5E6610,
`RestoreBackground` 0x5E6630, `SetPos` 0x5E6750, `SetSoftwareMode` 0x5E67B0, `FrameArray_SetSize` 0x5E6810; `Cursors_Register`
0x5DAEA0, `Cursors_StopAndRegister` 0x5DAE70; `ScreenFade::ScreenFade` 0x5E6990, `Draw` 0x5E69C0, `StartFade` 0x5E6B90.

Quads / controllers / previews: `CObject3D::SetScreenPos2D` 0x5E6FA0, `GetScreenPos2D` 0x5E7010; `UIQuadMesh::SetUniformScale`
0x5E7060, `SetScaleY` 0x5E7090, `GetScaleX` 0x5E70B0, `InitRect` 0x5E70E0, `InitGrid` 0x5E7680, `InitBorderStrips` 0x5E7D60,
`InitBorderRing` 0x5E8450, `InitDisc` 0x5E8C60, `InitRing` 0x5E91F0, `InitQuadUV` 0x5E99D0, `ResetVertices` 0x5E9F30, `BindOwnArrays`
0x5EA150; `SoABlockArray_SetSize` 0x5EA210, `UvArray_SetSize` 0x5EA380, `IndexArray_SetSize` 0x5EA4C0, `VertexUvMapArray_SetSize`
0x5EA600; `CamCtlClassic::*` 0x5EA810..0x5EABD0; `CamCtlFree::*` 0x5EACB0..0x5EB580; `CamCtlSimple::*` 0x5EB600..0x5EB790;
`UIKeyHandler741154` 0x5EB8A0 / 0x5EB8B0; `UIOrderClickHandler::*` 0x5EB8C0..0x5EBBC0; `IdArray::*` 0x5EBBE0..0x5EBDB0;
`UIUnitPreview::GetDefaultQuad` 0x5EBFB0, `LoadPictureTo256Target` 0x5EC020, `Render` 0x5EC300, `GetOrCreatePortrait` 0x5ECA10,
`PickFaceFigure` 0x5ED7B0, `TrimPortraitCache` 0x5F2F20, `TrimFaceCache` 0x5F2F90; `PortraitCache_FindOrInsert` 0x5F3000,
`FaceCache_FindOrInsert` 0x5F3170; `CampScreen::CreateFrameWidget` 0x5EDD80; `UIFrameWidget::BuildFrameMesh` 0x5F2A90; `UIBar::
Layout` 0x5F2E00; `UICampHelp::Draw` 0x5F2910; `CRect::CRect` 0x5EC9F0.

Item info: `Item_IsUsable` 0x5EE130; `Party_TransferItem` 0x5EE1B0; `Party_MoveAllItems` 0x5EE2C0; `ItemLink::Clone` 0x5ED710;
`UIItemSlot::OnDropThunk` 0x5EE310; `UIInventoryWin::~body` 0x5EE380, `Draw` 0x5EE5C0, `OnKeyDown` 0x5EE640, `OnKeyUp` 0x5EE680,
`SetItem` 0x5EE6C0, `GetSelectedItem` 0x5EE720; `UIItemInfoWin::Create` 0x5EE440, `Refresh` 0x5EE760, `DrawItemOrSpellInfo`
0x5EF300, `DrawSpellItemInfo` 0x5EF5D0, `DrawMaterialInfo` 0x5EFD90, `DrawWeaponArmorInfo` 0x5F0370, `DrawSpellInfo` 0x5F0B90,
`DrawPerkInfo` 0x5F1130, `FormatResistLists` 0x5F13A0, `DrawPerkDetail` 0x5F1590; `SpellProtoTable_GetAt` 0x5F2EA0,
`PerkProtoTable_GetAt` 0x5F2EE0.

CInterface: `StaticInit_CInterface` 0x5F4680; `CInterface::CInterface` 0x5F46A0, `~CInterface` 0x5F4790, `Init` 0x5F4990, `Destroy`
0x5F4F90, `ResetSaveState` 0x5F51F0, `Render3D` 0x5F5220, `Draw2D` 0x5F5320, `ShowFpsBar` 0x5F5700, `ProcessPendingScreens` 0x5F57C0,
`FlushCloseRequest` 0x5F5AB0, `SetPendingScreen` 0x5F5B60, `CreateScreenById` 0x5F5BA0, `ReplaceScreens` 0x5F71C0, `PushScreen`
0x5F7350, `RequestPopScreen` 0x5F74E0, `CloseAllScreens` 0x5F74F0, `GetTopScreen` 0x5F7590, `ShowMessageBar` 0x5F75B0, `OnLButtonDown`
0x5F76A0, `OnLButtonUp` 0x5F76E0, `OnMouseMove` 0x5F7740, `OnRButtonDown` 0x5F7780, `OnRButtonUp` 0x5F77C0, `OnLButtonDblClk`
0x5F7800, `OnRButtonDblClk` 0x5F7840, `OnMouseWheel` 0x5F7880, `OnKeyDown` 0x5F78C0, `OnChar` 0x5F7B40, `OnKeyUp` 0x5F7BE0,
`EnableChatInput` 0x5F7C60, `ClearChatLines` 0x5F7C80, `UiScale::ScaleRect` 0x5F7CA0, `ScalePoint` 0x5F7D20, `UnscaleRect` 0x5F7DA0,
`UnscalePoint` 0x5F7E00 / 0x5F7E30, `UI_PlaySound` 0x5F7E60, `UI_StopSound` 0x5F7E90, `ConsoleAddText` 0x5F7EB0, `ConsoleSetRect`
0x5F7ED0, `ConsoleSetMaxLines` 0x5F7EF0, `DebugText_SetRect` 0x5F7F10, `OnItemsChanged` 0x5F7F30, `OnUnitAdded29 / 30` 0x5F7F60 /
0x5F7F80, `OnEndOfGame` 0x5F7FA0, `OnNamedValue` 0x5F7FC0, `HandleScreenCommand` 0x5F8070, `OnSay` 0x5F82A0, `OnLobbyListChanged`
0x5F82D0, `AddChatLine` 0x5F82F0, `OnLobbyRef` 0x5F8310, `MeasureText` 0x5F8340, `ReadSaveState` 0x5F8420, `WriteSaveState`
0x5F85C0, `OnSaveToSlot` 0x5F86D0, `ShowTutorialForTopScreen` 0x5F86F0, `ShowQueuedTutorial` 0x5F8A80, `ResetInputState` 0x5F8C50;
`Lobby_FormatSystemLine` 0x5F8C90; screen ctors / dtors 0x5F5F30..0x5F7010 and the sub-object thunks 0x5FC0B0..0x5FC140.

Widgets: `CI3DFigureHolder::*` 0x5FC1C0..0x5FC4F0; `UIButton::Destroy` 0x5FC600, `IsTooltipActive` 0x5FC660, `SetTooltipText`
0x5FC690, `CreateTooltip` 0x5FC6B0, `SetTooltipById` 0x5FC730, `OnMouseMove` 0x5FC880, `HitTest` 0x5FC8F0, `SetRect` 0x5FC9E0,
`Clear` 0x5FCA40, `AddPoint` 0x5FCA70; `CUIBlitSurface::*` 0x5FCC00..0x5FE440 (ctor x3, `~body` 0x5FD120, `GetPixel` 0x5FD240,
`SetRect` 0x5FD3A0, `BlitToBack` 0x5FD3D0, `StretchTo` 0x5FD440, `SetVisible` 0x5FD760, `SuspendOrRestore` 0x5FD7A0, `Blt` 0x5FDA80,
`GetDC` 0x5FDC00, `ReleaseDC` 0x5FDC60, `DrawFrameRect` 0x5FDCB0, `DrawText` 0x5FDE20, `DrawWrappedText` 0x5FDFA0, `FillBackground`
0x5FE2F0, `FillColor` 0x5FE370, `MoveTo` 0x5FE400, `ToGrayscale` 0x5FE440); `CUIWidget::CUIWidget` 0x5FE700, `~body` 0x5FE930,
`Draw` 0x5FEB90, `UpdateHover` 0x5FEBF0, `BlitSurfaces` 0x5FECC0, `UpdateHoverAndButtons` 0x5FED00, `AddTexture` 0x5FEDD0,
`AddTextureByName` 0x5FEF20, `AddRenderTargetTexture` 0x5FF0B0, `RemoveTexture[At]` 0x5FF110 / 0x5FF180, `ClearTextures` 0x5FF1E0,
`AddQuadMesh` 0x5FF3C0, `CloneQuadMesh` 0x5FF460, `RemoveQuadMesh[At]` 0x5FF700 / 0x5FF770, `ClearQuadMeshes` 0x5FF7D0, `AddButton`
0x5FF830, `RemoveButtonAt` 0x5FF9C0, `ClearButtons` 0x5FFA20, `AddBlitSurface` 0x5FFA90, `RemoveBlitSurfaceAt` 0x5FFBF0,
`ClearBlitSurfaces` 0x5FFC50, `ButtonAtPoint` 0x5FFCC0, `PointInside` 0x5FFD30, `AddFigureHolder` 0x5FFD80, `ClearFigureHolders`
0x5FFF40, `BlitPictureIntoTexture` 0x5FFFB0, `SetEnabled` 0x6002A0, `Activate` 0x6002C0, `Time_GetSeconds` 0x600270; `CUIWindow::
CUIWindow` 0x600B20, `~body` 0x600BA0, `AddEventHandler` 0x600CC0, `DeleteEventHandlers` 0x600D00, `AddChildWindow` 0x600D30,
`DeleteChildWindows` 0x600E90, `Draw` 0x600EF0, `Update` 0x600F70, `BlitSurfaces` 0x601040, `UpdateHoverAndButtons` 0x601090, `Tick`
0x6010E0, `Dispatch_OnLButtonDown` 0x601110, `_OnLButtonUp` 0x6011F0, `_OnMouseMove` 0x6012D0, `_OnRButtonDown` 0x6013B0,
`_OnRButtonUp` 0x601490, `_OnLButtonDblClk` 0x601570, `_OnRButtonDblClk` 0x601650, `_OnMouseWheel` 0x601730, `_OnKeyDown` 0x601820,
`_OnChar` 0x6018E0, `_OnKeyUp` 0x6019A0, `_Deactivate` 0x601A60, `Activate` 0x601AC0; slot defaults 0x579550..0x579680;
`CDemoScreen::*` 0x5794B0.., 0x601ED0..0x602190.
