"""Uncovered branch targets must remain guest dispatches, not undefined C++."""
import struct
import subprocess
import tempfile
from pathlib import Path

repo = Path(__file__).resolve().parents[3]
base, target = 0x08804000, 0x0ABC0000
data = bytearray(0x110)
data[:7] = b'\x7fELF\x01\x01\x01'
struct.pack_into('<HHIIIIIHHHHHH', data, 16, 2, 8, 1, base, 52, 0, 0, 52, 32, 1, 40, 0, 0)
struct.pack_into('<8I', data, 52, 1, 0x100, base, base, 16, 16, 5, 16)
struct.pack_into('<4I', data, 0x100, 0x0C000000 | (target >> 2), 0, 0x03E00008, 0)
with tempfile.TemporaryDirectory(prefix='chili_unknown_target_') as temp:
    root = Path(temp)
    elf = root / 'unknown.elf'
    elf.write_bytes(data)
    output = root / 'generated'
    subprocess.run([str(repo / 'out/framework/Release/psp_recomp.exe'), str(elf), '--auto',
                    str(output), '0x08804000', '64'], check=True, timeout=10, capture_output=True)
    source = (output / 'generated_unit_0000.cpp').read_text()
    assert 'ctx.pc = 0x0ABC0000u, rt.invoke_chained_call(ctx, &aot_mem)' in source
    assert '&recomp_unit_57728' not in source
    print('PASS: unresolved target retained for runtime dispatch without undefined native symbols')
