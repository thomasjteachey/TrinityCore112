"""Remove records by id from a WDBC binary, in place.

Records are fixed-size, so removal is a filter pass plus a header fix. Strings
belonging to removed records are left orphaned in the string block, which the
format tolerates - every remaining offset stays valid.

Used when a row's VALUES change: the appenders (vhr_dbc.py and friends) skip
ids that already exist, so a changed row has to be stripped first and then
re-appended. Written for the Violet Hold WorldStateUI reformat but generic.

usage: strip_dbc_rows.py <file.dbc> <id> [<id> ...]
"""
import os
import struct
import sys


def strip(path, ids):
    blob = open(path, "rb").read()
    magic, recs, fields, rsize, ssize = struct.unpack_from("<4sIIII", blob, 0)
    if magic != b"WDBC":
        raise SystemExit("%s: not a WDBC file" % path)

    body = blob[20:20 + recs * rsize]
    kept = bytearray()
    removed = 0
    for i in range(recs):
        rec = body[i * rsize:(i + 1) * rsize]
        if struct.unpack_from("<i", rec, 0)[0] in ids:
            removed += 1
        else:
            kept += rec

    if not removed:
        print("%s: nothing to strip" % os.path.basename(path))
        return

    out = struct.pack("<4sIIII", b"WDBC", recs - removed, fields, rsize, ssize)
    out += bytes(kept)
    out += blob[20 + recs * rsize:]

    tmp = path + ".strip-tmp"
    with open(tmp, "wb") as f:
        f.write(out)
    os.replace(tmp, path)
    print("%s: stripped %d record(s)" % (os.path.basename(path), removed))


if __name__ == "__main__":
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    strip(sys.argv[1], {int(a) for a in sys.argv[2:]})
