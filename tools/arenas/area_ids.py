"""Report the area ids baked into each arena's terrain.

The zone name a player sees is not taken from Map.dbc. The client reads the
`areaid` field out of each MCNK terrain chunk and looks THAT up in AreaTable.dbc.
So an arena ported from someone else's map keeps that map's zone name until the
area ids are dealt with -- which is why map 982 announces itself as The Violet
Hold.

Two ways out, and which one applies depends on what this prints:
  * the ADTs reference an id that is not in AreaTable.dbc -> just add a row with
    that id and the arena names itself, no terrain edit needed.
  * they reference a real zone's id -> that id cannot be renamed without
    renaming the real zone, so the MCNKs have to be rewritten to a new id.

MCNK header: 128 bytes, with areaid at offset 0x34.
"""

import os
import struct
import sys
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from adt_probe import iter_chunks  # noqa: E402
from gen_arena_sql import ARENAS   # noqa: E402

MAPS = r"C:\Ascension\ExtractedMaps\World\Maps"
MCNK_AREAID_OFFSET = 0x34


def adt_area_ids(path):
    with open(path, "rb") as fh:
        data = fh.read()
    out = Counter()
    for magic, off, size in iter_chunks(data):
        if magic != "MCNK" or size < 128:
            continue
        out[struct.unpack_from("<I", data, off + MCNK_AREAID_OFFSET)[0]] += 1
    return out


def main():
    only = sys.argv[1:] or None
    for bg, mid, directory, name, cx, cy, cz, conf, ev in ARENAS:
        if only and str(mid) not in only and directory not in only:
            continue
        d = os.path.join(MAPS, directory)
        if not os.path.isdir(d):
            print("%-22s missing" % directory)
            continue
        total = Counter()
        for fn in sorted(os.listdir(d)):
            if fn.lower().endswith(".adt"):
                total += adt_area_ids(os.path.join(d, fn))
        chunks = sum(total.values())
        top = total.most_common(6)
        print("%-22s map %-5d %5d chunks  area ids: %s" % (
            directory, mid, chunks,
            ", ".join("%d(x%d)" % (a, n) for a, n in top) + ("" if len(total) <= 6 else " ...")))


if __name__ == "__main__":
    main()
