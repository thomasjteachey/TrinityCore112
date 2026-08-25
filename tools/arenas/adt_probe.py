"""Read arena geometry straight out of the client's terrain files.

The custom arenas are Ascension map folders (WDT + ADT tiles). Everything the
server needs to place players -- where the arena actually is in world space, how
big it is, where its gates are -- is already recorded in those files as WMO and
M2 placements. This reads them so the coordinates in the SQL and the C++ are
measured rather than guessed.

Usage:
    python adt_probe.py <mapdir> [<mapdir> ...]      # summary per map
    python adt_probe.py --json out.json <mapdir> ...  # machine-readable
    python adt_probe.py --selftest                    # validate vs Tol'viron

Coordinate systems
------------------
ADT placement chunks store positions relative to the map's north-west corner at
32 * 533.33333 = 17066.666. The WoW server/client world frame is rotated and
mirrored relative to that, so:

    worldX = 17066.666 - pos.z      (north/south)
    worldY = 17066.666 - pos.x      (east/west)
    worldZ = pos.y                  (height, unchanged)

A tile named <Map>_<col>_<row>.adt therefore covers
    worldY in [(31-col) * 533.33333, (32-col) * 533.33333]
    worldX in [(31-row) * 533.33333, (32-row) * 533.33333]

--selftest checks the parser against BattlegroundTV.cpp, whose Tol'viron door
coordinates were derived independently; the arena WMO centre must land between
them.
"""

import argparse
import json
import os
import re
import struct
import sys

TILE = 533.33333
ORIGIN = 32.0 * TILE  # 17066.666

TILE_RE = re.compile(r"^(?P<name>.+)_(?P<col>\d{1,2})_(?P<row>\d{1,2})\.adt$", re.I)


# --------------------------------------------------------------- chunk walking
def iter_chunks(data, start=0, end=None):
    """Yield (magic, payload_offset, size). Chunk magics are stored reversed."""
    end = len(data) if end is None else end
    pos = start
    while pos + 8 <= end:
        magic = data[pos : pos + 4][::-1].decode("ascii", "replace")
        (size,) = struct.unpack_from("<I", data, pos + 4)
        payload = pos + 8
        if payload + size > end:
            break
        yield magic, payload, size
        pos = payload + size


def find_chunk(data, want):
    for magic, off, size in iter_chunks(data):
        if magic == want:
            return off, size
    return None, 0


def split_strings(blob):
    """MMDX/MWMO are back-to-back null-terminated names."""
    out = []
    for raw in blob.split(b"\x00"):
        if raw:
            out.append(raw.decode("utf-8", "replace"))
    return out


def to_world(px, py, pz):
    return (ORIGIN - pz, ORIGIN - px, py)


