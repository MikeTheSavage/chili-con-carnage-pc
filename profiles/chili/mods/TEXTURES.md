# Textures

Every texture of the game can be written out as a PNG, and every texture has a key: a 16-digit name computed
from its pixels that is the same in the archive and in the running game.

What works: extraction, the key, replacement while the game runs (`mods\<name>\textures\<key>.png`, any
size) and a dump of the textures the game actually draws with. Replacement and dump need the Direct3D 12
renderer, which is the default; "Replacing textures" below has the rules.

## Extracting

Python 3.9 or later, no extra packages. `<archive>` is `<disc>\PSP_GAME\USRDIR\assets00.naz`.

```
python tools\extract_textures.py extract <archive> <output folder>            every texture, about 30 seconds
python tools\extract_textures.py extract <archive> <output folder> --mips     also the smaller mip levels
python tools\extract_textures.py extract <archive> <output folder> --jobs 3   decode in three processes
python tools\extract_textures.py list    <archive>                            key, format, size, name of each
python tools\extract_textures.py list    <archive> healthbar                  only names or keys containing a word
python tools\extract_textures.py list    <archive> --block mission01_1.map    only one block
python tools\extract_textures.py one     <archive> ram_shirt shirt.png        one texture, by name or by key
python tools\extract_textures.py one     <archive> 608ab736c7fd64f6 silo.png --level 1      mip level 1 of Silo_Red
```

`extract` writes into the output folder (it refuses a folder inside the disc):

```
data\textures\characters\ingame_ram\ram_shirt.png      one PNG per asset name, in the asset's own folder
data\textures\hud\hud_psp\pistol.png
data\fonts\odmidtext_24.font.png                       the glyph sheet of a font
data\textures\menu\loadscreens\bootscreen.png          a standalone .texture_psp file
...\<name>.mip1.png, .mip2.png                         with --mips
index.csv, index.json
```

A PNG holds the texture's level 0 as the game shows it: 8-bit RGBA with the texture's own alpha.

### The index

One row per key. `index.csv` columns:

| Column | Meaning |
|---|---|
| `key` | the replacement key, 16 hex digits |
| `name` | asset path inside the game, for example `/data/Textures/Hud/HUD_PSP/Pistol.bmp` |
| `blocks` | every block or file that stores it, separated by `;` (`mission01_1.map`, `todpsp_game.main`, or the archive name of a `.texture_psp` file) |
| `source` | `block` (a texture asset), `font` (inside a font asset) or `file` (standalone) |
| `format` | `CLUT4`, `CLUT8`, `DXT1`, `DXT3`, `8888` (the game uses no others) |
| `palette_format` | `8888` for the paletted formats |
| `width`, `height` | size of the PNG |
| `mip_levels` | levels the game uses, level 0 included |
| `bytes` | stored size of the pixel data, all levels |
| `png` | path of the PNG below the output folder |
| `decoded`, `error` | `yes`, or `no` with the reason |
| `other_names` | further asset names with the same content |

`index.json` has the same rows under `textures`, plus `names`, `pngs`, `stored` (how many copies the archive
holds), `declared_height`, `palette_bytes`, `swizzled`, `stored_in` (`payload`, `record` or `segment`) and
`level_keys` (the key each mip level would have if the game bound it as level 0). Its `summary` has the counts.

### What the archive holds

| | |
|---|---|
| Textures stored | 7,333: 7,274 texture assets in the 172 blocks, 2 font glyph sheets, 57 standalone files |
| Distinct textures (keys) | 1,363 |
| Asset names | 1,399; a name never has two different contents, 36 keys have two names |
| Distinct, by format | CLUT4 791, DXT1 219, DXT3 209, CLUT8 87, 8888 57 |
| Stored, by format | CLUT4 3,826, CLUT8 1,587, DXT1 1,015, DXT3 848, 8888 57 |
| Pixels at level 0 | 17,403,520 distinct (69,150,912 counting every stored copy) |
| Failed to decode | none |
| Largest | 512x272 (the standalone pictures); in blocks 512x256 (the front screen logo), 256x512 (the font sheets), 256x256 |

