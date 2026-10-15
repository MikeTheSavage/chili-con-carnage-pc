"""Build and execute synthetic AOT regressions; no copyrighted game input needed."""
import hashlib
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path


BASE = 0x08804000


def elf_fixture(extra_seed=False, wide=False):
    span = 262144 if wide else 64
    target = BASE + span + 0x20000 if wide else BASE + 48
    size = span + 0x20010 if wide else 128
    data = bytearray(0x100 + size)
    data[:7] = b'\x7fELF\x01\x01\x01'
    struct.pack_into('<HHIIIIIHHHHHH', data, 16,
                     2, 8, 1, BASE + 64, 52, 0, 0, 52, 32, 1, 0, 0, 0)
    struct.pack_into('<8I', data, 52, 1, 0x100, BASE, BASE, size, size, 5, 16)

    def code(address, words):
        for i, word in enumerate(words):
            struct.pack_into('<I', data, 0x100 + address - BASE + i * 4, word)

    code(BASE + 16, [0x27BDFFF0, 0xAFBF0000 if extra_seed else 0xAFB00000,
                     0x24020063, 0x03E00008, 0x27BD0010])
    code(target, [0x24020025, 0x03E00008, 0])
    code(BASE + 64, [0x27BDFFF0, 0xAFBF0000,
                     0x0C000000 | ((target >> 2) & 0x03FFFFFF), 0,
                     0x8FBF0000, 0x03E00008, 0x27BD0010])
    if wide:
        # A destination unit with widely separated entries must dispatch by PC,
        # so its caller cannot rely on an ID while leaving ctx.pc stale.
        code(BASE + span + 16, [0x27BDFFF0, 0xAFBF0000,
                               0x03E00008, 0x27BD0010])
    return data


def uint16_boundary_fixture():
    # The final aligned word in a 65535-slot bucket has ID 65535. Its JR delay
    # slot lives in the next bucket; no integer ID may wrap to the reserved zero.
    span = 65535 * 4
    size = span + 4
    data = bytearray(0x100 + size)
    data[:7] = b'\x7fELF\x01\x01\x01'
    struct.pack_into('<HHIIIIIHHHHHH', data, 16,
                     2, 8, 1, BASE + span - 4, 52, 0, 0, 52, 32, 1, 0, 0, 0)
    struct.pack_into('<8I', data, 52, 1, 0x100, BASE, BASE, size, size, 5, 16)
    struct.pack_into('<I', data, 0x100 + span - 4, 0x03E00008)
    return data


def run(command, **kwargs):
    completed = subprocess.run(command, capture_output=True, text=True,
                               timeout=120, **kwargs)
    if completed.returncode:
        raise RuntimeError('Command failed: ' + str(command) + '\n' +
                           completed.stdout + completed.stderr)
    return completed.stdout


