# Building from source

You need your own copy of Chili Con Carnage (USA, ULUS-10216) as an `.iso`. The repository holds no game data
and no code generated from the game: the C++ of the game's code (the "corpus") is made on your PC from your
disc, and it must never be committed or published.

## Requirements

- Windows 10 or 11, 64-bit.
- Visual Studio 2026 (version 18) with "Desktop development with C++": the x64 C++ tools and a Windows SDK
  with Direct3D 12. The scripts use the CMake and Ninja that come with it. They look for
  `C:\Program Files\Microsoft Visual Studio\18\Community` first and otherwise ask `vswhere`. Visual Studio
  2022 has not been tried.
- The .NET Framework 4.8 targeting pack (a Visual Studio component) for the launcher and the installer. They
  are compiled with Visual Studio's own C# compiler; nothing is downloaded.
- Python 3.9 or later, started as `py -3`.
- Windows PowerShell 5.1 (part of Windows).
- 16 GB of memory is comfortable; a full build at 6 jobs peaked at about 6.4 GB of compiler memory.
- About 3 GB of disk: 0.8 GB for the extracted disc, 0.2 GB for the corpus, 0.7 GB per build folder.

Run the scripts from a PowerShell prompt as
`powershell -NoProfile -ExecutionPolicy Bypass -File <script> <arguments>`; the lines below leave that prefix
out.

## 1. The corpus, from your disc

```
profiles\chili\scripts\analyze.ps1 -Iso "<your .iso>"
```

It does three things:

1. Extracts the disc into `profiles\chili\game\disc0` (`tools\extract_iso.py`, standard library only).
2. Checks `PSP_GAME\SYSDIR\BOOT.BIN`: the game's executable of the USA release, 8,616,992 bytes, FNV-1a 64
   `0xA18A6D5249286C4A` (`tools\check_boot.py`). Another release or a modified copy is refused.
3. Builds the generator `psp_recomp` (in `out\framework`) and writes the corpus into
   `profiles\chili\analysis\generated`: about 380 C++ files, 200 MB, in under a minute (`scripts\regen.ps1`).

`BOOT.BIN` on the disc is a plain ELF; no decryption is involved. The generator reads nothing but that file:
the same generator makes the same C++ units from the same `BOOT.BIN`.

`profiles\chili\game\`, `profiles\chili\analysis\` and everything else made from the game are ignored by git
(`.gitignore`). Keep it that way.

## 2. The program

```
profiles\chili\scripts\build.ps1 -Config rel
```

- Builds `out\chili-rel\bin\chili_recomp.exe` with the five FFmpeg DLLs beside it.
- The first build compiles the whole corpus: about 80 minutes at 6 jobs on an 8-thread processor. After a
  change to the host (`profiles\chili\host`) only the host recompiles, about a minute.
- `-Jobs <n>` sets the parallel compilers (default 6; each needs up to 3 GB on the largest units).
- `-Config od` builds `out\chili-od` with the generated code unoptimized: quicker to compile, slower to run.
- `-ConfigureOnly` stops after CMake. Without the corpus the configure step stops with
  `Generate Chili AOT code before configuring this profile`.

The same by hand, from a Developer PowerShell or x64 Native Tools prompt (what `build.ps1 -Config rel` passes):

```
cmake -S . -B out\chili-rel -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=cl -DPSPRECOMP_MSVC_MP_JOBS=1 ^
  -DPSPRECOMP_PROFILE=chili ^
  -DPSPRECOMP_GENERATED_OPT_LEVEL=2 -DPSPRECOMP_AOT_PRODUCTION_FASTPATHS=ON -DPSPRECOMP_AOT_ASSUME_NO_WRITE_WATCH=ON ^
  -DPSPRECOMP_LTO=OFF -DPSPRECOMP_NATIVE_AVX2=OFF -DCHILI_PARALLEL_SHADE_OBSERVATION=OFF ^
  -DPSPRECOMP_BUILD_TESTS=OFF -DPSPRECOMP_BUILD_PROFILE_TESTS=OFF