The commonest textures are 16-colour 64x64 (261), 64x128 (158) and 128x64 (151); 740 of the 1,363 have no more
pixels than 64x64. At a high internal resolution that is what shows, and it is what replacement textures are for.

## Names

A texture's name is its asset path, the string the game's models and scripts use. They end in `.bmp` whatever
the stored format. The PNG path is that name in lower case without `.bmp`. The same texture is stored again in
every level block that needs it (the red barrel is in 41 blocks); the index lists it once with all its blocks.

## The key

The key identifies a texture by content, so it does not depend on which block the texture came from, where the
game loaded it, or whether the game copied it into video memory.

It is XXH64 (`chili::xxh64` in `host/state_hash.hpp`, `xxh64()` in `tools/extract_textures.py`) over three byte
strings, chained through the seed:

```
key = XXH64(P, seed = XXH64(T, seed = XXH64(H, seed = 0)))
```

written as 16 lower-case hex digits. All values are those of the GE state when the texture is bound.

**H, 16 bytes**

| Offset | Size | Value |
|---|---|---|
| 0 | 4 | `43 54 4B 31` ("CTK1") |
| 4 | 1 | texture format: `TPSM & 15` (0 5650, 1 5551, 2 4444, 3 8888, 4 CLUT4, 5 CLUT8, 6 CLUT16, 7 CLUT32, 8 DXT1, 9 DXT3, 10 DXT5) |
| 5 | 1 | palette entry format `CMODE & 3` for formats 4 to 7, otherwise 0 |
| 6 | 2 | 0 |
| 8 | 2 | width, `1 << (TSIZE0 & 15)`, little-endian |
| 10 | 2 | height, `1 << ((TSIZE0 >> 8) & 15)`, little-endian |
| 12 | 4 | 0 |

**T, the level 0 texel bytes in row order, without padding**

`address = (TBP0 & 0xFFFFF0) | ((TBW0 << 8) & 0x0F000000)`, `buffer_width = max(1, TBW0 & 0x7FF)`.
`rows = height`, except `rows = 272` for a 32-bit texture (format 3) declared 512x512: that is a screen image,
and the game stores only 272 rows of it.

- Formats 0 to 7: `bits` per texel are 16, 16, 16, 32, 4, 8, 16, 32. `pitch = (buffer_width * bits + 7) / 8`,
  `used = (width * bits + 7) / 8`. For each row `y` from 0 to `rows - 1`, the bytes `b` from 0 to `used - 1` at
  `address + y * pitch + b`, or, when the texture is swizzled (`TMODE & 1`), at
  `address + (((y >> 3) * ((pitch + 15) >> 4) + (b >> 4)) << 7) + ((y & 7) << 4) + (b & 15)`.
  For CLUT4 with an odd width the last byte of each row is ANDed with `0x0F`.
- Formats 8 to 10: a block is 8 bytes (DXT1) or 16. `per_row = max(1, (buffer_width + 3) / 4)` blocks. For each
  block row `r` from 0 to `(rows + 3) / 4 - 1`, the first `(width + 3) / 4` blocks at
  `address + r * per_row * block size`. The swizzle bit is ignored.

For most textures nothing is skipped: when the buffer is as wide as the texture, an unswizzled T is one
contiguous range of guest memory, and a swizzled T (row a multiple of 16 bytes, height a multiple of 8) is that
range unswizzled. Padding exists only in small textures: 19 of the game's (HUD bars and plain colours) are 8 or 16
texels wide in a 32-wide swizzled buffer. If any byte of T or P lies outside guest memory the texture has no key.

**P, the palette entries that occur**

Empty for formats 0 to 3 and 8 to 10. Otherwise take the index of every texel of T (CLUT4: `width` nibbles per
row, low nibble first; CLUT8: each byte; CLUT16 and CLUT32: each little-endian word) and, for each distinct
index value `v` in ascending order, append one palette entry:

```
shift = (CMODE >> 2) & 31     mask = (CMODE >> 8) & 255     start = ((CMODE >> 16) & 31) << 4
entry = (((v >> shift) & mask) | start) & (CMODE & 3 == 3 ? 255 : 511)
```

