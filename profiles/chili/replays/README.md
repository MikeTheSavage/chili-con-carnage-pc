# Replays

Input recordings for the deterministic test mode (`CHILI_DETERMINISTIC=1`). A line is
`<controller sample> <buttons> <analog x> <analog y> [<right x> <right y> [<turn> <pitch>]]`: the second stick
is 128 128 when left out, and `<turn> <pitch>` are the degrees the mouse moved the view in one sample (right
and down positive, decimal), which the dual_analog scheme applies as exactly that angle in every sample the
line covers. The game reads one sample per game frame, so a recording only fits the frame rate it was made
at. The recordings were made at the default `frame_lock` 2 (30 game frames per second in a mission).

**A save profile is needed.** The replays load a profile from save slot `DATA00` of `profiles\chili\game\ms0`
(the memory stick folder of the build tree; make one by playing the build from the tree until the game has
saved a profile). Every run works on a copy of that folder and the scripted dialog picks the slot. No save is
part of this repository.

A scripted run (deterministic, replay, scripted dialogs, script channel) opens its window behind the others
and never activates it, so it does not take the keyboard and the mouse from whatever is in use on the same
desktop. `-Watch` on `run_replay.ps1` and `run_live.ps1`, or `CHILI_WATCH=1`, gives an ordinary window for a
run that is meant to be watched.

The scripted dialog answers every list of save slots the game opens, in order, from `CHILI_AUTO_SAVE_SLOT`
(`focus`, `cancel` or a slot index per dialog; without it `focus`). The game's automatic save writes over the
save of the run's copy (a replay that leaves Profile Options after a change, or ends a mission), and the
answer to a save list or to the delete list ("Delete save games") stands for the player's Overwrite or Delete
as well. No replay of this folder saves.

## State hashes

A deterministic run prints a state hash every `-HashEvery` frames: the processor state (`cpu=`), the guest
clock (`clock_us=`) and a hash of the guest memory. Two builds that print the same hashes on the same replay
ran the game's code the same way. A change to the host that moves guest state changes them; a renderer change
should not. The hashes below were taken with one particular save profile in `DATA00`; a run with another
profile can differ, so compare builds against each other on the same machine with the same save.

A scripted run reads `scripts\unattended.ini`, not the player's `ChiliNative.ini` beside the exe or the mods
folder there, unless `-Environment @{ CHILI_CONFIG = '<ini>' }` names another file; so the hashes do not depend
on what the player has set. It shows the game's own PSP button pictures (`Prompts=psp`) and `Glow=game`, so its
frame pictures are comparable from build to build. Forcing another prompt style (`CHILI_PROMPTS`) or glow
(`CHILI_RENDER_GLOW`) changes the pictures and not the hashes. Texture packs do not change the hashes either,
except a text pack that carries a letter spacing file (`textures\5d0c6d1a663a8515.advances.txt`): the game is
then given other glyph tables and the run goes another way.

`-HashExclude '<address>:<size>,...'` leaves memory ranges out of the hash. `CHILI_TEST_HOOK=<hex guest
address>` counts the calls of one guest function through a host override and prints `[test-hook]` at exit (it
moves the guest clock, so the hashes change).

## mission1_baseline.replay

The fixed measuring run. Boot, skip the intros, load the profile, start Story 1-1 "Rancho Virgillo", skip the
story movie and the in-engine intro, then 67 seconds of play. Recorded through `scripts\run_live.ps1` and
`live_send.ps1`. It ends at sample 2304; stop at frame 2340.

```
scripts\run_replay.ps1 -Exe <chili_recomp.exe> -Replay replays\mission1_baseline.replay `
    -Frames 2340 -Seconds 1500 -HashEvery 100 -OutDir <new folder>
