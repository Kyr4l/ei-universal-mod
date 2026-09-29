# um-modelviewer2

Item model viewer for Evil Islands. Pick an item from the game's database and see its ground/inventory model with the right texture.

## Tabs

- **Weapons, Armors, Quick Items, Quest Items, Loot Items**: the rows of that block of `items.idb`.
  - Search by name, and filter by type.
  - Use the arrow keys to walk through the list.
- **Figure**: the model. It's worked out from the database, and you can pick another model of the same family by hand, for example for the mod's `scepter`, which has no model yet.
- **Material**: the materials of the item's class (`M.Type`). The default is the first one in database order that has a texture, so a Metal axe opens in bronze.
- **Texture**: every texture that fits the item.
  - Candidates: the item's own class first, human male first, then the other races, then the material-less ground texture from `textures.res`.
  - "all textures": lists everything, for unique items with odd names.
  - The preview shows the texture itself.
- **Sources**: where everything comes from.
  - Figures: `figures.res` or a folder.
  - Textures: `textures.res` and `redress.res`, or a folder of `.mmp` or `.dds` files.
  - Database: `database.res` or `databaselmp.res`, whichever holds `items.idb`. In Universal-Mod that is `databaselmp.res`.
  - Layers: add the base game first, then mods on top. The top layer wins. Settings are saved in `um-modelviewer2.cfg` next to the program.

In the viewport, left-drag to orbit, right-drag to pan, and use the wheel to zoom. **Frame** re-centres the model.

**Rotate X / Y / Z** turns the models of the current tab by 45° about that axis, around their centre; **Reset** undoes it. Each tab keeps its own rotation, saved in `um-modelviewer2.cfg` in degrees (e.g. `ROTATION_WEAPONS=90,0,0`).

**Export GIF...** saves a looping 360° turn of the shown item, from the current camera angle and zoom and with the tab's rotation.
- Settings: size, frames per second, rotation speed (degrees per second, so one turn lasts 360/speed seconds), the axis the model spins about (X, Y or Z, through its centre), direction, and background.
- **Preview** plays the GIF's exact frames in the viewport, at its size, frame rate and speed. A transparent background shows as a checkerboard.
- Background: transparent by default, or a colour.
- The settings are remembered. The ground grid is left out.
- The size can't exceed the window's height, because the frames are rendered in the viewer's own window.
- GIF counts time in 1/100 s, so frame rates that don't divide 100 are rounded; the dialog shows the real one.
- Transparency in GIF is on or off per pixel, so soft texture edges keep a thin dark-grey fringe.

## How an item finds its model and texture

| Category | Figure | Texture |
|---|---|---|
| Weapons | `initwe<code><TTI>`: axe `ax`, sword `sw`, dagger `dg`, spear `sp`, hammer `hm`, crossbow `cb`, bow `bw` | `redress.res` `<race><code>_<TTI:2>.<material code>.<TTI2>`, e.g. `unhumaax_03.br.0`; `textures.res` `<code>_<TTI:2>.<TTI2>` |
| Armors | `initar<code><TTI>`: helm `hl`, plate `pl`, leggings `lg`, shirt `sh`, pants `pt`, boots `bt`, gloves `gl` | same as weapons |
| Quick Items | `initqi<TTI>` | `qitem<TTI:4>`, plus `.<material code>` for wands |
| Quest Items | `initqu<TTI>` | `quitem<TTI:4>` |
| Loot Items | treasure `initlitr<TTI>`, material `initlimt<material ID>` | `litem<TTI:4>`, `material<ID:4>` |

These rules resolve every vanilla row except:
- 8 armor rows with TTI −1, which have no model.
- The 5 `instruction` loot rows, which have no ground model.

Material codes are not unique (`br` is both bronze and dragon red bones), so a texture's code is matched against the materials of the item's own class only.

Item figures don't address their texture directly. Their UVs point into the bottom-left corner of a 256×256 atlas the game builds, where a texture W pixels wide takes a W/256 square. The viewer undoes that ("Atlas UVs" in the toolbar). An HD texture pack with textures larger than the originals would break that rule.

## Command line

```
um-modelviewer2 --list <category>                        every item and what it resolves to
um-modelviewer2 --resolve <category> <item> [--material <name>]
um-modelviewer2 --render <category> <item> <out.bmp> [--material <name>] [--texture <name>]
um-modelviewer2 --gif <category> <item> <out.gif> [--material <name>] [--texture <name>]   uses the saved GIF settings
um-modelviewer2 --open <category> <item> [--screenshot <out.bmp>]
--config <file>                                          another sources file
```

Categories: `weapons`, `armors`, `quick`, `quest`, `loot`.

## Building

- `make linux`: needs the distro's GLFW (`pkg-config glfw3`).
- `make win`: cross-builds the `.exe` with MinGW, using the static GLFW from `../../um-multitool/vendor/glfw-mingw` (built by that tool's `vendor/build-glfw-mingw.sh`).
- `make all`: builds both.

## Code

| File | Contents |
|---|---|
| `item_db.hpp` | `items.idb` parser, all six blocks |
| `item_resolve.hpp` | database row → figure and candidate textures (no GL) |
| `library.hpp` | sources, database and name indexes (no GL) |
| `scene.hpp` | the OpenGL viewport |
| `ui_items.hpp`, `ui_sources.hpp` | the tabs |
| `dds_texture.hpp` | DDS decoder (DXT1/3/5, uncompressed) |
| `gif_writer.hpp` | animated GIF encoder (median-cut palette shared by all frames, LZW) |

The figure, RES and MMP readers come from um-modelviewer. A Units tab can be added later as one more tab over the same `Library` and `Scene`.
