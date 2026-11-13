"""GameObjectDisplayInfo rows 11204-11205: the Violet Hold runes while locked.

  dim_godi_rows.py <in GameObjectDisplayInfo.dbc> <out GameObjectDisplayInfo.dbc>

11204 is the grey see-through crystal every crystal rune (11200-11203) shows while it
waits for the wave; it copies 11200's row (the Un'Goro donor, crystal geobox). 11205 is
the grey see-through Berserking rune; it copies stock 5995's row. Replaces a row that
exists, appends one that does not, reads the result back.
"""
import struct, sys

ROWS = {
    11204: (11200, r"World\Centurion\VioletHold\Runes\VhPowerCrystalDim.mdx"),
    11205: (5995, r"World\Centurion\VioletHold\Runes\VhRuneBerserkerDim.mdx"),
}

src, dst = sys.argv[1], sys.argv[2]
d = open(src, "rb").read()
magic, n, f, rs, ss = struct.unpack("<4s4I", d[:20])
assert magic == b"WDBC" and f == 19 and rs == 76, (magic, f, rs)
sb = 20 + n * rs
idx = {struct.unpack_from("<I", d, 20 + i * rs)[0]: i for i in range(n)}

strings = bytearray(d[sb:])
body = bytearray(d[20:sb])
appended = []
for rid, (donor, model) in ROWS.items():
    assert donor in idx, donor
    rec = list(struct.unpack_from("<19I", d, 20 + idx[donor] * rs))
    rec[0] = rid
    rec[1] = len(strings)
    strings.extend(model.encode() + b"\0")
    packed = struct.pack("<19I", *rec)
    if rid in idx:
        body[idx[rid] * rs: (idx[rid] + 1) * rs] = packed
    else:
        appended.append(packed)

out = struct.pack("<4s4I", b"WDBC", n + len(appended), f, rs, len(strings)) + bytes(body) + b"".join(appended) + bytes(strings)
open(dst, "wb").write(out)

d2 = open(dst, "rb").read()
n2 = struct.unpack_from("<I", d2, 4)[0]
sb2 = 20 + n2 * rs
idx2 = {struct.unpack_from("<I", d2, 20 + i * rs)[0]: i for i in range(n2)}
for rid, (donor, model) in ROWS.items():
    r = struct.unpack_from("<19I", d2, 20 + idx2[rid] * rs)
    assert d2[sb2 + r[1]: d2.index(b"\0", sb2 + r[1])].decode() == model
    assert r[2:] == struct.unpack_from("<19I", d, 20 + idx[donor] * rs)[2:], rid
for rid in idx:
    if rid not in ROWS:
        a = struct.unpack_from("<19I", d, 20 + idx[rid] * rs)
        b = struct.unpack_from("<19I", d2, 20 + idx2[rid] * rs)
        assert a[0] == b[0] and a[2:] == b[2:]
print("ok: %d -> %d rows" % (n, n2))
