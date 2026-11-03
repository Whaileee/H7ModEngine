"""Quick static scan of the game exe: sections, anchor strings (ASCII + UTF-16), and UE3
native-function lookup tables ({"execFoo", &Class::execFoo} pairs)."""
import struct, sys, re

path = sys.argv[1]
data = open(path, 'rb').read()

pe = struct.unpack_from('<I', data, 0x3C)[0]
nsec = struct.unpack_from('<H', data, pe + 6)[0]
opt_size = struct.unpack_from('<H', data, pe + 20)[0]
opt = pe + 24
image_base = struct.unpack_from('<Q', data, opt + 24)[0]
secs = []
for i in range(nsec):
    o = opt + opt_size + i * 40
    name = data[o:o + 8].rstrip(b'\0').decode()
    vsize, va, rsize, raw = struct.unpack_from('<IIII', data, o + 8)
    secs.append((name, va, vsize, raw, rsize))
print('ImageBase', hex(image_base))
for s in secs:
    print('  %-8s va=%08x vsize=%08x raw=%08x rsize=%08x' % s)

def off2va(off):
    for n, va, vs, raw, rs in secs:
        if raw <= off < raw + rs:
            return image_base + va + off - raw
def va2off(v):
    rva = v - image_base
    for n, va, vs, raw, rs in secs:
        if va <= rva < va + rs:
            return raw + rva - va

anchors = sys.argv[2:] or []
for a in anchors:
    for enc, pat in (('A', a.encode()), ('W', a.encode('utf-16-le'))):
        for m in list(re.finditer(re.escape(pat), data))[:5]:
            # show surrounding string
            s = m.start()
            if enc == 'A':
                b = s
                while b > 0 and data[b - 1] != 0: b -= 1
                e = data.index(b'\0', s)
                txt = data[b:e].decode('latin1')
            else:
                b = s
                while b > 1 and data[b - 2:b] != b'\0\0': b -= 2
                e = s
                while data[e:e + 2] != b'\0\0': e += 2
                txt = data[b:e].decode('utf-16-le', 'replace')
            print(f'[{enc}] {a!r} va={off2va(b):#x}  {txt[:120]!r}')

# Native lookup tables: pointer to ASCII "exec..." immediately followed by code pointer
text = [s for s in secs if s[0] == '.text'][0]
tlo, thi = image_base + text[1], image_base + text[1] + text[2]
exec_strs = {}
for m in re.finditer(rb'\0(exec[A-Za-z0-9_]{2,80})\0', data):
    exec_strs[off2va(m.start(1))] = m.group(1).decode()
print('exec* ASCII strings:', len(exec_strs))
found = 0
rdata = [s for s in secs if s[0] in ('.rdata', '.data')]
for n, va, vs, raw, rs in rdata:
    for o in range(raw, raw + rs - 16, 8):
        p = struct.unpack_from('<Q', data, o)[0]
        if p in exec_strs:
            f = struct.unpack_from('<Q', data, o + 8)[0]
            if tlo <= f < thi:
                found += 1
                if found <= 15 or exec_strs[p] in ('execDynamicLoadObject', 'execFindObject', 'execGetName'):
                    print(f'  table@{off2va(o):#x} {exec_strs[p]} -> {f:#x}')
print('exec table entries:', found)