```

About 30 seconds with Direct3D 12 (`-Backend d3d12`, the default renderer), 15 seconds with
`-Environment @{ CHILI_GE_NULL_DRAW = '1' }` (the draws are consumed without drawing; the same state hashes),
and about 9.5 minutes on the software rasterizer (`-Backend software`).

| Game frame | Sample | Guest s | What happens |
|---|---|---|---|
| 61, 79 | 40, 60 | 2.0, 2.4 | Cross twice: skips the two intro movies |
| 116 | 80 | 3.6 | Cross: Load profile (scripted dialog answers DATA00) |
| 159, 179 | 100, 120 | 5.0, 5.7 | Cross: autosave notice, title screen |
| 199 | 140 | 6.4 | Cross: main menu, El Gringo Loco. Frames 201-207 load the mission (two frames over 1 s) |
| 208 | 160 | 7.4 | Cross: Continue on the Story 1-1 briefing; the story movie starts |
| 227 | 180 | 7.8 | Cross: skips the movie; the in-engine intro starts (first frames drawn by the mission renderer) |
| 246 | 200 | 8.4 | Cross: skips the intro; the player has control from about frame 250 |
| 258-1170 | 212-1124 | 8.8-39.3 | Courtyard: stand and shoot, run forward, turn left and right, jump (479, 862), shoot-dodges to the side and back (539, 766, 942, 1089), backpedal, melee takedowns, combo up to 8x |
| 1171-2267 | 1125-2221 | 39.3-75.9 | Inside the trailer: run, jump (1232, 1497, 1580, 2043), dives (1323, 1712, 2100), wall bounce, rifle picked up near 1700 and fired until about 1900, D-pad right at 1919 back to the pistol |
| 2267-2340 | 2221-2294 | 75.9-78.3 | No input |

Windows for frame-time numbers: mission 250-2339, courtyard 250-1170, trailer 1171-2339.
`python replays\frame_stats.py <frames.csv> 250-2339:mission 250-1170:outdoor 1171-2339:indoor`

Reference hashes of version 0.9.0: frame 100 `eac22d2cef3bd844`, 200 `ef0154fd7d8a785a`, 300
`11e5a290021e7ea1`, 1000 `395f85a5735574e4`, 2000 `3fa98e7a08801878`, 2340 `817ded4a1e89e140`
(`clock_us=78328564`). The first voice line starts at frame 252, and the game's sound buffers are part of the
memory hash; `-HashExclude '0x09AD3000:0x9000,0x09B1D000:0x2000'` leaves those buffers out.

With `-Environment @{ CHILI_PAGE = '1' }` (the settings lines in the game's menus, which a deterministic run
otherwise leaves out) the main menu's block and 31 mission blocks are served changed and the run is another
one: frame 100 `b100a453299e6dfd`, 1000 `14bd9330bdd9c8ea`, 2000 `5a10295987c2f0f8`, 2340 `6c35ea4bb3cd2a1e`
(`clock_us=78295094`).

Baseline on the software rasterizer, wall ms per game frame over frames 250-2339 (2,090 frames):

| Build | mean | p50 | p90 | p99 | max |
|---|---|---|---|---|---|
| `rel` (generated code `/O2`) | 256.7 | 244.2 | 324.2 | 389.4 | 441.9 |
| `od` (generated code `/Od`) | 267.0 | 253.4 | 336.2 | 427.0 | 618.0 |

## mission1_intro_cutscene.replay

The same route, but the in-engine intro of Story 1-1 is left to play: no input after sample 183. The
intro takes no input, so it is the same scene at any frame rate, which makes it the like-for-like scene
for comparing `frame_lock` 2 and 1 or two renderers. It starts at frame 227 and hands over to gameplay
between frames 720 and 730 (16.5 guest seconds); run it with `-Frames 740`, about 3 minutes. The crash and
explosion at frames 528-627 are the heaviest frames in either replay: 500-600 ms mean, 1.36 s worst, on the
software rasterizer.

At `frame_lock` 1 the intro starts at frame 268 and a frame is one vblank, so guest time `t` after the start
is frame `227 + t/0.033367` at 30 fps and frame `268 + t/0.016683` at 60 fps. A second disc folder carries
the key (`game\disc0_fl1`: `assets00.naz` hard-linked, the small files copied, `frame_lock = 1` appended to
`configpsp.txt`):

```
scripts\run_replay.ps1 -Exe <chili_recomp.exe> -Replay replays\mission1_intro_cutscene.replay `
    -Disc game\disc0_fl1 -Frames 1250 -Seconds 1500 -PngEvery 20 -OutDir <new folder>
```

## mission1_baseline_fl1.replay

`mission1_baseline.replay` with every sample index from 182 on doubled, so the same input arrives at the
same guest time when the mission runs at 60 game frames per second. Run it with `-Disc game\disc0_fl1`.
Covering all of its input takes `-Frames 4400`, about 20 minutes on the software rasterizer. For the first
15 seconds the player has the same heading and position as the 30 fps run at the same guest time. After that
the fights go differently and he ends up somewhere else, so this is a 60 fps workload, not a frame-for-frame
twin of the baseline.

