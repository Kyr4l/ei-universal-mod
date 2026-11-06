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
| texture uv | pixel coordinates of a 256 px texture: `u = px / 256`, `v = 1 - py / 256`, then every uv shifted by `+1/512, -1/512` (= +0.5 texel on both pixel coordinates: texel-centre sampling, not an inset towards the centre; every builder, 5.5) [V] |
| colours | GDI `COLORREF` BGR for text (`0xA7D7E4` = light yellow, `0xFFFFFF` white, `0x00FF00` green); `'^N'` in a message log line = colour escape, N bit0 blue, bit1 green, bit2 red (`^7` white, `^4` red, `^2` green, `^1` blue, `^6` yellow) (`CUIMessageLog::AddMessage` 0x5E2A30) [V] |
| fonts | `CInterface +0x70 / +0x74 / +0x78` = Times New Roman at `size * 0x68 / 0x70 / 0x90` (size from a 75 pt probe, i.e. scaled with the resolution); `MeasureText` 0x5F8340 returns UI units; the console uses Courier New 9 pt [V] |
| time | `timeGetTime()` ms everywhere in the UI; game ticks `g_0x78FF34` ms (55 normal, 27 at x2); `CCamera::Update` dt = ms * 0.0181818 = ticks [V] |
| widget rects | `CUIWidget +0xD8` int[4] in UI units; `PointInside` 0x5FFD30 unscales the screen point first [V] |
| key events | `OnKeyDown(vk, repeat, flags)`: `flags & 0x1FF` = scancode (bit 8 extended), `flags & 0x4000` = autorepeat [V] |

## 2. The application shell (0x456000-0x461000)

### 2.1 `main` (WinMain, 0x456440) - startup in order [V]

