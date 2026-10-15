# VFPU integer pack generated-native fixture

This test generates fourteen Allegrex functions with the **live**
`out/framework/Release/psp_recomp.exe`, compiles their emitted C++, and executes
it against the current core library. It includes the actual Chili words
`D03D8282` (VI2C.q) and `D03C8383` (VI2UC.q).

Tests cover the four integer packing operations, threshold bit extraction and
signed clipping, lane order, scalar/pair outputs, row/column views, overlap,
swizzles, destination masks, neighboring physical lanes, prefix consumption and
condition flags. Unsupported vector lengths, short-vector invalid swizzles and
disputed prefix modes must stop before changing VFPU registers or prefixes.

Baseline golden values come from [PSP conversion tests](https://github.com/hrydgard/pspautotests/blob/master/tests/cpu/vfpu/convert.c)
and their [expected output](https://github.com/hrydgard/pspautotests/blob/master/tests/cpu/vfpu/convert.expected).
The [pspdev VFPU description](https://github.com/pspdev/vfpu-docs/blob/master/inst-vfpu-desc.yaml)
and [current PPSSPP interpreter](https://github.com/hrydgard/ppsspp/blob/master/Core/MIPS/InterpreterVFPU.cpp)
disagree on S absolute/negate/constants and D saturation. These modes remain
explicitly unsupported pending pack-specific hardware evidence. Passing this
fixture does not prove the game reaches these routines or verify every VFPU
instruction.

From the repository root, with the existing framework library/generator built:

```powershell
.\profiles\chili\tools\vfpu_pack_fixture\generate_fixture.ps1
cmake -S profiles/chili/tools/vfpu_pack_fixture -B out/chili-vfpu-pack -G 'Visual Studio 18 2026' -A x64
cmake --build out/chili-vfpu-pack --config Release --target chili_vfpu_pack_test --parallel 1
ctest --test-dir out/chili-vfpu-pack -C Release --output-on-failure
```

Generated data stays in ignored `profiles/chili/analysis/vfpu_pack_fixture`.
Optional `RepositoryRoot`, `OutputDirectory` and `CodegenPath` script parameters,
plus CMake `REPOSITORY_ROOT` and `FIXTURE_OUTPUT_DIR` overrides, allow isolated
verification without writing into a checkout.
