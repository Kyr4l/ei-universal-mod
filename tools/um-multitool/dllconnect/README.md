# UM DLL Connector

The multitool tab that connects to um.dll inside the running game. um.dll's DLL server listens on
127.0.0.1 only, so only programs on the same computer can connect.

Turn the server on in `um.cfg`, next to um.dll, then start the game:

| Setting | Default | What it does |
|---|---|---|
| `DLL_SERVER_ENABLED` | false | turns the server on |
| `DLL_SERVER_PORT` | 18888 | the port, the same as in the tab |
| `DLL_SERVER_DEBUG` | false | **development only, dangerous**: allows the commands that change the game: writing memory, breakpoints, `CONSOLE send`, and `CALL` (runs any code in the game) |

**Security.** The server has no password: any program running on the same computer can connect.
- With the server on, any of them can read the game's memory.
- With `DLL_SERVER_DEBUG=true`, any of them can also change the game's memory and run any code inside
  game.exe (`CALL`). The game process then becomes an attack vector for whatever else runs on the
  computer.
- Turn `DLL_SERVER_DEBUG` on only in a safe, controlled development environment, and never on a
  player's install. It is off by default; um.cfg is not shipped with the mod; um.dll writes a WARN
  line to um.log at start while it is on.

The tab's top bar has the port, Connect / Disconnect, and **Connect automatically**. With it on, the
tab retries every 2 seconds, so it connects as soon as the game starts. It is off by default.

## Sub-tabs

**Statistics**
- game.exe's CPU and RAM, with graphs of the last 2 minutes.
- Its address space: it is a 32-bit program, and it crashes when the address space is full,
  whatever RAM is free.
- The computer's RAM.

**Radar**: the units of the map the game runs, on a flat top view of the zone (no 3D). The view is
built from the terrain (.mpr), shaded by height, with water in blue.
- A dot per unit, with an arrow for its facing, and its health and mana bars.
- Colors come from the map's diplomacy table, from each unit's side towards the player: allies
  green, neutral yellow, enemies red. They are grey when the map files are not found.
- Bodies waiting to be looted are drawn as an X. Looted bodies disappear.
- View circles (the sight range) for my character or for all players' characters.
- Players' characters are labelled with their names. "Me" (a white ring) is picked in the **Me**
  list, or automatically: the player nearest the camera's aim point. The other players have a faint
  ring. Colors are from "me"'s point of view.
- **Turn with the camera**: the camera's view direction points up, as on the game's screen. The
  camera is drawn as a blue square with a line to the point it aims at (the **Camera** box).
- The **View** list: the whole map (the default), following my character, following the camera's aim
  point, or free. The wheel zooms (it goes free from the whole map), a drag pans (it goes free), and a
  click on a unit follows that unit.
- Hovering a unit shows its name, kind and file (from the .mob, by ID), its side and attitude, its
  HP, mana, position and facing.
- **Open in the Map Editor** opens the game's terrain, base map and quest in the Map Editor tab.

The files are found in the map folders, the quest folders, or next to the quest (Settings).

**Game console**
- The lines of the in-game console, live, and what is typed there.
- The command line under them types a command into the game window and presses Enter. Open the
  console in the game first: otherwise the keys reach the game as shortcuts. It needs
  `DLL_SERVER_DEBUG=true`.

**Quests**: the quests of the map the game runs, from its files, with the game's state of them.
- Each quest declared by the map's scripts (`QStart` and its `QObj*` calls): its title and description
  (from its .mq: beside the .mob, else the quest folders and language packs of the Settings, English
  first), running or completed.
- Its objectives in order: done, active or not received yet (from the game's quest variables), the
  objective's title (its description on hover), what it asks (the unit, group, area or object it names,
  with the unit's name from the .mob), and for the active ones what can be seen now: whether the unit is
  alive, its HP and how far the nearest player is (and if within sight range), how many of a group are
  alive, how far a player is from an area or an object (7 units for "find").
- Quest variables of other quests (received or done before).
- The map's scripts, running (started, waiting for their condition) or not.
- **Script view**: a map file's script (the list marks the files where something waits), highlighted as
  in the Map Editor, with markers in the margin: "<- waiting" on the condition of each running script
  (highlighted: a script stuck there waits for that condition), "running" / "not running" on each
  `Script` line, "done" / "<- active" on the quest's `QObj` lines. **Go to where it waits** scrolls to it.
  The engine shows no position inside `WorldScript` or an action list.
The state comes from um.dll's `VARS` and `SCRIPTS` (every 2 seconds while the tab is shown).

**Commands**: um.dll's own commands and everything it answers. `HELP` lists them: memory, threads,
breakpoints, `UNITS`, `CONSOLE`... The same commands work from a terminal with
`um-multitool dll "COMMAND" ...`.

## How it works

The protocol is described in `resources/universal-mod/um-dll/dll_server.hpp`.

While a sub-tab is shown, the tab polls um.dll:

| Command | How often | When |
|---|---|---|
| `MAP` | every 2 s | always |
| `UNITS`, `CAMERA` | 10 times a second | while the Radar is shown |
| `CONSOLE lines` | twice a second | while the Game console is shown |

Their answers are routed to the radar and the console, not to the Commands log.

Where these values are in the game's memory is described in `docs/game-memory.md`.

| File | |
|---|---|
| `connector_app.cpp` | the connection (a thread), the sub-tabs, and `um-multitool dll` |
| `radar.hpp` | the map files, diplomacy, the terrain picture and the radar's drawing |
