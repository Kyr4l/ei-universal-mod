# 3D Viewer (um-multitool)

The **3D Viewer** tab of um-multitool (formerly the standalone um-modelviewer2): an item model viewer for Evil Islands. Pick an item from the game's database and see its ground/inventory model with the right texture.

## Tabs (inside the 3D Viewer tab)

- **Weapons, Armors, Quick Items, Quest Items, Loot Items**: the rows of that block of `items.idb`.
  - Search by name, and filter by type.
  - Use the arrow keys to walk through the list.
- **Units**: a unit of the database (its Monsters sheet), dressed like the game and the Map Editor dress it: its race's figure, skin, hair, weapons and armour (the armour's textures painted over the skin, inside out).
  - Change anything on top:
    - **Skin**: the race's list, or a **custom skin**: a texture name or any file (.png, .dds, .mmp), with **Reload** to see a skin being painted.
    - **Hair** (a helm hides it).
    - **Complection**.
    - **Weapons**, and each **armour** piece with its material.
  - **Remove all** shows the bare skin. **Reset to the database's** brings back the unit's own equipment.
  - **Animation**: the controls are there but disabled until the game's `.anm` files are decoded.
  - From the command line: `um-multitool gui --viewer units "Human Hero" --skin myskin.png --naked`.
- **Figure**: the model. It's worked out from the database, and you can pick another model of the same family by hand, for example for the mod's `scepter`, which has no model yet.
- **Material**: the materials of the item's class (`M.Type`). The default is the first one in database order that has a texture, so a Metal axe opens in bronze.
- **Texture**: every texture that fits the item.
  - Candidates: the item's own class first, human male first, then the other races, then the greyscale blueprint texture from `textures.res` (a blueprint has no material, hence no colour).
  - "all textures": lists everything, for unique items with odd names.
  - The preview shows the texture itself; **Export PNG** saves it as a PNG, transparency included.
