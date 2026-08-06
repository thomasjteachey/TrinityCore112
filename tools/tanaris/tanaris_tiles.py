"""Work out exactly which map tiles Tanaris occupies, from the server's own
area data rather than from a bounding box.

Every 001*.map tile carries an area map: either a single gridArea for the whole
tile, or a 16x16 grid of area ids. A tile belongs to Tanaris if any cell in it
is a Tanaris area. That is much tighter than the WorldMapArea bounding box,
which sweeps in a lot of open ocean and neighbouring zones.

Server tiles are named %03u<gx><gy>.map, but the client's ADT for the same tile
is <Map>_<gy>_<gx>.adt -- the extractor writes (y, x) while the ADT is named
(x, y). Getting that backwards ships the wrong half of the continent, so the
mapping is done in one place here.
"""
import os
import struct
import sys

DATA = sys.argv[1] if len(sys.argv) > 1 else "/home/brokilodeluxe/wow/servers/tc-lplus-dev/data"
RING = int(sys.argv[2]) if len(sys.argv) > 2 else 2

MAP_AREA_NO_AREA = 0x0001

# Tanaris (440) and every child area of it in AreaTable, plus the Caverns of
# Time exterior (2300) which sits inside the zone.
TANARIS_AREAS = {440, 976, 977, 978, 979, 980, 981, 982, 983, 984, 985, 986,
                 987, 988, 989, 990, 991, 992, 1336, 1937, 1938, 1939, 1940, 2300}


def tile_areas(path):
    blob = open(path, "rb").read()
    (_magic, _ver, _build, areaOfs, _areaSize,
     _hOfs, _hSize, _lOfs, _lSize, _holeOfs, _holeSize) = struct.unpack_from("<4sI4sIIIIIIII", blob, 0)
    fourcc, flags, gridArea = struct.unpack_from("<IHH", blob, areaOfs)
    if flags & MAP_AREA_NO_AREA:
        return {gridArea}
    cells = struct.unpack_from("<256H", blob, areaOfs + 8)
    return set(cells)


hits = set()
scanned = 0
for name in sorted(os.listdir(os.path.join(DATA, "maps"))):
    if not name.startswith("001") or not name.endswith(".map"):
        continue
    gx, gy = int(name[3:5]), int(name[5:7])
    scanned += 1
    if tile_areas(os.path.join(DATA, "maps", name)) & TANARIS_AREAS:
        hits.add((gx, gy))

print("scanned %d Kalimdor tiles" % scanned)
print("tiles containing Tanaris area ids: %d" % len(hits))
if not hits:
    raise SystemExit("no Tanaris tiles found - area id list is wrong")

gxs = sorted({g[0] for g in hits})
gys = sorted({g[1] for g in hits})
print("  gx (from world X) %d..%d" % (gxs[0], gxs[-1]))
print("  gy (from world Y) %d..%d" % (gys[0], gys[-1]))

# Grow by RING tiles in every direction so there is terrain on the horizon
# instead of an abrupt edge, but only where Kalimdor actually has a tile.
have = set()
for name in os.listdir(os.path.join(DATA, "maps")):
    if name.startswith("001") and name.endswith(".map"):
        have.add((int(name[3:5]), int(name[5:7])))

grown = set(hits)
for gx, gy in hits:
    for dx in range(-RING, RING + 1):
        for dy in range(-RING, RING + 1):
            t = (gx + dx, gy + dy)
            if t in have:
                grown.add(t)

print("with a %d-tile horizon ring: %d tiles (+%d)" % (RING, len(grown), len(grown) - len(hits)))

# Server (gx, gy) -> client ADT (x=gy, y=gx)
adts = sorted((gy, gx) for gx, gy in grown)
print("\nADT files needed (Kalimdor_<x>_<y>.adt): %d" % len(adts))
print("  x range %d..%d   y range %d..%d"
      % (min(a[0] for a in adts), max(a[0] for a in adts),
         min(a[1] for a in adts), max(a[1] for a in adts)))

with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "tiles.txt"), "w") as f:
    for x, y in adts:
        f.write("%d %d\n" % (x, y))
print("wrote tiles.txt")

# The arena's own tile, as a sanity check that it is in the set.
print("\narena tile server 16204737 -> gx 47, gy 37 -> ADT Kalimdor_37_47.adt : %s"
      % ("PRESENT" if (37, 47) in adts else "MISSING !!"))
