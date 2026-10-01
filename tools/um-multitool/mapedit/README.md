# Map Editor

The third main tab of `um-multitool`. It shows an `.mpr` terrain with the objects of one or more `.mob` files, lists and describes those objects, shows the mission scripts, and checks everything. The checks include the ones `um.dll` logs when the game opens a map (`MOB_VALIDATION`), so problems are found before the game meets them.

## Files tab

- **Quests**: the quests set in **Settings → Quests**, packed `.mq` files or unpacked quests (`<name>_mq/<name>/map.txt`). There are two kinds of source:
  - **quest folders**: different quests; a later folder's quest replaces the same quest.
  - **language packs** (e.g. `Universal-Mod/lang-packs/eng/maps`, `fra/maps`, `kor/maps`): folders holding the *same* quests in different languages. Their quests come before the folders above, and the list shows how many languages each quest has. Changes that do not depend on the language are written to every pack: the areas, `map.txt`, `quest.ini`/`quest.reg`. Click a quest to open it. It loads its terrain and its zone's base map (both from its `map.txt`'s `#res` line), plus its own `<name>.mob`, which becomes the active map. The files are found in the map folders or next to the quest. Opening a quest also picks its lighting file (`lights<region>.ini`, or `lightscave<region>.ini` under a `#sky cave`) and moves the camera to the first deploy area.
- The open quest's exits are listed with their titles (`## To Ruins`). Click one to go to it. For each exit the view draws its `#deploy` rectangle (green: where the party arrives) and its `#remove` rectangle (red: the area that leaves the map), from their `x1 y1 x2 y2` lines. **Exits** in the toolbar hides them.
- **Resizing the areas**: drag a handle in the view (corners and sides resize, the centre moves; whole units, Shift for tenths), or edit the numbers in the quest panel. **Saving**: **Ctrl+S**, the orange **Save quest** button that appears in the toolbar when something is unsaved, or **Save areas** in the quest panel. It writes the areas into `map.txt` of every copy of the quest (every language pack), changing only those lines. Packed quests are rewritten with every other file kept as it was. **Revert** reloads them from disk.

- **In the map folders**: the `.mpr` and `.mob` files of the map folders set in **Settings → Maps**, for example the game's `maps` folder, then a mod's on top. A mod's file hides the game file with the same name. Click a file to load it: an `.mpr` becomes the terrain, and a `.mob` is loaded on top of the maps already loaded. **Rescan** looks for new files.
- **Terrain** and **Maps**: what is loaded. You can hide a map, move it up or remove it, or open a file outside the map folders by its path (**File...**).
- Maps are loaded in order, each on top of the ones before it, the way the game loads a quest on top of its zone.
- One map is **active** (the radio button, or **Ctrl+T**, which also shows the list while held): only its objects are listed in the Objects tab and can be selected. The others stay visible.
- **U** unloads whichever file was loaded last, terrain or map. When a quest map (`zNqM.mob`) has its `.mq` beside it, the tab offers to load the zone's base map and terrain named in its `map.txt`.
- The loaded files are reopened on the next start.

## View

- Objects stand on the terrain, following ei_maper's rules:
  - An object's `z` is its height above the ground. Units stand on their lowest point.
  - Lights, particles and sounds use absolute positions.
  - The rotation is the stored quaternion (w, x, y, z).
  - Figures are blended by the object's complection and show only the listed body parts.
