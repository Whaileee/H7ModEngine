"""swfinfo.py <file.swf> [--strings] : tags, symbol classes, imports/exports and AS3 classes of a SWF/GFX file."""
import struct
import sys
import zlib


class R:
    def __init__(self, b, p=0):
        self.b, self.p = b, p

    def u8(self):
        v = self.b[self.p]; self.p += 1; return v

    def u16(self):
        v = struct.unpack_from('<H', self.b, self.p)[0]; self.p += 2; return v

    def u32(self):
        v = struct.unpack_from('<I', self.b, self.p)[0]; self.p += 4; return v

    def d64(self):
        v = struct.unpack_from('<d', self.b, self.p)[0]; self.p += 8; return v

    def cstr(self):
        e = self.b.index(0, self.p); s = self.b[self.p:e].decode('utf-8', 'replace'); self.p = e + 1; return s

    def u30(self):
        v = 0
        for i in range(5):
            c = self.u8(); v |= (c & 0x7f) << (7 * i)
            if not c & 0x80:
                break
        return v

    def s24(self):
        v = self.b[self.p] | self.b[self.p + 1] << 8 | self.b[self.p + 2] << 16; self.p += 3
        return v - (1 << 24) if v & 0x800000 else v


def tags(b, p, end):
    while p < end:
        code_len = struct.unpack_from('<H', b, p)[0]; p += 2
        code, ln = code_len >> 6, code_len & 0x3f
        if ln == 0x3f:
            ln = struct.unpack_from('<I', b, p)[0]; p += 4
        yield code, b[p:p + ln]
        p += ln


def parse_abc(d):
    r = R(d)
    r.u16(); r.u16()
    ints = [0] + [r.u30() for _ in range(max(r.u30() - 1, 0))]
    n = r.u30(); [r.u30() for _ in range(max(n - 1, 0))]
    n = r.u30(); [r.d64() for _ in range(max(n - 1, 0))]
    strs = ['']
    for _ in range(max(r.u30() - 1, 0)):
        ln = r.u30(); strs.append(d[r.p:r.p + ln].decode('utf-8', 'replace')); r.p += ln
    nss = [('', '')]
    for _ in range(max(r.u30() - 1, 0)):
        k = r.u8(); nss.append((k, strs[r.u30()]))
    nssets = [[]]
    for _ in range(max(r.u30() - 1, 0)):
        nssets.append([r.u30() for _ in range(r.u30())])
    mns = [None]
    for _ in range(max(r.u30() - 1, 0)):
        k = r.u8()
        if k in (0x07, 0x0D):
            ns, nm = r.u30(), r.u30(); mns.append((nss[ns][1], strs[nm]))
        elif k in (0x0F, 0x10):
            mns.append(('', strs[r.u30()]))
        elif k in (0x11, 0x12):
            mns.append(('', '*'))
        elif k in (0x09, 0x0E):
            nm, _ = r.u30(), r.u30(); mns.append(('', strs[nm]))
        elif k in (0x1B, 0x1C):
            r.u30(); mns.append(('', '[late]'))
        elif k == 0x1D:
            base = r.u30(); params = [r.u30() for _ in range(r.u30())]
            mns.append(('', 'Vector.<...>'))
        else:
            raise ValueError('multiname kind %x' % k)

    def mn(i):
        m = mns[i] if i < len(mns) else None
        return '*' if not m else (m[0] + '::' + m[1] if m[0] else m[1])

    methods = []
    for _ in range(r.u30()):
        pc = r.u30(); ret = r.u30(); params = [r.u30() for _ in range(pc)]
        name = r.u30(); flags = r.u8()
        if flags & 0x08:
            for _ in range(r.u30()):
                r.u30(); r.u8()
        if flags & 0x80:
            [r.u30() for _ in range(pc)]
        methods.append((params, ret))
    for _ in range(r.u30()):
        r.u30(); [(r.u30(), r.u30()) for _ in range(r.u30())]

    def traits():
        out = []
        for _ in range(r.u30()):
            name = r.u30(); kind = r.u8(); k = kind & 0x0f
            if k in (0, 6):
                r.u30(); tn = r.u30(); vi = r.u30()
                if vi:
                    r.u8()
                out.append(('var' if k == 0 else 'const', mn(name), mn(tn)))
            elif k in (1, 2, 3):
                r.u30(); m = r.u30()
                params, ret = methods[m]
                out.append(({1: 'function', 2: 'get', 3: 'set'}[k], mn(name),
                             '(' + ', '.join(mn(p) for p in params) + '):' + mn(ret)))
            elif k == 4:
                r.u30(); r.u30(); out.append(('class', mn(name), ''))
            elif k == 5:
                r.u30(); r.u30(); out.append(('function', mn(name), ''))
            if kind & 0x40:
                [r.u30() for _ in range(r.u30())]
        return out

    insts = []
    for _ in range(r.u30()):
        name = r.u30(); sup = r.u30(); fl = r.u8()
        if fl & 0x08:
            r.u30()
        ifaces = [r.u30() for _ in range(r.u30())]
        r.u30()
        insts.append((mn(name), mn(sup), traits()))
    return strs, insts


def main():
    path = sys.argv[1]
    b = open(path, 'rb').read()
    sig = b[:3]
    if sig in (b'CFX', b'CWS'):
        b = b[:8] + zlib.decompress(b[8:])
    nbits = b[8] >> 3
    p = 8 + (5 + 4 * nbits + 7) // 8 + 4
    show_strings = '--strings' in sys.argv
    counts = {}
    for code, d in tags(b, p, len(b)):
        counts[code] = counts.get(code, 0) + 1
        if code == 76:  # SymbolClass
            r = R(d)
            for _ in range(r.u16()):
                cid = r.u16(); print('symbol %5d %s' % (cid, r.cstr()))
        elif code in (56,):
            r = R(d)
            for _ in range(r.u16()):
                cid = r.u16(); print('export %5d %s' % (cid, r.cstr()))
        elif code in (57, 71):
            r = R(d); url = r.cstr()
            if code == 71:
                r.u16()
            for _ in range(r.u16()):
                cid = r.u16(); print('import %5d %s from %s' % (cid, r.cstr(), url))
        elif code == 82:  # DoABC
            r = R(d); r.u32(); nm = r.cstr()
            strs, insts = parse_abc(d[r.p:])
            for name, sup, tr in insts:
                print('\nclass %s extends %s' % (name, sup))
                for t in tr:
                    print('    %-8s %s %s' % t)
            if show_strings:
                print('\nstrings:', ' | '.join(s for s in strs if s))
    print('\ntag counts:', dict(sorted(counts.items())))


main()
