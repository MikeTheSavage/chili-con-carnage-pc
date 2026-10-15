import struct,subprocess
from pathlib import Path
from test_import_codegen import fixture
repo=Path(__file__).resolve().parents[3]
out=repo/'profiles/chili/analysis/vfpu_store_fixture';out.mkdir(exist_ok=True)
data=fixture()
# Four operations: column left/right and row left/right; separate base registers.
words=[0xF4800000,0xF4A00002,0xF4C00001,0xF4E00003,0x03E00008,0]
for i in range(0x48//4):struct.pack_into('<I',data,0x100+i*4,words[i] if i<len(words) else 0)
struct.pack_into('<I',data,0x1AC,0);struct.pack_into('<I',data,0x1B0,0)
elf=out/'fixture.elf';elf.write_bytes(data)
subprocess.run([str(repo/'out/framework/Release/psp_recomp.exe'),str(elf),'--auto',str(out/'generated'),'0x08804000','16384'],check=True)