- The **Layers** dropdown in the toolbar shows or hides the terrain, water, objects, units and markers, and **Selected unit's logic** shows the selected units' paths, points and radii outside logic mode too.
- **Wireframe** alone draws the edges only; with **Textured** on, the edges are drawn over the textured scene.
- With **Lighting** on, the sun casts shadows (Layers → **Shadows**): the terrain and the figures (leaves included) shadow the terrain. The sun follows the hour of the bar under the view: from the east at 6, high at noon, from the west at 18; there are no shadows at night.
- Layers → **Walkability** tints red the ground units cannot walk on, as this editor computes it (2 x 2 unit cells): water deeper than about a knee, ground too steep, and the parts near the ground of objects at least 1.6 units tall (a house's walls, a tree's trunk; not bushes, grass or tree crowns).
- Layers → **Game navmesh (AI_GRAPH)**: the graph stored in the zone's .mob (a node per 4 x 4 units, 8 layers, one per AI class: **Navmesh layer**), as lines to the neighbours a unit can step to (green cheap, red dear) and red squares where a node can go nowhere. **Navmesh differences** marks the nodes where the game's graph and the computed walkability disagree: orange where the game walks and this editor finds it blocked, blue the other way round (the counts are in its tooltip). An out-of-date graph (a map edited without recomputing it) shows many.
- Layers → **Script areas**: the areas the loaded maps' scripts declare (`AddRoundToArea`, `AddRectToArea`), magenta outlines on the ground, labelled with their number and the quest objectives that use them (`QObjArea`, with the objective's title from the quest's .mq).
- Script tab → **Areas**: the same areas, editable: change a circle's x, y, radius or a rectangle's corners and press Enter, or **Place here** then click the map to move the area's centre there; or **Alt + drag** an area in the view. The call in the script is rewritten (one undo step; saved with the map).
- A selected magic trap shows its activation areas (orange circles with a handle at the centre) and cast points (magenta, joined to the trap), like ei_maper. They are selected like logic points (click, Shift+click, rectangle), moved with **G** (X / Y, typed values) and removed with **Delete**; **Ctrl+click** on the ground adds a cast point, **Ctrl+Shift+click** an activation area. Their radius is edited in the details.
- An open quest's exit areas (`#remove`) sparkle with the game's floating stars (`zoneexit`), as in the game.
- Clicks pick what is under the mouse by the figures' actual triangles (not boxes around them), so a unit next to a tall pillar can be clicked.
- Coloured cubes (**Markers**) show objects without a figure:
  - yellow: lights
  - magenta: particles
  - cyan: sounds
  - red: objects whose figure is missing
- Mouse: the left button selects (click; Shift+click adds or removes; a drag draws a selection rectangle, Shift+drag adds to the selection). The wheel click orbits the camera (like ei_maper) around the point under the cursor (an object, else the ground) without re-aiming the view, the right button drags the ground, and the wheel zooms. The orbit and drag buttons can be changed in **Settings → Mouse** (the 3D Viewer uses them too).
- The camera moves freely: its height changes only with the up/down keys, not with the ground. The **speed** slider in the toolbar scales the movement keys (1x by default, saved).
- The **Tools** dropdown next to Layers holds Offset..., Randomize..., Minimap..., MOB parameters..., Undo history, Simulate patrols, Clear patrol paths and Save active MOB as.... The bar under the view holds the lighting (on/off, file, hour), the active map, the ground position under the mouse (x, y, z), what is loading, and the last action's result. The panel can sit on either side of the view (**Settings → Layout**), resized by dragging the bar between them.

### Keys

All keys can be rebound in **Settings → Map Editor**. Plain keys are stored as key positions, not letters: the defaults below are US-keyboard positions, which an AZERTY keyboard shows as Z Q S D for W A S D, and A for Q. Ctrl+letter shortcuts follow the letter instead (Ctrl+Z is Ctrl+Z on AZERTY too).

