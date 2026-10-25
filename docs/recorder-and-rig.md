# Frame recorder and rig commands (um-engine 0.14.0, um.dll 1.8.0)

Two tools for finding bugs that only show while playing (shadow flicker, shadows that vanish at a screen edge) and for driving a test game without mouse clicks.
Both live in um-engine (`um_rig.cpp`) and hook the same place as the guard band: the wrapper of `CWorldClient::Render`.

## The frame recorder

Every world frame leaves one record in a ring of the last `Recorder::Frames` frames (600 by default, about ten seconds at 60 FPS; the cost is a few hundred bytes per frame):
what the sun shadows did (drawn / kept the last parameters / none / off), the guard band's pads, the shadowed area (centre, radius, depth range, map size), the light direction, what D7VK did with
the frame (caster draws, result code), the frame time and the time the world pass took, the camera, the game hour.

**When a glitch is on screen, press the recorder key (Pause by default).** The game does not stop; a moment later `um-reports\` beside `um-engine.dll` (the mod folder) holds

* `um-report-<date>-<time>-<ms>.txt`: a summary (frame times, how often the pads / radius / map size / D7VK result changed, the mode counts), a list of the changes between frames, and the table of every frame;
* `um-report-<date>-<time>-<ms>.png`: the world as drawn in the frame of the key press, without the interface (6 MB at 1080p; `Recorder::Screenshot=2` writes half the size).

Send both (and the map / what you did, if it is not obvious). The last row of the table is the frame of the picture; `+ms` is the time before it, so the glitch you saw lies in the rows just above.
At most `Recorder::MaxReports` (20) reports are kept: the oldest pair is deleted when a new one is made (only `um-report-*` files in that folder).

| `um-engine.cfg` key | Default | |
|---|---|---|
| `Recorder::Enabled` | 1 | 0 switches the recorder and its key off (needs a restart: the hook is not installed) |
| `Recorder::Key` | 19 | virtual-key code (19 Pause, 119 F8, 0 = none); only while the game window has the focus |
| `Recorder::Ctrl` | 0 | 1 = Ctrl has to be held too |
| `Recorder::Frames` | 600 | frames in a report (60..7200) |
| `Recorder::Screenshot` | 1 | 1 PNG, 2 half-size PNG, 0 none |
| `Recorder::MaxReports` | 20 | reports kept |

Reading a report: `mode drawn` = shadows with this frame's own parameters, `kept` = the matrices could not be read and the last good parameters were used, `none` = no shadows in that frame,
`off` = not wanted (`SunShadows::Enabled=0`, the game's shadow setting, a cave). `g` (guard) 1 = band drawn, 2 = the matrix stacks were not as expected (no band this frame). `d7vk(draws res)` = caster
draws captured and D7VK's result (0 drawn, 1 no draws, 2 no colour / depth target, 3 shadow map not made, 4 pipeline not made, 5 depth buffer not readable, 6 no light direction).
A flicker usually shows as a row where `mode`, `res`, the caster count, the radius or the map size jumps for a frame or two, or as a frame time spike.

## `SunShadows::GuardLog`

A research switch for um.log: `0` nothing, `1` problems of the guard band (the game's matrix stacks not as expected, once per 256 frames), `2` also every change of the pads and of the shadowed
area (radius, depth range, map size, whether the terrain was found) and, every five seconds, the number of world frames / frames without shadows / failed parameter gathers. The recorder
supersedes it for most purposes; leave it at 0 for normal play. (`SunShadows::Debug` is separate: 1 shadow term only, 2 lit fraction, 4 normals, 5 world position, 10 shadow term, 11 the real result.)

## ENGINE commands of the DLL server

`ENGINE <verb>` forwards to um-engine (`UmEngineRig`); the work runs on the game's thread at the end of its next world frame (5 s at most, an error when no world frame is drawn: menu, loading, minimized).
`ENGINE STATE` is read-only; the others are `[debug]` commands: `DLL_SERVER_DEBUG=true`, `DLL_SERVER_PASSWORD` and `AUTH` first (see `tools/um-multitool/dllconnect/README.md`).

| Command | |
|---|---|
| `ENGINE STATE` | the last world frame: shadow mode, pads, radius, map size, D7VK result, hour, frame time, camera, window focus |
| `ENGINE REC` | writes a report + picture like the key |
| `ENGINE SHOT file.png [half]` | the world as drawn at the end of the next world frame, without the interface (`half`: half size); the path is the game's (Wine: `/tmp/x.png` or `C:\...`) |
| `ENGINE CAM x y [dist [yaw]]` | camera over the map point (x, y), no longer following a unit (the scroll key's job); `dist` 2..200, `yaw` in degrees |
| `ENGINE PAUSE 0\|1` | the two pause flags (0x7AF624 / 0x7AFFCC) |
| `ENGINE HOUR h` | the client world clock to hour `h` (0..24) |
| `ENGINE SCRIPT text` | runs a mob script line in the game's console VM (`WorldScript_Execute`, wrapped as `WorldScript( text )` like the console's `@` prefix, no cheat switch needed): `MoveToPoint(GetObject(id), x, y)`, `Attack(GetObject(a), GetObject(b))`, `GodMode(0, 1)`, `FixWorldTime(11)`... A value is printed with `ConsoleFloat(x)` and read with `CONSOLE lines`. See `docs/game-exe/gs-functions.md` for every command |

The rig scripts in `_cpr/game-src/tools` use them: `dll.py "ENGINE CAM 81.5 63 40" "ENGINE SHOT /tmp/a.png"`.
