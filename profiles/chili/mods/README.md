# Mods

A mod is a folder of replacement files and value changes. The game reads them in place of its own; the disc
folder is never changed, and deleting the mod folder undoes the mod.

## Where mods go

In a folder named `mods` beside `ChiliNative.ini` (which sits beside `chili_recomp.exe`). To use another
folder, set `Folder` under `[Mods]` in the ini or the environment variable `CHILI_MODS_DIR`.

```
mods\
  my_mod\
    properties.ini
    data_psp\textures\menu\loadscreens\generated\bootscreen.texture_psp
    data_stream_psp\...
    disc0\PSP_GAME\USRDIR\...
```

| Inside the mod folder | What it does |
|---|---|
| `properties.ini` | Changes gameplay values by name: weapon clip sizes and damage, enemy health, combo timing and so on. See "Changing a gameplay value". |
| `data_psp\...`, `data_stream_psp\...`, `data\...`, `data_baked\...` (any top folder whose name starts with `data`) | Replaces the file of the same path inside `assets00.naz`, or adds it if the archive has none. |
| `facecoll.mat`, `facecollisionmaterial.mat` in the mod's top folder | Replaces the archive's root file of that name. Other top-level files (a readme, say) are ignored. |
| `textures\<key>.png` | Replaces the texture with that key, at any resolution. See "Replacing a texture" and `TEXTURES.md`. |
| `textures\5d0c6d1a663a8515.advances.txt` | The letter spacing of the typeface a replaced text font sheet is drawn in. See `TEXTURES.md`, "The letter spacing of a replaced font". |
| `disc0\...` | Shadows loose files of the disc, for example `disc0\PSP_GAME\USRDIR\configpsp.txt`. A whole replacement archive goes at `disc0\PSP_GAME\USRDIR\assets00.naz`. |

Paths inside the archive are lower case with forward slashes; the game does not care about case, and neither
does the mod folder. Names must be plain ASCII. Empty files are ignored.

Mods are read when the game starts, so restart it after a change. While it runs, the mod files it has opened
cannot be overwritten.

## Which mods load, and in what order

```ini
[Mods]
Enabled=hd_menus,my_tweaks
```

A later name wins over an earlier one, and every mod wins over the disc. Without an `Enabled` line every
folder in `mods` loads in alphabetical order (so `zz_last` beats `aa_first`), except folders whose name starts
with a dot. `Enabled=` with nothing after it loads none.

The start of the game's log says what was picked up:

```
[mods] sample_bootscreen: 1 archive file
[mods] replace data_psp/textures/menu/loadscreens/generated/bootscreen.texture_psp <- sample_bootscreen\data_psp\...
[mods] archive: 1 replaced, 0 added, 395 files, 737527045 bytes
```

## Looking inside the archive

The tools are Python scripts in `profiles\chili\tools` and need nothing beyond Python 3.9.

```
python tools\naz.py list   <disc>\PSP_GAME\USRDIR\assets00.naz
python tools\naz.py unpack <disc>\PSP_GAME\USRDIR\assets00.naz out data_psp/textures/menu/loadscreens/generated/bootscreen.texture_psp
python tools\naz.py unpack <disc>\PSP_GAME\USRDIR\assets00.naz out
python tools\naz.py pack   out new.naz --like <disc>\PSP_GAME\USRDIR\assets00.naz
```

`list` prints the 395 files; `unpack` writes one or more named files, or everything (703 MB) when no name is
given; `pack` builds an archive from a folder. The archive holds 168 `.map` level blocks, 4 `.main` blocks,
146 audio streams, 57 standalone textures, 14 `.pmf` movies and a few small files.

To find out which file a screen or level uses, set `LogFiles=true` under `[Mods]`. The log then names every
archive file the first time the game reads it:

```
[mods] game reads data_psp/textures/menu/loadscreens/generated/bootscreen.texture_psp
```

## Replacing a texture

Every texture of the game, including those inside blocks, can be written out as PNG with
`tools\extract_textures.py`, and replaced by a PNG of any size named after its key:

```
mods\my_mod\textures\6489fecea0109e54.png      the HUD's pistol icon
```

`TEXTURES.md` has the commands, the index, the key, the rules for the PNG and the dump switch
(`[Mods] DumpTextures=true`) that writes the textures the game draws with.

The other way, for whole files:

The 57 standalone textures (boot screen, load screens, concept art) are 512x272, 32-bit. Only the left 480
columns are shown.

```
python tools\naz.py unpack assets00.naz work data_psp/textures/menu/loadscreens/generated/bootscreen.texture_psp
python tools\texture_psp.py to-png   work\data_psp\textures\menu\loadscreens\generated\bootscreen.texture_psp boot.png
   (edit boot.png, keep it 512x272)
python tools\texture_psp.py from-png boot.png work\data_psp\textures\menu\loadscreens\generated\bootscreen.texture_psp ^
    mods\my_mod\data_psp\textures\menu\loadscreens\generated\bootscreen.texture_psp
```

Create the target folder first. For a ready-made example, `python tools\make_sample_mod.py assets00.naz mods`
writes `mods\sample_bootscreen`: the boot screen with red and blue swapped and a MOD label.

## Changing a gameplay value

