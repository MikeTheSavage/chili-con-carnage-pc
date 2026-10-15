# PSPRecomp

This repository is built on PSPRecomp, a static recompilation framework for PSP software
(<https://github.com/jessicanataliagta/PSPRecomp>, MIT). It reads an Allegrex/MIPS executable, analyzes guest
code, emits C++ translation units, and runs them through a native host runtime instead of shipping a PSP
interpreter or JIT.

The framework here is a full copy of upstream with this project's changes to the generator, the runtime and
the tests; it is not a submodule. The upstream VCS profile (`profiles/vcs`) is not part of this repository:
the Chili Con Carnage profile (`profiles/chili`) carries the two pieces of it that it uses, the FFmpeg kit
(`profiles/chili/third_party/ffmpeg`) and the media decoder (`profiles/chili/media`), and the Direct3D 12
renderer it lifted (`profiles/chili/host/gpu`, see `UPSTREAM.md` there).

## Repository layout

```text
include/psprecomp/   Public runtime and Allegrex interfaces
src/                 ELF/PRX loading, decoder, memory, runtime and support code
tools/               Generic analyzer, recompiler and reverse-engineering helpers
tests/               Framework regression tests
configs/             Generic examples and PSP NID data
profiles/            Game-specific hosts, configuration and tests
  chili/             Chili Con Carnage (USA, ULUS-10216)
```

Game-specific addresses, HLE behavior, native fast paths, renderer integration and generated AOT code belong
under a profile. The framework is usable without any profile selected.

## Build the framework only

```bash
cmake -S . -B out/framework -DPSPRECOMP_PROFILE=""
cmake --build out/framework --config Release
ctest --test-dir out/framework -C Release --output-on-failure
```

This builds `psprecomp_core`, `psp_analyze`, `psp_recomp`, `dump_function` and the framework tests. None of
it needs a game.

## Build a profile

Profiles are selected with `PSPRECOMP_PROFILE`:

```bash
cmake -S . -B out/chili -DPSPRECOMP_PROFILE=chili
```

The Chili profile needs its generated corpus first; see [BUILDING.md](../BUILDING.md).

## Create another profile

See [`PROFILE_GUIDE.md`](PROFILE_GUIDE.md). A new title normally provides its own generated corpus, HLE/profile
host, configuration, tests and optional native fast paths without modifying the framework for game-specific
addresses.

## Game files

No commercial game executable or asset is included. PSPRecomp does not ship an EBOOT decryption
implementation. Profiles expect files obtained from your own copy in the format documented by that profile.

## Source provenance

See [`SOURCE_PROVENANCE.md`](SOURCE_PROVENANCE.md) for the rules around independently written code, profile
boundaries, decryption and third-party source.
