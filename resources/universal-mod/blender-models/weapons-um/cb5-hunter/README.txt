cb5-hunter: crossbow 05, the "hunter crossbow" (the Ingos crossbow until 2026-10-27), from the reshaped ../cb4.blend.

cb5-uv.blend / cb5_uv.obj   ../cb4.blend with the UV layer "um_uv" (_cpr/art/unwrap_cb4.py): 14 islands, all in columns 0-38 of the 128 px texture
                            (the quiver and the helmets sample the rest: see ../cb4-new/README.txt, "the empty zone").
cb5.umweapon                the import project (number 5, replace): um-multitool viewer --config <layers.cfg> --weapon-project cb5.umweapon --out <folder>
                            (layers.cfg: FIGURE_LAYER=<game>/Evil Islands/res/figures.res, then FIGURE_LAYER=<repo>/resources/universal-mod/res-unpacked/figures_res).
png/                        the textures as painted (_cpr/lia-weapons/cb5paint.py: wood stock, metal prod and fittings, cord strings; every island grown
                            4 texels, colour and alpha, inside columns 0-41, so filtering and mip levels do not fray the thin parts into holes), and the blueprint.

Redo it after editing cb4.blend:
  UM_OUT=<work> blender -b cb4.blend --python _cpr/art/unwrap_cb4.py        (flatpak: flatpak run --env=UM_OUT=<work> --filesystem=<repo> org.blender.Blender -b ...)
  um-multitool viewer --config layers.cfg --weapon-project cb5.umweapon --out <work>/imp   (model= the new cb4_uv.obj)
  python3 _cpr/lia-weapons/cb5paint.py <unpacked imp/unhuma.mod> <work>/tex
  python3 _cpr/lia-weapons/cb5install.py <work>/imp <work>/tex <work>/backup      (rigs, ground figure, 14 textures, blueprint into the sources)
then pack as usual.
