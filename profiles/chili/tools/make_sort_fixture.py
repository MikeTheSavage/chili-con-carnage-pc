"""Generate redistributable synthetic VFPU test code, not game code."""
import struct
import subprocess
from pathlib import Path

repo = Path(__file__).resolve().parents[3]
root = repo / 'out/chili-analysis/sort-fixture'
root.mkdir(parents=True, exist_ok=True)
base = 0x08804000
data = bytearray(0x180)
data[:7] = b'\x7fELF\x01\x01\x01'
struct.pack_into('<HHIIIIIHHHHHH', data, 16, 2, 8, 1, base, 52, 0, 0, 52, 32, 1, 40, 0, 0)
struct.pack_into('<8I', data, 52, 1, 0x100, base, base, 0x80, 0x80, 5, 16)
rows = ['name,address,size']
for index, opcode in enumerate([0xD0408080, 0xD0418080, 0xD0488080, 0xD0498080]):
    offset = index * 0x20
    struct.pack_into('<3I', data, 0x100 + offset, opcode, 0x03E00008, 0)
    rows.append(f'sort{index + 1},{base + offset:08X},0000000C')
(root / 'sort.elf').write_bytes(data)
(root / 'functions.csv').write_text('\n'.join(rows) + '\n')
subprocess.run([str(repo / 'out/framework/Release/psp_recomp.exe'),
                str(root / 'sort.elf'), str(root / 'functions.csv'), str(root / 'generated.cpp')],
               check=True, timeout=10)
