# Rig commands (um-engine 0.14.2, um.dll 1.8.1a)

Drive a test game without mouse clicks, and read what the sun shadows did in the last world frame.
They live in um-engine (`um_rig.cpp`) and hook the same place as the guard band: the wrapper of `CWorldClient::Render`.
(um-engine 0.14.0 to 0.14.1 also had a frame recorder with a report key and `Recorder::*` keys; it was removed in 0.14.2.)

## `SunShadows::GuardLog`

A research switch for um.log: `0` nothing, `1` problems of the guard band (the game's matrix stacks not as expected, once per 256 frames), `2` also every change of the pads and of the shadowed
area (radius, depth range, map size, whether the terrain was found) and, every five seconds, the number of world frames / frames without shadows / failed parameter gathers.
Leave it at 0 for normal play. (`SunShadows::Debug` is separate: 1 shadow term only, 2 lit fraction, 4 normals, 5 world position, 10 shadow term, 11 the real result.)

## ENGINE commands of the DLL server

`ENGINE <verb>` forwards to um-engine (`UmEngineRig`); the work runs on the game's thread at the end of its next world frame (5 s at most, an error when no world frame is drawn: menu, loading, minimized).
`ENGINE STATE` is read-only; the others are `[debug]` commands: `DLL_SERVER_DEBUG=true`, `DLL_SERVER_PASSWORD` and `AUTH` first (see `tools/um-multitool/dllconnect/README.md`).

| Command | |
|---|---|
| `ENGINE STATE` | the last world frame: shadow mode, pads, radius, map size, D7VK result, hour, frame time, camera, window focus |
| `ENGINE SHOT file.png [half]` | the world as drawn at the end of the next world frame, without the interface (`half`: half size); the path is the game's (Wine: `/tmp/x.png` or `C:\...`) |
| `ENGINE CAM x y [dist [yaw]]` | camera over the map point (x, y), no longer following a unit (the scroll key's job); `dist` 2..200, `yaw` in degrees |
| `ENGINE PAUSE 0\|1` | the two pause flags (0x7AF624 / 0x7AFFCC) |
| `ENGINE HOUR h` | the client world clock to hour `h` (0..24) |
| `ENGINE SCRIPT text` | runs a mob script line in the game's console VM (`WorldScript_Execute`, wrapped as `WorldScript( text )` like the console's `@` prefix, no cheat switch needed): `MoveToPoint(GetObject(id), x, y)`, `Attack(GetObject(a), GetObject(b))`, `GodMode(0, 1)`, `FixWorldTime(11)`... A value is printed with `ConsoleFloat(x)` and read with `CONSOLE lines`. See `docs/game-exe/gs-functions.md` for every command |

The rig scripts in `_cpr/game-src/tools` use them: `dll.py "ENGINE CAM 81.5 63 40" "ENGINE SHOT /tmp/a.png"`.
