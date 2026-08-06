"""Verify an arena map as the CLIENT sees it, through its whole MPQ chain.

A crash on entering a custom map is almost never "the files are missing" in the
obvious sense -- it is usually one of:

  * the WDT's MAIN chunk flags a tile as present that was never shipped. The
    client trusts that flag, tries to load the ADT, and dies.
  * an ADT references a WMO or M2 that no archive in the chain has.
  * Map.dbc's Directory does not match the folder that was actually packed.

So this reads the client's own archives in load order, resolves the map exactly
the way the client would, and reports which of those three (if any) is true.

Usage: python check_client_map.py [mapid ...]     default: every arena
"""

import os
import re
import struct
import sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
SCRATCH = (r"C:\Users\broki\AppData\Local\Temp\claude"
           r"\C--Ascension-Launcher-resources-ascension-live"
           r"\b4c02561-8768-40b5-abb1-376e8e1417b6\scratchpad")
sys.path.insert(0, HERE)
sys.path.insert(0, SCRATCH)

from mpq import MPQArchive                       # noqa: E402
from dbc import DBC                              # noqa: E402
from adt_probe import find_chunk, split_strings  # noqa: E402
from gen_arena_sql import ARENAS                 # noqa: E402

CLIENT = r"C:\Projects\Gamedev\wow\clients\centurion"
DATA = os.path.join(CLIENT, "Data")
LOCALE = os.path.join(DATA, "enUS")


def load_order():
    """Archives in the order the client applies them: later wins."""
    def key(fn):
        base = fn.lower()
        if not base.startswith("patch"):
            return (0, base)
        m = re.match(r"patch-(.+)\.mpq$", base)
        if not m:
            return (1, "")            # plain patch.MPQ
        tag = m.group(1)
        return (2, len(tag), tag)     # patch-2, patch-3, then A..Z
    out = []
    for d in (DATA, LOCALE):
        if not os.path.isdir(d):
            continue
        for fn in sorted((f for f in os.listdir(d) if f.lower().endswith(".mpq")), key=key):
            out.append(os.path.join(d, fn))
    return out


class Client:
    def __init__(self):
        self.owner = {}
        self._open = {}
        for path in load_order():
            try:
                a = MPQArchive(path)
            except Exception as e:
                print("  !! cannot open %s: %s" % (os.path.basename(path), e))
                continue
            for f in a.list_files():
                self.owner[f.upper().replace("/", "\\")] = (path, f)
            a.close()

    def has(self, p):
        return p.upper().replace("/", "\\") in self.owner

    def read(self, p):
        hit = self.owner.get(p.upper().replace("/", "\\"))
        if not hit:
            return None
        path, real = hit
        if path not in self._open:
            self._open[path] = MPQArchive(path)
        try:
            return self._open[path].read_file(real)
        except Exception:
            return None

    def where(self, p):
        hit = self.owner.get(p.upper().replace("/", "\\"))
        return os.path.basename(hit[0]) if hit else None

    def close(self):
        for a in self._open.values():
            a.close()


def wdt_tiles(blob):
    """(col, row) pairs the WDT's MAIN chunk flags as present."""
    off, size = find_chunk(blob, "MAIN")
    tiles = set()
    if off is None:
        return tiles
    for row in range(64):
        for col in range(64):
            base = off + (row * 64 + col) * 8
            if base + 8 > off + size:
                continue
            if struct.unpack_from("<I", blob, base)[0] & 1:
                tiles.add((col, row))
    return tiles


def main():
    want = [int(a) for a in sys.argv[1:]] or [a[1] for a in ARENAS]

    print("opening the client's archives ...", flush=True)
    c = Client()
    print("%d unique paths across %d archives\n" % (len(c.owner), len(load_order())))

    mapdbc = c.read("DBFilesClient\\Map.dbc")
    if not mapdbc:
        raise SystemExit("!! the client has no Map.dbc")
    m = DBC(mapdbc)
    dirs = {m.uint(i, 0): m.str(i, 1) for i in range(m.count)}

    problems = 0
    for bg, mid, directory, name, cx, cy, cz, conf, ev in ARENAS:
        if mid not in want:
            continue
        print("=" * 74)
        cdir = dirs.get(mid)
        print("map %-5d %-24s Map.dbc Directory = %r" % (mid, name, cdir))
        if cdir is None:
            print("   !! NOT IN THE CLIENT'S Map.dbc")
            problems += 1
            continue
        if cdir.lower() != directory.lower():
            print("   !! Directory mismatch: expected %r" % directory)
            problems += 1

        wdt_path = "World\\Maps\\%s\\%s.wdt" % (cdir, cdir)
        if not c.has(wdt_path):
            print("   !! NO WDT at %s" % wdt_path)
            problems += 1
            continue
        wdt = c.read(wdt_path)
        print("   WDT in %s, %d bytes" % (c.where(wdt_path), len(wdt)))

        flagged = wdt_tiles(wdt)
        shipped, missing = set(), []
        for (col, row) in sorted(flagged):
            adt = "World\\Maps\\%s\\%s_%d_%d.adt" % (cdir, cdir, col, row)
            if c.has(adt):
                shipped.add((col, row))
            else:
                missing.append((col, row))
        print("   WDT flags %d tiles; %d present, %d MISSING" % (len(flagged), len(shipped), len(missing)))
        if missing:
            problems += 1
            print("   !! the client will try to load these and has nothing to load:")
            for col, row in missing[:12]:
                print("        %s_%d_%d.adt" % (cdir, col, row))
            if len(missing) > 12:
                print("        ... and %d more" % (len(missing) - 12))

        # assets referenced by the tiles that ARE present
        miss_wmo, miss_m2, miss_blp = set(), set(), set()
        for (col, row) in sorted(shipped):
            blob = c.read("World\\Maps\\%s\\%s_%d_%d.adt" % (cdir, cdir, col, row))
            if blob is None:
                continue
            for magic, bucket in (("MWMO", miss_wmo), ("MMDX", miss_m2), ("MTEX", miss_blp)):
                off, size = find_chunk(blob, magic)
                if off is None:
                    continue
                for ref in split_strings(blob[off:off + size]):
                    cand = ref
                    if magic == "MMDX" and cand.lower().endswith((".mdx", ".mdl")):
                        cand = cand[:-4] + ".m2"
                    if not c.has(cand):
                        bucket.add(cand)
        for label, bucket in (("WMO", miss_wmo), ("M2", miss_m2), ("texture", miss_blp)):
            if bucket:
                problems += 1
                print("   !! %d %s reference(s) the client cannot resolve:" % (len(bucket), label))
                for r in sorted(bucket)[:8]:
                    print("        %s" % r)
                if len(bucket) > 8:
                    print("        ... and %d more" % (len(bucket) - 8))
        if not missing and not miss_wmo and not miss_m2 and not miss_blp:
            print("   OK: every flagged tile is present and every reference resolves")

    c.close()
    print("\n%s" % ("%d problem area(s) found" % problems if problems else "no problems found"))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