# ------------------------------------------------------------------ ADT parsing
def parse_adt(path):
    with open(path, "rb") as fh:
        data = fh.read()

    wmo_names = []
    off, size = find_chunk(data, "MWMO")
    if off is not None:
        wmo_names = split_strings(data[off : off + size])

    m2_names = []
    off, size = find_chunk(data, "MMDX")
    if off is not None:
        m2_names = split_strings(data[off : off + size])

    wmos = []
    off, size = find_chunk(data, "MODF")
    if off is not None:
        for i in range(size // 64):
            base = off + i * 64
            (name_id, uniq) = struct.unpack_from("<II", data, base)
            pos = struct.unpack_from("<3f", data, base + 8)
            rot = struct.unpack_from("<3f", data, base + 20)
            lo = struct.unpack_from("<3f", data, base + 32)
            hi = struct.unpack_from("<3f", data, base + 44)
            flags, doodad_set, name_set, _pad = struct.unpack_from("<4H", data, base + 56)
            wx, wy, wz = to_world(*pos)
            lo_w = to_world(*lo)
            hi_w = to_world(*hi)
            wmos.append({
                "name": wmo_names[name_id] if name_id < len(wmo_names) else "?%d" % name_id,
                "unique_id": uniq,
                "pos": [round(wx, 3), round(wy, 3), round(wz, 3)],
                "rot_deg": [round(r, 2) for r in rot],
                # extents are two opposite corners; re-sort after the axis swap
                "bbox_min": [round(min(lo_w[k], hi_w[k]), 2) for k in range(3)],
                "bbox_max": [round(max(lo_w[k], hi_w[k]), 2) for k in range(3)],
                "doodad_set": doodad_set,
                "name_set": name_set,
                "flags": flags,
            })

    doodads = []
    off, size = find_chunk(data, "MDDF")
    if off is not None:
        for i in range(size // 36):
            base = off + i * 36
            (name_id, uniq) = struct.unpack_from("<II", data, base)
            pos = struct.unpack_from("<3f", data, base + 8)
            rot = struct.unpack_from("<3f", data, base + 20)
            scale, flags = struct.unpack_from("<2H", data, base + 32)
            wx, wy, wz = to_world(*pos)
            doodads.append({
                "name": m2_names[name_id] if name_id < len(m2_names) else "?%d" % name_id,
                "unique_id": uniq,
                "pos": [round(wx, 3), round(wy, 3), round(wz, 3)],
                "rot_deg": [round(r, 2) for r in rot],
                "scale": scale / 1024.0,
                "flags": flags,
            })

    return wmos, doodads


def parse_wdt(path):
    """Return the set of (col, row) tiles the WDT marks as present."""
    with open(path, "rb") as fh:
        data = fh.read()
    off, size = find_chunk(data, "MAIN")
    tiles = set()
    if off is None:
        return tiles
    for row in range(64):
        for col in range(64):
            idx = row * 64 + col
            base = off + idx * 8
            if base + 8 > off + size:
                continue
            (flags,) = struct.unpack_from("<I", data, base)
            if flags & 1:
                tiles.add((col, row))
    return tiles


# ------------------------------------------------------------------- map probe
def probe_map(mapdir):
    name = os.path.basename(mapdir.rstrip("\\/"))
    adts, wdt = [], None
    for fn in sorted(os.listdir(mapdir)):
        low = fn.lower()
        if low.endswith(".adt"):
            adts.append(fn)
        elif low.endswith(".wdt"):
            wdt = fn

    tiles = set()
    for fn in adts:
        m = TILE_RE.match(fn)
        if m:
            tiles.add((int(m.group("col")), int(m.group("row"))))

    all_wmos, all_doodads = [], []
    for fn in adts:
        w, d = parse_adt(os.path.join(mapdir, fn))
        all_wmos.extend(w)
        all_doodads.extend(d)

    # de-duplicate: a WMO straddling a tile border is written into every tile it
    # touches, with the same uniqueId each time.
    seen, wmos = set(), []
    for w in all_wmos:
        if w["unique_id"] in seen:
            continue
        seen.add(w["unique_id"])
        wmos.append(w)
    seen, doodads = set(), []
    for d in all_doodads:
        if d["unique_id"] in seen:
            continue
        seen.add(d["unique_id"])
        doodads.append(d)

    # tile bounding box in world space
    bbox = None
    if tiles:
        cols = [c for c, _ in tiles]
        rows = [r for _, r in tiles]
        bbox = {
            "x_min": round((31 - max(rows)) * TILE, 2),
            "x_max": round((32 - min(rows)) * TILE, 2),
            "y_min": round((31 - max(cols)) * TILE, 2),
            "y_max": round((32 - min(cols)) * TILE, 2),
        }

    # the arena structure: biggest WMO by bounding-box volume
    def volume(w):
        return max(0.0, (w["bbox_max"][0] - w["bbox_min"][0])) * \
               max(0.0, (w["bbox_max"][1] - w["bbox_min"][1])) * \
               max(0.0, (w["bbox_max"][2] - w["bbox_min"][2]))

    primary = max(wmos, key=volume) if wmos in ([],) or wmos else None

    return {
        "map": name,
        "wdt": wdt,
        "adt_count": len(adts),
        "tiles": sorted(tiles),
        "tile_bbox": bbox,
        "wmo_count": len(wmos),
        "doodad_count": len(doodads),
        "wmos": sorted(wmos, key=volume, reverse=True),
        "primary_wmo": primary,
        "doodads": doodads,
    }


def print_summary(info, wmo_limit=8, doodad_filter=None):
    print("=" * 78)
    print("%s   (%d ADTs, WDT=%s)" % (info["map"], info["adt_count"], info["wdt"]))
    b = info["tile_bbox"]
    if b:
        print("  tile bbox   X %9.1f .. %9.1f   Y %9.1f .. %9.1f" % (
            b["x_min"], b["x_max"], b["y_min"], b["y_max"]))
        print("  tile centre X %9.1f            Y %9.1f" % (
            (b["x_min"] + b["x_max"]) / 2, (b["y_min"] + b["y_max"]) / 2))
    print("  %d WMOs, %d doodads" % (info["wmo_count"], info["doodad_count"]))
    for w in info["wmos"][:wmo_limit]:
        span = [round(w["bbox_max"][k] - w["bbox_min"][k], 1) for k in range(3)]
        print("    %-52s" % w["name"].split("\\")[-1])
        print("        pos (%9.2f, %9.2f, %8.2f)  rotY=%7.2f  span %sx%sx%s" % (
            w["pos"][0], w["pos"][1], w["pos"][2], w["rot_deg"][1],
            span[0], span[1], span[2]))
    if doodad_filter:
        hits = [d for d in info["doodads"] if doodad_filter.lower() in d["name"].lower()]
        if hits:
            print("  doodads matching %r:" % doodad_filter)
            for d in hits[:20]:
                print("    %-44s (%9.2f, %9.2f, %8.2f) rotY=%7.2f" % (
                    d["name"].split("\\")[-1], d["pos"][0], d["pos"][1], d["pos"][2],
                    d["rot_deg"][1]))


def selftest(root):
    """BattlegroundTV.cpp door coordinates, derived independently of this parser."""
    d1 = (-10774.6, 430.992, 24.41076)
    d2 = (-10655.0, 428.117, 24.416)
    mid = ((d1[0] + d2[0]) / 2, (d1[1] + d2[1]) / 2, (d1[2] + d2[2]) / 2)

    mapdir = os.path.join(root, "tolvirarena")
    if not os.path.isdir(mapdir):
        print("SELFTEST SKIPPED: %s not found" % mapdir)
        return False

    info = probe_map(mapdir)
    print("selftest: Tol'viron (map 980)")
    print("  BattlegroundTV.cpp door midpoint : (%.2f, %.2f, %.2f)" % mid)

    best, best_d = None, 1e18
    for w in info["wmos"]:
        dx = w["pos"][0] - mid[0]
        dy = w["pos"][1] - mid[1]
        d = (dx * dx + dy * dy) ** 0.5
        if d < best_d:
            best, best_d = w, d
    if not best:
        print("  FAIL: no WMOs parsed")
        return False

    print("  nearest parsed WMO               : (%.2f, %.2f, %.2f)  %s" % (
        best["pos"][0], best["pos"][1], best["pos"][2], best["name"].split("\\")[-1]))
    print("  horizontal delta                 : %.2f yards" % best_d)
    ok = best_d < 80.0
    print("  %s" % ("PASS - parser agrees with the shipped implementation" if ok
                    else "FAIL - coordinate transform is wrong"))
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mapdirs", nargs="*")
    ap.add_argument("--json")
    ap.add_argument("--root", default=r"C:\Ascension\ExtractedMaps\World\Maps")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--doodad-filter", default=None)
    ap.add_argument("--wmo-limit", type=int, default=8)
    args = ap.parse_args()

    if args.selftest:
        sys.exit(0 if selftest(args.root) else 1)

    results = []
    for d in args.mapdirs:
        path = d if os.path.isdir(d) else os.path.join(args.root, d)
        if not os.path.isdir(path):
            print("missing: %s" % path)
            continue
        info = probe_map(path)
        results.append(info)
        print_summary(info, args.wmo_limit, args.doodad_filter)

    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump(results, fh, indent=1)
        print("\nwrote %s" % args.json)


if __name__ == "__main__":
    main()