- **Name and description**: the item's in-game name above its stats and its description below, from the text sources. Weapons, armors and wands follow the picked material (`WEAPON <Blueprint> <Material>`); quest items use `QUESTITEM <Name>`, loot `LITEM <Name>` or `MATERIAL <Name>`. When no text exists, the key looked for is shown.
- **Sources** (now in the GUI's **Settings** tab, shared with the Map Editor): where everything comes from.
  - Figures: `figures.res` or a folder.
  - Textures: `textures.res` and `redress.res`, or a folder of `.mmp` or `.dds` files.
  - Texts: `texts.res` and `textslmp.res`, or folders of loose text files (e.g. `resources/universal-mod/res-texts/texts-eng_res`). The top source decides the language; the encoding is detected per text: UTF-8, Korean (CP949) or Russian (CP1251).
  - Database: `database.res` or `databaselmp.res`, whichever holds `items.idb`. In Universal-Mod that is `databaselmp.res`.
  - Layers: add the base game first, then mods on top. The top layer wins. Settings are saved in `um-multitool.cfg` next to the program (an older `um-multitool-viewer.cfg` is read when that file does not exist yet).

In the viewport the mouse works as in the Map Editor: the wheel click orbits, the right button drags (both set in **Settings → Mouse**), and the wheel zooms. **Frame** re-centres the model. **Browse...** next to the texture previews any picture file on the figure (a game texture .dds/.mmp, or .png, .jpg, .bmp, .tga...). With **Textured** and **Wireframe** both on, the edges are drawn over the texture. The item list can sit on either side of the view (**Settings → Layout**), resized by dragging the bar between them.

**Settings → Language** sets the display language of the GUI: English or Russian. The first time the program starts it asks for it in a popup (English is the default); the choice is kept as `LANGUAGE=en|ru` in `um-multitool.cfg`. Texts without a Russian translation stay English. The translations are in `i18n_ru.inc`.

**Settings → Background** puts a picture (any picture: .jpg, .png, .bmp, .tga, .gif, .dds, .mmp...) behind the menus: one for every tab, and one per main tab (File Processing, 3D Viewer, Map Editor, Settings) that wins over it. **Opacity** sets how much it shows over the plain background; the pictures fill the window, cropped to its shape.

**X / Y / Z** (each labelled with the current angle, as turns about X, then Y, then Z) turn the models of the current tab by 45° about the grid's fixed X, Y or Z axis, around their centre, whatever turns came before (right-click turns the other way); **Reset** undoes them. Each tab keeps its own orientation, saved in `um-multitool.cfg` as a quaternion w,x,y,z (e.g. `ROTATION_WEAPONS=0.923880,0.382683,0.000000,0.000000`); older files with three angles are converted on load.

**Export GIF...** saves a looping 360° turn of the shown item, from the current camera angle and zoom and with the tab's rotation.
- Settings: size (typed, capped by the window in the GUI), frames per second (typed, 1-120), rotation speed (degrees per second, so one turn lasts 360/speed seconds), the axis the model spins about (the grid's X, Y or Z, through its centre), direction, and background.
- **Camera from** +X / -X / +Y / -Y / Top / Bottom points the camera straight along an axis (Default restores the 3/4 view), so a spin about X or Y is seen square on.
- **Preview** plays the GIF's exact frames in the viewport, at its size, frame rate and speed. A transparent background shows as a checkerboard.
- Background: transparent by default, or a colour.
- The settings are remembered. The ground grid is left out.
- The size can't exceed the window's height, because the frames are rendered in the viewer's own window.
- GIF counts time in 1/100 s, so frame rates that don't divide 100 are rounded; the dialog shows the real one.
- Transparency in GIF is on or off per pixel, so soft texture edges keep a thin dark-grey fringe.

## How an item finds its model and texture

| Category | Figure | Texture |
|---|---|---|
| Weapons | `initwe<code><TTI>`: axe `ax`, sword `sw`, dagger `dg`, spear `sp`, hammer `hm`, crossbow `cb`, bow `bw` | `redress.res` `<race><code>_<TTI:2>.<material code>.<TTI2>`, e.g. `unhumaax_03.br.0`; `textures.res` `<code>_<TTI:2>.<TTI2>`: the blueprint (greyscale) texture |
| Armors | `initar<code><TTI>`: helm `hl`, plate `pl`, leggings `lg`, shirt `sh`, pants `pt`, boots `bt`, gloves `gl` | same as weapons |
| Quick Items | `initqi<TTI>` | `qitem<TTI:4>`, plus `.<material code>` for wands (their plain `qitem<TTI:4>` is the blueprint) |
| Quest Items | `initqu<TTI>` | `quitem<TTI:4>` |
| Loot Items | treasure `initlitr<TTI>`, material `initlimt<material ID>` | `litem<TTI:4>`, `material<ID:4>` |

These rules resolve every vanilla row except:
- 8 armor rows with TTI −1, which have no model.
- The 5 `instruction` loot rows, which have no ground model.

Material codes are not unique (`br` is both bronze and dragon red bones), so a texture's code is matched against the materials of the item's own class only.

Item figures don't address their texture directly. Their UVs point into the bottom-left corner of a 256×256 atlas the game builds, where a texture W pixels wide takes a W/256 square. The viewer undoes that ("Atlas UVs" in the toolbar). An HD texture pack with textures larger than the originals would break that rule.

## Command line

```
um-multitool gui --viewer <category> <item>                                  open the GUI on that item
um-multitool viewer --list <category>                                         every item and what it resolves to
um-multitool viewer --resolve <category> <item> [--material <name>]
um-multitool viewer --render <category> <item> <out.bmp> [--material <name>] [--texture <name>]
um-multitool viewer --gif <category> <item> <out.gif> [--material <name>] [--texture <name>]   uses the saved GIF settings
um-multitool gui --viewer units <unit> [--skin <file>] [--naked]               a unit, a skin to try on it, without equipment
um-multitool viewer --uvdump <figure> [out.txt]                               every triangle: part, texture number, per corner u v x y z
um-multitool viewer --uvmap <figure> <out.png> [--texture <name|file>] [--size <px>]   the parts' UV regions over a skin
--config <file>                                                               another settings file
```

Categories: `weapons`, `armors`, `quick`, `quest`, `loot` (and `units` for `gui --viewer`). `--render` and `--gif` need a display.

**Painting a skin.** `--uvmap unhuma map.png --texture unhumaskin_00` draws where each body part takes its texels on the 256×256 skin:
- `hd`: the head, stored upside down. `hr.00`: the hair strip the hair meshes use.
- `bd`: the body. `hp`: the hips. Both are stored front and back, upside down.
- `lh`/`rh` 1-3: the arms (3 is the hand). `ll`/`rl` 1-3: the legs (3 is the foot).

`--uvdump` gives the same with each corner's 3D position. Figures face −Y, and their left is +X. Paint, then try the skin on a unit in the Units tab.

## Building

Part of um-multitool: `make linux`, `make win` or `make all` in `tools/um-multitool` builds the one `um-multitool` binary, GUI included.

## Code

| File | Contents |
|---|---|
| `item_db.hpp` | `items.idb` parser, all six blocks |
| `item_resolve.hpp` | database row → figure and candidate textures (no GL) |
| `library.hpp` | sources, database and name indexes (no GL) |
| `scene.hpp` | the OpenGL viewport |
| `ui_items.hpp` | the item tabs |
| `ui_sources.hpp` | the Settings tab's sources and Map Editor keys (drawn by `gui_main.cpp`) |
| `viewer_app.cpp` | the tab inside um-multitool's window, and the `viewer` command-line modes |
| `dds_texture.hpp` | DDS decoder (DXT1/3/5, uncompressed) |
| `gif_writer.hpp` | animated GIF encoder (median-cut palette shared by all frames, LZW) |
| `png_writer.hpp` | PNG writer for the texture export |
| `item_texts.hpp`, `cp949_table.hpp` | item names and descriptions, encoding detection and the Korean table |

The figure, RES and MMP readers come from um-modelviewer; the text-encoding table is um-multitool's `cp1251.hpp`. A Units tab can be added later as one more tab over the same `Library` and `Scene`.

## Fonts

The built-in font only has basic Latin letters. For accents, Cyrillic and Korean, um-multitool merges system fonts behind it:
- **Windows:** Segoe UI or Arial, and Malgun Gothic.
- **Linux:** DejaVu Sans, and Nanum Gothic.

Without a Korean font, Korean text shows as `?`. The variable "-VF" Noto CJK fonts some Linux distributions ship can't be read.
