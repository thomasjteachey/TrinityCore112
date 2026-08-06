"""Pick the arena structure out of each map's WMO list.

adt_probe's "biggest bounding box" heuristic finds terrain shells (ULDUAR_EXT03,
TB_TOWER, whole Uldum temples) rather than the fighting area. The arena itself is
named like one, so match on name and fall back to the largest WMO only when
nothing matches.

Also surfaces PVP_COLLISIONPANE* -- the invisible walls that fence an arena in.
Where present they bound the playable area exactly, which is better evidence than
the arena WMO's own bounding box.
"""

import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from adt_probe import probe_map  # noqa: E402

ROOT = r"C:\Ascension\ExtractedMaps\World\Maps"

# strongest signal first
PATTERNS = [
    re.compile(r"CUSTOM_.*ARENA", re.I),
    re.compile(r"[\\/][^\\/]*ARENA[^\\/]*\.WMO$", re.I),
    re.compile(r"PVPSTADIUM", re.I),
    re.compile(r"COLISEUM|COLOSSEUM", re.I),
]

MAPS = [
    ("coliseumarena", 982),
    ("nerubianarena", 983),
    ("MaldraxxusColiseum", 984),
    ("nagrandarena2", 985),
    ("bladesedgearena2b", 986),
    ("karazhanarena", 1007),
    ("ulduararena", 1008),
    ("BaradinHoldArena", 1401),
    ("obeliskofthestarts", 1402),
    ("thetwistingnether", 1403),
    ("blackrookholdarena", 1504),
    ("valsharaharena", 1552),
    ("ulduaroutarena", 1683),
    ("gundrakarena", 1684),
    ("tolvirarena", 980),      # reference: already shipped as BATTLEGROUND_TV
    ("ShadoPanArena", 1134),   # reference: already shipped as BATTLEGROUND_TTP
]


def classify(mapdir):
    info = probe_map(os.path.join(ROOT, mapdir))
    picks = []
    for pat in PATTERNS:
        for w in info["wmos"]:
            if pat.search(w["name"]) and w not in picks:
                picks.append(w)
        if picks:
            break

    panes = [w for w in info["wmos"] if "COLLISIONPANE" in w["name"].upper()]
    doors = [d for d in info["doodads"]
             if re.search(r"door|gate|portcullis", d["name"], re.I)]
    return info, picks, panes, doors


def main():
    out = {}
    for mapdir, mapid in MAPS:
        if not os.path.isdir(os.path.join(ROOT, mapdir)):
            print("!! missing %s" % mapdir)
            continue
        info, picks, panes, doors = classify(mapdir)
        print("=" * 78)
        print("%-22s map %-5d  %d WMOs / %d doodads" % (
            mapdir, mapid, info["wmo_count"], info["doodad_count"]))

        chosen = picks[0] if picks else info["primary_wmo"]
        for w in picks[:4]:
            span = [round(w["bbox_max"][k] - w["bbox_min"][k], 1) for k in range(3)]
            mark = "->" if w is chosen else "  "
            print("  %s %-46s (%9.2f,%10.2f,%9.2f) span %sx%sx%s" % (
                mark, w["name"].split("\\")[-1][:46],
                w["pos"][0], w["pos"][1], w["pos"][2], span[0], span[1], span[2]))
        if not picks:
            print("     (no name match; largest WMO = %s)" %
                  (chosen["name"].split("\\")[-1] if chosen else "none"))

        if panes:
            xs = [p["pos"][0] for p in panes]
            ys = [p["pos"][1] for p in panes]
            zs = [p["pos"][2] for p in panes]
            print("     %d collision panes  X %.1f..%.1f  Y %.1f..%.1f  Z %.1f..%.1f"
                  % (len(panes), min(xs), max(xs), min(ys), max(ys), min(zs), max(zs)))
        if doors:
            print("     %d door/gate doodads:" % len(doors))
            for d in doors[:6]:
                print("        %-40s (%9.2f,%10.2f,%9.2f) rotY=%7.2f" % (
                    d["name"].split("\\")[-1][:40],
                    d["pos"][0], d["pos"][1], d["pos"][2], d["rot_deg"][1]))

        out[mapdir] = {
            "map_id": mapid,
            "arena_wmo": chosen,
            "candidates": picks[:4],
            "collision_panes": panes,
            "doors": doors,
            "tile_bbox": info["tile_bbox"],
        }

    dest = os.path.join(os.path.dirname(os.path.abspath(__file__)), "arena_geometry.json")
    with open(dest, "w", encoding="utf-8") as fh:
        json.dump(out, fh, indent=1)
    print("\nwrote %s" % dest)


if __name__ == "__main__":
    main()
