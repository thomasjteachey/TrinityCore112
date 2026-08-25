"""Measure an arena's real floor height from its WMO mesh.

Why this exists: the arena's world position comes from the ADT placement
(adt_probe.py), but that gives the WMO's origin, not its floor. Those are not the
same thing and the gap is not constant -- Tiger's Peak puts its origin within a
yard of the floor, Tol'viron puts it 22 yards below. Spawning a player at the
origin would drop them through the map in one arena and into the ceiling in
another, so the floor has to be measured.

How: WMO placements are rotated about the vertical axis only, and a rotation
about the vertical axis does not change height. The MODF position's height
component and a group vertex's local up component therefore just add:

    worldZ(vertex) = wmoWorldZ + vertex.y

so the floor can be read off the mesh without reconstructing the full placement
transform. The floor is found by accumulating the surface area of near-horizontal
triangles into a height histogram; the tallest bucket is the fighting floor.

Validated by --selftest against the two arenas already shipped, whose door
heights were established independently:
    Tol'viron (BattlegroundTV.cpp)   doors at Z 24.41
    Tiger's Peak (BattlegroundTTP.cpp) doors at Z 380.71
"""

import argparse
import json
import os
import struct
import sys
from collections import defaultdict

SCRATCH = (r"C:\Users\broki\AppData\Local\Temp\claude"
           r"\C--Ascension-Launcher-resources-ascension-live"
           r"\b4c02561-8768-40b5-abb1-376e8e1417b6\scratchpad")
sys.path.insert(0, SCRATCH)
from mpq import MPQArchive  # noqa: E402

DATA = r"C:\Ascension\Launcher\resources\ascension-live\Data"
CACHE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "_wmocache")


# ------------------------------------------------------------------ chunk walk
def iter_chunks(data, start, end):
    pos = start
    while pos + 8 <= end:
        magic = data[pos:pos + 4][::-1].decode("ascii", "replace")
        (size,) = struct.unpack_from("<I", data, pos + 4)
        payload = pos + 8
        if payload + size > end:
            break
        yield magic, payload, size
        pos = payload + size


def find_chunk(data, want, start=0, end=None):
    end = len(data) if end is None else end
    for magic, off, size in iter_chunks(data, start, end):
        if magic == want:
            return off, size
    return None, 0


# ------------------------------------------------------------------ extraction
def extract(archive_name, names):
    """Pull WMO files out of an MPQ into a local cache; returns {name: path}."""
    os.makedirs(CACHE, exist_ok=True)
    out = {}
    todo = []
    for n in names:
        dest = os.path.join(CACHE, n.replace("\\", "_").replace("/", "_"))
        out[n] = dest
        if not os.path.exists(dest):
            todo.append((n, dest))
    if todo:
        a = MPQArchive(os.path.join(DATA, archive_name))
        for n, dest in todo:
            try:
                with open(dest, "wb") as fh:
                    fh.write(a.read_file(n))
            except Exception as e:
                print("   !! could not extract %s: %s" % (n, e))
                out.pop(n, None)
        a.close()
    return out


# ----------------------------------------------------------------- group parse
MOGP_HEADER = 68


def group_triangles(path):
    """Yield (v0, v1, v2, collides) in WMO-local space for one group file."""
    with open(path, "rb") as fh:
        data = fh.read()

    mogp_off, mogp_size = find_chunk(data, "MOGP")
    if mogp_off is None:
        return [], []
    sub_start = mogp_off + MOGP_HEADER
    sub_end = mogp_off + mogp_size

    vt_off, vt_size = find_chunk(data, "MOVT", sub_start, sub_end)
    vi_off, vi_size = find_chunk(data, "MOVI", sub_start, sub_end)
    py_off, py_size = find_chunk(data, "MOPY", sub_start, sub_end)
    if vt_off is None or vi_off is None:
        return [], []

    nverts = vt_size // 12
    verts = [struct.unpack_from("<3f", data, vt_off + i * 12) for i in range(nverts)]
    ntris = vi_size // 6
    idx = struct.unpack_from("<%dH" % (ntris * 3), data, vi_off)

    flags = None
    if py_off is not None and py_size // 2 >= ntris:
        flags = [struct.unpack_from("<2B", data, py_off + i * 2) for i in range(ntris)]

    tris = []
    for t in range(ntris):
        a, b, c = idx[t * 3], idx[t * 3 + 1], idx[t * 3 + 2]
        if a >= nverts or b >= nverts or c >= nverts:
            continue
        collides = True
        if flags:
            f, mat = flags[t]
            # 0x04 = "detail"/no-collision render geometry; 0xFF material = collision only
            collides = not (f & 0x04) or mat == 0xFF
        tris.append((verts[a], verts[b], verts[c], collides))
    return tris, verts


