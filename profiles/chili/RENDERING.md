# Rendering

The game draws with Direct3D 12 by default. The CPU rasterizer the port started with is still there as the
reference: it is about nine times slower in a mission and is what a picture is compared against.
Code: `host/gpu` (the renderer, lifted from the framework's VCS profile, see `host/gpu/UPSTREAM.md`;
`ge_gpu_post_dx12.hpp` has the anti-aliasing and ambient occlusion passes, `ge_gpu_glow_dx12.hpp` the game's
glow at the PSP's size, `ge_texture_mods.cpp` texture replacement), `host/gpu_video.hpp` (start-up and
screenshots), `host/render_settings.hpp` (settings), and the wiring in `host/main.cpp`.

## Settings

`[Rendering]` in `ChiliNative.ini` beside `chili_recomp.exe`. A run by a player adds the section, commented
out, to a file that lacks it.

The first value is the default. Cost is per game frame on the RTX A4500 at `InternalScale=4` (1920x1088),
measured as described under "What the settings cost"; the frame budget is 33 ms at 30 frames per second and
16.7 ms at 60.

| Key | Values | Meaning | Cost |
|---|---|---|---|
| `Preset` | `high`, `medium`, `low`, `custom` | How much the picture may cost. For now it limits what `InternalScale=auto` picks: `low` 2 (960x544), `medium` 4 (1920x1088), `high` and `custom` 8. A key given in the file wins over the preset | |
| `Backend` | `d3d12`, `software` | Graphics card, or the CPU reference renderer (the window title then says so) | software is about nine times slower |
| `InternalScale` | `auto`, `1` to `8` | Rendering resolution in multiples of 480x272. `auto` fits the monitor: 4 at 1080p, 6 at 1440p, 8 at 2160p; at most what the preset allows, and at most 6 on a card with under 6 GB, 4 under 3 GB, 2 under 1 GB | against scale 1: +0.6 ms at 4, +2.5 ms at 6, +2.7 ms at 8; about 10 MB of card memory times the scale squared |
| `AntiAliasing` | `fxaa`, `off`, `msaa2`, `msaa4`, `msaa8` | `fxaa`: the edges of the 3D scene are smoothed after it is drawn; HUD and text stay sharp. `msaaN`: every surface is drawn with N samples per pixel (the edges of plants and fences, which are cut out by alpha, are not smoothed by it) | `fxaa` +0.1 ms; `msaa2` +1.7 ms, `msaa4` +2.6 ms, `msaa8` +2.9 ms, and N times the memory of the surfaces |
| `Anisotropy` | `16`, `8`, `4`, `2`, `1` | Anisotropic filtering of the textures of the 3D scene: ground and walls seen at a slant stay sharp without shimmer. `1` is the game's own filter | none measurable |
| `TextureFilter` | `game`, `linear`, `nearest` | Textures of the 3D scene as the game asks, always smoothed, or always blocky. The game asks for smoothing everywhere, so `linear` changes nothing in the first mission | none |
| `SSAO` | `off`, `low`, `high` | Ambient occlusion: corners, creases and the ground under things are darkened a little. `low` works at half resolution with 8 samples, `high` at full resolution with 16 | `low` +1.3 ms, `high` +1.8 ms (+3.9 ms at scale 8) |
| `Glow` | `smooth`, `game`, `off` | The game's full-screen glow. `smooth`: its passes are drawn at the PSP's own size and the result is enlarged smoothly. `game`: drawn at the rendering resolution, as the game's draws say, which above scale 1 gives a lattice of squares and ghost edges. `off`: no glow. See "The glow" | on the card `smooth` costs 0.1 to 0.3 ms less than `game` without multisampling and 0.8 ms (scale 4, msaa2) to 3 ms (scale 8, msaa4) less with it; `off` 0.1 to 0.2 ms less again |
| `Fullscreen` | `false`, `true` | Start in borderless full screen | none |
| `EarlyPresent` | `true`, `false` | Show a mission's scene as soon as it is drawn, one game frame before the game's own copy to the screen | none |
| `GeThread` | `true`, `false` | Draw the game's display lists on a second processor core while the game computes its next frame. `false`: one core does both | `true` takes about 5 ms a frame off the game thread |
| `MSAA` | `1`, `2`, `4`, `8` | Older spelling of `AntiAliasing=off`, `msaa2`, `msaa4`, `msaa8` | |

`[Mods] DumpTextures=true`, texture replacement (`mods\<name>\textures\<key>.png`) and the memory its
pictures may take (`[Mods] PictureStoreMB`) are described in `mods/TEXTURES.md`; with neither a dump nor
a replacement in use they cost nothing.

F11 or Alt+Enter switches between the window and borderless full screen while playing. The picture keeps
the PSP's 30:17 shape inside the window. The window opens at the largest whole multiple of 480x272 that
leaves a fifth of the desktop free (960x544 on a 1366x768 desktop, 1440x816 at 1080p, 2880x1632 at 2160p)
and can be resized freely. The internal resolution is fixed when the game starts and does not follow the
window: a picture larger than the window is averaged down (up to 8 to 1), a smaller one is stretched
smoothly.

For scripts, the environment wins over the file: `CHILI_RENDER_PRESET`, `CHILI_RENDER_BACKEND`,
`CHILI_RENDER_SCALE`, `CHILI_RENDER_MSAA`, `CHILI_RENDER_AA`, `CHILI_RENDER_ANISOTROPY`, `CHILI_RENDER_FILTER`,
`CHILI_RENDER_SSAO`, `CHILI_RENDER_GLOW`, `CHILI_RENDER_EARLY_PRESENT`, `CHILI_RENDER_GE_THREAD` (`force`
turns the list thread on in a deterministic run too, to test the thread), and `CHILI_RENDER_STRICT=1` (stop
when Direct3D 12 cannot start instead of falling back to software, and stop on a draw the renderer cannot
decode instead of skipping it). `scripts/run_replay.ps1` and `scripts/run_live.ps1` take
`-Backend d3d12|software`, `-Scale` and `-Msaa`;
`-Backend d3d12` sets the strict switch. An unattended run (deterministic, scripted dialogs or a replay)
reads `scripts/unattended.ini`, which sets `AntiAliasing=off`, `Anisotropy=1`, `Glow=game` and
`GeThread=false`, renders at scale 1 with `InternalScale=auto` and never starts in full screen: its frame
images are the game's own picture at 480x272, like the software renderer's.

