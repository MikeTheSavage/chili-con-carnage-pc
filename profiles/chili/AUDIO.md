# Sound

The game mixes its own sound (44.1 kHz, 16 bit, stereo) and hands it over in blocks: 512 frames (11.6 ms)
from the mixer, 2048 frames (46 ms) while a movie plays. Code: `host/audio_runtime.cpp` (the PSP audio calls),
`host/pcm_queue.hpp` (when a block counts as played), `host/audio_feed.cpp` (the ring and the device thread),
`host/audio_stats.hpp` (counters, capture), `host/audio_settings.hpp` (settings).

What goes into that mix comes from two firmware services:

| Sound | Data | Path | Code |
|---|---|---|---|
| Effects (shots, steps, impacts, engines) | ADPCM samples inside the level blocks | 32 mixer voices, `sceSasCore` | `host/sas_runtime.cpp` |
| Music, ambience, the sound of the in-engine cutscenes | ATRAC3plus streams, two channels | decoded by `sceAtrac` (decoder 0), mixed by the game | `host/atrac_runtime.cpp`, `host/atrac_stream.cpp` |
| Every voice line: the player's (Ram's), the enemies', the announcer's; a few jingles | ATRAC3plus streams, one channel | decoded by `sceAtrac` (decoder 1), mixed by the game | the same |
| Movies | ATRAC3plus inside the movie file | the movie player's own blocks | `host/mpeg_runtime.cpp` |

The game has two stream decoders and uses one for the two-channel streams and one for the one-channel ones,
so one voice line plays at a time. `tools\sound_assets.py list <assets00.naz>` names all 8,685 streams.

## How it works

- Every block goes into a ring on the host. A separate thread gives the ring to the Windows default output
  device whenever the device asks (WASAPI shared mode, every 10 ms). The device is never waiting for the
  game. Windows converts 44.1 kHz to the device's own rate; the port does no resampling, so pitch and speed
  are the game's.
- The ring holds about 70 ms of sound (`BufferMs`). That is what plays while the game is busy with a frame.
- The game's Sound thread is woken when the ring needs sound, also in the middle of a frame, not only when
  the frame is done. When the game runs slower than real time the sound keeps its real speed.
- When the ring does run dry (a stall longer than the buffer: loading, a hitch of 100 ms) the sound fades
  out over 3 ms, stays silent until the ring is full again and fades in. One soft gap, no clicks.
- Movies time their own sound against the game clock, so their blocks are released on that clock and the
  device only steps in when the buffer is nearly empty or far too full.
- In a player's run the game thread runs one priority step above normal, so other programs that load every
  core cannot hold it back for 100 ms at a time. `[Game] Priority=high` or `normal` in `ChiliNative.ini` says
  so outright. A scripted run (replay, scripted dialogs, script channel) stays at normal priority, so frame
  times measured by scripts compare from build to build; `CHILI_GAME_PRIORITY=high` gives a scripted run the
  player's priority, and `scripts\listen.ps1` sets it.
- When the game gives its output up and takes it again (a movie starts or ends, a movie is skipped), sound
  from before that is still in the ring fades out over 3 ms and the rest is dropped, so the new sound does not
  start behind up to 160 ms of the old. The exit report says how often that happened and how much was dropped
  (`[audio] output taken again with sound still queued: ...`).
- A new Windows default output device is picked up within about three seconds.
- Deterministic runs (`CHILI_DETERMINISTIC=1`) and `CHILI_NULL_AUDIO=1` open no device; the blocks are
  timed on the game clock alone, as before.

## Settings

`[Audio]` in `ChiliNative.ini` beside `chili_recomp.exe`. A run by a player adds the section, commented out,
to a file that lacks it.

| Key | Values | Meaning |
|---|---|---|
| `BufferMs` | `20` to `500`, default `70` | Milliseconds of sound kept ahead of the device. Every sound is heard this much later, plus about 25 ms in the device |

For scripts and measurements (the environment wins over the file):

