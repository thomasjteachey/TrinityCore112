"""Append one GameObjectDisplayInfo row (3.3.5, 19 fields / 76 bytes) to a DBC, verified.

usage: python3 add_godi_row.py <in.dbc> <out.dbc> <id> <ModelName> <minX> <minY> <minZ> <maxX> <maxY> <maxZ>

Layout: ID, ModelName (string offset), Sound[10], GeoBoxMin[3], GeoBoxMax[3], ObjectEffectPackageID.
The model string is APPENDED to the string block (records index it by byte offset, so nothing in
place is ever edited). Refuses if the id exists. Re-reads the output and checks every old record
and the old string block are byte-identical before declaring success.
"""
import struct, sys

src, dst, new_id, model = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
geo = [float(v) for v in sys.argv[5:11]]
assert len(geo) == 6

d = open(src, "rb").read()
magic, n, f, rs, ss = struct.unpack_from("<4s4I", d, 0)
assert magic == b"WDBC" and f == 19 and rs == 76, (magic, f, rs)
recs = d[20:20 + n * rs]
strings = d[20 + n * rs:]
assert len(strings) == ss
ids = {struct.unpack_from("<I", recs, i * rs)[0] for i in range(n)}
assert new_id not in ids, "id %d already present" % new_id

off = len(strings)
strings2 = strings + model.encode("latin1") + b"\0"
rec = struct.pack("<II10I6fI", new_id, off, *([0] * 10), *geo, 0)
assert len(rec) == rs
out = struct.pack("<4s4I", b"WDBC", n + 1, f, rs, len(strings2)) + recs + rec + strings2
open(dst, "wb").write(out)

# verify
o = open(dst, "rb").read()
_, n2, f2, rs2, ss2 = struct.unpack_from("<4s4I", o, 0)
assert (n2, f2, rs2, ss2) == (n + 1, 19, 76, len(strings2))
assert o[20:20 + n * rs] == recs, "old records changed"
ostr = o[20 + n2 * rs2:]
assert ostr[:len(strings)] == strings, "old strings changed"
r = struct.unpack_from("<II10I6fI", o, 20 + n * rs)
name = ostr[r[1]:ostr.index(b"\0", r[1])].decode("latin1")
assert r[0] == new_id and name == model and all(abs(a - b) < 1e-5 for a, b in zip(r[12:18], geo)), r
print("ok: %s -> %s, %d -> %d records, id %d model %s geo %s" % (src, dst, n, n2, r[0], name,
      [round(v, 3) for v in r[12:18]]))