4 bytes at `palette + 4 * entry` when `CMODE & 3 == 3`, else 2 bytes at `palette + 2 * entry`, read from guest
memory at `palette = (CBP & 0xFFFFF0) | ((CBPH << 8) & 0x0F000000)`, which is where the renderer reads palettes.
Entries no texel uses are not hashed, and neither is anything behind the palette.

This game always uses `CMODE = 0xFF03` (32-bit entries, no shift, mask 255, start 0): the entry number is the
index.

**Worked examples**

```
format 3, 4x1, buffer width 4, texel bytes 00 01 02 ... 0F
  H = 43 54 4B 31 03 00 00 00 04 00 01 00 00 00 00 00     XXH64(H)        = 99775e98aec79ee7
  T = 00 01 ... 0F                                        XXH64(T, above) = bff2edbcb6ccd685
  P = (empty)                                             key             = 23d0f47a4635f1ff

format 4 (CLUT4), 4x2, buffer width 4, not swizzled, texel bytes 10 32 10 10, CMODE FF03,
palette entry i = (16i, 16i, 16i, 255)
  H = 43 54 4B 31 04 03 00 00 04 00 02 00 00 00 00 00     XXH64(H)        = d328666244d0eba7
  T = 10 32 10 10  (indices 0 1 2 3 / 0 1 0 1)            XXH64(T, above) = 3b4a15e0222095b3
  P = 00 00 00 FF 10 10 10 FF 20 20 20 FF 30 30 30 FF     key             = b7713e030a7e39f4
```

`tools/test_extract_textures.py` holds four small textures of the game with their keys.

### Textures the game changes while it runs

- **Render targets.** A texture whose address is a surface the GE draws into (any address that has been a
  frame buffer or depth buffer) has no key. It changes every frame; it is neither dumped nor replaced. The
  renderer already tells these apart (`ge_gpu_backend_is_framebuffer_feedback_texture`).
- **Movie frames.** The video decoder writes each frame into a buffer up to 512 wide that the game then binds
  as a texture. The host knows that buffer: `output_video` in `host/mpeg_runtime.cpp` fills it and passes its
  address to `frame_observer`. A texture inside it has no key either.
- **Anything else that keeps changing.** A texture at one address whose bytes change again and again gets a new
  key each time, so no replacement ever matches it, and a dump would fill the disk. The host should stop
  keying an address after a few changes within a second and say so in the log. Nothing in the archive suggests
  the game does this to its own textures, but it has not been watched.
- **Palette changes.** A paletted texture drawn with a different palette is a different picture and has a
  different key. The index covers the palette stored with each texture. If the game recolours one at run time,
  that state needs its own replacement, found by dumping.

### Textures stored more than once

The same bytes give the same key, wherever they are stored: 7,333 stored textures are 1,363 keys. One
replacement file therefore changes the texture in every level that has it, and under both names where two
asset names share a content (the 36 cases are in `other_names`: the effect sprites that exist as
`/data/Textures/effects/...` and `/data/Textures/Sprites/...`, a few vehicle and prop textures). A different
replacement per level is not possible through the key.

## Replacing textures

```
mods\<name>\textures\<key>.png
```

- The file name is the key and nothing else. To find a key: `index.csv`, `extract_textures.py list <archive>
  <word>`, or the dump below.
- The PNG may have any resolution up to 8192x8192. It covers the whole texture: `width` x `height` of the
  index, so keep that shape. For the standalone screen pictures that is 512x272, of which the game shows the
  left 480 columns.
- Any PNG that Windows can read: 8-bit or 16-bit, RGB, RGBA, grey or palette. Alpha is straight (not
  premultiplied). The game's own alpha matters: HUD pieces, decals, fences and plants are cut out by it.
- The game builds the smaller mip levels itself, down to 1x1 (2x2 box, colour weighted by alpha so cut-out
  edges do not darken), and samples them smoothly, with the filter settings of `ChiliNative.ini`.
