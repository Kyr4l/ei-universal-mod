# .umtexts: grouped texts (Universal Mod, not a game format)

A `texts.res` / `textslmp.res` holds one entry per string (3170 in the vanilla `texts.res`). Kept as loose files in
a repository, that is thousands of files. um-multitool groups them by string type instead: one `<TYPE>.umtexts`
file per first word of the entry names, in capitals (`ARMOR`, `WEAPON`, `QUESTITEM`, `STRING`, `PERS`, `ZONE`...):
the vanilla `texts.res` becomes 26 files, the mod's English `texts-eng_res` 13.

```
# um-texts 1
=== ARMOR Gipat_Brigand_Boots Hyena_Hide
Hyena hide boots
Walk confidently with these durable boots.
=== ARMOR Gipat_Brigand_Boots Wolf_Hide
...
```

- `=== <entry name>` starts an entry; its text follows byte for byte (any encoding: CP1251, CP949; CRLF kept) up to
  the next `=== ` line, less the one newline the format adds after it (`\n`, or `\r\n` once an editor converted the
  file's line ends: the texts survive that; a text ending with a lone `\r` would not, and none does).
- A text line that starts with `===` or `\` is written with a `\` before it.
- A folder may mix `.umtexts` files and loose files (one per entry, the old layout); a loose file wins over the same
  entry in a group. Packing gives the same `.res` entries either way (checked on the vanilla `texts.res` and
  `textslmp.res`, and the mod's folders: identical entries).

Commands: `um-multitool texts --unpack <texts.res> -o <folder> [--loose]`, `--pack <folder> -o <texts.res>`,
`--group <folder>` (loose files -> groups), `--set <folder> "<entry name>" <file>` (one entry; makemod writes
`string version_name` with it). The GUI's text editor and text sources read both layouts and save into the group.
