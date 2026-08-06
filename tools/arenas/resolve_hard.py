"""Resolve the arenas that name-matching alone could not place.

Four maps have no WMO whose name says "arena": Baradin Hold, the Obelisk of the
Stars, the Inventor's Library, and Maldraxxus (whose mesh would not parse). For
those, better evidence than the name is available:

  * PVP_COLLISIONPANE WMOs -- invisible walls placed only to fence a fighting
    area in. Where they exist they bound the arena exactly, and their own height
    is the floor plus a little.
  * arena-sized WMOs -- a fighting area is roughly 50-300 yards across. Terrain
    shells (ULDUAR_EXT03, whole Uldum temples) are far bigger, so a size band
    filters them out where a name match cannot.
  * gate doodads clustered near a candidate centre -- doors rest on the floor,
    so their height is direct evidence of it.
"""

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from adt_probe import probe_map  # noqa: E402

ROOT = r"C:\Ascension\ExtractedMaps\World\Maps"
HARD = ["BaradinHoldArena", "obeliskofthestarts", "ulduaroutarena", "MaldraxxusColiseum"]


def span(w, k):
    return w["bbox_max"][k] - w["bbox_min"][k]


def report(mapdir):
    info = probe_map(os.path.join(ROOT, mapdir))
    print("=" * 78)
    print("%s  -- %d WMOs, %d doodads" % (mapdir, info["wmo_count"], info["doodad_count"]))

    panes = [w for w in info["wmos"] if "COLLISIONPANE" in w["name"].upper()]
    if panes:
        xs = [p["pos"][0] for p in panes]
        ys = [p["pos"][1] for p in panes]
        zs = [p["pos"][2] for p in panes]
        cx, cy = (min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2
        print("  COLLISION PANES: %d" % len(panes))
        print("     box  X %9.2f .. %9.2f   (%.1f wide)" % (min(xs), max(xs), max(xs) - min(xs)))
        print("          Y %9.2f .. %9.2f   (%.1f wide)" % (min(ys), max(ys), max(ys) - min(ys)))
        print("          Z %9.2f .. %9.2f" % (min(zs), max(zs)))
        print("     centre (%9.2f, %9.2f)   pane base Z %9.2f" % (cx, cy, min(zs)))

    print("  arena-sized WMOs (largest horizontal span 40..320 yards):")
    sized = [w for w in info["wmos"]
             if 40 <= max(span(w, 0), span(w, 1)) <= 320]
    sized.sort(key=lambda w: -(span(w, 0) * span(w, 1)))
    for w in sized[:10]:
        print("     %-46s (%9.2f,%10.2f,%9.2f)  %5.1f x %5.1f x %5.1f" % (
            w["name"].split("\\")[-1][:46], w["pos"][0], w["pos"][1], w["pos"][2],
            span(w, 0), span(w, 1), span(w, 2)))
    if not sized:
        print("     (none)")

    # doodad clustering: where do the gates bunch up?
    gates = [d for d in info["doodads"]
             if any(k in d["name"].upper() for k in
                    ("DOOR", "GATE", "PORTCULLIS", "ARENA"))]
    if gates:
        print("  gate/arena doodads: %d" % len(gates))
        for d in gates[:10]:
            print("     %-44s (%9.2f,%10.2f,%9.2f)" % (
                d["name"].split("\\")[-1][:44], d["pos"][0], d["pos"][1], d["pos"][2]))


if __name__ == "__main__":
    for m in (sys.argv[1:] or HARD):
        report(m)