- A replaced picture in the 3D world (a pickup, a wall, a shirt) is read from the levels that have about
  one texel per screen pixel, the two nearest blended. The game asks for a level bias of -1.5 on its own
  textures, which sharpened a 480x272 picture; with it a picture eight times the size was read at every
  second texel on a 1920x1088 picture, and fine detail that moves can sparkle. In a 3D draw a replaced
  picture does not get that bias.
- A replaced picture in a sprite (HUD, menus, text) keeps the game's bias and is read from the picture
  itself, full size, at every internal resolution down to 2.8 texels per screen pixel: its edges are as
  sharp as the file has them. One picture serves 1080p and 4K either way; a smaller copy is not needed.
- Mods follow the order of `README.md`: a later mod's file wins. Mods are read when the game starts.
- The Direct3D 12 renderer only; `Backend=software` draws the game's own pixels.

The log says what was found and, at the end, what was used:

```
[mods] textures: 3 replacement pictures in 1 mod folder
[mods] textures: 3 of 3 pictures decoded ahead in 31 ms on 2 threads, 4 MiB kept (store 1024 MiB)
[mods] textures: 3 replaced, 0 unreadable, 0 dumped
```

`replaced` counts the pictures the game drew with. `, N decoded on first use` at the end of the last line
means N of them were drawn before they were decoded: the game's own texture showed for a few frames first.
When the mods replace textures the game draws from its start to the main menu, the second line ends with
`; the 18 of the start to the main menu first, all there in 280 ms` and the last one says how many of the
late pictures were those (`(0 of them of the start)`).
A file that cannot be read (not a PNG, larger than 8192, a name that is not a key) is named on the error
output and the game's own texture stays.

`python tools\sample_texture_mod.py <extracted folder> mods` writes `mods\sample_textures`: the HUD's pistol
icon, Ram's shirt and the purple car's body at four times the size, recoloured, with a fine hatch that only a
picture larger than the game's can hold.

### What happens while the game runs

The key is computed when a texture enters the renderer's cache (once per texture and palette state, not per
draw), and only when a mod has a `textures` folder or the dump is on; otherwise only for textures with the
format and size of the seven the button prompts go into (below; 33 textures and 0.2 ms in 78 seconds of
play, against 314 textures and 1.9 ms for all of them).

