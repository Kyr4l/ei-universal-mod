# Materials of custom terrains

One `<terrain>-materials.tsv` per custom terrain (the name inside the .mpr): one line per tile of its textures,
`tile<TAB>ground A<TAB>ground B<TAB>pattern` (B empty for a plain tile; the pattern = which corners show B at
rotation 0, bits 0 NW, 1 NE, 2 SW, 3 SE). The Map Editor reads them for the Grounds tab (paint by ground) of
terrains the vanilla data does not know; the game does not use them.