| Variable | Meaning |
|---|---|
| `CHILI_AUDIO_BUFFER_MS=<n>` | Same as `BufferMs` |
| `CHILI_AUDIO_WAV=<file>` | Records exactly what the device was given, silence included. In a deterministic run (no device) it records the game's blocks back to back: the same file every run |
| `PSPRECOMP_LOG=sound` | A `[sound]` line for every stream started, rewound and ended (channels, bytes per frame, samples, key) and for every mixer voice keyed on (data address, size, pitch, volume). `out=` is the seconds of sound handed over so far |
| `CHILI_ATRAC_TAP=<folder>` | One WAV per stream of the samples the game was handed for it, read from the game's buffer the way the game reads it |
| `CHILI_SOUND_SOLO=voices`, `streams` or `mono_streams` | Test switch: only the mixer voices, only the streams, or only the one-channel streams (the voice lines) reach the output |
| `CHILI_AUDIO_TRACE=<csv>` | One row per block: game time and host time at the call and at its return, ring fill |
| `PSPRECOMP_LOG=audio` | A `[audio-stats]` line every second: blocks per second, waits, ring fill, times the ring ran dry |
| `CHILI_AUDIO_BAND_MS`, `CHILI_AUDIO_CATCH_UP` | Test switches of the release rule (`host/pcm_queue.hpp`) |
| `CHILI_GAME_PRIORITY=high` or `normal` | The game thread's priority for this run (`[Game] Priority`) |

Every run prints the device at start (`[audio] output: ...`) and totals at exit (`[audio] total ...`,
`[audio] device took ... frames per second`).

## Hearing it

`scripts\listen.ps1 mission` (or `menu`, or `movies`) plays a recorded run in the game window in real time,
from a copy of the save, with nobody at the controls. The window opens in front like any program you start
(`-Background` keeps it behind the others, as other scripted runs are). `-FrameLock 1` runs it at 60 frames per second,
`-BufferMs 100` with another buffer. It prints the sound totals and leaves `audio.wav`, the capture of what
the device was given, in its output folder.

`scripts\listen.ps1 speech` plays 22 seconds of Story 1-1 in which the enemies shout from the first seconds
on (`replays\ram_taunt.replay`) and lists, at the end, the voice lines that were played by name. Ram's own
taunts follow some of his kills (one kill in ten, at least 7.5 s apart) and a real-time run never goes
exactly like the last, so it may or may not have one: of two such runs one had `uk_g11_taunt_ram_004`, the
other none. `scripts\listen.ps1 speech -SoundOnly` computes the same replay on the game clock, where it
always goes the same way, and then plays the sound the game produced, without a picture: an enemy's taunt
0.7 s after the start, a dying enemy at 2.4 s, Ram's `uk_g11_taunt_ram_008` at 4.7 s.

## Voice lines

Early builds garbled every voice line: the player's and the enemies' alike, and the one-channel
jingles (the `mission_complet_*` tracks, `rewing`, `combo_1_stop`). Music, ambience and effects were not.

- A voice line is a one-channel ATRAC3plus stream. `sceAtracDecodeData` hands a PSP game two channels all
  the same (only the `sceAtracSetMOut*` calls, which this game does not import, give one), and the game
  counts on it: its decode wrapper at `0x0883BC34` takes `samples * 4` bytes from its buffer whatever the
  stream holds.
- The port wrote one channel: 4,096 bytes into a buffer the game read as 8,192. Each 46 ms block came out
  as 23 ms of the line at double speed (left = the even samples, right = the odd ones), then 23 ms of
  whatever the buffer held before.
- `host/atrac_stream.cpp` now writes two channels for every stream: a one-channel stream has each sample on
  the left and on the right at its full level.