Every PNG of the enabled mods is decoded when the game starts, by Windows Imaging
Component on two worker threads below normal priority, while the game boots: first the pictures of what
the game draws from its start to the main menu, in the order it draws them (a list of 48
keys in `ge_texture_mods.cpp`, among them the boot screen, the first loading screen, the two font sheets,
the title and the menus' walls, logos and buttons), then the other screen-shaped pictures (k x 512 by
k x 272: the missions' loading pictures and the trophies' pictures; a mission's loading picture is drawn
before the mission's own), then the largest first, because a large picture is the one a draw would wait
for longest. With 27 loading pictures of 2048x1088 decoded first, as an earlier order did, the font sheets
came after them and the "Loading" line of the first loading screen was drawn with the game's own letters;
now the pictures of the start are there 0.3 seconds after the start and the 27 add 576 MiB and about half a
second to the rest. The pictures of the start are decoded at normal priority, the others below it: on a PC
whose other programs keep every core busy a thread below normal priority gets the processor only every few
seconds. No frame waits for the disk or the decoder. Four large packs (156 pictures, 90 MB
of files, 420 MiB decoded with their smaller levels) take 0.6 seconds; the boot's first text is drawn
1.3 seconds after the start and the first menu screen after four. A texture whose picture is decoded is drawn
with it from its first frame. One that is drawn before its picture is decoded (packs that take longer
than the boot, or more pictures than the store below holds) is drawn with the game's own pixels for the
few frames the decoding takes, then swapped, as every replaced texture was before; its picture goes to
the front of the queue, and at most 24 MB of such late pictures are sent to the graphics card per frame.

Decoded pictures stay in memory for every later use of the same texture, in a store of
`[Mods] PictureStoreMB` megabytes: `auto` (the default) is a sixteenth of the PC's memory, at least 256
and at most 2048; a number from 64 to 8192 is taken as written. The
store is a limit, not a reservation: the memory used is the pictures decoded. When the enabled mods have
more than it holds, pictures are decoded ahead as far as it goes, the rest on first use, and the picture
found longest ago makes room for a new one. The second line of the log above says how many were decoded
ahead and how many wait. A replacement four times the size takes 16 times the memory of the original: a
256x256 texture becomes 5.6 MB with its mip levels. The card's texture cache holds up to 1 GB.

A deterministic run (`CHILI_DETERMINISTIC=1`, `scripts\run_replay.ps1`) waits for the decoding before the
game's first instruction. Its clock is the game's own, so the state hashes are those of a run without
mods, and its frame pictures with mods are the same in every run.

### Seven textures the game paints button pictures into

The on-screen button prompts show the key, mouse button or pad button that is bound, not the PSP's
(`[Input] Prompts`, `CONTROLS.md`). The game makes them by painting its own pictures over parts of seven
textures when they are uploaded:

| Key | Texture | What is painted |
|---|---|---|
| `5d0c6d1a663a8515` | `/data/Fonts/ODMidText_24.font`, 256x512 | 17 cells of 20 to 28 by 30 pixels in rows 330 to 419: the PSP button pictures of the text font |
| `002d10b31e312e9d` | `/data/Fonts/ODMidText_24_Stroke3.font`, 256x512 | the same cells of the outline font, with a dark edge |
| `dc8cc6b510da1d80` | `/data/Textures/Menu/ButtonCross.bmp`, 32x32 | the whole picture |
| `ebff042e1d0cf176` | `/data/Textures/Menu/ButtonCircle.bmp`, 32x32 | the whole picture |
| `e6fd3c31244ec517` | `/data/Textures/Menu/ButtonSquare.bmp`, 32x32 | the whole picture |
| `eabe4d818979a897` | `/data/Textures/Menu/ButtonTriangle.bmp`, 32x32 | the whole picture |
| `e8cc8b252a16c095` | `/data/Textures/Menu/ButtonDpadLR.bmp`, 32x32 | the whole picture |

A mod may replace these like any other texture. Of a replaced font sheet the letters show and the 17 cells
are painted over, at the mod's resolution; a replaced button sprite is painted over as a whole. To see a
mod's own button pictures set `[Input] Prompts=psp`, which paints nothing. The dump always holds the game's
own picture. With the software renderer nothing is painted.

### The letter spacing of a replaced font

The game moves its pen by a letter's cell width less 5, whatever is drawn in the cell, so a typeface whose
`f`, `r`, `t` or `j` is narrower than the game's stands loose ("Rif le"). A pack that replaces the text
font's sheet `5d0c6d1a663a8515.png` can carry the typeface's own spacing in a text file beside it:

```
mods\<name>\textures\5d0c6d1a663a8515.advances.txt

chili-font-advances 1
sheet 5d0c6d1a663a8515 5698284fa98999ad
face Saira
glyph U+0066 11.42 -4.79
glyph U+0072 10.92 -4.54
```

- `chili-font-advances 1` is the first line. `#` starts a comment; fields are separated by spaces or tabs.
- `sheet <key> <hash>`: the key of the font's sheet and the XXH64 of the bytes of the PNG file beside this
  file, 16 hex digits (`tools\extract_textures.py` has the function as `xxh64`). A file that was made for
  another picture is not used, and the log line that says so names the hash the picture has.
- `face <name>`: optional, shown in the log.
- `glyph U+XXXX <step> <offset>`: one line per letter, in texels of the game's 256x512 sheet (PSP pixels at
  text scale 1). The step is the letter's own advance: how far the pen moves after it, between 3 and 48. The
  offset is where the left edge of the letter's cell lies from the start of that advance, between -24 and 8:
  `(step - cell width) / 2` for a letter drawn in the middle of its cell. The game lets neighbouring cells
  overlap by 5 texels, half on each side, and that stays so: a line keeps its place between the edges of its
  box and beside a button picture, and the game's own numbers (step = cell width - 5, offset = -2.5) give the
  game's own spacing. A letter without a line keeps the game's step, and so do the button and medal pictures
  of the sheet.
