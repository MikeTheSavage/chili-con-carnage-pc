# Making your own HD texture pack

The game swaps in a replacement picture for any texture when a mod folder has
`textures\<key>.png` (details in `TEXTURES.md`). The key is a hash, so `tools\build_texture_pack.py`
lets you work with the game's own file names instead.

## 1. Get the originals under their names

"Dump textures.cmd" in the game folder writes every texture of your own disc into `textures\` (1,363
pictures, with `index.csv` listing name, size and format; `tools\dump_textures.py` is what it runs).
Copy them into a work folder:

```
python tools\build_texture_pack.py init "<game folder>\textures" D:\chili_hd_work
```

The work folder mirrors the game's layout, for example `data\textures\hud\hud_psp\pistol.png`,
`data\textures\characters\...`, `data\textures\sets\...`.

## 2. Upscale or repaint in place

- Keep the folder structure and the file names.
- Keep the shape (width to height). Any size up to 8192x8192; four times the original is a sensible start.
- PNG, with the alpha channel kept: HUD pieces, fences, plants and decals are cut out by it.
- Leave alone what you do not want to change. Only pictures that differ from the original go into the pack.
- The screen pictures (load screens, 512x272) show only their left 480 columns in the game.

## 3. Build the pack

```
python tools\build_texture_pack.py build D:\chili_hd_work "<game folder>\mods\hd_textures"
```

It writes `mods\hd_textures\textures\<key>.png` for every changed picture and `pack.txt` listing what each
key is. It also says what it could not place: a name the game does not have, a picture whose shape changed,
a file that is not a PNG. `check D:\chili_hd_work` gives the same report without writing anything.

Run `build` again whenever you change more pictures; pictures you reverted are removed from the pack.

## 4. Use it

The game uses every mod folder in its `mods` folder (or the ones named in `[Mods] Enabled` in
`ChiliNative.ini`). The launcher's "Standard / HD textures" switch turns the pack on and off. With the pack
on, the log says how many pictures were found and how many were used.

Notes:
- A texture stored under two names in the game is one texture: one replacement covers both (36 cases; the
  tool tells you when two of your files land on the same texture).
- Memory: a picture four times the size takes 16 times the memory. The tool prints an estimate. Every
  picture of the enabled packs is decoded when the game starts and kept in memory, up to
  `[Mods] PictureStoreMB` in `ChiliNative.ini` (the default `auto` is a sixteenth of the PC's memory, 256
  to 2048 MB). The log's line `pictures decoded ahead` says how many fitted; the rest are decoded when
  the game first draws them, and the game's own texture shows for a few frames first (`TEXTURES.md`,
  "What happens while the game runs").
- One size serves every resolution: in the 3D world the game reads a replaced picture from the smaller
  level that has about one texel per screen pixel; a HUD, menu or text picture is read full size, as
  sharp as the file. A pack made at eight times needs no second copy for 1080p.
- Only the Direct3D 12 renderer replaces textures.
