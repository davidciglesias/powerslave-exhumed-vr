"""Map AngelScript registration decl strings -> native function pointers in the retail exe.

Finds every rip-relative LEA that references an ASCII string in .rdata that looks like an
AngelScript declaration, then looks in a small window around it for LEAs into .text
(the native function / method pointer handed to Register*). Output: TSV.
usage: python -I as_registry.py <exe> <out.tsv>
"""
import sys, re, pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_MEM, X86_REG_RIP

exe, out = sys.argv[1], sys.argv[2]
pe = pefile.PE(exe, fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
secs = {s.Name.rstrip(b'\0').decode(): s for s in pe.sections}
text, rdata = secs['.text'], secs['.rdata']
tdata = text.get_data()
tva = base + text.VirtualAddress
rva0 = base + rdata.VirtualAddress
rbytes = rdata.get_data()

def cstr_at(va):
    off = va - rva0
    if off < 0 or off >= len(rbytes):
        return None
    end = rbytes.find(b'\0', off, off + 400)
    if end < 0:
        return None
    s = rbytes[off:end]
    if len(s) < 3 or any(c < 0x20 or c > 0x7e for c in s):
        return None
    return s.decode()

DECL = re.compile(r'^[A-Za-z_][\w:<>@&\s,*\[\]]*\s[\w:@&*]*[A-Za-z_~]\w*\s*\(.*\)\s*(const)?$')

md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True
md.skipdata = True
# linear sweep: gather all rip-relative LEAs with their addresses
leas = []  # (insn_addr, target, reg)
for ins in md.disasm(tdata, tva):
    if ins.mnemonic == 'lea' and len(ins.operands) == 2:
        m = ins.operands[1]
        if m.type == X86_OP_MEM and m.mem.base == X86_REG_RIP:
            leas.append((ins.address, ins.address + ins.size + m.mem.disp, ins.reg_name(ins.operands[0].reg)))

tend = tva + text.Misc_VirtualSize
rows = []
for i, (a, t, reg) in enumerate(leas):
    s = cstr_at(t)
    if not s or '(' not in s or not DECL.match(s):
        continue
    # nearby text-pointing LEAs (function pointers), within +-0x60 bytes
    fns = []
    j = i - 1
    while j >= 0 and a - leas[j][0] < 0x60:
        if tva <= leas[j][1] < tend:
            fns.append(leas[j][1])
        j -= 1
    j = i + 1
    while j < len(leas) and leas[j][0] - a < 0x60:
        if tva <= leas[j][1] < tend:
            fns.append(leas[j][1])
        j += 1
    # object name: closest preceding string LEA that isn't a decl
    obj = ''
    j = i - 1
    while j >= 0 and a - leas[j][0] < 0x40:
        s2 = cstr_at(leas[j][1])
        if s2 and '(' not in s2:
            obj = s2
            break
        j -= 1
    rows.append((a, obj, s, ' '.join(hex(f) for f in fns[:3])))

with open(out, 'w') as f:
    for a, obj, s, fns in rows:
        f.write(f'{a:#x}\t{obj}\t{s}\t{fns}\n')
print(len(leas), 'leas;', len(rows), 'decl refs')