- The outline sheet `002d10b31e312e9d.png` has no file of its own: both fonts get the same numbers.
- The file is taken from the mod folder whose `5d0c6d1a663a8515.png` is the one in force (the last in the load
  order that has it) and from no other folder. Without the file the spacing is the game's.
- The Direct3D 12 renderer only.

The game is given the two glyph tables of `todpsp_game.main` with these numbers when it loads the block; the
block on the disc is not changed, and a block whose tables are not the game's own is left as it is. The log
says what was done, or why not:

```
[mods] font advances: hd_text (Saira): 149 glyphs in both font tables of todpsp_game.main, 0 unknown; pen steps x0.96 of the game's over those letters (4.9 ms at the start)
[mods] font advances: hd_text\textures\5d0c6d1a663a8515.advances.txt ignored: line 12: the step 99 is not between 3 and 48
[mods] font advances: hd_menu's file ignored, the fill sheet in force is hd_text's
```

Text widths and line breaks follow, in the game and in the settings page. The code: `host/font_advances.hpp`,
served in `host/file_runtime.cpp`; test `chili_font_advances`.

A scripted run with such a file is not the run without it. The game's memory holds other tables and other text
positions, and the test clock counts executed game code (100 microseconds per 2,048 dispatches), which the text
code now takes other paths through: a frame boundary falls 100 microseconds earlier or later, the game's own
step is 34 ms where it was 33 in a few frames (measured with one typeface's file: 12 of 2,339 frames of
`mission1_baseline`, the first at frame 309), and the recorded input then meets another fight. The state
hashes of `replays\README.md` are those of runs without the file (`scripts\unattended.ini` loads no mod). A
file that names the game's own numbers changes neither the clock nor a frame's step.

### Dumping

```ini
[Mods]
DumpTextures=true
```

writes every keyed texture the game draws with, once, as `texture_dump\<key>.png` beside `ChiliNative.ini`
(for a scripted run: `CHILI_TEXTURE_DUMP=<folder>`). A file that is already there is not written again, so
the folder fills up as more of the game is played. The PNG is level 0 exactly as `extract_textures.py`
writes it (512x272 for a screen picture). Use it to find the key of something seen on screen, and for
textures the index does not have: pictures the game builds itself and palette states the archive does not
store with a texture.

`python tools\check_texture_dump.py <dump folder> <extracted folder>` compares a dump with the index.

The code: the key in `host/gpu/ge_texture_key.hpp`, computed in `ge_renderer.cpp` where the decoded mip chain
is handed to `ge_gpu_backend_upload_decoded_texture_chain_packed`; files, worker and PNG codec in
`host/gpu/ge_texture_mods.cpp` (the plan of the start is `ge_texture_ahead_plan`); the swap of a late
picture in `ge_gpu_backend_dx12.cpp` (`apply_ready_replacements`); the sampler of a replaced picture in
`host/gpu/ge_sampler_rule.hpp`; the store's size in `host/mod_files.hpp` (`picture_store_bytes`).

## How the pictures are decoded

As the Direct3D 12 renderer decodes them (`host/gpu/ge_renderer.cpp`, `decode_texture_rgba_into`):

- 8888: bytes are R, G, B, A. 5650, 5551, 4444: red in the low bits, high bits replicated into the low ones.
- CLUT4: low nibble first. Palette entries are 32-bit R, G, B, A.
- Swizzled textures are stored in blocks of 16 bytes by 8 rows.
- DXT blocks are in the PSP's order: four selector bytes, then the two 565 endpoints (red in the high bits, not
  replicated: full red is 248); DXT3 then eight alpha bytes (a nibble per texel, times 17); DXT5 six alpha
  selector bytes and then the two alpha endpoints. DXT1 with `endpoint 0 <= endpoint 1` has three colours and
  transparent black; DXT3 and DXT5 always have four colours.
- The mip chain ends where the renderer ends it: at the last level that is half the one before and has a
  buffer width. Four HUD textures (`ComboFlames*.bmp`) declare six levels and have five.

