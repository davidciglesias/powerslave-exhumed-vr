"""Disassemble N instructions at VA(s). usage: python -I dis.py <exe> <va> [count] [va2 ...]
Stops a function at the first ret/int3 unless count given with a trailing '!'"""
import sys, pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

pe = pefile.PE(sys.argv[1], fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
md = Cs(CS_ARCH_X86, CS_MODE_64)
args = sys.argv[2:]
count = 40
vas = []
for a in args:
    if a.startswith('n='):
        count = int(a[2:])
    else:
        vas.append(int(a, 16))
for va in vas:
    data = pe.get_data(va - base, count * 15)
    print(f'--- {va:#x}')
    for i, ins in enumerate(md.disasm(data, va)):
        if i >= count:
            break
        print(f'  {ins.address:#x}: {ins.mnemonic} {ins.op_str}')
        if ins.mnemonic in ('ret', 'int3') or ins.mnemonic == 'jmp' and i < 3 and count <= 40:
            break
