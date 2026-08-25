"""Extract a private copy of the Tanaris terrain for battleground map 1620.

Map 1620 currently points its Map.dbc Directory at "Kalimdor", so it renders
the real continent's ADTs. That is fine for standing the battleground up, but
it means any terrain edit for the battleground would change the live Tanaris
zone for everyone. This lifts the Tanaris tiles (plus a horizon ring) into a
directory of their own, so 1620 can be edited freely.

What it produces, laid out exactly as it must appear inside the MPQ:

    <out>/World/Maps/TanarisBG/TanarisBG.wdt
    <out>/World/Maps/TanarisBG/TanarisBG_<x>_<y>.adt

Two things worth knowing:

* Tiles are resolved through the client's whole MPQ chain, taking the copy from
  the HIGHEST-priority archive that holds it. Some Tanaris tiles - and the WDT
  itself - are already overridden in patch-Y, so reading from common/patch would
  quietly ship older terrain than the client actually uses.

* ADT internals reference textures, models and WMOs by absolute path and never
  name their own map directory, so renaming the file is a complete rename. The
  WDT is different: its MAIN chunk is a 64x64 grid saying which tiles exist, and
  it must be rewritten to flag only the tiles being shipped. Leave a tile flagged
  that is not shipped and the client tries to load a file that is not there.
"""
import os
import struct
import sys

sys.path.insert(0, r"C:\Projects\Gamedev\wow\tools\mpqpy")
from mpqread import MPQ

DATA = r"C:\Projects\Gamedev\wow\clients\centurion\Data"
OUT = r"C:\Projects\Gamedev\wow\data\patch-staging\TanarisBG"
SRC_DIR = "Kalimdor"
DST_DIR = "TanarisBG"

# Lowest priority first; a later archive overrides an earlier one.
ORDER = ["common.MPQ", "common-2.MPQ", "expansion.MPQ", "lichking.MPQ",
         "patch.MPQ", "patch-2.MPQ", "patch-3.MPQ", "patch-4.MPQ",
         "patch-F.MPQ", "patch-G.MPQ", "patch-H.MPQ", "patch-L.MPQ",
         "patch-T.MPQ", "patch-U.MPQ", "patch-Y.MPQ", "patch-Z.MPQ"]


def load_archives():
    out = []
    for name in ORDER:
        p = os.path.join(DATA, name)
        if os.path.exists(p):
            try:
                out.append((name, MPQ(p)))
            except Exception as exc:
                print("  skipping %s (%s)" % (name, exc))
    return out


def resolve(archives, rel):
    """Highest-priority archive holding rel, or None."""
    hit = None
    for name, m in archives:
        if m.find(rel) is not None:
            hit = (name, m)
    return hit


def rewrite_wdt(blob, keep):
    """Return the WDT with MAIN flags cleared for every tile not in `keep`.

    Chunks are <4-byte magic (little-endian, so reversed on disk)><uint32 size>
    <data>. MAIN holds 64*64 SMAreaInfo entries of 8 bytes, indexed [y][x];
    bit 0 of the first uint32 means "this tile has an ADT".
    """
    pos = 0
    out = bytearray(blob)
    kept = cleared = 0
    while pos + 8 <= len(blob):
        magic = blob[pos:pos + 4]
        size = struct.unpack_from("<I", blob, pos + 4)[0]
        body = pos + 8
        if magic == b"NIAM":                       # 'MAIN' reversed
            if size != 64 * 64 * 8:
                raise ValueError("MAIN chunk is %d bytes, expected %d" % (size, 64 * 64 * 8))
            for y in range(64):
                for x in range(64):
                    off = body + (y * 64 + x) * 8
                    flags = struct.unpack_from("<I", blob, off)[0]
                    if not (flags & 0x1):
                        continue
                    if (x, y) in keep:
                        kept += 1
                    else:
                        struct.pack_into("<I", out, off, flags & ~0x1)
                        cleared += 1
            print("  WDT: kept %d tile flags, cleared %d" % (kept, cleared))
            return bytes(out), kept, cleared
        pos = body + size
    raise ValueError("no MAIN chunk found in WDT")