1. `res\sysmsg.txt`: split at `\n`; fewer than 3 lines (or no file) -> the three English defaults (DirectX 7 missing, low disk space, insert CD 2 prompts).
2. `App_ProbeDirectX` 0x45E310 -> level code (0x100 IDirectDraw2 .. 0x700 IDirectDraw7, 0x800 D3D8 present); `< 0x700` -> `MessageBox(line 1, "CRITICAL ERROR", MB_ICONEXCLAMATION)` and `main` returns 0xDEAD.
3. `FactoryMap_Register` 0x458570: save-object factories 1 = `CGSPlayer_New` 0x457850, 2 = `NamedParty_New` 0x457740.
4. Command line (split at spaces; `g_0x79BA1C` defaults to `.\camera.cam`, the three others emptied): `-moviepath_<dir>` (prefix match: clears flag 0x20 of `g_0x7AF5C0`, dir + `\` -> `g_0x79BA0C`); while flag 0x10 of `g_0x7AF5C0` is clear: `-nocam` (case-insensitive) clears flag 8, a token **containing** `.cam` -> `g_0x79BA1C`; otherwise tokens containing `.scr` -> `g_0x79BA18`, `.mob` -> `g_0x79BA14`, `.mpr` -> `g_0x79BA10`; `-benchmark_<n>` -> `NumberOfFrames` (+0xF0, `g_0x79BD60`) = n and `CreateOutputInfo` (+0xEC, `g_0x79BD5C`) = 1; `-devicetarget_<n>` -> `DrawingTarget` (+0x2C). These three settings are written **before** `ReadFromRegistry`, which uses the field as the default only, so an existing registry value (always there after the first run, `WriteToRegistry` stores all) overrides them [V].
5. `LoadString` 0x65 = title "Evil Islands" (`g_0x79BB24`), 0x66 = class "EIGAME" (`g_0x79BA24`); `RegisterClassEx` (style `CS_DBLCLKS`, icon 0x69, **no cursor, no background brush**, wndproc `App_WndProc` 0x457D40). `FindWindow(class, title)` -> an existing instance is brought to front (restored when iconic, only when it is not already the foreground thread) and this one `_exit(-1)`.
6. `g_0x7AF450 = "Software\Nival Interactive\EvilIslands"`; `World_InitWorkDir` 0x55EA10 (work dir = current dir + `\`); `AiInfo_Load config\ai.reg`; `AppSettings::ReadFromRegistry`; `_set_new_handler(0x455A30)`; `CWorldClient::Init` / `CWorldServer::Init`; `CCamera::InitFromSettings` 0x40D0A0 (settings, `NearClipDistance`, `FarClipDistance`, fov pi/3.5) and `CWorldClient::SetCamera(g_0x79B2A8)`.
7. Free space on the drive of `g_0x7AF460` (`GetDiskFreeSpaceExA` when kernel32 exports it, else `GetDiskFreeSpaceA`) below 300 MB (314572800) -> `MessageBox(line 2, "Warning", MB_OKCANCEL)`, Cancel returns 0xDEAD; when `g_0x7AF460 != g_0x7AF458` the volume label of `g_0x7AF458`'s drive must equal `CD2_Label` (+0x21C, "EICD2"): `MessageBox(line 3)` repeats until it does, Cancel returns 0xDEAD; `%ssaves\`, `%smp\`, `%stextures\` created.
8. `DInputMouse` object (0x20 bytes, refcounted) -> `g_0x79BA08`; **`CreateWindowExA(0, "EIGAME", title, WS_POPUP | WS_VISIBLE (0x84000000), 0, 0, VideoResolution w, h)`** -> `g_0x79BC28` (failure returns 0); `ShowWindow(nCmdShow)`, `UpdateWindow`, `SetCursor(0)`; the camera eye `g_0x79B2C4` starts at (`CameraDefaultXDeploy`, `CameraDefaultYDeploy`, `CameraDefaultHeight`). The window has no frame in either mode: "fullscreen" only decides whether the DirectDraw device takes exclusive mode (`CDXDirectDraw +0x20 & 1`), so a borderless launcher only needs `FullScreen = 0` and a resolution equal to the desktop (um.dll BORDERLESS subclasses this window, `um-dll/um_window.cpp`).
9. Renderer `vt[2](hwnd, settings)` (`g_0x7AF080` DirectDraw / D3D7 device); `IDirectDraw7::GetAvailableVidMem` (vt +0x5C) free bytes: `g_0x784754` = texture memory (caps 0x1000) <= 8 MB, `g_0x784753` = (video memory 0x4000 minus texture memory, when larger) <= 8 MB (makes the transition screenshots 400x300), `g_0x784755` = non-local (AGP, 0x20004000) <= 8 MB; `g_0x784756 = DrawingTarget != 0`; `SetGamma(brightness, contrast, gamma)`.
10. Audio only when `Audio` (+0x50) != 0: `CSoundManager::Open(hwnd, 1, -1, 44100, 16)`, music `CMusicSystem::SetTrackPath(<g_0x7AF460>stream\, <g_0x7AF45C>streamsn.reg)`, listener resources `sfx.res` + `<g_0x7AF458>res\speech.res`, `SetMasterVolume(AudioSFX +0x58)`, `SetEffectsVolume(AudioSpeech +0x5C)`, `SetMusicVolume(AudioStreaming +0x54)`, Reverse_Stereo -> sound manager +0x8A4; `Movies_PlayIniList("Start")` 0x458270 (`config\movie.ini` section key `movies`, comma separated, `Movies\<name>` through `Movie_PlayFile` 0x5DB3F0); `Progres.bik` through `Movie_PlayIntro` + `Bink_Close`; databases from the resources path: perks, quests, units, spells, items (`database.res`), levers, acks, prints.
11. `DInputMouse::Create(hwnd, hInstance)`; `CWorldClient::InitClientResources`; `Cursors_Register` 0x5DAEA0 (kind 2 when FPSIndependentCursor else 1; the "small" `sm_cursor_*` argument is **always 0** here and in `App_WndProc`'s re-register, so the 16 px set is never used; the last argument `DrawingTarget == 0` is handed to every `AddFrame`); **`CInterface::Init(g_0x7C7D50, device)`**; `MapInfo_LoadBoth` 0x45EA00 (`map.txt`, `map_LMP.txt`) into `g_0x79BF00`; `App_LoadMapAndScript` 0x456140 (the `-mpr/-mob/-scr` arguments; without a `.mob` the game starts in the menu flow through `Game_SessionTick`); `SetGamma` again.
12. The message loop (2.3). On `WM_QUIT`: `CTextureManager::DeleteCachedFiles` (`textures\*.mmp`), `CInterface::Destroy` 0x5F4F90 (writes `config\tutorial.ini`, `KeyBindings_Save`), `CSoftCursor::Stop`, release the mouse, `SetGamma(0, 0, 0)`, renderer `vt[3]`, `AppSettings::WriteToRegistry` 0x45C560, `CWorldClient::SetCamera(0)`, `CWorldServer::Done`, `CWorldClient::Done`, the old new-handler restored; returns `wParam`.

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
    activation changed (g_0x79BC5D)? inactive: render-globals flag &= ~1, InvalidateRect(NULL); active: (device flag 2:
        IDirectDraw7::RestoreAllSurfaces (vt +0x64) under g_0x7AEE78, HRESULT -> device[2]) flag |= 1, SetCursor(0)
    dt = timeGetTime() - g_0x79B9F8 (ms since the last frame; first frame: 0)
    render-globals inactive  -> World_SetStatScramble(0); frameRendered = 0           (messages then go to App_WndProc only;
                                                     nothing sleeps here, so the minimized game spins this loop [I])
    else:
      World_SetStatScramble(1)
      CWorldClient::NetObjectIdle -> Lobby_IsAlive ? Lobby_Create : LobbySession_ReleaseActive2
      dt > 0x80000000 (timer wrap) -> 0
      Net_TickAll 0x4122D0                                             (1st call: um-engine mp_speed hooks it)
      dt = min(dt, 5 * g_0x78FF34);  movie capture (g_0x79BC54): dt = 1000 / MovieCapturingFPS
      serverAccum g_0x79BC64 += dt;  not CWorldClient::IsInWorld -> speed g_0x784758 = 1.0
      clientAccum g_0x79BC68 = ftol(clientAccum + dt * g_0x784758) [I: the multiply]; > 0x4000000 -> 0
      repeat:                                                          (server and client ticks interleave)
        serverAccum >= tick: CWorldServer::Tick (0 -> skip to the render), serverAccum -= tick, Net_TickAll
        while clientAccum >= tick and (CWorldClient::PumpNetHasPacketHeader or not IsInWorld):
            CWorldClient::Update (0 -> skip to the render), clientAccum -= tick, CCamera::Update(tick / 55), Net_TickAll
                                                                       (2nd call: mp_ghost hook 0x4573AE)
      until no server tick ran                                         (in a world the client ticks only on a received packet)
      Session_ProcessRequest(g_0x7AFE20) 0x57C700 (handles one pending SessionRequest: zone change, save, menu flow; world-server.md 1)
      g_0x7AFCA8 = clientAccum / tick (the render interpolation fraction) unless paused (g_0x7AF624)
      client speed balance: lag = CWorldClient::PumpNetCountPackets - fraction; speed = lag < 0 ? 2^(0.1 lag) : lag < 1 ? 1 :
            0.9 + 0.1 sqrt(lag); g_0x784758 = clamp(speed, 0.1, 10); 'Client speed ... balance' log when g_0x79BC60
      CCamera::Update once per whole tick of dt (while dt > tick), then once with the remainder (um-engine camera_smooth
            redirects these 3 call sites); together with the per-client-tick call the camera eases about twice per tick of
            real time while a world runs [I]
      not paused: recording flag 0x10 -> CCamera::RecordKeyframe; playback flag 0x20 -> CCamera::PlayCamFile(tick g_0x7AF61C + fraction)
      frameRendered = 1
      CInterface::ProcessPendingScreens 0x5F57C0      (pops / replaces screens requested during the frame, ticks the top screen)
      App_RenderFrame(g_0x784752 present, 1) 0x458010  (BeginFrame, Render3D, EndFrame, Draw2D, cursor, flip, screenshot capture)
  GetMessage -> 0 = WM_QUIT -> shutdown (2.1 step 12)
  TranslateMessage; frameRendered ? App_HandleInputMessage : App_WndProc
```

Screens are processed and the frame rendered only on the active branch [V]: an inactive window renders nothing.

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

Read by `ReadFromRegistry` 0x45B210 from **per-section subkeys** of `HKCU\Software\Nival Interactive\EvilIslands` (the
section names of the ctor are the subkey names: `Path Settings`, `General Settings`, `Shadow Settings`, `Terrain Settings`,
`Statistics Output Settings`, `Camera Settings`, `Game Specific Settings`, `Network Settings`; `CWinRegistry::GetDWord`
0x698610 opens `<key>\<section>` per value and falls back to the default when the subkey / value is missing or the value is
not 4 bytes), as string / DWORD / 4-byte binary float values with the current field (the ctor 0x458870 defaults) as the
default, and written back by `WriteToRegistry` 0x45C560 at exit (so every value exists after the first run). Value placement
[V]: Path = the three paths; General = video, audio, `Brightness..Gamma`, `Reverse_Stereo`; Shadow = the five `*Shadow`,
`LightingFreq` (+0xB4, 10000), `FloraSpecialLight`; Terrain = `LOD1` (+0xBC, 40) / `LOD2` (+0xC0, 75) (the fog start / end; short
value names), `BorderFogDistance` (32), water waves, dynamic lights, self shadowing, a **second** `LightingFreq` (+0xD4, 1000),
`ShadowingFreq` (6000), the four `*prints`; Statistics = `CreateOutputInfo`, `NumberOfFrames` (-1); Camera = every `Camera*`
plus `NearClipDistance` / `FarClipDistance`; Game = the rest from `DefaultWindForce` on; Network = `Master_Server_Name`.
Conversions: `VideoResolution` "WxH" split at the first `x` with `atol` (written back as `%dx%d`); `CameraFlipX/YAxis` stored
as DWORD bool (non-zero -> field -1.0, else +1.0); `Hero_Light_*` / `Quest_Light_*` R / G / B as DWORD 0..255 (field = value /
255), radii as DWORD ints; `Difficulty_Level` copied into +0x250 and +0x254 (only +0x254 written back); `Joystick` is read with
default 0 and always written as 0. `ReadFromIni` 0x459100 / `WriteToIni` 0x45A380 read the same keys from ini sections (`Path Settings`, `General
Settings`, `Terrain Settings`, `Statistics Output Settings`, `Camera Settings`, `Game Specific Settings`, `Shadow Settings`,
`Network Settings`) but **have no callers**. `SetDefaults` 0x458DA0 is an older default set (640x480, master server
`a3master.nival.com`), also unused.

| Group | Keys (offset in AppSettings) |
|---|---|
| paths | `Work path` +0x20, `Resources path` +0x24, `CD-ROM Path` +0x28, `CD1_Label`..`CD3_Label` +0x218.. (EICD1..3), `DefaultMapName` +0x258, `DefaultStreamingSound` +0x25C, `Master_Server_Name` +0x268 |
| video | `DrawingTarget` +0x2C (0 = hardware), `VideoResolution` "WxH" -> +0x30/+0x34 (800x600), `VideoColorDepthInBits` +0x38 (16), `VideoZBufferDepthInBits` +0x3C, `VideoRefreshRate` +0x40, `FullScreen` +0x44 (1), `Video` +0x4C, `Dithering` +0x60, `Specular` +0x64, `Antialiasing` +0x68, `TextureQuality` +0x6C, `Mipmapping` +0x70, `Filtering` +0x74, `PerspectiveCorrection` +0x78, `ShowParticles` +0x7C, `AutomaticQuantization` +0x84, `MovieCapturingFPS` +0x88 (15), `Brightness` +0x8C, `Contrast` +0x90, `Gamma` +0x94 (floats) |
| audio | `Audio` +0x50, `AudioStreaming` +0x54 (music), `AudioSFX` +0x58 (master), `AudioSpeech` +0x5C (volumes 100), `Reverse_Stereo` +0x98 |
| shadows / lighting | `ShadowQuality` +0x9C, `HeroShadow` +0xA0, `UnitShadow`, `BuildingShadow`, `FloraShadow`, `OtherShadow` +0xB0, `LightingFreq` +0xB4 (10000), `FloraSpecialLight` +0xB8, `EnableSelfShadowing` +0xCC, `EnableDynamicLights` +0xD0, `ShadowingFreq` +0xD8, `ObjectsLightingCoeff` +0x22C, `Hero_Light_R/G/B/Radius` +0x230.., `Quest_Light_*` +0x240.. |
| terrain / effects | `LOD1` +0xBC (40) / `LOD2` +0xC0 (75) fog start / end, `BorderFogDistance` +0xC4 (32), `LightingFreq` (Terrain) +0xD4 (1000), `ShadowingFreq` +0xD8 (6000), `FogNightStartDistance` +0x260 (50), `FogDayStartDistance` +0x264 (90), `EnableWaterWaves` +0xC8, `EnableFootprints` +0xDC, `EnableBloodprints` +0xE0, `EnableFireprints` +0xE4, `EnableBloodprintsRelight` +0xE8, `DefaultWindForce` +0x1E8 (0.4), `DefaultTimeScale` +0x1EC (60) |
| camera | `CameraFlipXAxis` +0xF4, `CameraFlipYAxis` +0xF8, `CameraMinHeight` +0xFC (4), `CameraMaxHeight` +0x100 (100), `CameraDefaultHeight` +0x104 (30), `CameraDefaultXDeploy / YDeploy` +0x108 / +0x10C, `CameraMin/Max/MinLimit/MaxLimit/DefaultDistanceToCarrier` +0x110..+0x120 (4 / 100 / 3 / 80 / 30: soft min / soft max / hard min / hard max; the hard max 80 being below the soft max 100, the zoom stops at 80), `CameraBriefing{Min,Max,MinLimit,MaxLimit}DistanceToCarrier` +0x124..+0x130 (4 / 100 / 3 / 80), `CameraBriefingMin/MaxPitch` +0x134 / +0x138 (30 / 60 deg), `CameraBorderScrollArea` +0x13C (5 px), `CameraFrameSelectionSensetiveArea` +0x140 (20), `CameraTerrainSensetiveArea` +0x144 (10), `CameraDefault{Scroll,Rotate,Pitch,Zoom}{Speed,PowerMouse,PowerKeyboard}` +0x148..+0x174 (1 / 1 / 0.5), `CameraShortcut_%d_{AxisX,AxisY,AxisZ,Angle,Distance,Pitch}` x4 +0x178 (0x18 each), `CameraDefaultMinPitch / MaxPitch` +0x1D8 / +0x1DC (20 / 80 deg), `NearClipDistance` +0x1E0 (2), `FarClipDistance` +0x1E4 (100) |
| game | `Joystick` +0x48 (0), `FPSIndependentCursor` +0x80 (1 = cursor thread), `UseSWCursors` +0x1F0, `SelectionType` +0x1F4, `CornerSize` +0x1F8, `DrawVisibleOnly` +0x1FC, `DrawPath` +0x200, `DrawFlyingHP` +0x204, `ShowTutorial` +0x208 (`g_0x79BE78`), `Autosave` +0x20C, `ToolTip_Time` +0x210 (0.5 s, `g_0x79BE80`), `SwitchFilters` +0x214, `Font_CharSet` +0x224 ("english"), `Keyboard_Layout` +0x228 (0x409; `CInterface::Init` activates that HKL), `Difficulty_Level` +0x254, `CreateOutputInfo` +0xEC, `NumberOfFrames` +0xF0 |

### 2.6 `map.txt` (`MapInfo_Load` 0x45EA20) [V]

`<resources>\map.txt` and `map_LMP.txt` (`MapInfo_LoadBoth` 0x45EA00 into `g_0x79BF00`; the second file **appends**) are walked
with `CTextManager::SeekSection(0)` (every `#` line) + `ReadLines(1)` (one line split into words) into `MapInfo` (`+4` allod
records 0x2C bytes, `+0x18` zone records 0x5C bytes, MFC `CArray` growth). Returns 0 when the file cannot be opened. Every
`#allod` / `#zone` / `#exit` line appends **one** record (the word after it is a name, not a count) [V, deep-read 2026-11-06]:

| Line | Effect |
|---|---|
| `#allod <name>` (>= 2 words) | new allod record: +0 = name, then the next 5 lines' first words -> +4 .. +0x14; +0x18 = `CDWordArray` of zone indices |
| `#zone <name> <allod> <kind>` (>= 4 words) | new zone: +0 name, +4 allod name; the zone index is appended to the first allod whose name matches case-insensitively; kind `game` 0 / `edge` 1 / `brief` 2 (+0x28; other words leave it 0); defaults weather +0x2C = 1, +0x30 = 0, restrict +0x34 = 0, cage +0x44 = 0; the exit cursor is reset |
| `#res` | next line `<mpr> <mob>` -> +0xC / +8 |
| `#camera` / `#figure` | next line word 1 -> +0x1C / +0x10 |
| `#maps` | next line two ints -> +0x20 / +0x24; the line after `<map picture> <quest picture>` -> +0x18 / +0x14 |
| `#weather` | next line: `none` 0, `snow` 2, `tornado` 3, anything else 1 (+0x2C) |
| `#sky` | next line `cave` -> +0x30 = 1, else 0 |
| `#cage` | +0x44 = 1 |
| `#restrict` | next line 3 numbers (atof) -> floats +0x38 / +0x3C / +0x40, +0x34 = 1 |
| `#exit` | new 0x3C exit record in +0x4C (count +0x50) with +0x30 / +0x34 / +0x38 = 0; next line `<target zone> <n>` -> +0x28, +0x2C = n - 1 |
| `#deploy` (needs an exit) | next line 4 numbers -> +0x14..+0x20 **and** +4..+0x10, +0x24 = 0; a 5th number -> +0x24 = deg * pi / 180 |
| `#remove` | the same into +0x14..+0x20 / +0x24 only (so it overrides the leave rectangle of `#deploy` when it comes later) |
| `#passtime` | next line number -> exit +0 |
| `#view` | next line number (missing -> "0") -> exit +0x30 in radians |
| `#deployangle` | exit +0x34 = 1, next line number (missing -> "0") -> +0x38 in radians |

Zone sub-keys before any `#zone` and exit sub-keys before any `#exit` of the current zone are ignored; `#position` and `#quest`
are not read here (`QuestFiles_Scan` re-reads the file for `#quest`). The world-map screen (0x740B08) and the quest log draw
from it; the game side of the exit record is `ZoneExit` in `structs.tsv`.

### 2.7 `CTextManager` text scanning and `CSysInfo` (0x4DD000-0x4E0000) [V, deep-read 2026-11-06]

`CTextManager` (`g_0x7AF788`, also `CWorldClient +0x1D0`) reads text files out of its archive list `+0x1C` (`OpenText` 0x4DEFC0:
cursor `+0x20` = buffer `+0x24`, size `+0x28`). `SeekSection(name)` 0x4DF030 moves the cursor and returns 1 when found, 0 at
the end of the text:

- `name == 0` (next section; `MapInfo_Load`): reads words; a word whose first character is the token `0x78DCFC` and whose first
  two characters are not the token `0x78DCF8` is a section start; any other word skips the rest of its line.
- one-word name: reads words until one equals the name exactly (case-sensitive); after a mismatch the rest of the line is
  skipped and then every following line whose first word starts with the two-character token `0x78DCF8` (a comment marker [I])
  is skipped too.
- multi-word name (split at spaces): reads whole lines (`ReadLines(.., 0)`); a line matches when it has at least as many words
  and each of the name's words equals the line's word **upper-cased** (so callers pass upper-case names and the file side is
  case-insensitive); mismatching lines are skipped the same way.

The two token strings are too short for the strings dump ([?]: `#` and `//` would fit every caller seen). DBCS lead bytes are
honoured (`GetMbChar`, `_mbsinc`).

`CSysInfo::CollectInfo` 0x4DDF28 (`CSysInfo::Init`, vt 0x73D644 slot 2): `GetOsInfo`, `GetSystemInfo` (+0x50), `GetMemoryStatus`
(+0x30), `DetectCpuFeatures`; flag `+8 |= 8`; platform `+0x84`: 0 -> "Windows 3.11/Win32s" (flag 0x20), 1 -> "Windows 95" (flag 0x40) or,
build `+0x80` low word >= 1998, "Windows 98" (flag 0x80), 2 -> "Windows NT" (flag 0x100), else "UNKNOWN OS" / "UNKNOWN VERSION" (flags
0x1E0 cleared); name `+0x24`, version `+0x28` ("%d.%d.%d"), `+0x2C`, computer name `+0x1C`, user name `+0x20`. It then builds a
report string ("OS ... build ...", "Total / Available physical / virtual memory") and **destroys it unused** (the log output
was compiled out); returns 1.

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
  `ThreadProc` in detail [V, deep-read 2026-11-06]: once at start, when `+0x91` initialised (under the cursor critical section `+0xA0`) `UpdateState` and the
  primary-surface pixels under `dstRect +0x30` are saved into the 32x32 scratch `+0x60`, `lastRect +0x20 = dstRect`; then until
  the stop flag `+0x9C`: sleep the rest of 41 ms (0x29); skip while the render-globals active bit is clear; `UpdateState`
  returns "changed" -> only when `mode +0xC != 0` and changed: **disjoint** old / new rects: `IDirectDraw7::WaitForVerticalBlank
  (BLOCKBEGIN)`, old background `+0x60` -> screen at `lastRect`, screen at `dstRect` -> `+0x60`, `ComposeFrame` into the 64x64
  scratch `+0x64`, `+0x64` -> screen at `dstRect`; **overlapping**: screen at the union rect -> `+0x64`, old background
  `+0x60` -> `+0x64` at the old offset, `+0x64` at the new offset -> `+0x60` (the new background), `ComposeFrame` at the new
  offset, wait for the vertical blank, `+0x64` -> screen at the union (one blit, no flicker); then `lastRect = dstRect`. Every
  Blt is `DDBLT_WAIT | DDBLT_ASYNC` (0x1000200) under the DirectDraw critical section `g_0x7AEE78` and skipped when source and
  destination are the same surface. On exit it signals the event `+0x1C` (`Stop` waits for it).
- `ComposeFrame(frame, dstRect, srcRect)` 0x5E5C40: locks the frame's strip surface at `srcRect`, the 64x64 scratch at `dstRect`
  and the 32x32 scratch at (0, 0, w, h) (`IDirectDrawSurface7::Lock`, "Can't lock surface." / "Can't unlock surface." fatal) and
  writes `background & mask | cursor pixel` per pixel for 16 and 32 bpp surfaces only (any other depth composes nothing) [V for
  the operation and the depths; which locked pointer feeds which operand is garbled in the decompile [I]].
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
(**radians**; the settings limits are degrees, converted with `pi / 180` on every clamp), distance `+0xF0`; `UpdateFromTarget` 0x40F8B0 derives the view quaternion `+0x34 = yaw * rot_x(-(90 + pitch))` and
the position `+0x1C = target - up(view) * distance`, then `AvoidObstacles` 0x40E4C0 lifts the camera above the terrain box
(`Terrain_SampleHeightBox` 0x40D9B0, +-2 m) and above objects within 16 m, by at least `CameraMinHeight` and at most
`CameraMaxHeight` above the ground.

`Update(dt)` 0x40FB30 (dt in ticks), in order [V, deep-read 2026-11-06]:

1. Sound listener (`CSoundManager::SetListener`, up axis of `+0x34`): at the target (z re-sampled as `GetHeight + heightOffset`
   when a world is loaded, `g_0x7AF738`), or at the eye when flags `0x60`; flags `0x60` (recording / playback) then return.
2. Carrier `+0x11C` (`FindById` in `g_0x7AF5F8`). Flag 8: eye = unit `+0x1C` with z `+= unit +0x130 + unit +0x14C`, looking along the
   unit's **backward** horizontal axis (`-forward`, z = 0, normalised; `RotateTowardDirection_B`, `LevelRoll`) and return - nothing
   below runs. Else the target x / y move **20 % of the gap per call** (not scaled by dt) and are clamped to
   `[0, mapW - 1] x [0, mapH - 1]` (`g_0x7AF738 +0x10C / +0x110`).
3. Scroll: `delta = (+0xA4 - +0x98) * +0xB0` (0.1333); `|delta| < 0.001` snaps, else `+0x98 += delta * dt` and snaps when `|v|^2`
   crosses `|target|^2` (the same overshoot rule is used for pitch / yaw / zoom). A non-zero velocity moves the target by
   `v.xy * dt * ((dist - softMin) / (1.125 (softMax - softMin)) + 0.2)` (v.z unused; briefing distances with flag 0x200), clamps
   it to the map, sets the carrier to -1 and clears flag 8; with the leash flag `0x100` the target is then pulled back onto the
   circle of radius `+0x118` around `+0x10C` (`SetLeash` 0x411360, dialogs). The leash is only applied on a scroll step.
4. Height: `Terrain_SampleHeightBox(target, box = CameraTerrainSensetiveArea g_0x79BDB4: 0.5 / 1 / 1 times)`; target z moves
   **10 % per call** towards (box height, or the carrier's z when one exists) `+ heightOffset`.
5. Pitch / yaw / zoom speeds `+0xC0 / +0xB4 / +0xCC` ease towards the inputs `+0xC4 / +0xB8 / +0xD0` (`SetPitchInput` 0x40F3C0,
   `SetRotateInput` 0x40F360, `SetZoomInput` 0x40F420; scroll: `SetScrollVelocity` 0x40F230) with `+0xC8 / +0xBC / +0xD4` (0.333)
   per dt; `pitch += speed * dt` then clamped to `CameraDefaultMin/MaxPitch` (+0x1D8 / +0x1DC) or the briefing pair (+0x134 /
   +0x138); `yawRot *= quat(z axis, speed * dt)`; `dist += speed * dt`.
6. Distance limits (normal: soft `+0x110 / +0x114`, hard `+0x118 / +0x11C`; briefing `+0x124 / +0x128`, `+0x12C / +0x130`), run every
   call even without zoom input: `d < hardMin` -> `hardMin`; `d < softMin` -> `d += (softMin - d) * 1.1 dt / (softMin - hardMin)`;
   `d > hardMax` -> `hardMax`; `d > softMax` -> `d -= (d - softMax) * 1.1 dt / (hardMax - softMax)`. With the shipped defaults (see
   2.5) the "MaxLimit" is below the "Max", so the hard max decides.
7. `UpdateFromTarget` (eye, view quaternion, obstacle lift), then the shake list `+0x108` (`AddShake` 0x4113A0: within 20 m of the
   source, for `strength * 30` ticks): `eye.z += sum(amp * exp(-freq * t) * cos(pi/2 * 0.26667 * t))` (`g_0x79B2A4` = pi/2: a
   15-tick = 0.83 s period), `t += dt` after the term, the node dropped once `t >= duration`.

The 20 % follow and 10 % height steps are per call while `Update` is called a variable number of times per frame (per client
tick, plus once per whole tick of the frame time and once with the remainder), so the follow lag depends on the frame rate [I]. The memory-scanned addresses in
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

`Init` 0x5F4990 (device) [V, deep-read 2026-11-06]: device `+8`; screen size `+0 / +4` = the primary surface's width / height
(device `+0x18` -> `+0x14 / +0x18`, only when a primary surface exists); fonts: a 75 pt Times New Roman probe
(`CreatePointFont(750)`, `GetObject` -> `|lfHeight| * 0.01`) then three Times New Roman fonts `+0x70 / +0x74 / +0x78` whose
point sizes are `ftol` of an FPU expression built from `screenWidth * 0x68 / 0x70 / 0x90` and the probe (the exact divisor is
hidden on the FPU stack) - "CInterface3D::Create. Cannot create font." is fatal; keyboard layout: every installed HKL
(`GetKeyboardLayoutList`, max 16) is activated in turn and its `GetKeyboardLayoutName` parsed as hex; the first whose low word
equals `Keyboard_Layout` (`g_0x79BE98`, e.g. 0x419 Russian) wins, else the 0x409 one, else the layout active at start, and it
stays activated; `KeyBindings_Load`; console `+0x14` (`CUIConsole`, 0x7C, `g_0x7C7D64`), `DebugText::Init(g_0x79B930)`, message
log `+0x1C` (`CUIMessageLog`, 0x158), capture handler / screenshots / transition / close-all / pending screen cleared, `+0x30 = 1`
(drawing enabled), fade `+0x18` (`ScreenFade`, 0x14), save-state bytes `+0x89..+0x94`, modifiers `+0x7C..+0x80`, autosave
`+0x81` zeroed; `<saves root>\config\tutorial.ini`: each line split at spaces, `<screen> <page>` -> `std::map<CString, int>`
`+0x98` (`atol`); `+0xA8 = 0`, FPS bar `+0x20 = 0`, `+0x28 = timeGetTime()`, `+0x2C = 0` frames, disconnect flag `+0x88 = 0`. `Destroy` 0x5F4F90 pops everything, writes the
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
| `AddChatLine(text, colour)` 0x5F82F0 | `LobbyClient::OnAddressLine` (um-engine mp_ghost hooks this call at 0x41E656) | `CUIMessageLog::AddMessage` (the colour is ignored there: use `^N` codes) |
| `EnableChatInput` 0x5F7C60 / `ClearChatLines` 0x5F7C80 | Enter in the battle / camp / dialog / trade screens | lobby chat line |
| `ConsoleAddText` 0x5F7EB0 | `LogStream::sync` (every game log line) | `CUIConsole::AddText` |

### 4.6 The message log `CUIMessageLog` (`CInterface +0x1C`, vt 0x7400DC / 0x7400A8, `CUIWidget` + 0x10) [V]

`AddMessage(text, colour)` 0x5E2A30 [V, deep-read 2026-11-06]: **the colour argument is never read**; every line starts white
(0xFFFFFF) and only `'^N'` escapes change it. Word wrap (DBCS aware, `_mbsinc`): the text is walked keeping a plain word / line
and a coded word / line (codes kept); a space closes the word; before adding a character, `MeasureText(line + word + ch, font
1) > 0x30C` (780 UI units) pushes the line, and if `word + ch` alone is still wider the word is pushed too (hard break inside
long words); `'^'` + `'0'..'7'` goes only into the coded text, `'^'` + another character **appends** a fixed
string `0x787BE8` [?] to the coded word (the call is the append `CString::operator+=` 0x700D4C, the only CString-from-text operator this function calls) and drops that character. Then the oldest lines are erased until `old + new <= 12`, and per new line a
0x18-byte node `{next, prev, +8, text +0xC (plain), time +0x10, surface +0x14}` is appended with a `CUIBlitSurface(0, 0,
MeasureText(text) + 3, 0x11, flags 5 = colour-keyed + visible)`; the coded line is drawn in segments split at `'^'`, each at
`x = MeasureText(previous plain segments)` in rect `(x, 0)-(0x30F, 0x11)`, font 1. Lines are placed at `(10, 10 + 16 i)` by
`BlitSurfaces` 0x5E29E0; `Tick` 0x5E2990 drops lines older than 20 s. The chat input line (surface
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

Mesh format (all builders, deep-read 2026-11-06 [V]): vertices are SoA blocks of 0x40 bytes `{x[4], y[4], z[4] = 0, w[4] = 1}`
in UI units / `scale` (y down); the normal block `+0x1D0` is one block `{0, 0, 1, 1}`; the **index list `+0x200` indexes the
6-byte triplets `+0x1E0`** `{vertex index, 0, uv index}`, not the vertices, so one vertex can carry several uvs; uvs `+0x210` are
`(u / 256, 1 - v / 256)` from texel coordinates; the colour arrays `+0x138 / +0x148` are sized 4. Position and scale: centre
builders use `SetScreenPos2D` of the rect centre, the others of their origin; uniform scale `+0x23C / +0x250 / +0x264 = z * k *
scale / 400`.

| Builder | Arguments | Geometry |
|---|---|---|
| `InitRect` 0x5E70E0 (202 callers) | z, scale, x, y, x2, y2, u0, v0, u1, v1 (ints) | 4 vertices `(-hw, -hh), (-hw, hh), (hw, -hh), (hw, hh)` with `hw = (x2 - x) / 2 / scale`, uvs `(u0, v0), (u0, v1), (u1, v0), (u1, v1)`, triplets `(i, 0, i)`, indices `0 1 2 1 3 2`; `u1 < u0` mirrors (both shifted by -1) |
| `InitGrid` 0x5E7680 | + cols, rows | `(cols + 1)(rows + 1)` vertices row-major from the **top-left** corner (position = that corner), cell `(c, r)` = triplets `(v, uv0), (v + cols + 1, uv1), (v + 1, uv2), (v + cols + 2, uv3)` with the same 4 uvs (tiled), indices `4k + {0 1 2 1 3 2}` |
| `InitBorderStrips` 0x5E7D60 (was InitFrame16) | z, scale, l, t, r, b, border (float), u0, v0, u1, v1 | **8** vertices: block 0 = outer TL, inner TL, outer TR, inner TR; block 1 = outer BR, inner BR, outer BL, inner BL (inner = inset by `border`); 16 triplets = 4 trapezoid strips **top, right, bottom, left** (strip s uses vertices `2s .. 2s + 3`, the last wraps to 0 / 1), each with the whole uv rect: v runs from the outer edge (v0) to the inner edge (v1), u along the strip; mitred corners |
| `InitBorderRing` 0x5E8450 (was InitFrame8) | same | 8 vertices (0..3 outer TL, BL, TR, BR; 4..7 inner, same order), 8 uvs: outer = the uv rect, inner = inset by `border * (u1 - u0) / (r - l)` and `border * (v1 - v0) / (b - t)` texels (the texture's own border lands on the frame border); 8 triangles `0 4 2, 4 6 2, 2 6 3, 6 7 3, 3 7 1, 7 5 1, 1 5 0, 5 4 0` |
| `InitDisc` 0x5E8C60 (was InitN) | z, scale, cx, cy, radius, n, uc, vc, ur (ints) | n vertices `radius / scale * (cos, sin)(2 pi i / n)` (no centre vertex), uv **circle** `(uc + ur cos, vc + ur sin)` texels (not a uv rect), fan `(0, i + 1, i)` for i = 1 .. n - 2 |
| `InitRing` 0x5E91F0 (was InitStrip) | z, scale, cx, cy, radius, n, thickness, u0, v0, u1, v1 | 2n vertices (outer / inner at angle i), n quads: triplets `(2i, uv0), (2i + 1, uv1), (2i + 2, uv2), (2i + 3, uv3)` (the last wraps), indices `4i + {0 1 2 1 3 2}`; v from outer to inner, u along the angle |
| `InitQuadUV` 0x5E99D0 | z, scale, x0, y0, x1, y1, x2, y2, u0, v0, u1, v1, u2, v2 | **one triangle** (indices `0 1 2` only): vertex 0 = the origin (position), 1 / 2 = `(xi - x0, yi - y0) / scale`, explicit uv per corner; the 4th vertex and uv are (0, 0) padding of the SoA block. Callers draw pointer arrows (`CUIWindow_648520::Build`: `(0, 0), (17, 11), (17, -11)`) and the quest-log marker (`CQuestLogWin::Rebuild`) |
| helpers | `SetUniformScale` 0x5E7060, `SetScaleY` 0x5E7090, `BindOwnArrays` 0x5EA150, `CloneQuadMesh` 0x5FF460 | |

Every builder ends with the same uv shift - **every** uv gets `u += 1/512`, `v -= 1/512` (= +0.5 texel on both pixel coordinates,
i.e. texel-centre sampling, not an inset towards the centre) - and `BindOwnArrays` (own arrays -> draw pointers `+0x9C..+0xC8`, colour arrays sized).

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
0x5FC2F0 (the camp / lobby heroes). `LoadPictureTo256Target` 0x5EC020 decodes a `.mmp` into a 256x256 A1R5G5B5 render target
and `CUIWidget::BlitPictureIntoTexture` 0x5FFFB0 writes one into a region of an existing texture (slot art, speaker portraits).

Deep-read 2026-11-06 [V] - the three "UIUnitPreview" functions draw straight into the frame (no render-target texture):

- `UIUnitPreview::Render(renderGlobals, unit, yaw, x, sixRects)` 0x5EC300 (`CCampScreen::Draw` tab 0: the hero at x = 400 with
  `sixRects`; `CCampScreen2::Draw`: x = 555 or 400): flushes the vertex buffers; `IDirect3DDevice7::Clear` of the **depth**
  (plus stencil when the device has one) over the whole target, then again over the preview rect(s) - UI `(200, 100)-(600, 500)`
  when `x <= 400` else `(410, 100)-(700, 500)`, or the six rects of the tables 0x7402F8 / 0x740310 / 0x740328 / 0x740340 - so the
  unit draws over the UI quads; saves the unit's position / quaternion, sets unit flag 0x800, animates it with
  `vt[25](timeGetTime / 1024 * 15)`, places it at view-space `((x / 400 - 1) * k * 6, 1.0113, 6)` (depth 6, feet at UI y = 440)
  with `quat(y axis, yaw) * quat(x axis, pi/2)` and, when `x > 400`, a further `quat(-z, 0.15 * pi/2)` turn; material colour
  scale (`+0x94` -> `+0xF0..+0xF8`) 1.2 and texture stage 0 `MIPFILTER` = none during `vt[22] Draw`, then everything restored.
- `UIUnitPreview::GetOrCreatePortrait` 0x5ECA10 is really the **3D item icon** of an inventory / camp slot (callers
  `UIInventoryWin::Draw`, `CCampBar(2)::Draw`, `CCampQuickBar::Draw`, `CCampTrade/Repair/SpellConstr/ItemConstrWin::Draw`). Slot
  descriptor `{item, x, y, z, size, kind}`; the item's `vt[20]` model name -> figure cache `std::map<name, {figure, time}>` (miss:
  `GetDefaultQuad` 0x5EBFB0 + `TrimPortraitCache`), `vt[19]` texture name -> texture cache (`LoadPictureTo256Target` +
  `TrimFaceCache`); figure at `SetScreenPos2D(x, y, z)`, scale `size * k * z * 0.0009375`, material = the widget's `+0xEC`. A
  0x18-byte spin state is attached to the item at `+0x1C` on first draw (vt 0x7403F8 `ItemLink`; `+8` speed, `+0xC` angle, `+0x14`
  random glow phase in [0, 2 pi)). `kind == 9` (greyed): ambient 0.5, no spin, fitted to the slot by the figure bounds. Else
  ambient 1.0; an item with a spell at `+0x44` (weapon 0x3004 / armour 0x3005) gets a pulsing specular colour from the 8-entry
  school table 0x740358 (render state `SPECULARENABLE` 0x1D on while drawn); the hovered item (`== screen field`) spins up by (dt = `Time_GetSeconds`, render-globals `vt[6]` ms * 0.001 [I: the frame time])
  `8 * dt` (max 5 rad/s), others spin back to rest (`speed = (2 pi - angle) * 4` until < 0.1); orientation: runes 0x3008 and
  0x3007 cat 2 sub 3 / 4 = `quat(z, pi)` (no spin), weapons `quat(x, pi) * quat((1, 1, 0), angle)`, 0x3007 cat 2 sub 0..2 =
  `quat(x, pi)`, others `quat(x, pi) * quat(y, angle)`; a backdrop `InitRect` 70x70 at `z * 1.3` (uv 157..227 of texture 2) first.
  Runes with modifiers draw up to 8 small 35x35 icons (texture 1, an 8x8 atlas: `u = ((m - 1) & 7) / 8`, `v = -((m - 1) >> 3) / 8`)
  clockwise from the top around the figure at `d = scaleX * 550 / z`. Finally, when `kind != 9` and the screen's `vt[43](item,
  kind)` returns 0, an untextured 70x70 overlay with colour `0x82804040` (half-transparent dark red) at `z * 1.25` (`z * 0.8` for
  runes / scrolls) marks the item.
- `UIUnitPreview::PickFaceFigure` 0x5ED7B0 draws a hero's **face figure** (`CCampBar2::DrawFace`): the prototype name -> the
  `UnitsDb` entry, whose model name (`+0xE8`) **equal** to `unhuma` / `unhufe` selects the sex part; figure
  `infa%sme%dface` and texture `face%s%02d` through the same two caches; ambient 1.0, `quat(-x, (y - 300) * 0.001) * quat(x, pi)`
  (a slight tilt by the screen row), the same 70x70 backdrop, then the face.

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

Deep-read 2026-11-06 [V] (the `CString::Format` arguments are lost in the decompiles, so which number fills which `%d` is
mostly [?]; "label n" = `infoitem_n` / `infoskill_n`, the array index):

- `Refresh` order of tests: 0x3007 cat 1 -> material; 0x3007 cat 2 sub 0..2 -> weapon / armour; 0x3007 cat 2 sub 3 -> spell
  scroll; sub 4 -> perk; 0x3006 with proto subtype 8 -> `DrawItemOrSpellInfo`; **Ctrl (`CInterface +0x7C`) held and the item has
  a spell `+0x44`** (an enchanted item), or kind 0x3008 -> `DrawSpellItemInfo`; else the generic panel. Generic panel: the
  material record `g_0x7C86D8 + (objectId & 0xFFFF) * 0x50`, its name with spaces -> `_` and upper-cased gives the
  `matshort <NAME>` text ("Unknown" default); name (2 wrapped lines, white 0xFFFFFF, 17 px), `%s %s` line, then: 0x3006 whose proto
  `+0x10` is not `none` and every kind other than 0x3006 / 0x3007 / 0x3009 -> `%s %s x %d` material + count; all but 0x3009 ->
  `%s %d` price; weapons / armour / 0x3006 subtype 5 -> `%s %d` (value of the item's spell `+0x50` when enchanted) and `%s
  %d/%d`; weapons / armour -> durability `%s %d/%d` (0 when `<= 0`); weapons -> the first non-zero of the 7 damage proportions
  names the type (`damage_%d`), `%s %d-%d %s`, then three `%s %d` lines (labels 38 / 39; ranged kinds 5 / 6 of
  `CItemWeapon::GetDamageKind` print a different value on the last); armour -> `FormatResistLists(item +0x4C[7])`: label 25 +
  list 1, label 24 + list 2 when not empty; 0x3007 -> description + `loot_item` text (7 lines), others the description (4
  lines) and, for enchanted weapons / armour / 0x3006 sub 5, the spell's `spell <name>` title (`%s %s`) and label 6.
- `DrawMaterialInfo` 0x5EFD90: record by item `+0x54`; `%s %s`, label 17 + `mattype_<record +4 upper>` text, four `%s %d` and
  one `%s %.1f` lines, label 4 + resist list 1, label 24 + list 2, description (4 lines).
- `DrawWeaponArmorInfo` 0x5F0370: proto table by item `+0x50` (0 weapon `g_0x7C86F8` 0xB4, 1 armour `g_0x7C8718` 200, 2
  `g_0x7C8738` 0x8C) and index `+0x54`; `%s %s` (`mattype_` of proto `+0x10`), `%s %s x %d`, `%s %d` lines (fewer for sub 2);
  weapons: damage type from proto `+0x98[7]`, `%s %d-%d %s`, two `%s %d`, a third for proto kind `+0xC` 5 / 6; armour: resist
  floats `proto +0x70[i] * proto +0x6C * 10`.
- `DrawSpellInfo` 0x5F0B90 (scroll) and `DrawSpellItemInfo` 0x5EF5D0 (rune / Ctrl view) share the layout: name, `%s %s`, two
  `%s %d`, label 18 + `school_%d`, mana `%s %d` (or `%s %s` when proto byte `+0x7B` is 0), range `%s %d` when speed >= 1 else
  `%s %s`, `%s %d%s`, area `%s %.1f%s` (or `%s %s` when spell flag 0x10000000), duration `%s %.1f%s` when >= 2 else `%s %s`, `%s
  %d`, description (4 lines). The rune view adds label 19 and the modifier names (perk table `g_0x7C8530`, 0x24 records, index
  `perk code - 1`; name = the third value line of the `modifier <name>` text section, "-" default) joined by `, ` (trailing
  separator trimmed) in 3 wrapped 16 px lines. The decompile shows two different `CString` functions under one name: 0x700AF9
  assigns, 0x700D4C appends; this function calls both, so which calls inside the modifier loop append is [I].
- `DrawPerkDetail(record {kind, id, slot, ?, unit, name})` 0x5F1590: kinds 0..3 title from the `perk <name>` section, 4..7
  from `<name>` / `<name>_desc` texts. Kind 0 = attribute (id 0 strength: `UnitStats_StrengthRatio` `%s %d%%` + `CarryWeight`;
  1 dexterity: `DexterityRatio` + `MoveSpeed`; 2: an `atan2` value + `ManaCostFactor` `%s %d%%`), kind 1 = skill
  (`UnitStats_GetByteStat6CB`, `SkillValue` / `PerkBonusA`), kind 2 = `GetByteStat6C8` + `AttrSkill`, then 13 lines of
  description. Kind 3 = perk family: the name with a trailing `0` trimmed and the level suffix appended finds the perk in
  `PerksDb` (`g_0x7AF268`, 0x44-byte records); `family = index / 3` (the 26 cases) and the three levels `3 family .. 3 family +
  2` are listed: level name, label 21 (right-aligned "owned" mark) for levels below the owned count (`record +4`), the family's
  value line (`"     %s %d"` for families 0..14 and 23..25, `"     %s %d%%"` for 15..22: 5-space indented) and a `Perk_ComputeCost` line (`"     %s %d"`, rect widened by 15);
  then 6 description lines. Kind 4 = equipment-slot resists: label 25 and the 7 damage types summed over 3 values of the unit's
  slot record `unit +0x11C + slot * 0xF4` (stride 0x48).

## 6. What is not known

- ~~InitQuadUV argument order~~ settled [V] (0x5E99D0): `(z, scale, x0, y0, x1, y1, x2, y2, u0, v0, u1, v1, u2, v2)`: corner 0 is
  the origin, corners 1 and 2 give the two edge vectors (`(x1 - x0) / scale`, `(x2 - x0) / scale`, stored at the mesh +0x1C0); the
  uv table (+0x210) gets `u / 256, 1 - v / 256` for corners 0..2 in the same order, and the 4th corner's uv is set to (0, 0), not
  derived; then every uv gets the half-texel shift (+1/512, -1/512). Deep-read 2026-11-06: the index list is only `0 1 2`, so it is **one triangle**, not a quad (5.5).
- ~~`InitBorderStrips` strips 3 / 4~~ settled [V] (deep-read 2026-11-06): both vertex blocks decoded: 8 vertices, strips top / right / bottom / left (5.5).
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
- ~~MapZoneRecord fields / allod +0x18~~ settled [V] from `MapInfo_Load` 0x45EA20 and a real `map.txt`: `#zone <name> <allod>
  <game|edge|brief>` -> +0 / +4 / +0x28 (0 / 1 / 2); `#res` next line `<mpr> <mob>` -> +0xC / +8; `#figure` -> +0x10; `#camera` ->
  +0x1C; `#maps` two lines: two ints -> +0x20 / +0x24 (`8 6` for zone1), then `<map picture> <quest picture>` -> +0x18 / +0x14 (the
  ints at +0x20 / +0x24 are NOT strings, the old rows were wrong); `#weather` -> +0x2C (none 0, snow 2, tornado 3, other 1); `#sky
  cave` -> +0x30; `#restrict a b c` -> +0x34 byte + +0x38..+0x40; `#cage` -> +0x44; `#exit n` / `#deploy` / `#remove` / `#passtime` /
  `#view` / `#deployangle` fill the 0x3C exit records. The allod record's "CObject" at +0x18 is a **CDWordArray of zone indices**:
  each `#zone` line appends its index to the allod named by its second word. `#position` is not read by this loader. Full rows:
  `structs.tsv` MapZoneRecord / ZoneRecord (same record).
- `CSoftCursorFrame::CreateFromMmp`: settled as far as needed [I]: the per-pixel copy loops are compiler-eliminated in the
  decompile; the surface comes from `CDXDirectDrawSurface::CreateFromPicture`.
- (2026-11-06) Short strings below the strings-dump threshold: `0x787BE8` (appended by `AddMessage` after `^` + a non-digit),
  the `SeekSection` tokens `0x78DCFC` / `0x78DCF8` (probably `#` / `//`). A byte read of the exe's .rdata would settle them.
- (2026-11-06) The `CString::Format` arguments of the `UIItemInfoWin` panels and of `CInterface::Init`'s font sizes are lost
  in the decompiles (FPU / varargs); which value fills which line needs the disassembly or a live look at the panels.
- (2026-11-06) `CSoftCursor::ComposeFrame`: which locked surface feeds which operand of `background & mask | cursor` [I].

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

Deep-read round 2026-11-06 (30 functions; wrong -> right):

| File / place | Wrong | Right |
|---|---|---|
| this page 3.6, `structs.tsv` was right | `CCamera +0x104` pitch "(degrees)" | radians; only the settings limits are degrees (`pi / 180` per clamp) |
| this page 3.6 | distance spring "`1.1 * dt` of the excess" | `excess * 1.1 * dt / (hard - soft gap)`; with the defaults the hard max 80 < soft max 100, so the zoom stops at 80 |
| this page 2.5, `structs.tsv` AppSettings +0x0 note | values flat under `HKCU\...\EvilIslands`, "the registry uses the value names only" | one subkey per section (`Camera Settings` ...); fog start / end are the values `LOD1` / `LOD2` |
| this page 2.1 step 4 | `-benchmark_` / `-devicetarget_` set their settings | they are written before `ReadFromRegistry`, which overrides them whenever the value exists (always after the first run) |
| this page 2.1 step 9 / 11 | "software device -> 16 px cursors", "small set when `g_0x784756`" | `Cursors_Register` always gets small = 0 (`main` and `App_WndProc`): the `sm_cursor_*` set is never used |
| this page 2.3 | reactivation "re-query the primary surface" | `IDirectDraw7::RestoreAllSurfaces`; server and client ticks interleave, the in-world client ticks only on a received packet |
| this page 2.6 | `#allod <n>` / `#zone <n>` / `#exit <n>` counts, kind words brief / camp / base, `#maps` 2 names -> +0x20 / +0x24 | one record per line with a name; kinds game / edge / brief; `#maps` = 2 ints then 2 picture names |
| this page 4.6 | `AddMessage(text, colour)` colours the line; node `{text, colour codes, time, surface}` | the colour argument is unused (lines start white; only `^N`); the list entry keeps the plain text only |
| this page 1 (texture uv) | uv "inset by 1/512 towards the centre" | the same `+1/512, -1/512` shift on every uv (+0.5 texel, texel-centre sampling) |
| this page 5.5 | `InitBorderStrips` 16 vertices; `InitDisc` with a uv rect; `InitQuadUV` "one quad" | 8 vertices (16 triplets), strips top / right / bottom / left; `InitDisc` takes a uv circle (`uc, vc, ur`); `InitQuadUV` is one triangle (rename `InitTriangle`) |
| this page 5.8 | `UIUnitPreview::Render` draws into a render-target texture for the inventory / trade; `GetOrCreatePortrait` = unit portraits; `PickFaceFigure` only picks a name "by prototype" | `Render` draws the camp hero straight into the frame after a depth clear; `GetOrCreatePortrait` draws the 3D item icon of a slot (rename `DrawItemFigure`); `PickFaceFigure` draws the face figure, the sex test is equality on the model name (rename `DrawFaceFigure`) |
| decompiles | one name `CString::operator_LPCSTR` | two functions: 0x700AF9 assigns, 0x700D4C appends (rename `operator_plus_assign_LPCSTR`) |

## 8. Function index (one line each; the full purposes are in `batch-ui.tsv`)

Application: `main` 0x456440 WinMain; `App_MessageBox` 0x4560D0; `App_LoadMapAndScript` 0x456140; `App_SetQuitAllowed` 0x455F10;
`App_PostQuit` 0x455F20; `App_HandleInputMessage` 0x457970; `App_WndProc` 0x457D40; `App_RenderFrame` 0x458010; `App_PumpMessages`
0x458220; `Movies_PlayIniList` 0x458270; `FactoryMap_Register` 0x458570; `NamedParty_New` 0x457740; `CGSPlayer_New` 0x457850;
`AppSettings::AppSettings` 0x458870 / `SetDefaults` 0x458DA0 / `ReadFromIni` 0x459100 / `WriteToIni` 0x45A380 / `ReadFromRegistry`
0x45B210 / `WriteToRegistry` 0x45C560 / `~AppSettings` 0x458730; `App_ProbeDirectX` 0x45E310; `MapInfo_Load` 0x45EA20 /
`LoadBoth` 0x45EA00 / `dtor` 0x45E6F0; `NamedFloatMap_Get / Set / Remove / Read` 0x460AF0 / 0x460BC0 / 0x460DB0 / 0x460FE0;
`IsHeroId` 0x4613A0; `Net_TickAll` 0x4122D0; `GSWorld::GetLastError` 0x456360.

Text / system info (2.7): `CTextManager::OpenText` 0x4DEFC0, `CloseText` 0x4DF020, `SeekSection` 0x4DF030, `SkipToSectionEnd`
0x4DF510, `SkipLine` 0x4DF6E0, `ReadWord` 0x4DF770, `ReadLineValue` 0x4DF8D0, `ReadLines` 0x4DFCB0; `CSysInfo::Init` 0x4DDF05,
`CollectInfo` 0x4DDF28.

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
`InitBorderRing` 0x5E8450, `InitDisc` 0x5E8C60, `InitRing` 0x5E91F0, `InitQuadUV` (= InitTriangle) 0x5E99D0, `ResetVertices` 0x5E9F30, `BindOwnArrays`
0x5EA150; `SoABlockArray_SetSize` 0x5EA210, `UvArray_SetSize` 0x5EA380, `IndexArray_SetSize` 0x5EA4C0, `VertexUvMapArray_SetSize`
0x5EA600; `CamCtlClassic::*` 0x5EA810..0x5EABD0; `CamCtlFree::*` 0x5EACB0..0x5EB580; `CamCtlSimple::*` 0x5EB600..0x5EB790;
`UIKeyHandler741154` 0x5EB8A0 / 0x5EB8B0; `UIOrderClickHandler::*` 0x5EB8C0..0x5EBBC0; `IdArray::*` 0x5EBBE0..0x5EBDB0;
`UIUnitPreview::GetDefaultQuad` 0x5EBFB0, `LoadPictureTo256Target` 0x5EC020, `Render` 0x5EC300, `GetOrCreatePortrait` (= DrawItemFigure) 0x5ECA10,
`PickFaceFigure` (= DrawFaceFigure) 0x5ED7B0, `TrimPortraitCache` 0x5F2F20, `TrimFaceCache` 0x5F2F90; `PortraitCache_FindOrInsert` 0x5F3000,
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