def main():
    repo = Path(__file__).resolve().parents[3]
    tool = repo / 'out/framework/Release/psp_recomp.exe'
    cmake = shutil.which('cmake') or str(Path(
        r'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'))
    with tempfile.TemporaryDirectory(prefix='chili_stable_dispatch_') as temp:
        root = Path(temp)
        variants = []
        for name, extra, span, wide in [('before', False, 64, False),
                                         ('after', True, 64, False),
                                         ('oversized', True, 262144, False),
                                         ('exact', True, 262144, True)]:
            folder = root / name
            folder.mkdir()
            elf = folder / 'fixture.elf'
            elf.write_bytes(elf_fixture(extra, wide))
            generated = folder / 'generated'
            run([str(tool), str(elf), '--auto', str(generated), hex(BASE), str(span)])
            variants.append((name, folder, generated))

        before = root / 'before/generated/generated_unit_0001.cpp'
        after = root / 'after/generated/generated_unit_0001.cpp'
        assert before.read_bytes() == after.read_bytes(), 'Adding a seed rewrote an unchanged caller'
        assert '0u, 13u, 0x08804030u' in after.read_text(), 'Cross-unit ID does not match PC offset'
        assert 'case 13u: goto L_08804030;' in (
            root / 'after/generated/generated_unit_0000.cpp').read_text()
        assert 'case 2u: goto L_08804030;' in (
            root / 'oversized/generated/generated_unit_0000.cpp').read_text(), 'Oversized span lost ordered-ID fallback'
        exact_caller = (root / 'exact/generated/generated_unit_0000.cpp').read_text()
        assert '(ctx.pc = 0x08864000u, rt.invoke_chained_call(ctx, &aot_mem))' in exact_caller
        assert 'switch (local_pc)' in (root / 'exact/generated/generated_unit_0001.cpp').read_text()

        boundary = root / 'uint16_boundary.elf'
        boundary.write_bytes(uint16_boundary_fixture())
        for name, span, expected_id in [('maximum_id', 65535 * 4, 65535),
                                       ('past_maximum_span', 65536 * 4, 1)]:
            output = root / name
            run([str(tool), str(boundary), '--auto', str(output), hex(BASE), str(span)])
            assert f'case {expected_id}u: goto L_08843FF8;' in (
                output / 'generated_unit_0000.cpp').read_text(), 'uint16 boundary wrapped or selected wrong fallback'

        # Manual mode has no fixed bucket span and retains its bounded ordered IDs.
        folder = root / 'manual'
        folder.mkdir()
        elf = folder / 'fixture.elf'
        elf.write_bytes(elf_fixture())
        csv = folder / 'functions.csv'
        csv.write_text('name,address,size\ncaller,08804040,1c\ncallee,08804030,c\n')
        generated = folder / 'generated'
        generated.mkdir()
        run([str(tool), str(elf), str(csv), str(generated / 'generated.cpp')])
        assert 'case 1u: goto L_08804040;' in (generated / 'generated.cpp').read_text()
        variants.append(('manual', folder, generated))

        main_source = root / 'fixture_main.cpp'
        main_source.write_text(r'''#include "psprecomp/runtime.hpp"
#include "psprecomp/elf32.hpp"
#include <iostream>
#include <stdexcept>
int main(int argc,char **argv){try{
    psprecomp::Runtime rt;
    auto elf=psprecomp::Elf32Image::from_file(argv[1]);
    (void)elf.load_and_relocate(rt.memory(),0x08804000u);
    psprecomp::register_generated_functions(rt);
    auto &ctx=rt.cpu();
    ctx.gpr[29]=0x08900000u;ctx.gpr[31]=0x08910000u;
    // Manual mode hands calls to the outer dispatcher; automatic mode chains
    // the complete call/return within one outer dispatch.
    rt.run(0x08804040u,argc>2 ? 10u : 1u);
    if(ctx.gpr[2]!=37u || ctx.gpr[29]!=0x08900000u || ctx.pc!=0x08910000u)
        throw std::runtime_error("Generated direct dispatch/return changed guest state: "+rt.stop_reason());
    std::cout<<"PASS: generated call reaches exact callee and preserves stack/return\n";
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}
''')
        cmake_lines = ['cmake_minimum_required(VERSION 3.20)',
                       'project(StableDispatchFixture LANGUAGES CXX)',
                       'set(CMAKE_CXX_STANDARD 20)']
        for name, folder, generated in variants:
            sources = ' '.join('"' + source.as_posix() + '"' for source in generated.glob('*.cpp'))
            cmake_lines += [f'add_executable({name} "{main_source.as_posix()}" {sources})',
                            f'target_include_directories({name} PRIVATE "{(repo / "include").as_posix()}" "{generated.as_posix()}")',
                            f'target_link_libraries({name} PRIVATE "{(repo / "out/framework/Release/psprecomp_core.lib").as_posix()}")']
        (root / 'CMakeLists.txt').write_text('\n'.join(cmake_lines))
        build = root / 'build'
        run([cmake, '-S', str(root), '-B', str(build), '-A', 'x64'])
        run([cmake, '--build', str(build), '--config', 'Release', '--parallel', '4'])
        for name, folder, generated in variants:
            command = [str(build / 'Release' / (name + '.exe')), str(folder / 'fixture.elf')]
            if name == 'manual':
                command.append('outer-dispatch')
            print(name + ': ' + run(command).strip())
        print('PASS: unchanged downstream source SHA256 ' + hashlib.sha256(after.read_bytes()).hexdigest())
        print('PASS: stable offsets, added seed, maximum uint16 ID, oversized-span fallback, exact-PC fallback, manual mode')


if __name__ == '__main__':
    main()