| Default | What |
| --- | --- |
| W A S D | forward, back, left, right, relative to the camera |
| E / Q | up / down |
| Left Shift (hold) | faster |
| Ctrl+Tab | logic mode on/off |
| Ctrl+T | next active map (the list stays up until Ctrl is released) |
| U | unload the last loaded file |
| Ctrl+R | reset the camera (Frame all) |
| Ctrl+L | lighting on/off |
| Ctrl+S | save every unsaved change: edited maps, the quest's areas, the Quest tab's text (works while typing there) |
| Ctrl+Z / Ctrl+Y | undo / redo: moves, scales, diplomacy changes, the quest's areas (text fields have their own) |
| G | move the selection (Blender's G): it follows the mouse over the ground |
| R | rotate the selection around its centre (default: about Z, the vertical); 1 degree steps, Ctrl for any angle |
| T | scale the selection's complection (like ei_maper's scale tool) |
| X / Y / Z (while moving or scaling) | only that axis; Shift+axis: all but that axis; the same key again: free. The axis shows as a red / green / blue line |
| digits . - , (while moving, rotating or scaling) | an exact value instead of the mouse: `5` goes to each axis in use, `1,-2` gives each its own (X, Y, Z order); degrees when rotating; Backspace erases |
| click / Enter, right-click / Esc (while moving or scaling) | apply / cancel |
| Delete | delete the selection |
| Ctrl+C / Ctrl+V | copy the selection / paste it at the mouse (new free IDs; a unit's patrol points move with it) |
| Ctrl+D | duplicate the selection in place, then move it (G) |
| Ctrl+P | clear the patrol paths of the selected units |
| Ctrl+F | find objects |
| Ctrl+A | select everything in the active map (only units in logic mode); by the letter A on any layout, not the key position |

### Logic mode

**Ctrl+Tab**, or the **Logic** box. It shows the units' behaviours the way ei_maper's logic mode does (`CLogic` in its `objects/unit.cpp`). Only units can be selected in this mode. Each used behaviour of a unit (`UNIT_LOGIC`) is drawn on the ground:

- patrol: the path through its points (small yellow flags on a round base, numbered; closed when cyclic), and each point's look points (blue eyes of Horus facing the camera) with their wait
- guard: the guard radius (orange)
- sentry: the place (green)
- the radius within which the unit calls for help (purple)

Labels show the behaviour and the wait (`UNIT_LOGIC_WAIT`, 15 = 1 second). With a unit selected, its patrol flags, look eyes and guard place are points of their own: a click selects one (Shift+click adds or removes, a rectangle over them takes those inside), and the selected ones go white (flags, guard place) or yellow-rimmed (eyes). **G** moves the selected points like objects (X / Y / Z, typed values; they keep to the ground), **Delete** removes them (a patrol point takes its look points with it; a guard place stays). The left button only selects: nothing is dragged with it. **Ctrl+P** (or Tools → Clear patrol paths) removes every patrol point of the selected units, like ei_maper's "Reset logic paths". **Ctrl+click** on the ground adds a patrol point. Clicking the unit or empty ground goes back to selecting units. Each of these is one undo step. By default only the selected unit's logic shows. **Layers → "Logic: selected only"** off shows every unit of the active map.

### Lighting

**Ctrl+L**, or the **Lighting** box. It lights the map with one of the lighting files set in **Settings → Lighting**: the game's `config/lights*.ini`, which hold sun, ambient and sky colours for each hour. The colours are taken at the map's time of day (`WORLD_SET`'s time, in hours), blended between hours. The bar under the view picks the file and can move the hour (right-click the slider to go back to the map's time).

## Objects tab

The active map's objects in a tree, like ei_maper's list. The tree goes by kind (world objects, units, levers, torches, traps, lights, particles, sounds), then by prototype or template, with counts. The filter matches names, IDs, prototypes, figures and textures. Selecting in the view opens the tree at the object. The details show everything the map stores about it, including each unit's behaviours and the checks' findings.

Units whose texture is the game's `default0` placeholder are dressed the way the game does it, from the database set in **Settings** (`units.udb` and `items.idb` of `databaselmp.res`): the prototype's monster record gives the race's skin (orcs, humans...), the hair and the default equipment, and the unit's own armor and weapon lists in the map replace that equipment when it has any. Helms, plates and leggings show their own meshes, shirts, pants, boots and gloves are laid over the skin as textures, weapons are held, all with their `redress.res` textures. The details say what it was dressed as ("Shown"). A unit whose prototype is not in the database keeps the stripes.

## Editing maps

Maps are edited in place: moving (G) changes the objects' positions and scaling (T) their complection. These are fixed-size fields, so only their bytes change and the rest of the `.mob` stays exactly as it was. **Offset...** (Tools) moves the selection along one axis by an exact typed value (positive or negative). **Randomize...** (Tools; ei_maper's "Randomize parameter") gives each selected object its own random value between a min and a max for one parameter: position X / Y / Z, rotation about X / Y / Z (degrees), complection X / Y / Z, or all three complections together (uniform size variation). "Set" replaces the value, "Add" adds to it (rotations then turn about the world axis, like R). One undo step per click. Moving snaps to tenths (hold Ctrl for free values); a figure's height stays relative to the ground. An edited map shows `*` in the Files tab and the toolbar shows **Save**; unloading or replacing it is refused until it is saved (Ctrl+S) or undone. Undo keeps the last 300 steps. **Tools → Save active MOB as...** writes the active map to another file, which the map then is (its unsaved state and undo steps follow it).

Besides the common fields, the details edit what each kind stores (as ei_maper shows them): for figures the parent ID, "used in script" and shadow; levers' state, number of states, cycled, door, recalculate graph, and how they open (disabled, enabled, sleight of hand, key) with the key ID and sleight; traps' diplomacy group, cast interval, cast once, activation areas (x, y, radius) and cast points; torches' strength, point link and sound; lights' shadow; particles' type; sounds' second range, min and max distance, ambient, music and their files.

**Creating and deleting.** The Objects tab's **New... / Duplicate / Copy / Paste / Delete** do what the keys do. **New...** makes an object from a pattern, like ei_maper: a copy of the selected object (else the map's first world object) with the figure, texture and name picked in its window, put at the mouse or the view's centre. New objects take IDs from the map's first ID range that no loaded map uses. The copies stay in the clipboard across maps, and the clipboard is also a file in the temporary folder, so another running um-multitool can paste them.