## dual_analog_move.replay, dual_analog_pitch.replay

The check of the two-stick control scheme (`CONTROLS.md`). Both name `scheme dual_analog`, reach Story 1-1
with the same nine Cross presses as the baseline and then drive the sticks for a few seconds, before the
first enemy reaches the player. A line's last two numbers are the second stick; a byte there is a rate
(`host/dual_analog_controls.hpp`): 196 and 60 are the PSP stick's own full rate, 218 and 38 three times that,
255 the limit.

```
scripts\run_replay.ps1 -Exe <chili_recomp.exe> -Replay replays\dual_analog_move.replay -Frames 416 `
    -HashEvery 100 -PngEvery 2 -OutDir <new folder> -Environment @{ CHILI_INPUT_TRACE = '<folder>\trace.csv' }
```

`CHILI_INPUT_TRACE` writes one row per controller sample: the input, the player script's move vector, turn
and look angles, and the position, heading and elevation of the player and of the camera, read from guest
memory. A row shows the state the game had reached when that sample was taken. Measured on the `od` build
(about two minutes each):

| Replay | Samples | Input | Result |
|---|---|---|---|
| move | 210-229 | left stick right | 2.07 m to the right of the heading, 0.00 m forward, heading unchanged |
| move | 234-253 | right stick X 196 | heading +5.54 to +5.71 degrees per frame (168 degrees per second), position unchanged |
| move | 258-267 | right stick X 38 | heading -16.8 to -17.4 degrees per frame (510 degrees per second) |
| move | 272-275 | right stick X 255 | heading +50 degrees per frame (1,500 degrees per second) |
| move | 280-299 | left stick up | 2.56 m along the heading, 0.00 m sideways |
| move | 304-323 | left stick left, right stick X 196 | strafes and turns at once |
| move | 330-349, Cross at 334 | left stick right | dives 2.35 m to the right; `m_vStickLeft` is (0.99, 0) |
| pitch | 210-225 | right stick Y 196 | `Look.x` 0 to +0.52 rad, camera elevation -8.5 to -35 degrees |
| pitch | 230-261 | right stick Y 60 | `Look.x` to -0.52 rad, camera elevation to +16 degrees |
| pitch | 266-270, 275-278 | right stick Y 218, 38 | `Look.x` changes 0.17 rad per frame (5.2 rad per second) |

Reference hashes of version 0.9.0, move: frame 100 `d55621c0b06800cc`, 200 `d71befb19f6ad41c`, 300
`127a4e9982fd6e12`, 400 `31c8faf280e7b120`, 416 `0ff10757f9a1ccb8`; pitch: 300 `d0f53de83740aa6b`, 336
`732bec32c776df88`, 400 `e0e49e49082bb3ed`, 416 `f27b18278a66a889`. They differ from the baseline's from the
first frame on because the scheme hands the game its own control ids; a change to the control layer changes
them.

## ram_taunt.replay

The scene for checking voice lines: `mission1_baseline.replay` with every sample from 212 on four samples
later, which makes the fight go another way. In a deterministic run the sound has an enemy's taunt
(`uk_G11_taunt_smallvir_006`) at 8.6 s, a dying enemy (`death_vir_01`) at 10.3 s and the player's own taunt
`uk_g11_taunt_ram_008`, 2.7 s long, at 12.6 s (frame 371). Stop at frame 900. Reference hashes: frame 300
`79724597d3332692`, 600 `6db480f595e5eba1`, 900 `c5e207a47c0c9a7c`.

```
scripts\run_replay.ps1 -Exe <chili_recomp.exe> -Replay replays\ram_taunt.replay -Frames 900 -HashEvery 100 `
    -Log sound -OutDir <new folder> -Environment @{ CHILI_AUDIO_WAV = '<folder>\audio.wav' }
```

`CHILI_AUDIO_WAV` in a deterministic run holds the game's sound blocks back to back, the same file every
run; `PSPRECOMP_LOG=sound` prints each stream as it starts and `python tools\sound_assets.py trace
<assets00.naz> <run.log>` names them. `scripts\listen.ps1 speech -SoundOnly` does this run and plays the
sound; `scripts\listen.ps1 speech` plays the replay in real time in the game window, where the fight and
with it the voice lines differ from run to run. Ram's taunts need a kill, 7.5 s since his last taunt and a
one-in-ten draw (`database_ram_taunts`: `MinDelay`, `Chance`), which is why `mission1_baseline.replay` has
none in 67 seconds; shifted by 10 samples instead of 4 it clears the courtyard and gets the scripted line
`uk_G11_speak_ram_001` at 76.8 s.

