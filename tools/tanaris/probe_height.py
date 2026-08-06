"""Sample real terrain height out of the server .map tiles for map 1620.

Mirrors GridMap::getHeightFromFloat's indexing exactly (Map.cpp), so the
numbers here are the heights the server itself will report. Used to confirm
the Tanaris arena really is the flat shelf the gameobject spawns suggested,
and that the start Z is sane, without having to stand in it.
"""
import os
import struct
import sys

SIZE_OF_GRIDS = 533.3333
CENTER_GRID_ID = 32
MAP_RESOLUTION = 128

MAP_HEIGHT_NO_HEIGHT = 0x0001
MAP_HEIGHT_AS_INT16 = 0x0002
MAP_HEIGHT_AS_INT8 = 0x0004

DATA = sys.argv[1] if len(sys.argv) > 1 else "/home/brokilodeluxe/wow/servers/tc-lplus-dev/data"
MAP_ID = 1620

_cache = {}


def load_tile(gx, gy):
    key = (gx, gy)
    if key in _cache:
        return _cache[key]
    path = os.path.join(DATA, "maps", "%03u%02u%02u.map" % (MAP_ID, gx, gy))
    if not os.path.exists(path):
        _cache[key] = None
        return None
    blob = open(path, "rb").read()
    # map_fileheader: 3 x 4-byte magic + 8 x uint32
    (_magic, _ver, _build, _areaOfs, _areaSize,
     hOfs, _hSize, _lOfs, _lSize, _holeOfs, _holeSize) = struct.unpack_from("<4sI4sIIIIIIII", blob, 0)
    fourcc, flags, grid_height, grid_max = struct.unpack_from("<IIff", blob, hOfs)
    body = hOfs + 16

    if flags & MAP_HEIGHT_NO_HEIGHT:
        v9 = [grid_height] * (129 * 129)
    elif flags & MAP_HEIGHT_AS_INT16:
        raw = struct.unpack_from("<%dH" % (129 * 129), blob, body)
        step = (grid_max - grid_height) / 65535.0
        v9 = [grid_height + r * step for r in raw]
    elif flags & MAP_HEIGHT_AS_INT8:
        raw = struct.unpack_from("<%dB" % (129 * 129), blob, body)
        step = (grid_max - grid_height) / 255.0
        v9 = [grid_height + r * step for r in raw]
    else:
        v9 = list(struct.unpack_from("<%df" % (129 * 129), blob, body))

    _cache[key] = v9
    return v9


def height(x, y):
    gx = int(CENTER_GRID_ID - x / SIZE_OF_GRIDS)
    gy = int(CENTER_GRID_ID - y / SIZE_OF_GRIDS)
    v9 = load_tile(gx, gy)
    if v9 is None:
        return None
    xs = MAP_RESOLUTION * (CENTER_GRID_ID - x / SIZE_OF_GRIDS)
    ys = MAP_RESOLUTION * (CENTER_GRID_ID - y / SIZE_OF_GRIDS)
    return v9[(int(xs) & 127) * 129 + (int(ys) & 127)]


POINTS = [
    ("Alliance start   ", -8470.0, -3010.0),
    ("Horde start      ", -8260.0, -3010.0),
    ("Alliance graveyard", -8500.0, -3010.0),
    ("Horde graveyard  ", -8230.0, -3010.0),
    ("Alliance gate    ", -8440.0, -3010.0),
    ("Horde gate       ", -8290.0, -3010.0),
    ("Buff N           ", -8365.0, -2950.0),
    ("Buff S           ", -8365.0, -3070.0),
    ("Buff W           ", -8420.0, -3010.0),
    ("Buff E           ", -8310.0, -3010.0),
    ("Arena centre     ", -8365.0, -3010.0),
]

print("=== tiles needed ===")
need = set()
for _n, x, y in POINTS:
    need.add((int(CENTER_GRID_ID - x / SIZE_OF_GRIDS), int(CENTER_GRID_ID - y / SIZE_OF_GRIDS)))
for gx, gy in sorted(need):
    p = os.path.join(DATA, "maps", "%03u%02u%02u.map" % (MAP_ID, gx, gy))
    print("  %03u%02u%02u.map  %s" % (MAP_ID, gx, gy, "present" if os.path.exists(p) else "MISSING"))

print()
print("=== placed object heights (server-authoritative) ===")
for name, x, y in POINTS:
    h = height(x, y)
    print("  %s (%8.1f, %8.1f)  ground z = %s" % (name, x, y, "TILE MISSING" if h is None else "%7.3f" % h))

print()
print("=== flatness sweep over the arena rectangle ===")
lo, hi, bad, n = 1e9, -1e9, 0, 0
worst = None
for i in range(0, 41):
    x = -8565.0 + i * (400.0 / 40)
    for j in range(0, 27):
        y = -3140.0 + j * (260.0 / 26)
        h = height(x, y)
        if h is None:
            bad += 1
            continue
        n += 1
        if h < lo:
            lo = h
        if h > hi:
            hi, worst = h, (x, y)
print("  sampled %d points, %d missing tiles" % (n, bad))
print("  min z %.3f   max z %.3f   spread %.3f" % (lo, hi, hi - lo))
print("  highest point at (%.1f, %.1f)" % worst)