Measured in deterministic runs of `replays\ram_taunt.replay` (900 frames) before and after the change,
against the same asset decoded apart from the game (`chili_at3_reference`, FFmpeg's own demuxer), with
`tools\speech_compare.py`:

| Line | What is compared | Before | After |
|---|---|---|---|
| Ram, `uk_g11_taunt_ram_008` (2.73 s) | the samples the game is handed (`CHILI_ATRAC_TAP`) | correlation 0.05, pitch 397 Hz for 200 Hz, 58 of 58 blocks are the one-channel block read as two channels | every sample equal |
| | the output with only the voice lines in it (`CHILI_SOUND_SOLO=mono_streams`) | correlation 0.05 | correlation 1.0000, 79 dB above the difference, at half level (the game's speech volume), left = right |
| | the full output (music, ambience, effects on top) | correlation 0.05 | correlation 0.89 |
| Enemy, `uk_G11_taunt_smallvir_006` (1.53 s) | the samples the game is handed | correlation 0.06, 33 of 33 blocks | every sample equal |
| | the output with only the voice lines in it | correlation 0.04 | correlation 1.0000, 48 dB above the difference |
| | the full output | correlation 0.04 | correlation 0.43 |

- In a real-time run through the audio device Ram's `uk_g11_taunt_ram_004` correlates 0.94 with its reference
  in the capture of the full output (gain 0.50, pitch 223 Hz for 220 Hz).
- The rest is unchanged: the six two-channel streams of `mission1_baseline.replay` are handed over byte for
  byte as before; with the streams silenced (`CHILI_SOUND_SOLO=voices`) the capture of the effects is the
  same file before and after; so are the captures of the intro movies (`idle.replay`) and of the main menu
  (`menu.replay`), 1,200 frames each.
- Real time, 30 fps, quiet machine, `mission1_baseline.replay` from second 12 on: 86.14 blocks per game
  second, 86.13 per real second, ran dry 0 times, no gap in the capture, as before; one dry spell of 0.42 s
  while the mission loads.
- Deterministic runs: the processor state and the game clock are the same at every checkpoint as before the
  change. The memory hash differs from the first voice line on, because the decoded samples in the game's
  sound buffers are different samples now. With those buffers left out (`-HashExclude
  '0x09AD3000:0x9000,0x09B1D000:0x2000'`) builds with and without the change agree on all 24 checkpoints of
  `mission1_baseline.replay`. The new reference hashes are in `replays\README.md`.

The sounds after a kill, checked after the fix: what plays when an enemy dies is his death shout (`death_vir_*`, a one-channel
stream), and with a combo the combo music (a two-channel file of four layers, `combo_1_track0`, the game
moves to the next layer at the same sample as the combo grows) and at its end the jingle `combo_1_stop` (one
channel). In a deterministic run of `mission1_baseline.replay` every one of the 15 streams the game started
equals an independent decode of its asset sample for sample over its whole played length: five death
shouts, three enemy taunts, `combo_1_stop`, layer 0 of the combo music (480,912 samples) and layer 1 from
sample 479,233 on (683,663 samples), the menu music, the ambience twice and the cutscene track; in
`ram_taunt.replay` also Ram's `uk_g11_taunt_ram_008`. Gun shots, hits and the other short effects are ADPCM
samples of the firmware mixer (`host/sas_runtime.cpp`); they were not at fault, were not changed, and were
not compared with a decoder of their own here (`chili_sas_test` covers the decoder).

To check a line without ears:

```
python tools\sound_assets.py list <assets00.naz> taunt_ram            # names, lengths, keys
python tools\sound_assets.py extract <assets00.naz> uk_g11_taunt_ram_008 line.at3 --block mission01_1_uk
out\chili-tests\chili_at3_reference.exe line.at3 line.wav              # the reference decode
scripts\run_replay.ps1 -Exe <chili_recomp.exe> -Replay replays\ram_taunt.replay -Frames 900 -Log sound `
    -OutDir <new folder> -Environment @{ CHILI_AUDIO_WAV = '<folder>\audio.wav'; CHILI_ATRAC_TAP = '<folder>\taps' }
python tools\sound_assets.py trace <assets00.naz> <folder>\run.log     # which stream is which line
python tools\speech_compare.py line.wav <folder>\taps\stream_0006_id1_1ch_120355.wav
python tools\speech_compare.py line.wav <folder>\audio.wav --from 12.3 --to 16.0
```

`chili_atrac_stream_test <SND0.AT3> <one-channel stream.at3>` (in `out\chili-tests`) checks the decoder
itself: two channels out, left and right equal to the reference, nothing written past `samples * 4` bytes.

Not known: whether a PSP gives a one-channel stream the full level on both sides or 3 dB less. Full level is
what PPSSPP does and what the port does; if the voices are too loud against the music, that is the place.
The game rewinds a looping stream (ambience, music) with `sceAtracResetPlayPosition` to sample 0; the reset
flushes FFmpeg's ATRAC3plus decoder and decodes again from two frames before that sample, so the samples after
a loop point are those of the first pass (measured on the menu music, "The installer's music" below).

## The installer's music

The title screen and the main menu play one stream: `data_stream_psp/sounds/music_no_combo/mainmenu_music_loop.mediastream_data_psp`
in `assets00.naz` (ATRAC3plus, two channels, 44.1 kHz, 10,101,105 samples, 229.05 s, key `a2a17ae6`). In a
deterministic run from an empty memory stick (the game makes a new profile; `PSPRECOMP_LOG=sound`) the intro
movies and the profile screens are silent, the stream starts with the title screen and plays on into the main
menu, and when all of its samples are played the game resets it to sample 0: its loop is the whole track. The
samples the game is handed for it (`CHILI_ATRAC_TAP`) equal FFmpeg's own decode (`chili_at3_reference`) sample
for sample, also after the loop point (the reset flushes the decoder). The track is 96 bars of 4/4 at 100.6
beats per minute, so the beat runs on across the loop.

`tools\sound_export.cpp` (`ChiliSoundExport.exe`, built with the tests) reads that track from a player's own
disc and writes it as a WAV for the installer, which plays it while it works. Nothing of the game's audio
ships with the installer.

```
ChiliSoundExport <disc.iso or disc folder> <out.wav> [--track menu|title] [--raw]
```

- `menu` (default) is the music above; `title` is `PSP_GAME/SND0.AT3`, the 31 s clip the PSP's own menu plays
  for the disc (the same theme with an opening hit, cut mid-phrase).
- The disc must be ULUS-10216 and the track the bytes the game ships (size and CRC-32). It is decoded the way
  `host/atrac_stream.cpp` decodes it for the game. `--raw` keeps those samples; without it the track is set to
  -16 LUFS (EBU R 128 integrated loudness), never above -1 dBFS: the menu music measures -12.34 LUFS and gets
  -3.66 dB (peak -4.02 dBFS).
- It writes `<out>.wav` (16-bit PCM) and `<out>.loop.txt` (`track`, `channels`, `rate`, `frames`,
  `loop_start`, `loop_end`, `lufs`, `gain_db`, `peak_dbfs`; the loop in frames, end exclusive) and prints
  `ok channels=2 rate=44100 samples=10101105 loop=0,10101105 track=menu lufs=-16.00 gain_db=-3.66 peak_dbfs=-4.02 ms=...`.
  Exit codes: 1 usage, 2 the disc cannot be read, 3 another disc, 4 the track is missing or differs, 5
  decoding failed, 6 the output cannot be written. A failure prints one line on stderr and leaves no file.
- About one second for the menu music. The exe needs `avcodec-61.dll`, `avutil-59.dll` and
  `swresample-5.dll` beside it and nothing else.
- `ctest -R chili_sound_export` checks the stream header and storage readers, the loudness meter against the
  EBU Tech 3341 tones, the files and the tool's answers to discs that are not the game, with no game data.
  Configured with `-DCHILI_TEST_ISO=<the game's .iso>` it also exports both tracks and checks their length,
  loop and level.

## If it still crackles

1. Look at the last lines of the log: `underruns=` is how often the ring ran dry and `dry_ms=` for how long.
   A few at start-up, when a movie is skipped and while a mission loads are normal: the game sends nothing then.
2. Raise `BufferMs` to 100 or 150. If that cures it, frames are taking longer than the buffer.
3. If `underruns` is 0 and it still crackles, the cause is after the game: the Windows device or the
   stream to your screen. Play the `audio.wav` of a `listen.ps1` run in a media player: if the file crackles
   too, the game produced it; if the file is clean, the path after the game did.
4. `python tools\audio_gaps.py <capture.wav> --from 12 --list 20` lists every gap in a capture.
5. Lower `BufferMs` (40 to 50) if you want sound closer to the picture and the log shows no underruns.

## Measured: the sound ring

All runs in real time with Direct3D 12 on a machine that other builds and other game instances kept at
100% CPU the whole time, so these are worst-case numbers. Continuous sound is 86.13 mixer blocks per second.

| Phase | Before: blocks per game second | Before: silence | After: blocks per game second | After: ran dry | After: gaps in the capture |
|---|---|---|---|---|---|
| Mission, 30 fps (`mission1_baseline.replay`, after 12 s) | 43.7 | 35.9 s of 70.7 s, 1,534 gaps | 86.14 | 0 | 0 |
| Front end, 30 fps (before: `idle.replay` after the intros; after: main menu, `menu.replay`) | 49.4 | 48.0 s of 112.5 s | 86.13 | 0 | 0 |
| Intro movies (`idle.replay`, 2 to 31 s) | 22.5 of 21.5 | 1.7 s of 29.7 s | 23.7 | 0 | 1 of 46 ms (see below) |
| Mission, `FrameLock=1` (`mission1_baseline_fl1.replay`) | | | 86.14 per real second | 0 | 0 |
| Main menu, `FrameLock=1` | | | 86.13 | 0 | 0 |
| Intro movies, `FrameLock=1` | | | 22.9 | 0 | 0 |

- Before, the blocking output call waited 22 ms of game time per 11.6 ms block.
- The device took 44,101.5 to 44,102.4 frames per second of host time in every run.
- At `FrameLock=1` the loaded machine reached 47.5 of 59.9 frames per second in the mission. The game clock
  then runs slow, so the count per game second is 105; per real second it is 86.14 and the capture has no gap.
- At start-up each run has two to four dry spells (0.5 to 0.7 s in total): the intro movies being skipped and
  the mission loading.
- Movies: the sound waits 134 ms in the ring (126 to 140 in a run without a stall) plus 23.5 ms in the
  device before it is played, about 110 ms more than on a PSP. It does not drift: 134.2 ms in the first fifth of the two intro movies,
  133.6 ms in the last. The one 46 ms gap in the 30 fps movie run is the movie player's own filler block
  after the host stalled for 100 ms; the same movies played without a gap in three other runs.
- Deterministic mode is unchanged: `mission1_baseline.replay` gives the same 24 state hashes as before
  (frame 2340 `7a9e209f65bf3180`, `clock_us=78328564`).

## Measured: sound, mouse and aim together

Sound, mouse and aim work together in one `rel` build. Four real-time runs of 100 s each with Direct3D 12,
scripted dialogs and the replay as input (`mission1_baseline.replay`, and `mission1_baseline_fl1.replay` at
`FrameLock=1`), on a machine that was not building anything and ran no other game: 6 to 49% CPU in total,
from desktop programs and other scripts. Frame figures are for the mission (frames 250-2339 at 30,
320-4399 at 60: 68 to 70 s of play); sound figures are from second 12 on, as above.

| Run | Game frames per real second | Work per frame, p50 / p90 / p99 | Frames over the budget | Blocks per real second | Ran dry | Gaps in the capture | Lowest ring fill | Pad read to present, p50 / p90 |
|---|---|---|---|---|---|---|---|---|
| 30 fps, as scripts run it (normal priority, no early present) | 29.99 of 29.97 | 10.1 / 14.5 / 18.3 ms | 1 of 2,090 over 33.4 ms | 86.14 | 0 | 0 | 66 ms | 45 / 47 ms |
| 30 fps, as a player runs it (`Priority=high`, `EarlyPresent=true`) | 29.99 | 11.1 / 14.4 / 17.1 ms | 0 | 86.14 | 0 | 0 | 69 ms | 33 / 34 ms |
| 60 fps, as scripts run it | 59.95 of 59.94 | 9.7 / 12.8 / 15.4 ms | 10 of 4,080 over 16.7 ms (0.25%) | 86.14 | 0 | 0 | 49 ms | 28 / 29 ms |
| 60 fps, as a player runs it | 59.95 | 9.2 / 13.1 / 16.5 ms | 34 of 4,080 (0.83%) | 86.14 | 0 | 0 | 69 ms | 16 / 17 ms |

- Each run has one dry spell of 210 to 230 ms at start-up (the mission loading) and the capture's gaps all
  lie in the first 10 s, as before.
- The device took 44,101.3 to 44,101.6 frames per second.
- The game reserves its output once and switches between movie and mixer blocks by changing the block
  length, so the drop of sound queued before a new start did not happen in these runs.
- 60 frames per second is held, but 1 frame in 120 to 400 takes longer than its 16.7 ms: the 99th percentile
  of the work is 15.4 to 16.5 ms where about 14 was asked for before making 60 the default. 30 stays the
  default; `[Game] FrameLock=1` switches. The renderer's CPU side (vertex decode, lighting, batching: about
  7 of the 10 ms) is what has to shrink.
- Deterministic mode: the 24 reference hashes of `mission1_baseline.replay` with Direct3D 12 (frame 2340
  `7a9e209f65bf3180`, `clock_us=78328564`).

## Not done

- The movie sound is about 110 ms later against the picture than on a PSP. `BufferMs=40` takes 30 ms off;
  delaying the movie picture by the buffer would remove it.
- A stall longer than the buffer is still a gap. The longest single frame in a mission run was 129 ms.