def measure_floor(group_paths, wmo_world_z, bucket=0.25, min_flat=0.90, z_bounds=None):
    """Area-weighted histogram of near-horizontal surface height.

    z_bounds, when given, is the placed WMO's world-space height range taken from
    its ADT bounding box. Group files can contain geometry belonging to other
    parts of a shared model, so a candidate floor outside the box the ADT says
    this instance occupies is not this arena's floor and is discarded.
    """
    hist = defaultdict(float)
    total_area = 0.0
    for gp in group_paths:
        tris, _ = group_triangles(gp)
        for v0, v1, v2, collides in tris:
            if not collides:
                continue
            # edge vectors
            ax, ay, az = v1[0] - v0[0], v1[1] - v0[1], v1[2] - v0[2]
            bx, by, bz = v2[0] - v0[0], v2[1] - v0[1], v2[2] - v0[2]
            # cross product
            nx = ay * bz - az * by
            ny = az * bx - ax * bz
            nz = ax * by - ay * bx
            mag = (nx * nx + ny * ny + nz * nz) ** 0.5
            if mag <= 1e-6:
                continue
            area = 0.5 * mag
            # local up is Y; |ny|/mag near 1 means horizontal
            if abs(ny) / mag < min_flat:
                continue
            mean_y = (v0[1] + v1[1] + v2[1]) / 3.0
            hist[round(mean_y / bucket) * bucket] += area
            total_area += area

    if not hist:
        return None
    ranked = sorted(hist.items(), key=lambda kv: kv[1], reverse=True)

    # The single largest flat surface is not reliably the fighting floor -- an
    # arena built over a basement can have more area below it than on it, which
    # is exactly what Tiger's Peak does (its biggest slab is 16 yards under the
    # real floor). But a WMO's origin is authored at or below the structure's
    # base, never above it, so the fighting floor is the largest flat surface
    # that is not below the origin. That picks correctly for both arenas whose
    # floor height is independently known -- see --selftest.
    #
    # Heuristic, not a guarantee: top5 is always reported so the choice can be
    # eyeballed, and every coordinate still wants .gps confirmation in-game.
    at_or_above = [(y, a) for y, a in ranked if y >= -1.0]
    if z_bounds:
        lo, hi = z_bounds
        inside = [(y, a) for y, a in at_or_above if lo - 2.0 <= wmo_world_z + y <= hi + 2.0]
        at_or_above = inside or at_or_above
    best_y, best_area = (at_or_above or ranked)[0]

    return {
        "floor_local_y": best_y,
        "floor_world_z": round(wmo_world_z + best_y, 2),
        "floor_area": round(best_area, 1),
        "largest_area_at": round(wmo_world_z + ranked[0][0], 2),
        "flat_area_total": round(total_area, 1),
        "top5": [(round(y, 2), round(wmo_world_z + y, 2), round(a, 1))
                 for y, a in ranked[:5]],
    }


# -------------------------------------------------------------------- driver
def analyse(root_path_in_mpq, wmo_world_z, index, z_bounds=None):
    """index: {WMONAME: {archive: [files]}} from find_wmos."""
    key = os.path.basename(root_path_in_mpq).rsplit(".", 1)[0].upper()
    entry = index.get(key)
    if not entry:
        return None
    archive, files = next(iter(entry.items()))
    groups = [f for f in files if not f.upper().endswith(key + ".WMO")]
    paths = extract(archive, groups)
    return measure_floor(list(paths.values()), wmo_world_z, z_bounds=z_bounds)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--index", default=os.path.join(SCRATCH, "wmo_index.json"))
    ap.add_argument("--geometry", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "arena_geometry.json"))
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--out", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "arena_floors.json"))
    args = ap.parse_args()

    with open(args.index) as fh:
        index = {k.upper(): v for k, v in json.load(fh).items()}
    with open(args.geometry) as fh:
        geom = json.load(fh)

    if args.selftest:
        checks = [("tolvirarena", 24.41), ("ShadoPanArena", 380.71)]
        ok = True
        for mapdir, expected in checks:
            g = geom[mapdir]["arena_wmo"]
            res = analyse(g["name"], g["pos"][2], index,
                          (g["bbox_min"][2], g["bbox_max"][2]))
            if not res:
                print("%-16s FAIL (no mesh)" % mapdir)
                ok = False
                continue
            delta = abs(res["floor_world_z"] - expected)
            good = delta < 3.0
            ok &= good
            print("%-16s floor Z %8.2f   shipped %8.2f   delta %5.2f  %s" % (
                mapdir, res["floor_world_z"], expected, delta,
                "PASS" if good else "FAIL"))
            print("                 candidates %s" % (res["top5"],))
        sys.exit(0 if ok else 1)

    out = {}
    for mapdir, info in geom.items():
        g = info.get("arena_wmo")
        if not g:
            continue
        res = analyse(g["name"], g["pos"][2], index)
        out[mapdir] = {
            "map_id": info["map_id"],
            "wmo": g["name"].split("\\")[-1],
            "centre_x": g["pos"][0],
            "centre_y": g["pos"][1],
            "wmo_origin_z": g["pos"][2],
            "floor": res,
        }
        if res:
            print("%-22s map %-5d centre (%9.2f,%9.2f) floor Z %8.2f  (origin Z %8.2f)" % (
                mapdir, info["map_id"], g["pos"][0], g["pos"][1],
                res["floor_world_z"], g["pos"][2]))
        else:
            print("%-22s map %-5d centre (%9.2f,%9.2f) floor UNRESOLVED" % (
                mapdir, info["map_id"], g["pos"][0], g["pos"][1]))

    with open(args.out, "w") as fh:
        json.dump(out, fh, indent=1)
    print("\nwrote %s" % args.out)


if __name__ == "__main__":
    main()