The game's tunable numbers are properties of named entities, grouped in fragments: `Weapon_Gun_Light` has the
pistol, `DamageModDict` the damage multipliers, `ComboSystem` the combo timer, the `CharacterSettings_...`
fragments the enemies. `mods\my_mod\properties.ini` changes them by name:

```ini
; the block every mission loads (todpsp_game.main)
[DamageModDict]
ShellDamageMod.StructureDamMod = 6

[ComboSystem]
Combo_Ctrl.ComboTimeLimit = 20

; a level block: <archive file name> : <fragment>
[mission01_1.map : Weapon_Gun_Light]
Gun_Light.Clipsize_m = 3
Gun_Light.ShortRangeDamage_m = 60

[mission01_1.map : CharacterSettings_virgillo_brothers_badguy_small]
VB_Small_ModelStat.Health = 1
```

Start a mod by listing what is there (below) and changing one line at a time. A line is `Entity.Property = value`. `#3.Property` names the entity by its number in the fragment (for
entities without a name of their own), and `Property = value` alone changes every entity of the fragment that
has it. A value is a number, a whole number, `true`/`false`, three or four numbers separated by commas, or
text in quotes that is no longer than the old text. `;` starts a comment.

`props.py` prints the lines to copy, with the current values:

```
python tools\props.py fragments assets00.naz                          the fragments of todpsp_game.main
python tools\props.py fragments assets00.naz --block mission01_1.map  the fragments of a level
python tools\props.py list assets00.naz DamageModDict ComboSystem     their values, ready to paste
python tools\props.py list assets00.naz Weapon_Gun_Light --block mission01_1.map
python tools\props.py blocks assets00.naz                             every block of the archive
```

The log shows what was applied, and names the file and line of anything that was not:

```
[mods] tuning: 0 archive files, 5 property lines
[mods] set [mission01_1.map : Weapon_Gun_Light] Gun_Light.Clipsize_m = 3
[mods] data_psp/maps/todpsp_the_game/todpsp_game/shared/mission01_1.map: 3 values changed
[mods] tuning/properties.ini line 14: no entity nosuchentity in damagemoddict sets StructureDamMod
```

Things to know:

- A value belongs to one block. Weapons, enemies and vehicles are stored once per level, so a pistol change
  for the whole game needs a section per level block (`mission01_1.map`, `mission01_2.map`, ...).
- When two mods set the same value, the one that loads later wins. A block that a mod also replaces as a file
  is changed in that file's bytes.
- The names come from the game's own database (5,710 properties). What a value does is not documented; the
  name is the guide. Tried in Story 1-1 so far: `Health = 1` for the two common enemy types (they fall to the
  first hit) and `Gun_Light.Clipsize_m = 3` (play changes after the third shot). The other lines of the
  example are applied the same way but their effect has not been watched.
- Entity references, lists and the properties the engine defines itself (position, model, parent) cannot be
  changed this way.

## Changing an engine setting

The engine reads its settings from `configpsp.txt` on the disc. `ChiliNative.ini` writes over that file in
memory:

```ini
[Game]
; 1 = 60 game frames per second, 2 = 30 as on the PSP
FrameLock=1

[Game.Config]
; any key = value line of configpsp.txt
filesystem = "mod.naz, assets00.naz"
```

A key the file already has gets the new value; a new key is added at the end. The log prints
`[mods] configpsp.txt: frame_lock = 1;`.

Two keys are known to work. `frame_lock` sets the frame rate. `filesystem` is the engine's own list of
archives, searched in the order given: with the line above and a `mod.naz` made by `naz.py pack` at
`mods\my_mod\disc0\PSP_GAME\USRDIR\mod.naz`, a file in `mod.naz` wins over the one in `assets00.naz`. That is a
second way to ship replacement files: one archive instead of loose files.

The engine's string table also has `fixedframerate`, `show_hud`, `draw_particles`, `mipmapbias`,
`max_texture_size`, `screensize`, `ratioxy`, `language_mode` and `loadblocks`. Their effect in this build is
untested, except that `show_hud = false` was tried and the HUD stayed.

## The disc folder stays as it is

The game opens everything under the disc folder read-only and refuses every guest call that would create,
change, rename or remove a file there, mods mounted or not. It will not start with the memory stick folder, the
ini or any output path (frame images, logs, captures) inside the disc folder. The tools refuse to write into a
folder that holds `PSP_GAME\SYSDIR\BOOT.BIN` or over a file that has a second hard link, so a copy of the disc
that shares `assets00.naz` through a hard link (`game\disc0_fl1`) is protected under both names.

## Limits

- Files are replaced whole. A texture, model or sound that lives inside a `.map` or `.main` block cannot be
  swapped on its own yet; the whole block has to be rebuilt. Values inside the blocks are the exception
  (`properties.ini`).
- `texture_psp.py` converts the standalone 32-bit textures in both directions. The paletted and DXT textures
  inside blocks can be extracted (`extract_textures.py`, see `TEXTURES.md`) but not put back.
- `properties.ini` changes values the game data sets. Values the game code computes or hard-codes, and anything
  the scripts change while the game runs, are out of its reach.
- The archive the game sees must stay under 2 GiB (the original is 703 MB; replaced files and changed blocks
  are added at the end).
- Game code is compiled into `chili_recomp.exe`, so a changed `BOOT.BIN` has no effect.
