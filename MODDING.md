# Modding

A mod is a folder in `mods\` beside the game. Delete the folder and the mod is gone; nothing in the game's
disc or in its own files is changed. The game reads the mods when it starts.

| You want to | Read |
|---|---|
| Change gameplay values (weapons, health, timers) or replace files of the game's archive | [profiles/chili/mods/README.md](profiles/chili/mods/README.md) |
| Replace textures with your own pictures | [profiles/chili/mods/TEXTURES.md](profiles/chili/mods/TEXTURES.md) |
| Make an HD texture pack from the game's own textures | [profiles/chili/mods/HD_TEXTURES.md](profiles/chili/mods/HD_TEXTURES.md) |

The game folder holds `mods\README.md` and `mods\TEXTURES.md` too.

## In short

- **Order.** Every folder in `mods\` is used, in name order, a later one winning; `[Mods] Enabled=a,b` in
  `ChiliNative.ini` names which ones and in what order. The launcher's Textures & mods page switches mods
  on and off and orders them.
- **Gameplay values.** A mod's `properties.ini` sets values of the engine's entities; `tools\props.py`
  lists the names and the current values from your disc.
- **Files.** A file in a mod folder under the path it has in the game's archive (`assets00.naz`) replaces
  that file. `tools\naz.py` lists and unpacks the archive.
- **Textures.** A mod with a `textures\` folder of `<key>.png` files replaces those textures. "Dump
  textures.cmd" in the game folder writes every texture of your disc with its key (`textures\`, with
  `index.csv`), and `[Mods] DumpTextures=true` writes every texture the game draws while you play
  (`texture_dump\`). Pictures may be larger than the original, up to 8192x8192, and keep their shape.
- **HD packs.** `tools\build_texture_pack.py` turns a folder of edited pictures that keep the game's own
  names into a pack.
- **Tools.** The Python tools are in `tools\` of the game folder (Python 3.9 or later); a sample mod is made by
  `tools\make_sample_mod.py`.

Mods made from the game's pictures or files are made from your own copy: share your tools and your own work,
not the game's files.