The start of `run.log` says which renderer runs (`[render] Direct3D 12 GE backend active on <card> scale=..`),
the end what it did (`[render] d3d12 draws=.. approximated: ..`). GE state the renderer approximates is named
there and logged once when first met (`[gpu] unsupported GE state: ..`).

## How it is wired

- Every GE `PRIM` goes to the lifted front end (`GeRuntime::draw_backend`), which queues draws for the GPU.
  Guest VRAM is not drawn to; the game's four surfaces (LCD `0x04000000`, scene `0x040E8000`, composite
  `0x040D8000`, the bloom and depth sheet at `0x04088000`) live as GPU textures of 512x512 times the scale.
  With `Glow=smooth` or `off` the bloom is drawn on a second sheet of 512x512 at every scale ("The glow").
- A picture is presented when a list ends with the displayed buffer selected (`GeRuntime::list_end`): the
  single list of a menu, or the list that copies a finished scene to the screen in a mission. `Present(0)`
  with tearing allowed; the game thread never waits for the display. Frame pacing stays with the real-time
  pacer on guest time. The software renderer shows its picture at the same moment, through GDI.
- Early present: a list with draws that ends on the composite `0x040D8000` while the game's "copy due" byte
  (`0x08DD03E0`) is set is a finished scene, HUD and fades included, and is presented at once from that
  surface; the game's copy to the LCD a frame later presents the same picture again. `CHILI_RENDER_EARLY_PRESENT`
  overrides the ini; `scripts/unattended.ini` switches it off so frame images of the two renderers show the
  same frame. Frame images and state hashes are numbered by frame boundary, `frames.csv` rows by the interval
  that ended there, which is one less.
- Movie pictures are the one thing the CPU writes into VRAM. Each decoded picture is also handed to the
  renderer as the content of its surface (`MpegRuntime::set_frame_observer`), in order with the draws.
- Screenshots (`PSPRECOMP_FRAME_DUMP`, `CHILI_FRAME_PNG_EVERY`, the live channel's `capture`) read the
  surface the window shows back from the GPU. A `.png` is written at the internal resolution.
- The process is per-monitor DPI aware so the swapchain has real pixels; the window is 960x544 at 100%
  scaling and grows with the desktop's scale. The native save and message dialogs are created as
  DPI-unaware windows, which Windows scales.

## The frame path

What a game frame costs the thread the game runs on, and when its picture reaches the window. Code:
`host/ge_runtime.hpp` (the list thread, the early draw), `host/frame_probe.hpp` (the hook at the frame
boundary), `host/gpu_warm_up.hpp`, `host/frame_timer.hpp` and `host/frame_costs.hpp` (the stall log), and
the wiring in `host/main.cpp`.

- **The list thread** (`GeThread`). A PSP's GE draws while the CPU goes on, and the game is written for
  that: it hands a frame's scene list over, computes the next frame, and only then waits for the list
  (`sceGeListSync`, `sceGeDrawSync` at the start of its frame-end function); its scene lists alternate between
  two buffers. With the thread on, `sceGeListEnQueue` only finds the list's end, queues the list and returns;
  the thread runs it with every draw, texture decode, submission and present. The finish callback is
  delivered at the enqueue, as before, and list ids are the same. Everything else that touches the renderer
  (movie pictures, the displayed buffer, screenshots) goes through `GeRuntime::render_call`, in order with the
  lists. Deterministic runs, the software renderer and runs that capture, trace or log lists draw where the
  game hands a list over (`scripts/unattended.ini` has `GeThread=false`); nothing a list does changes guest
  state either way.
- **The early draw.** The game closes a frame's scene list, then waits for the vertical blank (twice at 30
  frames per second) and only then enqueues it, so the whole wait sat between the pad read and the picture:
  16.7 ms at 60, 33 ms at 30. At the frame boundary the list the game is about to enqueue is complete, so with
  `EarlyPresent` it is run there (`GeRuntime::run_early`; the list is the one `GfxInternal_PSP::EndFrame`
  picks at `0x088257B8`) and the game's enqueue only collects its id and its finish callback. The game's copy
  of that scene to the LCD then shows nothing new: its two draws go out with the next scene, without a
  submission and a present of their own. Menus, loading screens and movies keep the game's order. The log
  says how often it happened (`[render] scenes shown early: .., drawn at the frame boundary ahead of the
  game's enqueue: ..`).
- **Warm-up.** Direct3D 12 builds a pipeline state the first time a draw needs one. The keys and the
  surfaces a run met are written to `ChiliNative.gpucache` beside the exe at exit and created at the next
  start, before the game runs; a built-in list covers a first start (boot, menus, movies, Story 1-1). Delete
  the file to start over; a file from another renderer version is ignored.
- **Textures.** Decoded textures are placed in 32 MB heaps (a committed resource each cost 0.4 to 0.6 ms: the
  intro of Story 1-1 shows 78 new textures in one frame, which took 33 to 46 ms). CLUT8, 8888 and DXT
  textures are decoded a row or a block at a time; the decode worker pool is off. The pictures of
  texture mods are never decoded on these threads: all of them are decoded when the game starts, on two
  workers of their own below normal priority (`ge_texture_mods.cpp`), so a replaced
  texture is uploaded as its picture the first time it is drawn and the game's own picture does not show
  first. What the game draws from its start to the main menu is decoded before everything else,
  so the font sheets and the title are ready for the boot whatever else the packs hold
  (`mods/TEXTURES.md`).
