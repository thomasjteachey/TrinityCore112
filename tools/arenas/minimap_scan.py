"""Find every arena minimap tile in the source, under any naming convention.

Ascension is not consistent about this. Two flat layouts are in use:

    textures\\minimap\\coliseumarena_35_14.blp      <dir>_<col>_<row>.blp
    textures\\minimap\\BladesEdgeArena2b18_24.blp   <dir><col>_<row>.blp

An earlier scan only handled the first and reported Blade's Edge as having no
minimap at all, when it has 25 tiles. So the directory prefix is matched
case-insensitively and the tile numbers are read off the end, with the separator
optional.

Usage: python minimap_scan.py            # what exists in the source
       python minimap_scan.py --client   # and what the client already has
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SCRATCH = (r"C:\Users\broki\AppData\Local\Temp\claude"
           r"\C--Ascension-Launcher-resources-ascension-live"
           r"\b4c02561-8768-40b5-abb1-376e8e1417b6\scratchpad")
sys.path.insert(0, HERE)
sys.path.insert(0, SCRATCH)

from mpq import MPQArchive       # noqa: E402
from deps import Index           # noqa: E402
from gen_arena_sql import ARENAS  # noqa: E402

CLIENT_PATCH_Z = r"C:\Projects\Gamedev\wow\clients\centurion\Data\patch-Z.MPQ"
PREFIX = "TEXTURES\\MINIMAP\\"

# what follows the directory name: an optional underscore, then col_row
TAIL = re.compile(r"^_?(\d{1,2})_(\d{1,2})\.BLP$")


def tiles_for(idx, directory):
    """[(archive key, real name, col, row)] for one map directory."""
    up = directory.upper()
    out = []
    for key in idx.by_path:
        if not key.startswith(PREFIX):
            continue
        base = key[len(PREFIX):]
        if "\\" in base:                      # classic subdirectory layout
            d, _, rest = base.partition("\\")
            if d != up:
                continue
            m = re.match(r"^MAP(\d{1,2})_(\d{1,2})\.BLP$", rest)
            if m:
                out.append((key, idx.by_path[key][1], int(m.group(1)), int(m.group(2))))
            continue
        if not base.startswith(up):
            continue
        m = TAIL.match(base[len(up):])
        if m:
            out.append((key, idx.by_path[key][1], int(m.group(1)), int(m.group(2))))
    return sorted(out, key=lambda t: (t[2], t[3]))


def main():
    idx = Index()
    print("indexed %d source files\n" % len(idx.by_path))

    have = set()
    if "--client" in sys.argv:
        a = MPQArchive(CLIENT_PATCH_Z)
        have = {f.upper().replace("/", "\\") for f in a.list_files()}
        a.close()
        print("patch-Z currently holds %d files\n" % len(have))

    print("%-22s %-6s %6s %6s %8s  %s" % ("DIRECTORY", "MAP", "TILES", "ADTs", "IN PATCH", "SAMPLE"))
    print("-" * 96)
    total_missing = []
    for bg, mid, directory, name, cx, cy, cz, conf, ev in ARENAS:
        tiles = tiles_for(idx, directory)
        adts = sum(1 for k in idx.by_path
                   if k.startswith("WORLD\\MAPS\\" + directory.upper() + "\\") and k.endswith(".ADT"))
        packed = sum(1 for _k, real, _c, _r in tiles
                     if ("TEXTURES\\MINIMAP\\" + os.path.basename(real)).upper() in have)
        missing = [t for t in tiles
                   if ("TEXTURES\\MINIMAP\\" + os.path.basename(t[1])).upper() not in have]
        total_missing.extend((directory, t) for t in missing)
        print("%-22s %-6d %6d %6d %8s  %s" % (
            directory, mid, len(tiles), adts,
            ("%d/%d" % (packed, len(tiles))) if have else "-",
            os.path.basename(tiles[0][1]) if tiles else "*** NONE IN SOURCE ***"))

    if have:
        print("\n%d tile(s) exist in the source but are not in patch-Z" % len(total_missing))
        by_dir = {}
        for d, _t in total_missing:
            by_dir[d] = by_dir.get(d, 0) + 1
        for d, n in sorted(by_dir.items()):
            print("   %-22s %d" % (d, n))
    idx.close()


if __name__ == "__main__":
    main()