def build_minimap_trs(archives, keep, out_root):
    """Add a TanarisBG section to textures/Minimap/md5translate.trs.

    The minimap is looked up by map directory too, so without this the
    battleground's minimap is simply blank. The table maps
    "<Dir>\\map<x>_<y>.blp" to an md5-named BLP that lives loose in
    textures/Minimap/ -- so the new section can point at the SAME md5 files
    Kalimdor already uses and no artwork has to be copied at all.

    Caveat worth remembering: this file is shipped whole, not merged. Placing a
    copy in a higher-lettered patch shadows the one in patch-Y entirely, so any
    later minimap work there has to be folded back into this copy.
    """
    rel = "textures\\Minimap\\md5translate.trs"
    hit = resolve(archives, rel)
    if not hit:
        print("  md5translate.trs not found - skipping minimap")
        return
    print("minimap table from %s" % hit[0])
    text = hit[1].extract(rel).decode("utf-8", "replace")
    lines = text.splitlines()

    # Collect the Kalimdor section's entries, keyed by (x, y).
    kal = {}
    in_kal = False
    for line in lines:
        low = line.lower().strip()
        if low.startswith("dir:"):
            in_kal = (low == "dir: kalimdor")
            continue
        if not in_kal or not line.strip():
            continue
        name, _, md5 = line.partition("\t")
        base = name.split("\\")[-1]
        if not base.lower().startswith("map"):
            continue
        try:
            x, y = base[3:-4].split("_")
            kal[(int(x), int(y))] = md5
        except ValueError:
            continue

    have = [t for t in sorted(keep) if t in kal]
    absent = [t for t in sorted(keep) if t not in kal]
    print("  Kalimdor minimap entries: %d; matched for our tiles: %d, absent: %d"
          % (len(kal), len(have), len(absent)))

    section = ["dir: %s" % DST_DIR]
    for x, y in have:
        section.append("%s\\map%d_%d.blp\t%s" % (DST_DIR, x, y, kal[(x, y)]))

    out = os.path.join(out_root, "textures", "Minimap")
    os.makedirs(out, exist_ok=True)
    # Trailing newline preserved; the client parses this line by line.
    with open(os.path.join(out, "md5translate.trs"), "w", encoding="utf-8", newline="\r\n") as f:
        f.write("\n".join(lines + section) + "\n")
    print("  wrote md5translate.trs with a %s section of %d tiles" % (DST_DIR, len(have)))


def main():
    tiles_file = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tiles.txt")
    if len(sys.argv) > 1:
        tiles_file = sys.argv[1]
    keep = set()
    for line in open(tiles_file):
        line = line.split()
        if len(line) == 2:
            keep.add((int(line[0]), int(line[1])))
    print("tiles requested: %d" % len(keep))

    archives = load_archives()
    print("archives readable: %d" % len(archives))

    dest = os.path.join(OUT, "World", "Maps", DST_DIR)
    os.makedirs(dest, exist_ok=True)

    # --- the WDT -----------------------------------------------------------
    wdt_rel = "World\\Maps\\%s\\%s.wdt" % (SRC_DIR, SRC_DIR)
    hit = resolve(archives, wdt_rel)
    if not hit:
        raise SystemExit("Kalimdor.wdt not found in any archive")
    print("WDT from %s" % hit[0])
    wdt, kept, cleared = rewrite_wdt(hit[1].extract(wdt_rel), keep)
    with open(os.path.join(dest, "%s.wdt" % DST_DIR), "wb") as f:
        f.write(wdt)

    if kept != len(keep):
        print("  NOTE: %d requested tiles were not flagged in the source WDT"
              % (len(keep) - kept))

    # --- the ADTs ----------------------------------------------------------
    written = 0
    total = 0
    bysrc = {}
    for x, y in sorted(keep):
        rel = "World\\Maps\\%s\\%s_%d_%d.adt" % (SRC_DIR, SRC_DIR, x, y)
        hit = resolve(archives, rel)
        if not hit:
            print("  MISSING %s" % rel)
            continue
        blob = hit[1].extract(rel)
        if blob is None:
            print("  FAILED TO EXTRACT %s" % rel)
            continue
        with open(os.path.join(dest, "%s_%d_%d.adt" % (DST_DIR, x, y)), "wb") as f:
            f.write(blob)
        bysrc[hit[0]] = bysrc.get(hit[0], 0) + 1
        written += 1
        total += len(blob)

    print("\nwrote %d ADTs, %.1f MB" % (written, total / 1e6))
    for name in ORDER:
        if name in bysrc:
            print("  from %-16s %d" % (name, bysrc[name]))

    # --- the minimap lookup table -----------------------------------------
    build_minimap_trs(archives, keep, OUT)

    print("\noutput: %s" % OUT)


if __name__ == "__main__":
    main()
