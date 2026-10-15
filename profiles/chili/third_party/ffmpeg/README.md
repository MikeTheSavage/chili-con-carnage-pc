# FFmpeg 7.1 for the Chili profile

The game's sound is ATRAC3plus and its movies are H.264 in MPEG-PS. The host decodes both with FFmpeg,
through `profiles/chili/media/vcs_media_decoder.cpp`.

| Folder | What |
|---|---|
| `include/` | FFmpeg's public headers (libavcodec, libavformat, libavutil, libswresample, libswscale) |
| `lib/` | the import libraries `avcodec.lib`, `avformat.lib`, `avutil.lib`, `swresample.lib`, `swscale.lib` |
| `bin/` | `avcodec-61.dll`, `avformat-61.dll`, `avutil-59.dll`, `swresample-5.dll`, `swscale-8.dll`; the build copies them beside every program that uses them |
| `COPYING.LGPLv2.1` | the licence; the build copies it beside the program as `COPYING.FFmpeg.LGPLv2.1` |

These files come unchanged from upstream PSPRecomp's VCS profile (`profiles/vcs/third_party/ffmpeg`). The
DLLs report FFmpeg version 7.1, "LGPL version 2.1 or later", and this configure line:

```
--toolchain=msvc --target-os=win64 --arch=x86_64 --enable-shared --disable-static --disable-programs
--disable-doc --disable-network --disable-autodetect --disable-avdevice --enable-swscale --disable-postproc
--disable-avfilter --disable-everything --enable-decoder=atrac3p --enable-decoder=h264 --enable-parser=h264
--enable-demuxer=wav --enable-demuxer=mpegps --enable-demuxer=h264 --enable-bsf=extract_extradata
--enable-protocol=file
```

No `--enable-gpl` and no `--enable-nonfree`: everything here is LGPL.

## Building the same libraries from source

Upstream left no record of the build itself. To make these files from source:

1. Get `ffmpeg-7.1.tar.xz` from <https://ffmpeg.org/releases/> and check its signature.
2. Install MSYS2 with `make`, `diffutils`, `pkgconf` and `nasm`.
3. Open the "x64 Native Tools Command Prompt" of Visual Studio, start the MSYS2 shell from it with
   `msys2_shell.cmd -msys2 -use-full-path`, so that `cl.exe` and `link.exe` are on the path.
4. In the unpacked source folder: `./configure --prefix=<out folder>` followed by the line above, then
   `make -j8` and `make install`.
5. Copy the five DLLs into `bin/`, the five import libraries into `lib/` and the `include/` folder over this
   one.

The steps follow FFmpeg's own instructions for MSVC builds; they have not yet been run to reproduce these
exact files byte for byte.