## The mouse replays: mouse_dive_forward, _back, _left, _right, mouse_play

Replays with a mouse angle in every sample, for the turn through a dive (`CONTROLS.md`, "Where the turn is
applied"). The four dive replays run in one direction from sample 240, turn 3 degrees per sample from 250 to
399, press Cross at 262 and let the stick go at 300: take-off 262-268, in the air to about 363 (slow
motion), on the ground to about 382. `mouse_play.replay` is 38 seconds of varied play with the mouse always
turning (a different angle per sample every ten samples, -4 to 6 degrees): running, a dive forward (sample
300), right (470), back (650) and left (830), a jump (980), shooting, running on with Cross (1150); the men
with shovels knock the player down several times on the way.

They want a player's input settings, which `scripts\unattended.ini` does not have (it keeps the game's own
turning and camera): `replays\mouse.ini` has them.

```
scripts\run_replay.ps1 -Exe <chili_recomp.exe> -Replay replays\mouse_dive_forward.replay -Frames 450 -HashEvery 100 `
    -OutDir <new folder> -Environment @{ CHILI_CONFIG = '<profile>\replays\mouse.ini'; CHILI_INPUT_TRACE = '<folder>\trace.csv' }
python replays\turn_share.py <folder>\trace.csv 250
```

`mouse_play.replay` takes `-Frames 1490`. `turn_share.py` counts the samples with a mouse angle in which the
player's heading changed by exactly that angle (within 0.05 degrees), by move. Measured on the `od` build,
before the exact mouse turn and with it:

| Replay | Samples with a mouse angle | Exact before | Exact after |
|---|---|---|---|
| mouse_dive_forward | 150 | 31 | 128, and 19 on the ground |
| mouse_dive_back | 150 | 27 | 128, and 19 on the ground |
| mouse_dive_left | 150 | 33 | 128, and 19 on the ground |
| mouse_dive_right | 150 | 64 | 128, and 19 on the ground |
| mouse_play | 1150 | 610 (53.0%) | 1036 (90.1%), and 58 more on the ground after a dive |

"On the ground": the samples after a dive of the player's own, where the angle is applied in full and the
game also eases the player to the camera's heading (4.5 degrees over the 19 samples here), so the heading
does not change by exactly the angle; the camera does (3.000 per sample). Of a dive's 150 samples three
more are not exact in the trace: its first frame, where the game sets the player to the camera's heading,
and the frame he leaves the ground in together with the next one, which gives that frame's angle as well.
In `mouse_play` 31 samples are left to the game on purpose (22 on the ground after a blow, 9 in a move that
holds the player in place, state 51) and 9 are not exact (dive starts). The game's own log line gives the
same picture without the trace: `[input] frames with a turn to make=1145: player turned by exactly that
angle=1031, turned on the ground after a dive (where the game also eases him to the view)=74; left to the game
..., by move state: 51 x9, 52 x22; not exactly, by move state: 0 x1, 4 x2, 5 x1, 6 x1, 7 x1, 27 x1, 29 x2`.

Reference hashes with `mouse.ini`: dive forward frame 450 `68bda1e08b133496`, back `d7f4f39f029f22fd`, left
`7654068a57016ddb`, right `83aa0e7516981793`; `mouse_play` frame 1490 `3a6645343a6ea30b`. A change to the
control layer, to the host functions it puts over guest code or to the camera settings changes them.

## settings_tour.replay

The settings inside the game, worked by a pad from the game's own menus (`CONTROLS.md`, "The settings page
inside the game"). It only means something with the feature in the run: `CHILI_PAGE=1`, and a copy of a
settings file for the page to write, never the file itself. The header of the replay lists every press with
its sample. In short: the main menu, Down three times to `Settings`, Cross (the page, sample 251, frame 310);
`Display` (the resolution a step on, the frame rate, `Show frames early`), `Graphics` (`Quality`, `Texture
filter`), `Controls` (`Prompts` to `Keyboard`), `Keyboard & mouse` (`Turn speed` held, `Invert up/down`,
`Defaults` with its question), `Gamepad`, `Textures & mods`, `Sound`; Circle held on the top page (closed at
frame 1005, the menu still on its line); Up three times and Cross into Story 1-1; Start, Down three times to
the pause menu's `Settings`, Cross (the page in the pause box, sample 1340, frame 1386); a locked row tried,
`Gamepad` rows changed, `Prompts` to `Gamepad`, `Button pictures` to `PlayStation`; Start held (closed at
frame 1776, the pause menu still on its line); Circle resumes. Stop at frame 1915.

```
copy scripts\unattended.ini <folder>\tour.ini
scripts\run_replay.ps1 -Exe <chili_recomp.exe> -Replay replays\settings_tour.replay -Frames 1915 -HashEvery 100 `
    -OutDir <new folder> -Environment @{ CHILI_PAGE = '1'; CHILI_PROMPTS = 'auto'; CHILI_CONFIG = '<folder>\tour.ini' }
```

