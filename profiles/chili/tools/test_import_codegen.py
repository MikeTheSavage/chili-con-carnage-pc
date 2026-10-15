"""Regression for the direct-import JAL generator loop; no game data needed."""
import struct
import subprocess
import tempfile
from pathlib import Path


def fixture():
    base = 0x08804000
    data = bytearray(0x400)
    def u32(offset, value):
        struct.pack_into('<I', data, offset, value)
    def u16(offset, value):
        struct.pack_into('<H', data, offset, value)
    data[:7] = b'\x7fELF\x01\x01\x01'
    u16(16, 2); u16(18, 8); u32(20, 1); u32(24, base)
    u32(28, 52); u32(32, 0x300)
    u16(40, 52); u16(42, 32); u16(44, 1)
    u16(46, 40); u16(48, 4); u16(50, 3)
    struct.pack_into('<8I', data, 52, 1, 0x100, base, base, 0x180, 0x180, 5, 16)
    # Save $ra, call a firmware import with a non-NOP delay slot, restore $ra.
    code = [0x27BDFFF0, 0xAFBF0000,
            0x0C000000 | ((base + 0x40) >> 2), 0x24040007,
            0x8FBF0000, 0x03E00008, 0x27BD0010]
    for i, word in enumerate(code):
        u32(0x100 + i * 4, word)
    u32(0x140, 0x03E00008); u32(0x144, 0x0000000C)
    data[0x184:0x18C] = b'fixture\x00'
    u32(0x180 + 44, base + 0xC0); u32(0x180 + 48, base + 0xD4)
    u32(0x1C0, base + 0xE0); data[0x1C8] = 5; u16(0x1CA, 1)
    u32(0x1CC, base + 0xF0); u32(0x1D0, base + 0x40)
    data[0x1E0:0x1E8] = b'sceCtrl\x00'; u32(0x1F0, 0x1F4011E6)
    names = b'\x00.text\x00.rodata.sceModuleInfo\x00.shstrtab\x00'
    data[0x280:0x280 + len(names)] = names
    struct.pack_into('<10I', data, 0x328, 1, 1, 6, base, 0x100, 0x48, 0, 0, 4, 0)
    struct.pack_into('<10I', data, 0x350, 7, 1, 2, base + 0x80, 0x180, 52, 0, 0, 4, 0)
    struct.pack_into('<10I', data, 0x378, 29, 3, 0, 0, 0x280, len(names), 0, 0, 1, 0)
    return data


if __name__ == '__main__':
    repo = Path(__file__).resolve().parents[3]
    tool = repo / 'out/framework/Release/psp_recomp.exe'
    with tempfile.TemporaryDirectory(prefix='chili_import_codegen_') as temp:
        root = Path(temp)
        elf = root / 'fixture.elf'
        elf.write_bytes(fixture())
        output = root / 'generated'
        subprocess.run([str(tool), str(elf), '--auto', str(output), '0x08804000', '64'],
                       check=True, timeout=10, capture_output=True)
        unit = (output / 'generated_unit_0000.cpp').read_text()
        registry = (output / 'generated_registry.cpp').read_text()
        assert 'ctx.pc = 0x08804040u;' in unit
        assert 'ctx.gpr[31] = (0x08804010u)' in unit
        assert 'ctx.gpr[4] = (0u + static_cast<std::uint32_t>(7))' in unit
        assert 'sceCtrl' in registry and '0x1F4011E6u' in registry
        print('PASS: direct import generation terminates and preserves link/delay-slot writes')