cmake --build out\chili-rel --target chili_recomp
```

## 3. Running it from the build tree

`profiles\chili\run.cmd` starts `out\chili-rel\bin\chili_recomp.exe` on `profiles\chili\game\disc0`. It
plays on the save folder `profiles\chili\game\ms0_play` (made on the first start; if `game\ms0` exists it is
copied there first). The log goes to `out\chili-rel\last_run.log`.

`chili_recomp.exe <BOOT.BIN> <disc folder> [--ms0 <save folder>]` is the developer's start: no dialog, no log
file, output on the console. `chili_recomp.exe` with no arguments is a player's start, with `ChiliNative.ini`,
`saves\` and `logs\` beside the program.

## 4. The game folder

```
profiles\chili\scripts\package.ps1 -Build out\chili-rel -Out "<new folder>" [-Disc "<your .iso>"]
```

Makes the folder a player runs: `ChiliConCarnage.exe` (the build's program marked as a Windows program
without a console), the launcher (compiled here), the FFmpeg DLLs, the Visual C++ runtime DLLs from Visual
Studio, `README.txt`, `CHANGELOG.md`, `CONTROLS.md`, `RENDERING.md`, `licenses\`, `mods\README.md`,
`mods\TEXTURES.md`, the Python mod tools and "Dump textures.cmd". It checks that the two programs carry the
same icon and stops if it finds a game file in the folder. `-Disc` writes the disc's path into the new
`ChiliNative.ini`.

## 5. The launcher alone

```
profiles\chili\launcher\build.ps1 [-Out <folder>] [-Test]
```

Compiles `profiles\chili\launcher\src\*.cs` into `ChiliConCarnageLauncher.exe` (needs no game, about ten
seconds). `-Test` also runs its tests that need no window. See `profiles\chili\launcher\README.md`.

## 6. The installer

The installer is in `installer\`; its `build.ps1` builds the Setup program and its `pack.ps1` adds a game
folder made by step 4 (and, optionally, the HD loading pictures) to it. See the README there.

## Tests

- **Framework** (no game needed):

  ```
  cmake -S . -B out\framework -G "Visual Studio 18 2026" -A x64 -DPSPRECOMP_PROFILE=
  cmake --build out\framework --config Release
  ctest --test-dir out\framework -C Release --output-on-failure
  ```

- **The profile's tests** are a CMake project of their own, `profiles\chili\tools`. It links
  `out\framework\Release\psprecomp_core.lib` (or `out\framework\psprecomp_core.lib` from a Ninja build;
  `-DCHILI_FRAMEWORK_LIB=<file>` names another), so build the framework first:

  ```
  cmake -S profiles\chili\tools -B out\chili-tests -G "Visual Studio 18 2026" -A x64
  cmake --build out\chili-tests --config Release
  ctest --test-dir out\chili-tests -C Release --output-on-failure
  ```

  34 of the tests read captures of a running game or files of the disc (`profiles\chili\analysis\...`,
  `game\disc0\...`) that are not in the repository. They are listed but disabled; `-DCHILI_CAPTURE_TESTS=ON`
  turns them on for a developer who has made those files. A few more read the extracted disc when it is
  there (`-DCHILI_MOD_TEST_DISC=<disc folder>`, `-DCHILI_TEST_ISO=<your .iso>`) and run a smaller check
  without it.

- **Replays**: deterministic runs of recorded input with state hashes, `profiles\chili\replays\README.md`.
  They need a save profile in `profiles\chili\game\ms0`, which you make by playing.

- **The launcher against the game**: `profiles\chili\scripts\check_launcher_settings.ps1` and
  `check_package_start.ps1` (see `profiles\chili\launcher\README.md`).

## What is not reproducible yet

- **The corpus is made from your disc** on your PC. That is by design: it is the game's code.
- **The prebuilt program in a release is not bit-for-bit what you build.** It is built from this source and
  a corpus made the same way, but the Microsoft linker writes time stamps into the program, and no
  deterministic-build switch is set.
- **The FFmpeg files are prebuilt binaries** taken unchanged from upstream PSPRecomp, whose build was not
  recorded. `profiles\chili\third_party\ffmpeg\README.md` has the configure line and the steps to build the
  same version from source; they have not been run to reproduce those exact files.
- **34 tests need captures** that are not published (see Tests).
- **Replay reference hashes** were taken with one particular save profile. With another profile a run can go
  differently; compare two builds on the same machine with the same save.
- **Toolchain**: only Visual Studio 2026 (version 18) on Windows 11 has been used.

## Keeping game data out

Before a commit, `git status` must show nothing under `profiles\chili\game`, `profiles\chili\analysis`, no
`.iso`, no `.at3`, no picture taken from the game and no save. The `.gitignore` blocks the usual names; it is
not a substitute for looking.