For a larger picture put `InternalScale=4` (or 8) under `[Rendering]` of the copy; `-Scale` sets
`CHILI_RENDER_SCALE`, which locks the page's `Resolution` row ("Set outside the game.").

Reference hashes: frame 100 `b100a453299e6dfd` (the changed blocks, as in the baseline with `CHILI_PAGE=1`),
1000 `c9b11b2521383dc5`, 1900 `cd4b2c78d19ae771`, 1915 `774467b681ac11f4` (`clock_us=64114661`, 1869
samples). They are the same at internal scale 1, 4 and 8, with and without a texture pack, and with or without
the page's own pictures being written (`CHILI_PAGE_AT` with `state` and `shot:<name>` steps): the page draws
on the window and writes the settings file, not the game's memory. What does move them: the page's rows (a
pad press that lands on another row leaves the game's menu a sample earlier or later), the entries, and
anything that moves the baseline. The run's log has one `[settings] frame N: ...` line per change, and the
copy of the settings file differs from `scripts\unattended.ini` in fifteen keys afterwards: `[Rendering]
InternalScale`, `EarlyPresent`, `AntiAliasing`, `Anisotropy`, `TextureFilter`, `[Game] FrameLock`, `[Input]
Prompts`, `[Input.Mouse] SensitivityX` and `InvertY` (written with their defaults by `Defaults`),
`[Input.Gamepad] InvertY`, `DeadzoneLeft`, `TurnSensitivity`, `Glyphs`, `[Mods] DumpTextures` and `[Audio]
BufferMs`.

## Recording another one

`scripts\run_live.ps1 -Replay <prefix> -HashEvery 100` plays a prefix and then takes `live_send.ps1`
commands; `<session>\input.rec` is the new replay. The first 18 input lines of `mission1_baseline.replay`
plus `end 204` are a prefix that reaches gameplay 20 seconds after launch. Things that cost time here:

- Cross skips the intro movies, the story movie and the in-engine intro, one press each. Start does not
  skip the story movie, and in the intro it opens the pause menu. The in-engine intro ignores a press
  0.3 s after it starts and takes one at 0.6 s, so this replay run unchanged at `frame_lock` 1 misses the
  skip (sample 200 comes at 0.3 s there).
- The briefing screen reads the pad in a loop that never ends a game frame. While it waits for Continue the
  sample counter runs at about 2,000 per second of wall time, `frames.csv` stands still and
  `-ThenWaitFrames` times out, so the Continue press belongs in the prefix, not in a live command.
- A mission frame takes a quarter of a second on the software rasterizer, and `tap` ends in neutral until the
  next command arrives. For movement send `hold` after `hold` and finish with `release`.
- The live session prints the same state hashes a replay of its recording must print; compare them.

## frame_stats.py

`python replays\frame_stats.py <frames.csv> <from>-<to>[:label] ...` prints count, mean, p50, p90, p99 and
max wall ms per game frame and the guest frame rate for each frame range of a `CHILI_FRAME_LOG` file.
`--skip-every N` leaves out the frames that also wrote a PNG.

## picture_diff.py

`python replays\picture_diff.py <a.png> <b.png> [<name>=<x0>,<y0>,<x1>,<y1> ...]` compares two frame pictures of
the same size: pixels that differ at all, by more than 2, 8 and 32 levels, and the largest difference, for the
whole picture and for rectangles given in PSP pixels. Exit code 0 when they are equal. Plain Python: for
pictures at internal scale 1.