- A draw that samples the surface it draws into (the game's bloom passes, 25 per mission frame) gets a copy
  of the part it reads that was drawn into since the last copy, not of the whole sheet.

Diagnostics, all printed at exit into `run.log` (`last_run.log` in the play folder):

- `[frame-timer] .. work_ms_p50=.. work_ms_p99=..`: host time the game thread worked per game frame.
- `[stall] ..`: every frame with more than 25 ms of work, with what happened in it: lists and draws, the
  wait for the list thread, textures decoded, pipeline states and surfaces created, time in present and
  waiting for the graphics card, file reads, the sound hand-off, and the rest (recompiled game code).
  `frames.csv` (`CHILI_FRAME_LOG`) has `ge_ms`, `submit_ms`, `file_ms` and `ge_wait_ms` per frame.
- `[render] pad read to present, mission pictures: ..`: from the pad read to the present call of the
  picture drawn from it.
- `[render] gpu time per submission at scale N: ..`: what the graphics card needed, from timestamp queries.
  A mission frame is one submission.
- The sampling profiler (`CHILI_SAMPLER`) samples the game thread only. To profile the drawing, run with
  `GeThread=false` so the lists run on that thread.

Measured with `mission1_baseline_fl1.replay` at `FrameLock=1` in real time with sound, as a
player runs it (raised priority, early present), frames 320 to 4399, each run under the timing lock with no
other build or game instance (two idle Python processes of other work were alive):

| Build | work mean | p50 | p99 | max | frames over 16.7 ms | pad read to present p50 / p99 |
|---|---|---|---|---|---|---|
| before (with the stall log) | 10.9 ms | 10.3 | 17.9 | 27.0 | 111 of 4,080 | 16 / 21 ms |
| textures, list words, one submission | 9.8 | 9.3 | 15.9 | 25.1 | 23 | 16 / 20 |
| list thread, no early draw | 6.2 | 6.0 | 10.8 | 14.7 | 0 | about 22, computed: the pad is read 5 ms earlier and the picture comes at the same time |
| list thread and early draw (the default) | 4.4 | 4.3 | 7.5 | 10.2 | 0 | 9 / 15 |
| the same with the region copy | 3.7 | 3.6 | 7.4 | 9.9 | 0 | 8 / 14 |
| `GeThread=false` with the early draw | 10.7 | 10.3 | 16.4 | 23.9 | 35 | 10 / 20 |

The "before" run had two other game instances alongside for its first 30 seconds; the earlier quiet figures
for that build were p50 9.2 to 9.7 and p99 15.4 to 16.5 (`AUDIO.md`). The list thread itself needed 4.9 ms
per frame at the mean and 8.6 at p99, with 0.8 ms for the submission. At 30 frames per second
(`mission1_baseline.replay`, list thread and early draw) the work is 5.5 ms at p50 and 8.9 at p99 and the
pad read reaches the window in 10 ms at p50 (33 before). Sound in every run: 86.13 blocks per second after
the mission load, no underrun.

What is left of the long frames, all before a mission starts or in its first frame: boot (250 ms and 140 ms,
the intro movies' first pictures), the profile load (310 ms), the mission load (180 ms and 130 ms), the first
frame of the in-engine intro (60 ms) and of the mission (25 to 30 ms). The stall log puts 96 to 99% of that
in "rest": recompiled game code and movie decode, not the renderer and not file reads (the mission's 6.3 MB
are read in 2.5 ms). The sound has one gap there, during the mission load (about 210 ms), as before.

## Hardware (measured and estimated)

Measured on one machine: 4 cores (8 threads) of an AMD EPYC 7543P at 2.8 GHz and an NVIDIA RTX
A4500, Story 1-1 at 60 frames per second, each run under the timing lock.

- Processor: the game thread works 3.6 ms per frame at p50 and 7.4 ms at p99; the list thread 4.6 ms at the
  mean and 9.7 ms at p99; the sound is a third, light thread. A frame has 16.7 ms.
- Graphics card, time per submission from timestamp queries (it includes whatever else the card did in
  between, the desktop and a stream encoder, so it is an upper bound):

  | Internal scale | mean | p50 | p99 |
  |---|---|---|---|
  | 1 (480x272) | 2.4 ms | 2.3 | 4.4 |
  | 2 (960x544) | 2.8 | 3.0 | 5.1 |
  | 4 (1920x1088) | 2.5 | 2.3 | 7.3 |

  The time does not grow with the resolution on this card: it is the per-draw cost of about 700 draws and
  the surface switches of a frame, not pixels. Before the region copy scale 4 cost 3.8 ms against 2.3 at scale 1.

Estimated, not measured (no such machine was at hand):

- A processor half as fast per core would need about 7 ms at p50 and 15 ms at p99 on the game thread and 9
  and 19 ms on the list thread: 60 frames per second mostly, with late pictures in the heaviest fights; at
  `FrameLock=2` (30) it has a threefold margin. Two cores carry the load, so a four-core processor is enough.
  With `GeThread=false` one core does both and 60 needs this machine's speed (10.3 ms at p50, 16.4 at p99).
- Integrated graphics: nothing was measured. The frame is small (about 700 draws into 512x512 surfaces
  times the scale), so `Preset=low` (scale 2 at most) is the careful choice and
  `InternalScale=1` the fallback; the `[render] gpu time per submission` line of `run.log` says what a card
  actually needs.

## Anti-aliasing, filtering, ambient occlusion

A mission frame is drawn in this order: sky, world and characters into the scene surface (`0x040E8000`,
512x320 of it), decals and particles on top, then the scene is copied into the composite surface
(`0x040D8000`) by a handful of sprites, and bloom, tints, HUD and text are drawn there. The added passes work
on the scene surface, at two points the renderer finds in each frame's draws:

- **Ambient occlusion** runs after the last draw that writes depth, which is the end of the solid world, so
  the particles and glows that follow are not darkened, and the sky (depth never written) is left alone. The
  depth buffer is turned back into distance with the game's own projection; each pixel looks at 8 or 16
  neighbours within a metre (at most 48 PSP pixels on screen), the result is blurred along depth edges and
  multiplied into the picture at 70%. Beyond 100 to 160 metres there is none. Plants and fences are blended
  cut-outs whose see-through fringe still writes depth; they are recognised by their alpha (with SSAO on, a
  depth-writing 3D draw also writes its alpha into the surface's alpha plane, which the game does not use
  there), and neither they nor the ground right around them is darkened.
- **FXAA** runs just before the first draw that samples the scene from another surface, that is on the
  finished 3D picture and before anything of the HUD exists. With `msaaN` it is skipped.
- A surface counts as a scene when it received at least 30 depth-writing 3D draws before another surface
  samples it. Menus, movies and load screens never do, so nothing is applied to them.
- **Multisampling**: a surface keeps track of what was drawn since its last resolve and since its last
  snapshot (the bloom samples its own target two dozen times a frame), and only those rectangles are
  resolved and copied.
- **Texture filtering** changes only the sampler of 3D draws. Through-mode sprites (HUD, text), the screen
  copy and the bloom keep the game's filter (with `Glow=smooth` the one sprite that enlarges the glow reads
  through a cubic filter instead: "The glow"). With `Anisotropy` above 1 a texture is sampled with its real
  footprint instead of the GE's axis-only level rule, and without the game's level bias of -1.5 (which
  sharpened the 480x272 picture and shimmers on a large one).
- **A texture mod's picture in a 3D draw** is sampled without that bias under every setting
  (`host/gpu/ge_sampler_rule.hpp`): from the levels with about one texel per screen
  pixel, the two nearest blended (a coin that turns must not jump between levels). With the bias a
  picture eight times the size of its texture was read from its largest level until it was shrunk 2.8
  times, at every second texel at scale 4. Within a level the texels are filtered as the game or
  `TextureFilter` says. **In a sprite** (HUD, text, menus: through mode) a mod's picture keeps the
  sampler every replaced texture had before, with the game's bias: the full-size picture at every
  internal scale, pixel for pixel what earlier builds drew. (Read from its half-size level,
  as an earlier version of the rule did, an eight times HUD or text picture was a third less
  steep at its edges on a 1920x1088 picture.) The game's own textures, and the button pictures painted
  over them, keep their samplers.

`PSPRECOMP_GE_AO_STRENGTH=<0..100>` sets how much of the occlusion is applied (70), for tuning;
`PSPRECOMP_GE_AO_DEBUG=1` shows the occlusion term in place of the scene (the game's bloom then washes it
out: dividing a capture with SSAO by one without shows it better).

### What the settings cost

Measured on a quiet machine (timing lock, nobody playing), RTX A4500, `mission1_baseline.replay`,
mission frames 300 to 899, with `PSPRECOMP_GPU_TIMING_DIAG=1`: the game thread then waits for the card after
every frame, so a frame's work is the CPU's plus the card's. (Without that switch the game thread never
waits for the card, and none of these settings shows in its frame time on this card.) Median frame work:

