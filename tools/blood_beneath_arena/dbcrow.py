"""Minimal WDBC reader/appender.

  python3 dbcrow.py show <file.dbc> <id> [id...]        print every field (int, float, string guess)
  python3 dbcrow.py ids <file.dbc>                       count, min, max id
  python3 dbcrow.py where <file.dbc> <field> <value>     ids whose int field == value

Append (used as a library): DBC(path).append(template_id, new_id, ints={idx: v},
floats={idx: v}, strings={idx: 'text'}) then .save(path). Strings are APPENDED to the
string block, never edited in place, so no other record moves.
"""
import struct, sys


class DBC:
    def __init__(self, path):
        d = open(path, 'rb').read()
        if d[:4] != b'WDBC':
            raise ValueError('%s: not WDBC' % path)
        self.n, self.f, self.rs, self.ss = struct.unpack_from('<4I', d, 4)
        if self.rs != self.f * 4:
            raise ValueError('%s: record size %d != 4*%d fields' % (path, self.rs, self.f))
        self.recs = [bytearray(d[20 + i * self.rs: 20 + (i + 1) * self.rs]) for i in range(self.n)]
        self.strings = bytearray(d[20 + self.n * self.rs:])
        if len(self.strings) != self.ss:
            raise ValueError('%s: string block size mismatch' % path)
        self.path = path

    def ids(self):
        return [struct.unpack_from('<I', r, 0)[0] for r in self.recs]

    def row(self, rid):
        for r in self.recs:
            if struct.unpack_from('<I', r, 0)[0] == rid:
                return r
        return None

    def u(self, r, i):
        return struct.unpack_from('<I', r, i * 4)[0]

    def i32(self, r, i):
        return struct.unpack_from('<i', r, i * 4)[0]

    def fl(self, r, i):
        return struct.unpack_from('<f', r, i * 4)[0]

    def s(self, off):
        if off >= len(self.strings):
            return None
        end = self.strings.index(b'\0', off)
        return self.strings[off:end].decode('utf-8', 'replace')

    def add_string(self, text):
        if text == '':
            return 0
        off = len(self.strings)
        self.strings += text.encode('utf-8') + b'\0'
        return off

    def append(self, template_id, new_id, ints=None, floats=None, strings=None):
        if self.row(new_id) is not None:
            raise ValueError('id %d already present in %s' % (new_id, self.path))
        t = self.row(template_id)
        if t is None:
            raise ValueError('template %d missing in %s' % (template_id, self.path))
        r = bytearray(t)
        struct.pack_into('<I', r, 0, new_id)
        for i, v in (ints or {}).items():
            struct.pack_into('<i' if v < 0 else '<I', r, i * 4, v)
        for i, v in (floats or {}).items():
            struct.pack_into('<f', r, i * 4, v)
        for i, v in (strings or {}).items():
            struct.pack_into('<I', r, i * 4, self.add_string(v))
        self.recs.append(r)
        self.n += 1
        return r

    def save(self, path):
        out = bytearray(b'WDBC')
        out += struct.pack('<4I', self.n, self.f, self.rs, len(self.strings))
        for r in self.recs:
            out += r
        out += self.strings
        open(path, 'wb').write(out)

    def show(self, rid):
        r = self.row(rid)
        if r is None:
            print('  id %d: not present' % rid)
            return
        parts = []
        for i in range(self.f):
            v = self.u(r, i)
            fv = self.fl(r, i)
            s = self.s(v) if 0 < v < len(self.strings) and (v == 0 or self.strings[v - 1] == 0) else None
            if s:
                parts.append('%d=%r' % (i, s))
            elif v and abs(fv) > 1e-6 and abs(fv) < 1e7 and (v & 0x7f800000) and v > 0x00ffffff:
                parts.append('%d=%gf' % (i, fv))
            elif v:
                parts.append('%d=%d' % (i, self.i32(r, i)))
        print('  id %d: %s' % (rid, ' '.join(parts)))


if __name__ == '__main__':
    cmd, path = sys.argv[1], sys.argv[2]
    d = DBC(path)
    if cmd == 'ids':
        ids = d.ids()
        print('%s: %d rows, %d fields, min %d max %d' % (path, d.n, d.f, min(ids), max(ids)))
    elif cmd == 'show':
        print('%s (%d fields)' % (path, d.f))
        for a in sys.argv[3:]:
            d.show(int(a))
    elif cmd == 'where':
        fi, val = int(sys.argv[3]), int(sys.argv[4])
        print([i for i, r in zip(d.ids(), d.recs) if d.i32(r, fi) == val])
