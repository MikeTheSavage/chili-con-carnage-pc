# Third-party notices

This project's own code is under the MIT licence in [LICENSE](LICENSE). The components below keep their own
licences. Chili Con Carnage itself (its code, names, art, music and text) belongs to its rights holders; none
of it is in this repository, and this project is not affiliated with them.

## PSPRecomp

- What: the static recompilation framework this project is built on (`src/`, `include/`, `tools/`, `tests/`,
  `configs/`), the renderer lifted from its VCS profile (`profiles/chili/host/gpu/`, see `UPSTREAM.md`
  there), the media decoder (`profiles/chili/media/`) and the host files that say they were adapted from it.
- Where from: <https://github.com/jessicanataliagta/PSPRecomp>
- Licence: MIT, "Copyright (c) 2026 PSPRecomp contributors". The notice is in [LICENSE](LICENSE).

## FFmpeg

- What: FFmpeg 7.1, the libraries avcodec-61, avformat-61, avutil-59, swresample-5 and swscale-8, used for
  the game's ATRAC3plus sound and its movies. In this repository: their headers, import libraries and DLLs in
  `profiles/chili/third_party/ffmpeg/`. In the release: the five DLLs beside the program.
- Licence: GNU Lesser General Public License, version 2.1 or later. The text is
  `profiles/chili/third_party/ffmpeg/COPYING.LGPLv2.1`, and `licenses\COPYING.FFmpeg.LGPLv2.1.txt` in the
  installed game.
- Built without `--enable-gpl` and without `--enable-nonfree`, with only the ATRAC3plus and H.264 decoders.
  The configure line is in [profiles/chili/third_party/ffmpeg/README.md](profiles/chili/third_party/ffmpeg/README.md).
- Source: <https://ffmpeg.org/download.html>. The source archive of FFmpeg 7.1 that corresponds to the
  shipped DLLs is published with each release of this program, beside the download.
- The program links the DLLs dynamically. They can be replaced by other builds of the same versions.
- FFmpeg is a trademark of Fabrice Bellard, originator of the FFmpeg project.

## Microsoft Visual C++ runtime

- What: `msvcp140.dll`, `msvcp140_atomic_wait.dll`, `vcruntime140.dll`, `vcruntime140_1.dll`, copied beside
  the program in the release (not in this repository).
- Licence: Microsoft's Visual Studio licence terms, which allow these files to be redistributed with an
  application ("Distributable Code"). They are shipped unmodified.

## Typefaces of the installer

- What: Changa One, Saira and Tektur, in `installer/fonts/`, used only by the installer.
- Licence: SIL Open Font License 1.1. The licence text and the copyright notice of each typeface are beside
  the font files. Reserved Font Names are not used for anything but the unmodified fonts.

## Test data

- `profiles/chili/tools/ge_fog_fixture_data.hpp` and `profiles/chili/tools/ge_texture_formats_dxt5_vectors.inc`
  hold numbers recorded from PSP hardware by the pspautotests project
  (<https://github.com/hrydgard/pspautotests>); the files name their source.

## Credits without a licence obligation

- The FXAA pass (`profiles/chili/host/gpu/ge_gpu_post_dx12.hpp`) is written here and follows the published
  FXAA 3.11 algorithm by Timothy Lottes.
- PPSSPP (<https://www.ppsspp.org>) was used as a behaviour reference only; no PPSSPP code is in this project.