| Setting | scale 1 | 2 | 4 | 6 | 8 |
|---|---|---|---|---|---|
| everything off | 16.9 ms | 16.5 | 17.5 | 19.4 | 19.6 |
| `AntiAliasing=fxaa` | | | 17.6 | | 20.1 |
| `Anisotropy=16` | | | 17.3 | | |
| `AntiAliasing=msaa2` / `msaa4` / `msaa8` | | | 19.2 / 20.1 / 20.4 | | - / 23.2 / - |
| `SSAO=low` / `high` | | | 18.8 / 19.3 | | - / 23.5 |
| `fxaa` + `Anisotropy=16` + `SSAO=high` | | | 19.2 | | |

Runs differ by about 0.3 ms. Of the 17.5 ms about 4 ms is the card and 11 the CPU; the rest is the waiting
itself.

In real time, with a player's defaults (`fxaa`, `Anisotropy=16`, early present) at scale 8, which is what
`auto` picks on a 3024x1964 desktop, same conditions, sound off: 29.97 game frames per second at
30 (frames 300 to 1299: frame to frame p50 33.4 ms, p99 36.7), and 59.94 at `[Game] FrameLock=1` (frames 600
to 2399: p50 16.7 ms, p99 19.2, worst 26.6; frame work p50 9.4 ms over the whole run). That is where the
build stood before these settings existed. Card memory: a surface sheet is 512x512 times the scale squared, 4 bytes a pixel (16 MB at scale 4,
64 MB at scale 8), and the game's four surfaces, their snapshots and the depth buffer make about ten sheets;
`msaaN` adds N sheets for each surface and the depth buffer; SSAO adds two to three sheets; FXAA one.

### Looked at, and not

Captures of `mission1_baseline.replay` (boot, movies, menu, in-engine intro, courtyard, trailer) and of
`mission1_intro_cutscene.replay`, each setting on and off at the same frames:

- With every new setting off (`scripts/unattended.ini`) all 47 captured frames of the baseline are identical,
  pixel for pixel, to the build before these changes, and the state hashes are the reference ones.
- Boot screen, movies and menus are identical with every setting on: no pass touches them.
- `fxaa`: edges of the beams, the fence and the characters are smoothed; HUD, text and the reticle are
  unchanged.
- `msaa2`, `msaa4`, `msaa8`: same picture as before the rectangle tracking, with bloom, HUD and early
  present; also with `SSAO=high`.
- `SSAO`: a soft shadow where walls, posts, the trailer, the car and characters meet the ground, and in the
  corners of the trailer's room; plants are left as they are. It is off by default because only stills of the
  first mission were looked at: whether it flickers while the camera moves, and how it looks in the other
  missions, needs someone playing. At close range the 16 samples show as faint grain on characters.
- `Anisotropy=16`: at 480x272 the ground in the distance is cleaner; at scale 4 nearly every texture is
  magnified and the picture hardly changes.
- `InternalScale` 6 and 8 run on this card (captures of 2880x1632 and 3840x2176).
- Not looked at: the window itself (captures are read from the surfaces, so the averaging down of a large
  picture into a small window and the new window size were not seen), and monitors other than one
  3024x1964 desktop, where `auto` gives scale 8.
- Not done: textures without mip levels (characters, vehicles) get none generated, so anisotropic filtering
  does nothing for them; cut-out edges are not smoothed by multisampling (alpha to coverage).

## The glow

