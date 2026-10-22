cb4-new: the unfinished bone crossbow (weapons-um/cb4.blend), unwrapped and textured. 2026-10-22.

cb4-uv.blend / cb4_uv.obj   the model with a new UV layer "um_uv" (the original cb4.blend is untouched): 5 pieces (stock, 2 limbs, 2 strings), uniform
                            texel density (about 1.7x coarser than the vanilla crossbows'), the two limbs and the two strings share their UV islands.
                            ALL islands are inside columns 0-41 of the 128 px texture (u < 0.31): see "the empty zone" below.
dds/                        unhumacb_04.<m>.0.dds / unhufecb_04.<m>.0.dds for the 7 bone materials (bn bd bb bs bg bc br): 128x128 uncompressed ARGB, the format of
                            the old ones, transparent outside the islands. These REPLACE the old bone crossbow's textures of the same names (the old ground model
                            initwecb4weapon.fig uses them with its own UVs, so it will look wrong until it is re-exported with these UVs).
blueprint/cb_04.0.dds        the BLUEPRINT texture (textures.res, replaces res-dds/textures_res_dds/cb_04.0.dds): the coloured texture as a flat grey, 128x128, same layout and
                            footprint, in the way of the vanilla cb_01.0 .. cb_03.0 (about 0.51 x luminance + 96, faint warm tint). Used by the ground / inventory model.
ground/                     the ground / inventory figure (initwecb4weapon.fig + .bon + initwecb4.lnk), made from the same mesh by the importer (viewer --weapon-project ... --replace
                            also writes it): the 5 parts merged, turned 45 degrees and upside down, scale 1.8642 (the vanilla crossbows' numbers), UVs through the 256 atlas,
                            so it takes the dds/ textures and the blueprint. Copy the 3 files into res-unpacked/figures_res/ (they replace the old unfinished ones).
cb4.umweapon                the weapon import project: model, options and shape edits (um-multitool viewer --weapon-project cb4.umweapon --out <folder>, or open it in
                            3D Viewer > Units > Weapon import). Add e.g. "edit=main,0.62,0.86,0.7,0.06" for a thinner handle: both races and every build follow.
png/                        the same as PNG, in the game's orientation (v counts DOWN from the top row). Blender shows the image the other way up:
cb_04.bn.0-for-blender-flipped.png is the decayed-bone one flipped for looking at in Blender.
uv-layout-on-texture.png    the layout over the texture; the red part is the empty zone.

The empty zone: a crossbow's texture is not only the crossbow's. The unit's quiver and arrows are painted with it (rows 63-127, columns 42-83), and the helmet's face
part samples it too (rows 0-49, columns 55-84, measured in the real game: a tan face where the helmet should be dark). Every vanilla crossbow texture keeps all its art in
columns 0-41 and is transparent elsewhere, so every helmet and the quiver see nothing there. Keep any repainted version the same: art only in columns 0-41.
Checked in the game with 8 characters and as many different helmets: none picks anything up.

Orientation: cb4.blend stands on its side (the limbs along Z), so the project says up=down (half a turn about the stock; checked in the real game: with the model's +Z the crossbow hung upside down
and its limbs showed under the skull end). Another crossbow model: up=z (the default) or down in its .umweapon / --up z|down / the panel's Up box.

Use in the game:  um-multitool viewer --weapon-project cb4.umweapon --out <folder>     (the same as: --weapon-import cb4_uv.obj --number 4 --normals smooth)
  The limbs and strings are lowered by about 6 cm to sit at the rig's hinge (see the report): --keep-prod-height keeps the model's own height of the prod
  (then a limb that some female death animations turn over is thrown off the stock).
  copy <folder>/unhuma.mod, unhuma.bon, unhufe.mod, unhufe.bon into resources/universal-mod/res-unpacked/figures_res/ and the dds files into res-dds/redress_res_dds/,
  then repack as usual. Regenerate everything: UM_OUT=<dir> blender -b cb4.blend --python _cpr/art/unwrap_cb4.py, then python3 _cpr/art/paint_cb4.py <obj> <out dir>.
