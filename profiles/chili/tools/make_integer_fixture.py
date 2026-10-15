import struct
import subprocess
from pathlib import Path

repo = Path(__file__).resolve().parents[3]
root = repo / 'out/chili-analysis/integer-fixture'
root.mkdir(parents=True, exist_ok=True)
base = 0x08804000
data = bytearray(0x1A0)
data[:7] = b'\x7fELF\x01\x01\x01'
struct.pack_into('<HHIIIIIHHHHHH', data, 16, 2, 8, 1, base, 52, 0, 0, 52, 32, 1, 40, 0, 0)
struct.pack_into('<8I', data, 52, 1, 0x100, base, base, 0xA0, 0xA0, 5, 16)
rows = ['name,address,size']
for index, opcode in enumerate([0x2082FFFF, 0x0085001C, 0x0085001D, 0x0085002E, 0x0085002F]):
    offset = index * 0x20
    struct.pack_into('<3I', data, 0x100 + offset, opcode, 0x03E00008, 0)
    rows.append(f'integer{index},{base + offset:08X},0000000C')
(root / 'integer.elf').write_bytes(data)
(root / 'functions.csv').write_text('\n'.join(rows) + '\n')
subprocess.run([str(repo / 'out/framework/Release/psp_recomp.exe'), str(root / 'integer.elf'),
                str(root / 'functions.csv'), str(root / 'generated.cpp')], check=True, timeout=10)
