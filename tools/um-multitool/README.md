# um-multitool

__um-multitool__ (`tools/um-multitool`) is the toolkit we use to build this mod, in a single program for Windows and Linux. It runs as a GUI or from the command line.

- __File Processing__: converts textures (`.dds` <-> `.mmp`) and configs (`.ini` <-> `.reg`), packs and unpacks `.res` / `.mq` archives, dumps `.mob` maps, edits the gameplay database (with checks) and compares two of them (DB tab: Compare with; command line: `um-multitool dbdiff old.xlsx new.xlsx [--cells] [--sheet Monsters] [--csv changes.csv]`, any mix of `.res` / `.xlsx` / `.ods`), the language packs and multiplayer characters (`.mp`).
- __3D Viewer__: browses every item and unit of the database with its model and textures. Units are dressed like in the game, custom skins can be tried on them, and GIFs and UV maps can be exported.
- __Map Editor__: edits `.mob` maps and `.mpr` terrains, quests and their scripts, checks them like the game does, and rebuilds the navmesh like the game does (no more navmesh regeneration at load time).
- __UM DLL Connector__: talks to the running game through `um.dll`: stats, quests and scripts live, and a game console.

Tool windows (Comparison, Log, Find objects, Undo history...) have a __Pop out__ button that moves them into a window of their own, and __Dock back__ to return; Settings > General > Layout > __Separate tool windows__ opens them outside by default.

Releases ship `um-multitool.exe` (Windows), `um-multitool-xp32.exe` (Windows XP), `um-multitool` (Linux, needs the distribution's GLFW) and `um-multitool-x86_64.AppImage` (Linux, GLFW bundled: make it executable, then double-click it or run it from a terminal with the usual arguments). On Linux, `um-multitool install-desktop` adds the tool to the application menu with its icon and bash tab completion; from the AppImage it also links the `um-multitool` command into `~/.local/bin`. The AppImage keeps its settings and log in `~/.config/um-multitool`; the plain binary keeps them beside itself.