**Details.** Every field in the details can be edited, and with **Apply edits to all selected** on, a field goes to every selected object that has it (the ID and item lists never do). For units:

- **Stats imported** (`UNIT_NEED_IMPORT`): on, the game takes the stats from the prototype.
- **Stats**: the 48 values of `UNIT_STATS` (ei_maper's `SUnitStat`): health and mana, speeds, vision, combat, armour, senses, skills. With several units selected, each keeps its other stats.
- **Body parts** (`OBJ_BODYPARTS`): the figure's parts as boxes; none ticked shows the whole figure.
- **Logic**: each `UNIT_LOGIC` record (a unit has several; the ones in use apply): in use, behaviour, guard radius and place, cyclic, help radius, wait, alarm condition and count, aggression, always active, and the patrol points with their look points (position, wait, turn speed). Rewriting a record changes only that record: unedited maps come out byte-identical.

**Find** (Ctrl+F) searches the active map by name: `*` matches any run of characters and `?` any one; without them the text just has to appear in the name. It can be case-sensitive, can also match the figure, prototype and template, and can be limited to a kind and an ID range. **Select all matching** replaces the selection; **Add to selection** adds to it.

## IDs tab: the map's kind

**Tools → MOB parameters...** (ei_maper's MOB parameters) shows the active map's kind and its `WORLD_SET`: wind direction and strength, time of day, ambient and sun light, each editable (one undo step each); a map without a `WORLD_SET` can be given one. It links to the ID ranges, the diplomacy and the script.

### Patrol simulation

**Tools → Simulate patrols** makes the units with a patrol path walk it: at their walking speed (from their stats, 15 ticks a second), on the shortest route around what blocks them (the Walkability grid), turning to each look point and waiting its time, then on to the next point (back to the first when the path is cyclic, else back along it). A small window pauses, speeds up (0.25x to 8x) or stops it. Hovering the menu entry lists the limits: the routes are this editor's, not the game's; units do not avoid one another, fight or react; they glide without animation. Nothing is written to the map, and editing waits until the simulation stops.

**Tools → Undo history** opens a window listing every step undo can take back (oldest first) and those redo can do again (grey); a click goes back or forward to just after that step.

**Quest MOB** switches the map between a zone's own map (`SC_OBJECT_DB_FILE`, with `WORLD_SET`) and a quest's map loaded over a zone (`PR_OBJECT_DB_FILE`), like ei_maper's "is Quest Mob?". The tab says whether the map has a `WORLD_SET`.

## Terrain tab

Edits the loaded `.mpr` like ei_maper's tile brush and tile parameters:

- **Paint tiles in the view**: a left drag paints the brush's tile on the land (or, with **Water**, on the water layer with the chosen liquid material; "no water" removes it from the tile). Each stroke is one undo step. **Alt+click** takes the tile under the mouse. **1**-**8** take a quick tile, **comma** / **period** turn the tile; an outline shows the tile the brush is over. Objects are not selected while painting.
- The terrain's textures are shown as their 8 x 8 tiles: click one to paint with it. Right-click a quick tile slot to keep the brush's tile there.
- The brush's tile type (grass, ground, stone, sand, road, water...), the materials (terrain or water, colour and opacity, self-illumination, wave, warp speed; add or remove) and the animated tiles (first tile, phases) are edited here; they are saved in the terrain's header.
- **Save terrain** (or Ctrl+S with the maps) writes the edited sectors and the header back into the `.mpr`, every other file inside it kept as it was; **Save terrain as...** writes it to another file. An edited terrain cannot be unloaded until it is saved or undone.

## Diplomacy tab

The active map's diplomacy table (`DIPLOMATION`), like ei_maper's: 32 player groups, each cell the attitude of the row's group towards the column's, 0 friend (green), 1 neutral (yellow), 2 enemy (red). Click a cell for the next value, right-click for the previous one. **Both ways** (on by default) sets the opposite cell too. A group's cell with itself is not editable. Hovering a cell shows the groups' names. Changes are undoable and saved with Ctrl+S.

## Quest tab

The files of the open quest as text: `map.txt`, `quest.ini`, briefings, the quest text. A packed quest holds `quest.reg`, the game's binary registry, instead of `quest.ini`; it is shown as INI text and saved back as `.reg` (the conversion reproduces every `quest.reg` of the game and the mod byte for byte). With language packs, a picker chooses whose copy to show. **Save to every language** writes the file to all packs; it is on by default for `map.txt` and `quest.ini`/`.reg`, and off for briefings and texts. Texts keep their encoding (UTF-8, CP1251 or CP949, detected and changeable), and their line endings.

## Minimap

**Tools → Minimap...** exports the loaded terrain seen from straight above, like the game's `<zone>map` textures that ZoneView makes:

- a square image (256 to 2048 px), the map in its top-left corner with its longer side filling the image and y = 0 at the top, as in the game's
- rendered at 4x the size and averaged down, so textures and leaves come out smooth like ZoneView's; named after the `.mpr` file (`zone6x.mpr` -> `zone6xmap`)
- the rest of the square white
- relief lit from the top-left; objects optional, units off by default

It can be saved as `.mmp` (16-bit RGBA5551, the format of the game's own quest maps), `.dds` (uncompressed 32-bit A8R8G8B8, for texture tools or `um-multitool ddsmmp`) and `.png`, any of them.

## Checks tab

These checks work like `um.dll`'s. The script checker is the same file, `resources/universal-mod/um-dll/mob_script_check.hpp`, compiled into both programs.

- Damaged files: node lengths past the end of the file, and corrupted object entries.
- Units' weapon, armor, spell, quick item and quest item lists: truncated lists, bad entry lengths, blank entries, and names the items database does not know.
- The mission script: syntax, argument counts and types, undeclared variables, unknown commands, and item or spell names the database does not know.
- Object IDs and names used by the script, checked against the loaded maps, the quest's base map and the maps the script loads with `AddMob`.
- Object IDs that a map shares with a map loaded before it.

Checks added here:

- The same object ID used twice within one map.
- Figures and textures missing from the figure and texture sources.
- Magic trap spells the database does not know.
- Unit prototypes the database does not know, for units that import their stats (`UNIT_NEED_IMPORT`). Most units carry their own stats, and thousands of units in the shipped maps name prototypes that no database has.
- Objects outside the terrain.

Names are checked against the database set in Settings plus `database.res`, `databaselmp.res` and `databaseadb.res` in the same folder, like `um.dll`.

Click a finding to go to it: the object, or the script line. The same checks run from the command line:

    um-multitool map --check zone17-lmp.mob z17q1.mob [--mpr zone17.mpr]

The exit code is 1 when errors are found.

## Script tab

The script of each loaded map, highlighted:

- keywords and types
- known commands (`um.dll`'s command table)
- the file's own scripts and global variables
- strings, numbers and comments

Lines with findings are tinted in the finding's colour. The line numbers are the ones the checks report and that `um-multitool mobdump` writes to the `.eis` file.

**Edit** turns the view into a text editor (highlighted the same way, on a dark background); **Apply** (or Ctrl+S, which also saves) puts the text in the map as one undoable change, and **Cancel** drops it. The script is written back the way the map stores it (encrypted with the file's own key), so an unchanged script gives the same bytes. While typing:

- a list of completions follows the cursor after two letters: the script commands with their arguments and result (`IsAlive(object) -> float`), the language's words, and the names already written in the text (globals, scripts, variables)
- **Tab** or **Enter** takes the picked one (a command gets its `(`), **Up / Down** pick, **Esc** hides the list; without a list, Tab indents
- above the text, the command whose parentheses the cursor is in shows its arguments
- Esc never throws the typing away (the text box's own "revert" is off)

**Open in a window** moves the script (viewer and editor) into a window of its own beside the program's, placed and resized by the desktop (it has its own Ctrl+S); **Back to the tab**, or closing it, brings it back. **Open in external editor** writes the script (UTF-8) to a `.eis` file in the temporary folder and opens it with the system's editor for that type. Every save there comes back into the map as an undoable change (the tab shows the watched file; click it to copy the path).

## Files

| File | What |
| --- | --- |
| `mob_file.hpp` | `.mob` reader: objects, script, ID ranges, structural problems |
| `mpr_file.hpp` | `.mpr` reader (after ei_maper): header, materials, sectors, tiles, heights |
| `checks.hpp` | the map checks |
| `lighting.hpp` | `lights*.ini` reader |
| `quest_file.hpp` | quests (`.mq` / unpacked): `map.txt`, exits and areas, reading and writing their files, language packs |
| `text_codec.hpp` | UTF-8 / CP1251 / CP949 for the Quest tab |
| `map_scene.hpp` | the GL view: terrain display lists per texture, figures, markers, picking |
| `map_app.cpp` | the tab, and `um-multitool map` |
