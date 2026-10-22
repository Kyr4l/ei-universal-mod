examples: one small model per weapon kind for um-multitool's weapon import (3D Viewer > Units > Weapon import, or the command line).
They are made by _cpr/art/make_test_weapons.py (python3 _cpr/art/make_test_weapons.py <dir>) and are plain sketches, not finished art:
they show what the importer expects and let you try it in a minute.

  bow-recurve        4 objects: partb (upper limb half), parta (lower), tetivab, tetivaa (a string half each). Stands up along +Z, the string on +X.
  sword-falchion     the whole model is one weapon (pommel, grip, guard, blade as four objects). Stands along +Z, the point up.
  dagger-kris        the same, shorter.
  axe-bearded        shaft, head, cap. The head is on +X here on purpose: the vanilla axes have it on -X and the importer turns the weapon.
  pike-halberd       pole, spear point, axe blade, hook, sock.
  club-mace          shaft, ball, spikes.

Try one:   um-multitool viewer --weapon-project sword-falchion.umweapon --out some-folder
           (copy some-folder/unhuma.mod, unhuma.bon, unhufe.mod, unhufe.bon into resources/universal-mod/res-unpacked/figures_res/ to use it)
Each .umweapon names its model; "number=-1" takes the first free model number of that kind (the log says which).
Every *-texture.png is a flat test texture for its model: 128 x 128, one colour band per object, art only in columns 0-41 (u < 0.31): the quiver, the
arrows and the helmets' faces sample the rest of a weapon's texture. A real bow's texture also needs the quiver and arrows band (rows 64-127, columns
42-83) copied from a vanilla bow's texture, or the archer has no quiver.
UVs are direct texture coordinates (u across, v down from the top row), what the game draws for a hand-held part.

Authoring rules (docs/file-formats/figure-format.md, "Hand-held weapons"):
  - Bows: 4 pieces named as above (or limb1 / limb2, string1 / string2), strings cut in the middle of the bow, each ending where it is tied to a limb tip.
    The bow may stand up (its upper limb is partb) or lie flat (the model's up is the rig's up). The rig turns the limbs about the middle of the grip and the
    strings about points 0.113 further out: a bow of another depth (string to grip) gets its limbs bent until both hold (bow-recurve is 0.18 deep, the
    log says by how much); "keep depth" (--keep-depth) leaves it, and the draw clips then pull the limb halves apart at the grip.
  - Melee: one piece or many objects, any orientation. The importer finds the tip (a sword or dagger ends in a point; an axe, club or spear has its
    weight toward the head), puts the handle through the hand at the vanilla distance from the butt, and takes the model's own size for the weapon
    (Scale changes it; unhufe's weapons are made bigger by the vanilla ratio of its rig).