The host's software renderer (`host/ge_texture_formats.hpp`) differs in two points: DXT3 alpha is the nibble
times 16 (at most 240), and DXT3 and DXT5 keep the three-colour mode. The second never matters here (no DXT3
texture of the archive has such a block); the first means the software renderer's opaque DXT3 texels have
alpha 240 where these PNGs have 255.

In the game data the smallest mip levels of some textures are blank (all zero): every DXT1 level 4 texels wide or
narrower (21 levels) and every CLUT4 level 1 texel wide (5). `--mips` writes them as they are.

## Where textures are stored

Details are in the docstring of `tools/extract_textures.py`. In short: a texture is a small structure with a
pointer to its pixels, a pointer to its palette, and a ready-made list of GE commands (format, swizzle flag,
mip count, and per level the offset, buffer width and size). The pixels are the asset's payload, or inside the
asset record, or, in `.map` blocks, in the block's second segment (the part between the header and the asset
records that `props.py` calls the stream); the palette is inside the record or in that segment. Pixel offsets
in the command list are relative to the pixel data; the game adds the address when it loads the block. The
palette mode is not stored per texture.

## How this was checked

- **All found.** Every block is walked record by record; the number of records equals the block header's count
  and the payload sizes add up to the file, for all 172 blocks (25,032 asset records). Every record of the
  texture type gives one texture (7,274) and none fails to parse. Separately, all 395 archive files are
  searched byte by byte for the start of a texture command list (TMODE, TPSM, TFLT): the hits are exactly those
  7,274, the 2 font sheets and the 57 standalone files. `index.json` records this under `summary.census`.
- **Decoded right.** Sheets of the PNGs were looked at: characters, level surfaces, HUD, fonts, effects, skies,
  menu pictures, load screens, mip chains of the four block formats, the sixty noisiest pictures by a
  roughness measure, and random samples of each storage place. Colours, alpha and layout are right; the
  textures of a captured mission frame (purple car, pistol icon, health bar, chicken cage) match the frame.
- **Same key in the running game.** `captures/ge` (made with `CHILI_GE_CAPTURE_DIR`; not in the repository) has four GE lists of a mission frame with the whole guest
  memory. `extract_textures.py capture <trace.jsonl> index.json` computes the key of every texture those lists
  bind, from guest RAM and video memory and the palette address and mode of the trace: 86 bound textures, 84
  distinct keys, all 84 in the index under the right names (among them textures the game had copied into
  video memory, textures loaded twice at different addresses, the font sheet, and 8-pixel-wide HUD pieces in
  32-wide buffers). The three 32-bit surfaces skipped are render targets.
- **Same key and same pixels in the running game.** Story 1-1 from boot (`mission1_baseline.replay`,
  2,340 frames) with the dump on wrote 203 textures. 201 keys are in the index and their PNGs are equal pixel
  for pixel to the extracted ones: 127 CLUT4, 20 CLUT8, 21 DXT1, 30 DXT3 and 3 of the 512x272 screen pictures
  (which confirms the 272-row rule), among them both font sheets. The other two are textures the game makes
  itself: an 8x8 white square and a blank 512x272 screen buffer. No address was reported as changing.
- `python tools\test_extract_textures.py [<archive>]`: the hash against published values, every format's
  decoder against values worked out by hand, the key's definition, four real textures; with the archive, the
  counts above and the block walk against `props.py`.

## Not covered

- Only Story 1-1 has been dumped. Whether a later level recolours a texture through its palette while it
  runs (which would give keys the index does not have) is not known; a dump of that level shows it.
- A replacement is the same in every level and for every object that uses the texture.
- Textures the game makes while running: render targets, movie frames, anything drawn into a texture. The 14
  `.pmf` movies are video streams, not textures.
- `data/engine/save_assets/icon0.png` and `pic1.png` (save-game pictures) are ordinary PNG files in the
  archive; `naz.py unpack` gets them.
- The 16-bit word at offset 14 of the texture structure is not understood. It appears to be the same for
  textures of the same format, size and mip count, and nothing here needs it.
- Writing textures back into blocks. The key route makes that unnecessary for pictures; models and sounds
  inside blocks still need it.