`[Rendering] Glow=smooth | game | off`. Code: `host/gpu/ge_gpu_glow_dx12.hpp` (the renderer's part) and
`host/gpu/ge_glow_blocks.hpp` (which draws are the glow; ctest `chili_glow_blocks`).

**What the game draws.** A frame of a mission or of an in-engine cutscene ends with a full-screen glow: 27
sprite draws on a sheet of its own (`0x04088000`, the address of the depth buffer too; two resources on the
card) and one more that adds the result to the picture. All are through-mode sprites with the game's linear
filter and no depth test; colour and strength come from the material, not from the sheet's alpha.

| Draws | What they do |
|---|---|
| 1 (8 sprites) | the composite's 480x272 copied onto 240x136 of the sheet |
| 1 (4 sprites) | that copied onto 120x68: the working picture, the frame averaged over 4x4 PSP pixels |
| 1, 2, 1 | a white fill; the working picture minus half brightness, then doubled (what is brighter than half); a black fill where the sum will stand |
| 21 | the bright part added to itself at offsets of 0, 1 and 2 working pixels, with weights 72, 48, 32, 14 and 8 of 255 (2.0 in all) |
| 1 (8 sprites) | the sum added onto the composite, enlarged four times, at 137 of 255 |

The rectangles and numbers are the same in every frame looked at (the courtyard, the fire beside the car, the
trailer, the slow-motion dive, the intro's crash and explosion). Boot, the movies, the menus and the loading
screens have no such draws.

**Why it was coarse.** The renderer drew these sprites at the internal scale N like everything else. The
working picture was then 120N x 68N: an average over one PSP pixel at scale 4 instead of sixteen, so it kept
detail the PSP never had and the cut at half brightness turned that into specks. The 21 copies lay N pixels
apart, and after the fourfold enlargement 4N pixels apart, while the bilinear filter that joins them stayed 4
pixels wide. At 1920x1088 the glow was a lattice of 16-pixel squares with ghost copies of every bright edge,
and it changed more from frame to frame than the PSP's.

**`smooth`** (the default). The sprites into the sheet are drawn as they are, with the same shader, blend
states and rectangles, into a sheet of the PSP's own size (512x512 at every scale):

- a sprite that copies a full-size surface into the sheet (the first draw: the picture) reads a copy of that
  surface averaged over N x N pixels, made by one small pass (`run_glow_box`): the picture a PSP would have;
- a sprite that reads the sheet itself takes the renderer's ordinary path for a surface that reads itself,
  on the small sheet (its copies are 124x72 pixels instead of (120N + 4) x (68N + 4));
- the sprite that adds the sheet to the picture reads the small sheet through a cubic B-spline (the sixteen
  texels around each point, taken as four bilinear taps) instead of the bilinear filter, whose diamonds would
  be 4N pixels wide. It is the pixel shader compiled a second time (`GLOW_CUBIC`); the shader of every other
  draw is the same bytes as before. Blend, colour and the 137 of 255 are the game's own draw, so the glow is
  as strong as the game makes it, whatever the game sets.

**`game`**: none of that; every draw goes where it went before, pixel for pixel. **`off`**: the sheet is drawn
small and the sprite that adds it to the picture is left out. At `InternalScale=1` `smooth` is `game` (the
game's passes are the PSP's there). The software renderer always draws the game's own glow.

**How the glow's draws are found.** Nothing is read from the game's numbers. In each submission's draws
`find_glow_blocks` looks for a run of through-mode sprites without depth test (no clears, no uploads) into one
surface that is not the displayed one, which begins with a sprite that shrinks another surface at least 1.5 to
1 along both axes, has at least three draws, and in which a sprite reads the surface it draws into. From the
first sprite of such a run the surface is a glow sheet: sprites into it go to the small sheet, and a draw
elsewhere that samples it reads the small sheet. It stays one until a draw into it that is not such a sprite
(a 3D draw, a clear, an upload): from there it is drawn at full size again until the next run. The two sheets
do not share their content, which the game's draws do not need: each run begins by filling what it reads.
The end of `run.log` says how often it happened (`[render] glow: N blocks drawn at the PSP's size, M draws
read them`); a tool that reads the surface at `0x04088000` gets the full-size sheet, which `smooth` and `off`
leave untouched.

Not covered, and what happens then: another effect that draws a run of this shape (none is known) would be
drawn at the PSP's size too, soft where it was sharp; `Glow=game` is the way out. Only Story 1-1 and its intro
were looked at.

**Measured** (RTX A4500; `mission1_baseline`, `mission1_intro_cutscene`, and eight captured
frames replayed with `chili_ge_replay --glow smooth|game|off`: the fire beside the car, the courtyard, the
trailer twice, the slow-motion dive, the intro's crash and explosion three times).

- *Against the PSP's own glow.* The glow layer of a frame is the picture after the game's last sprite minus
  the picture before it; the reference is the scale-1 layer enlarged N times with the bilinear filter (a
  PSP's picture shown larger). The fire frame at scales 2, 3, 4, 6 and 8 and with 4x multisampling, the
  others at 4 and 8; on the pixels where neither picture is at white after the add (there the difference is
  the picture's headroom, not the glow; 12 to 56% of a frame, 0 to 2% in the trailer):

  | | mean difference | more than 8 levels off | more than 16 | largest | detail finer than the working grid, against the reference's | ghost copies at 4N pixels (fire frame) |
  |---|---|---|---|---|---|---|
  | `smooth` | 0.01 to 0.31 levels | 0.00 to 0.44% of the pixels | none | 15 | at or just under it in every frame (1.68 against 1.70) | under 0.01 |
  | `game` | 0.13 to 1.59 | 0.97 to 15.0% (22.5% over the feed sacks) | up to 3.0% | 76 | above it in every frame (2.25 at scale 4, 2.46 at 8, against 1.69) | 0.136 at scale 4, 0.096 at 8 |

  Over a whole frame, pixels at white included (after minus before is cut there at the picture's edges, which
  lie elsewhere at another scale), `smooth` differs from the reference by more than 8 levels in 0.0 to 3.3% of
  the pixels and by up to 122 levels at single places; `game` in 1.0 to 12.8% and by the same 122.
- *Strength.* The layer's sum is 0.99 to 1.05 times the scale-1 layer's with `smooth`: the strength is the
  game's own on a PSP. With `game` it was 1.03 to 1.33 times in the bright scenes and 1.6 to 2.5 times in
  the dim trailer (where the layer is 0.2 to 1.1 levels on average): the finer working picture lets more
  through the cut at half brightness. Averaged over a whole frame the two differ by under one level (the
  courtyard: 18.6 levels added against 19.3; the fire frame: 13.1 against 13.6); over the feed sacks of the
  fire frame `smooth` adds 15.3 levels of red where `game` added 19.2. No gain is added to make up for it
  (the game's own strength; a strength value for the last sprite is left for later).
- *What a player may notice beside `game` at a high scale* (the frames looked at one to one at 1920x1088 and
  3840x2176). The squares, the stripes beside bright edges and the crawl under a moving camera are
  gone; colour and reach are the same (the fire frame's veil adds 34.9 levels of red against 35.8, the fire's
  halo ends 24.1 PSP pixels out at 16 levels against 24.8), and a frame's mean brightness stays within one
  level. Two things differ, both as in the scale-1 glow, which `smooth` follows within the numbers above.
  Over bright detail the glow is 2 to 6% weaker (the feed sacks 36.5 against 39.0 levels of red, the
  courtyard's sky 8.1 against 8.9). And the halo of a thin bright strip is beaded: the tube light on the
  trailer's ceiling (list 2614, about frame 1323) gets three soft spots, the glow's largest value along the
  strip going from 10 to 42 levels, where `game` at scale 4 laid an even band of 50 to 56; over that part of
  the picture the glow is 6.0 levels on average against 10.2. The edge of a thin dark shape in front of a
  flash is softer for the same reason (the gun over the muzzle flash: the glow on its dark pixels 3.1
  against 3.7 levels, not brighter). Both are accepted. `off` takes the red veil and the
  fire's warmth out of the picture (the fire frame's mean luminance 102 against 114).
- *Frame to frame.* Six consecutive frames of a camera move (990 to 995), the window of the fire: the layer
  changes by 0.98 levels a frame in the scale-1 glow enlarged, by 1.20 at scale 4 and 1.29 at scale 8 with
  `game`, by 0.99 with `smooth` at both.
- *Everything else.* The picture before the game's last sprite is the same bytes in all three modes, and with
  `off` the picture after it is the picture before it (the eight frames, every scale).
- *`game` is the build before.* `mission1_baseline` (2,340 frames, deterministic) on the build before the
  change and on this one with `Glow=game`: every frame written is the same bytes at scale 1, 4 and 8,
  each without and with 4x multisampling; at scale 4 with FXAA, with `SSAO=high`, and with msaa2, nearest
  filtering and SSAO high together; at scale 8 with FXAA, anisotropy 16 and SSAO high (26 frames each at
  scale 1 and 4, 8 in the other runs), and in `mission1_intro_cutscene` at scale 4 (20 of 20). The software
  renderer with `Glow=off` writes the frames it wrote before. All 34 runs, in the three modes, print the
  reference hashes (24 lines, frame 2340 `817ded4a1e89e140`; the two software runs of 700 frames the first
  7); the settings tour prints its 20 (frame 1915 `774467b681ac11f4`).
- *Menus, movies, loading screens* are the same bytes with `smooth` and `off` as before the change: every
  tenth frame from 10 to 230 of the baseline (boot, the two movies, the profile load, the title, the main
  menu, the mission load, the briefing, the story movie) at scale 4, with multisampling and SSAO, and at
  scale 8. The first frames that differ are the in-engine intro's. Of the settings tour's 43 frames one
  differs: the Graphics page, which has the row `Glow`.
- *Cost.* The card's own time per submission with the fire frame replayed 299 times in each mode
  (`chili_ge_replay --repeat 300`, which prints it; under the timing lock, two rounds that agree within
  0.1 ms):

  | | `game` | `smooth` | `off` |
  |---|---|---|---|
  | scale 4 | 1.56 ms | 1.48 | 1.42 |
  | scale 8 | 3.46 | 3.20 | 3.01 |
  | scale 4, msaa2 | 2.30 | 1.52 | 1.48 |
  | scale 8, msaa4 | 7.22 | 4.05 | 3.87 |

  The processor's side of that frame (the list and its submission) is 4.0 ms in all three. Whole deterministic
  runs with `PSPRECOMP_GPU_TIMING_DIAG=1`, mission frames 300 to 899, two rounds each on a PC that was not
  quiet (two runs of one build differ by up to 1.5 ms): recording and submitting a frame, the wait for the card
  included, at the median:

  | | build before | `game` | `smooth` |
  |---|---|---|---|
  | scale 4 | 7.4 ms | 7.3 | 7.4 |
  | scale 8 | 8.9 | 8.7 | 8.7 |
  | scale 4, msaa2, SSAO high | 7.0 | 7.4 | 5.3 |
  | scale 8, msaa4 | 9.6 | 9.6 | 8.8 |

  Without multisampling `smooth` costs the card a little less than `game`; with it `smooth` saves the
  resolves and copies of the large sheet (0.8 ms at scale 4 with msaa2, 3 ms at scale 8 with msaa4 in the
  replayed frame). In real time at 60 game frames a second (frames 320 to 4399 of
  `mission1_baseline_fl1`, list thread and early present, two runs each, the same busy PC): with scale 4,
  msaa2 and SSAO high the game thread worked 6.7 ms a frame at the median on the build before, 6.1 with
  `game` and 6.2 with `smooth`, and the card needed 4.6, 4.8 and 4.3 ms per submission; at scale 8 with FXAA
  and anisotropy 16 the work was 6.4 ms before and 6.1 with `smooth`, the card's time 5.1 and 4.9. All ten
  runs held 58.9 to 59.0 game frames a second and had 250 to 380 frames with more than 16.7 ms of work, on
  the build before as with `smooth`: the game's own code held up by the PC's other load (97 to 100% of the
  time in them; the wait for the list thread 0 to 3%).
  Memory: three surfaces of 512x512 and a depth buffer, 4 MB. The glow's four pipeline states are in the
  built-in warm-up list: a first start creates none of them, and no surface, while the game runs.
## Where the passes meet

The list thread and early draw, the anti-aliasing, filtering and occlusion passes and the texture mods are one
renderer. Where they meet:

- A draw that samples its own target: the copy is brought up to date only in the rectangle the draw reads
  that was drawn into since the last copy (what a draw reads, intersected with what had been drawn). Multisampled targets are resolved by the rectangle
  drawn into.
- Texture images, a mod's replacement included, are placed in the shared 32 MB heaps when they are under
  8 MB, and get a resource of their own otherwise.
- `Preset` limits what `InternalScale=auto` picks to 2, 4 and 8 (`low`, `medium`, `high` and `custom`).

## Button prompt pictures in the textures

The on-screen button prompts (`CONTROLS.md`, "On-screen prompts") are pictures the host paints over seven of
the game's textures: the two font sheets and five menu sprites (`mods/TEXTURES.md` has their keys).

- Painting: `host/prompt_glyphs.cpp`, Direct2D and DirectWrite into WIC bitmaps, all three styles once at
  start. A change of style paints nothing.
- Into the texture: `ge_texture_mods` keeps a table of overlays per content key beside the mods' file index.
  Only textures of the seven shapes (256x512 CLUT4 or CLUT8, 32x32 CLUT8 or DXT3) are keyed for it when no
  texture mod is present. When a texture with an overlay is uploaded, the cells are painted over the game's
  picture, or over a mod's replacement, and the mip levels are made from that. A composed font sheet is 1, 2
  or 4 times the game's size (internal scale 1, 2 to 3, 4 to 8) and keeps the game's own sheet as its last
  level, so the game's text looks as it did; sprites follow the internal scale.
- A change of style: the prompt controller swaps the table's entries at a frame start on the game thread;
  the renderer, at the end of a submission, takes the cached textures with those keys out of its cache
  through the path the cache limit uses, and the next draw uploads them again with the new pictures. No
  frame shows half of each.
- Direct3D 12 only. With `[Input] Prompts=psp`, in a scripted run and on the software renderer the table is
  empty and nothing is keyed: the frame images of such a run are the game's own pictures.

## The overlay pass: a host page over the picture

What the settings pages inside the game are drawn with, and a test page behind a test variable.

- **Where.** `record_direct_present` in `host/gpu/ge_gpu_backend_dx12.cpp` puts the displayed surface on the
  swap chain's back buffer; `record_overlay` (`host/gpu/ge_gpu_overlay_dx12.hpp`) draws the page right after
  it, before the buffer goes to the window. The viewport is the picture's rectangle, so a PSP pixel of the
  page covers the window pixels that PSP pixel of the game's picture covers, at any window size and any
  internal scale; the back buffer has one sample, so multisampling of the game's surfaces changes nothing.
  No surface the game reads is touched. The page rides on the game's own presents: the scene shown early, the
  game's copy, a menu's own list, with the lists on their own thread or not.
- **What a page is.** `vcs::GeGpuOverlay` (`host/gpu/ge_gpu_backend.hpp`): triangles in PSP pixels with a
  texture coordinate and a colour, in runs that name a texture by content key (0: the colour alone), a
  flag for nearest-texel sampling, an alpha reference and an optional clip rectangle. The game thread hands
  one over with `ge_gpu_backend_set_overlay` (a shared pointer; null takes the page away); the thread that
  runs the lists takes the newest at each present. With none set a present is one atomic load longer.
- **Textures.** The renderer's own cache entries, found by content key: what is in force there, so a mod's
  picture and the button prompts' cells are in the page without anything done for them. A texture has a key
  only when its shape is watched: `ge_gpu_backend_watch_overlay_textures` (before the first draw) names
  them, and `ge_gpu_backend_overlay_texture_ready` says from any thread whether the cache has one. A texture
  is there once the game has drawn with it. At the main menu that is the two font sheets, both backgrounds,
  the logo, the white texture and the Cross, Circle and Square sprites; the left/right D-pad sprite only
  after the game's own Settings screen was shown, the Triangle sprite never. When the button pictures
  change (another device) the sprites the screen underneath does not draw leave the cache until the game
  draws them again: a page falls back to the font's picture of the same button, which the game's text keeps
  in the cache. A texture a page draws with counts as used, so the cache limit does not take it.
- **Text.** `host/page_draw.hpp` builds the list (`PageDraw`: `rect`, `triangle`, `sprite`, `glyph`, `text`,
  `clip`) and sets text as the game sets its menu text: the rules are at the top of that file, measured on
  the game's own draws (advance plus the box's spacing of -5 added up in single precision, a glyph one row
  less than its cell, whole PSP pixels, the tilt about the box's middle, a black copy of the fill sheet as
  the shadow, the outline sheet, the fill sheet, alpha test above 12). With them the glyphs of all 121
  one-line texts in eight captured menu frames come out in the game's own pixel columns. The glyph table is
  the game's: `PageFont::scan` finds the loaded font record in guest memory (181 glyphs, found at frame 12 of
  a start in 2 ms; `host/page_font_guest.hpp`).
- **The picture's rectangle** is one function, `chili::picture_rect` (`host/picture_rect.hpp`): the renderer
  calls it, and `host/window_picture.hpp` maps a mouse message's pointer through it to PSP pixels.
- **Pictures for tests.** While a page is set, frame pictures and screenshots (`CHILI_FRAME_PNG_EVERY`, the
  script channel's `capture`, `PSPRECOMP_FRAME_DUMP`) are the displayed surface with the page drawn over it at
  the internal resolution (`ge_gpu_backend_read_display_with_overlay`); with none set they are what they
  were. The window's own picture, bars and all, comes from `ge_gpu_backend_request_window_capture` and
  `ge_gpu_backend_take_window_capture` (the back buffer is copied in the present that follows).
- **The test page.** `CHILI_OVERLAY_TEST=page|pause|glyphs|echo[@<first frame>[-<last>]]` shows a fixed page
  (`host/overlay_test_page.hpp`): the look of a settings page on the menu's background, the same rows in the
  pause menu's black box from the font sheets alone, every glyph with the sprites and plain shapes, or the
  game's own Profile Options screen drawn again over itself.
  `CHILI_OVERLAY_SHOTS=<folder>@<frame>,...` writes the window's picture at those frames,
  `CHILI_OVERLAY_SIZE=<frame>:<w>x<h>` and `CHILI_OVERLAY_FULLSCREEN=<frame>` change the window (a scripted
  run's window stays behind the others in full screen too). The end of the log has the counts and the cost.
- **Not there.** The software renderer draws no page. Nothing presents on the host's own: a screen that
  ends no picture (the briefing) would show a page only when the game next presents.

Checked (`rel` build, RTX A4500, deterministic runs of a menu tour, the baseline replay and a
replay that pauses Story 1-1; the window behind the others):

- With no page: `mission1_baseline.replay` prints the 24 hashes of `replays\README.md` (frame 2340
  `817ded4a1e89e140`), equal line for line to a build from before the pass, and its frame pictures
  (three of the mission, nine of the menu tour) are equal to that build's byte for byte.
- With the test page: the window's picture is the same, pixel for pixel, at internal scale 1, 4 and 8 and
  with 4x multisampling, and with the lists on their own thread and early present on or off; in a mission
  the page is on every picture that reaches the window (one present per game frame with early present, the
  picture a frame sooner, the page's own pixels equal). Client areas of 1200x900 (bars above and below),
  1500x600 (bars beside) and borderless full screen at 3024x1964: the page sits in the picture's rectangle,
  and the rectangle `window_picture.hpp` gives for the pointer is the one the renderer reports. With
  `CHILI_PROMPTS=keyboard` the legends show the key caps (the sprites' and the font cells'), and after a
  change of style the Circle sprite is gone until the game draws it again, the page drawing the font's Circle
  meanwhile. With a mod of enlarged menu textures the page shows the mod's pictures. State hashes are those
  of the run without the page.
- The look against the game's own: Profile Options at internal scale 1, a frame picture with
  `CHILI_OVERLAY_TEST=echo` against the same frame without (`replays\picture_diff.py`). Outside the line the
  game has selected (it pulses) no pixel differs by more than 32 levels of 255. The menu lines and the
  legends' words differ by more than 8 levels in 0 to 21 pixels of 6,000 to 8,000 each (largest 14), the
  headline in 633 of 8,360 (largest 26), the logo with the tilted line in 1,879 of 17,490 (largest 31), a
  button sprite in 49 of 1,024 (largest 30). What is left is edge softness: the renderer samples the game's
  triangles (sprites, the tilted line) a sixteenth of a pixel off, as the PSP does, and the page does not
  copy that; at the window's resolution it is a fraction of a window pixel.
- Cost, menu tour at scale 4, frames 300 to 800, three runs each: host time per submission 0.280 to 0.285 ms
  (p50) on the build before, 0.284 to 0.287 with the pass and no page, 0.290 to 0.295 with the test page (387
  rectangles in 31 runs). Recording a page takes 10 to 13 us in a menu and 20 to 40 us in a mission's pause
  menu; the graphics card needs 0.02 to 0.03 ms for it (timestamps around the page's draws); building the
  test page's list on the game thread 40 to 55 us. Watching the page's eleven textures with no page ever
  shown (what a run with the pages enabled does all the time) keyed 40 textures in 0.48 ms over the whole
  baseline replay, and its 24 hashes and its frame pictures are those of a run that watches nothing. In a
  run paced at 30 frames per second, lists on their own thread and early present on, the window's picture
  is the deterministic run's, pixel for pixel; the card idles between frames there and takes 1.7 ms for a
  menu's whole submission, 0.24 ms of it for the page.

`tools\page_draw_test.cpp` (ctest `chili_page_draw`, no game data) checks the rectangle, the search for the
glyph table and the text places against the capture's.

## First checks of the Direct3D 12 renderer

- State: `mission1_baseline.replay`, 2,340 frames, deterministic, d3d12: all 24 state hashes equal the
  software renderer's, also at scale 2 and 4. The renderer does not change what the game computes.
- Picture: frame images every 100 game frames (and every 20 of the first 300) against the software
  renderer: boot screen, intro movie, title, menu, in-engine intro and gameplay agree; the mean difference
  is 0 to 5 levels of 255, more than 32 levels on at most 0.6% of the pixels.
- `mission1_intro_cutscene.replay` against the software renderer, every 20 frames: one defect found and
  fixed, the hills on the horizon were missing (the far plane clipped where the PSP clamps depth, see
  `host/gpu/UPSTREAM.md`). Still different: distant foliage is a little lighter and softer than the software
  renderer draws it (frames 320-360, 20 to 35% of the pixels differ by more than 8 levels, under 0.5% by
  more than 32). The crash and explosion of that intro cost 60 to 70 ms in the worst frames, against 1.3 s.
- Speed, frame work per game frame in the mission (frames 250-2339), quiet machine, RTX A4500:

  | Run | mean | p50 | p90 | p99 |
  |---|---|---|---|---|
  | software (`replays/README.md`) | 256.7 ms | 244.2 | 324.2 | 389.4 |
  | d3d12, scale 1 (three runs: 26.0 to 28.4 mean) | 26.9 ms | 26.0 | 34.2 | 42.0 |
  | d3d12, scale 4 | 26.7 ms | 25.1 | 35.5 | 48.4 |
  | d3d12, scale 1, regenerated game code (two runs) | 11.4 to 12.0 ms | 10.5 to 10.7 | 15.3 to 18.0 | 20.0 to 25.4 |

  The first three rows are an earlier generated corpus. In real time with `frame_lock` 2 it held
  29.7 of 29.97 game frames per second, and with `[Game] FrameLock=1` 41 to 46 of 59.94: three quarters of a
  frame was the recompiled game code, a quarter the
  renderer's CPU side. With the regenerated game code (last row) the mission holds 29.98 of
  29.97 at `frame_lock` 2 and 59.9 of 59.94 at `FrameLock=1`, where a frame costs 9 to 10.5 ms at p50 and 15.4
  to 16.5 ms at p99; the renderer's CPU side (about 7 ms) is now the larger part of a frame. The internal
  scale costs nothing measurable on this card (59.92 at scale 4).

## Known limits

- Stencil test, colour test, logic ops, the ABS blend equation and doubled alpha factors are approximated or
  ignored (`GeGpuFallback` in `host/gpu/ge_gpu_backend.hpp`). None of them was met from boot through the
  first mission.
- A pipeline state that neither the built-in list nor `ChiliNative.gpucache` knows is still built when first
  met. On the test machine that took 0.1 to 1.3 ms each (the driver's shader cache was warm); on a first start
  with a cold driver cache it can take tens of milliseconds, once.
- With `SSAO` on, depth-writing 3D draws also write the surface's alpha plane. Nothing in the first mission
  reads it (a run with the occlusion applied at 0% is identical to one without SSAO); a later mission that
  blends by destination alpha on its scene surface would show a difference.
- `Glow=smooth` and `off` find the glow's draws by their shape ("The glow"); only Story 1-1 and its intro
  were looked at. Another effect drawn the same way would be drawn at the PSP's size, and `Glow=game` draws
  everything as before.
- A lost graphics device is not recovered; restart the game.
- With something else loading every core (four compilers during these tests), frames take 38 to 42 ms and a
  real-time run showed stalls of 100 to 370 ms. They were gone once the machine was free.
- The game is not known to read VRAM back on the CPU. If something does (a screen grab for a save
  thumbnail, say), it reads zeros under d3d12.
