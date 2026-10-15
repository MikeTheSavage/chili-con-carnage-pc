# The Chili Con Carnage profile

Everything specific to Chili Con Carnage (USA, ULUS-10216). How to build it is in
[BUILDING.md](../../BUILDING.md) at the repository root.

| Folder or file | What |
|---|---|
| `CMakeLists.txt`, `media_backend.cmake` | The program `chili_recomp` (the generated corpus plus the host) and the offline GE list replay tool `chili_ge_replay` |
| `analysis/generated/` | The generated corpus. **Not in the repository**: made from your own disc by `scripts/analyze.ps1` and git-ignored |
| `game/` | Your extracted disc (`disc0`) and save folders (`ms0`). **Not in the repository**, git-ignored |
| `host/` | The host: PSP firmware services (files, threads, sound, movies, saves, input), the window, the settings, the mod layer; `host/gpu/` is the Direct3D 12 renderer and the CPU reference rasterizer |
| `media/` | The FFmpeg decoder used for ATRAC3plus sound and MPEG movies (from upstream PSPRecomp) |
| `third_party/ffmpeg/` | FFmpeg 7.1: headers, import libraries, DLLs, licence |
| `launcher/` | The launcher, C# for .NET Framework 4.8 |
| `resources/` | The program's icon, version information and Windows manifest |
| `scripts/` | Build, regenerate, package, run and check scripts (PowerShell) |
| `tools/` | Tests and tools of the profile (a CMake project of its own) and the Python format tools |
| `replays/` | Recorded input for deterministic runs, with reference state hashes |
| `mods/` | The documents for modders that the build copies beside the program |
| `package/` | The player's `README.txt` and "Dump textures.cmd" |
| `gpu_dev/` | The GE renderer's bring-up project: replays of captured GE lists and a self test |

Reference documents: [AUDIO.md](AUDIO.md) (sound), [CONTROLS.md](CONTROLS.md) (input and every binding),
[RENDERING.md](RENDERING.md) (the renderer and its settings), [NATIVE_SAVE_FORMAT.md](NATIVE_SAVE_FORMAT.md)
(saves), [mods/README.md](mods/README.md) and [mods/TEXTURES.md](mods/TEXTURES.md) (mods and texture packs),
[launcher/README.md](launcher/README.md) and [launcher/DESIGN.md](launcher/DESIGN.md) (the launcher),
[replays/README.md](replays/README.md) (deterministic runs).

## How the program starts

`chili_recomp.exe` (in a game folder `ChiliConCarnage.exe`) loads `PSP_GAME/SYSDIR/BOOT.BIN` from the
player's disc, checks it (disc id `ULUS-10216`, the executable's size and FNV-1a 64 hash), maps its sections
into guest memory and runs the generated code of its functions. The disc is read in place, from an `.iso` or
an unpacked folder; nothing of it is copied. The game's data stays on the disc.

## Rules for contributions

- Nothing derived from the game goes into the repository: no corpus, no disc file, no picture, sound, text or
  save of the game, no capture of a running game. Addresses and engine names needed to interoperate are fine.
- No code from projects whose licence is incompatible with MIT (PPSSPP and other GPL projects are behaviour
  references only). See [docs/SOURCE_PROVENANCE.md](../../docs/SOURCE_PROVENANCE.md).
