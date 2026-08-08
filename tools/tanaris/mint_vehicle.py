"""Vehicle.dbc row 1000: byte-clone of Stampy's kit 121 with FIXED_POSITION.

Kit 121's seat exits cleanly; FIXED_POSITION (0x200000, the cannon-root flag)
makes the vehicle structurally unmovable by the controlling client - the
data-level fix for garrisoned players driving buildings around. Raw byte
clone with two dwords patched, so the 40-field layout never needs decoding.
"""
import os, struct, sys
for d in sys.argv[1:]:
    p = os.path.join(d, "Vehicle.dbc")
    if not os.path.exists(p): continue
    blob = open(p, "rb").read()
    magic, rc, fc, rs, sb = struct.unpack_from("<4sIIII", blob, 0)
    assert magic == b"WDBC" and fc == 40
    recs = bytearray(blob[20:20 + rc * rs])
    src = None
    exists = False
    for i in range(rc):
        rid = struct.unpack_from("<i", recs, i * rs)[0]
        if rid == 121: src = bytes(recs[i * rs:(i + 1) * rs])
        if rid == 1000: exists = True
    if exists:
        print("%s: row 1000 already present" % d); continue
    row = bytearray(src)
    struct.pack_into("<i", row, 0, 1000)
    flags = struct.unpack_from("<I", row, 4)[0] | 0x00200000
    struct.pack_into("<I", row, 4, flags)
    out = struct.pack("<4sIIII", b"WDBC", rc + 1, fc, rs, sb) + bytes(recs) + bytes(row) + blob[20 + rc * rs:]
    tmp = p + ".veh-tmp"; open(tmp, "wb").write(out); os.replace(tmp, p)
    print("%s: row 1000 minted (flags 0x%08X)" % (d, flags))
