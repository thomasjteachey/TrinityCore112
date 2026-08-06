"""Read the Tanaris rows back out of the binary DBCs and print them decoded.

Reads with an independent parse of the same field spec, so a row that packed
wrong (bad offset, wrong type, misaligned field) shows up as garbage here
rather than as a silent client crash later.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tanaris_dbc import SPECS

WANT = {
    "Map.dbc": [1620],
    "AreaTable.dbc": [30234],
    "BattlemasterList.dbc": [104],
    "PvpDifficulty.dbc": [91620],
    "WorldSafeLocs.dbc": [52500, 52501, 52502, 52503],
    "WorldMapArea.dbc": [9532],
    "WorldStateUI.dbc": [90023, 90024],
}


def cstr(block, off):
    if off == 0:
        return ""
    end = block.index(b"\x00", off)
    return block[off:end].decode("utf-8", "replace")


def dump(path, spec, ids):
    blob = open(path, "rb").read()
    magic, rc, fc, rs, sb = struct.unpack_from("<4sIIII", blob, 0)
    recs = blob[20:20 + rc * rs]
    block = blob[20 + rc * rs:]
    print("  %-22s recs=%-6d fields=%-3d strblock=%d" % (os.path.basename(path), rc, fc, sb))
    if len(block) != sb:
        print("    !! string block length mismatch: %d vs header %d" % (len(block), sb))
    for i in range(rc):
        rid = struct.unpack_from("<i", recs, i * rs)[0]
        if rid not in ids:
            continue
        vals = []
        for fi, kind in enumerate(spec["fields"]):
            raw = recs[i * rs + fi * 4: i * rs + fi * 4 + 4]
            if kind == "i":
                vals.append(struct.unpack("<i", raw)[0])
            elif kind == "f":
                vals.append(round(struct.unpack("<f", raw)[0], 4))
            else:
                off = struct.unpack("<I", raw)[0]
                if off >= len(block):
                    vals.append("!!OFF %d OUT OF RANGE" % off)
                else:
                    vals.append(repr(cstr(block, off)))
        # collapse the 15 empty locale slots so the line stays readable
        out, run = [], 0
        for v in vals:
            if v == "''":
                run += 1
                continue
            if run:
                out.append("<%d empty>" % run)
                run = 0
            out.append(str(v))
        if run:
            out.append("<%d empty>" % run)
        print("    row %-6d @index %-5d  %s" % (rid, i, " | ".join(out)))


for d in sys.argv[1:]:
    print("=== %s ===" % d)
    for name, spec in SPECS.items():
        p = os.path.join(d, name)
        if os.path.exists(p):
            dump(p, spec, WANT[name])
