"""Find textures an arena's terrain needs that the client cannot resolve.

Bright green terrain is the client failing to load a ground texture. The ADT
names its ground textures in MTEX; each of those also implies a `_s.blp`
specular companion that the client loads alongside it and that MTEX never
mentions -- which is exactly the sort of file a reference walk misses.

Checks each name against what the Centurion client can actually see: all of its
Data\\*.MPQ, in load order.

Usage: python missing_textures.py [mapid ...]
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SCRATCH = (r"C:\Users\broki\AppData\Local\Temp\claude"
           r"\C--Ascension-Launcher-resources-ascension-live"
           r"\b4c02561-8768-40b5-abb1-376e8e1417b6\scratchpad")
sys.path.insert(0, HERE)
sys.path.insert(0, SCRATCH)

from mpq import MPQArchive       # noqa: E402
from adt_probe import find_chunk, split_strings  # noqa: E402
from gen_arena_sql import ARENAS  # noqa: E402

CLIENT_DATA = r"C:\Projects\Gamedev\wow\clients\centurion\Data"
MAPS = r"C:\Ascension\ExtractedMaps\World\Maps"
ASC_DATA = r"C:\Ascension\Launcher\resources\ascension-live\Data"


def index_dir(d, label):
    have = set()
    for fn in sorted(os.listdir(d)):
        if not fn.lower().endswith(".mpq"):
            continue
        try:
            a = MPQArchive(os.path.join(d, fn))
        except Exception as e:
            print("  !! %s: %s" % (fn, e))
            continue
        for f in a.list_files():
            have.add(f.upper().replace("/", "\\"))
        a.close()
    print("%s: %d paths" % (label, len(have)))
    return have


def main():
    want_maps = [int(x) for x in sys.argv[1:]] or [985, 986, 1504, 1552]

    client = index_dir(CLIENT_DATA, "centurion client")
    asc = index_dir(ASC_DATA, "ascension source")

    grand_missing = {}
    for bg, mid, directory, name, cx, cy, cz, conf, ev in ARENAS:
        if mid not in want_maps:
            continue
        d = os.path.join(MAPS, directory)
        tex = set()
        for fn in sorted(os.listdir(d)):
            if not fn.lower().endswith(".adt"):
                continue
            with open(os.path.join(d, fn), "rb") as fh:
                data = fh.read()
            off, size = find_chunk(data, "MTEX")
            if off is not None:
                for t in split_strings(data[off:off + size]):
                    tex.add(t)

        # each ground texture implies a specular companion the ADT never names
        implied = set()
        for t in tex:
            if t.lower().endswith(".blp"):
                implied.add(t[:-4] + "_s.blp")

        miss_main = sorted(t for t in tex if t.upper().replace("/", "\\") not in client)
        miss_spec = sorted(t for t in implied
                           if t.upper().replace("/", "\\") not in client
                           and t.upper().replace("/", "\\") in asc)

        print("\n=== %s (map %d) ===" % (directory, mid))
        print("  %d MTEX textures; %d missing from the client" % (len(tex), len(miss_main)))
        for t in miss_main[:12]:
            in_asc = "in ascension" if t.upper().replace("/", "\\") in asc else "NOT IN ASCENSION EITHER"
            print("     %-64s %s" % (t, in_asc))
        if len(miss_main) > 12:
            print("     ... and %d more" % (len(miss_main) - 12))
        print("  %d implied _s.blp specular maps missing but available" % len(miss_spec))
        for t in miss_spec[:6]:
            print("     %s" % t)
        if len(miss_spec) > 6:
            print("     ... and %d more" % (len(miss_spec) - 6))

        grand_missing[directory] = miss_main + miss_spec

    total = sorted({t for v in grand_missing.values() for t in v})
    print("\n\n=== %d distinct files to add ===" % len(total))
    out = os.path.join(HERE, "missing_textures.txt")
    with open(out, "w", encoding="utf-8") as fh:
        for t in total:
            fh.write(t + "\n")
    print("wrote %s" % out)


if __name__ == "__main__":
    main()
