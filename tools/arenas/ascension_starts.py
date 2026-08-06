"""Read the real arena start positions out of Ascension's own WorldSafeLocs.dbc.

Far better evidence than anything derived from geometry: these are the exact
teleport targets the source server used, so they are on the floor, inside the
arena, and facing the right way by construction -- the same way Tol'viron's
4136/4137 and Tiger's Peak's 4534/4535 already work here.

Usage: python ascension_starts.py [--all]
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from dbc import DBC          # noqa: E402
from gen_arena_sql import ARENAS  # noqa: E402

DBC_DIR = r"C:\Ascension\ExtractedMaps\DBFilesClient"

MAP_IDS = {a[1]: (a[0], a[2], a[3]) for a in ARENAS}
MAP_IDS[980] = (870, "tolvirarena", "Tol'Viron Arena")       # controls: already live here
MAP_IDS[1134] = (871, "ShadoPanArena", "The Tiger's Peak")


def main():
    path = os.path.join(DBC_DIR, "WorldSafeLocs.dbc")
    if not os.path.exists(path):
        raise SystemExit("not found: %s" % path)
    with open(path, "rb") as fh:
        d = DBC(fh.read())
    print("Ascension WorldSafeLocs.dbc: %d records, %d fields\n" % (d.count, d.fields))

    found = {}
    for i in range(d.count):
        mid = d.uint(i, 1)
        if mid in MAP_IDS:
            found.setdefault(mid, []).append({
                "id": d.uint(i, 0),
                "x": d.flt(i, 2), "y": d.flt(i, 3), "z": d.flt(i, 4),
                "name": d.str(i, 5),
            })

    print("%-6s %-5s %-22s %-9s %11s %11s %10s  %s"
          % ("MAP", "BG", "DIRECTORY", "WSL id", "X", "Y", "Z", "NAME"))
    print("-" * 110)
    for mid in sorted(MAP_IDS):
        bg, directory, name = MAP_IDS[mid]
        rows = found.get(mid)
        if not rows:
            print("%-6d %-5d %-22s  *** no rows in Ascension WorldSafeLocs ***" % (mid, bg, directory))
            continue
        for r in sorted(rows, key=lambda r: r["id"]):
            print("%-6d %-5d %-22s %-9d %11.3f %11.3f %10.3f  %s"
                  % (mid, bg, directory, r["id"], r["x"], r["y"], r["z"], r["name"]))

    emit(found)


def pick_teams(rows):
    """The two team start rows, out of a map's assorted WorldSafeLocs entries.

    The naming is not consistent across arenas -- some say "Team 1 Start", some
    just "Team 1" -- and every map also carries a graveyard, sometimes a
    "Middle", and occasionally BOTH an old "Team 1" and a newer
    "Start Position - Team 1". Prefer the explicit start rows; fall back to the
    plain team rows; never pick a graveyard or a middle.
    """
    def is_team(r, n):
        s = r["name"].lower()
        if "graveyard" in s or s.endswith(" - gy") or "middle" in s:
            return False
        return ("team %d" % n) in s

    out = []
    for n in (1, 2):
        cands = [r for r in rows if is_team(r, n)]
        if not cands:
            return None
        starts = [r for r in cands if "start" in r["name"].lower()]
        out.append(sorted(starts or cands, key=lambda r: -r["id"])[0])
    return out


def emit(found):
    import math
    print("\n\n=== paste-ready MEASURED block ===")
    print("# Derived from Ascension's own WorldSafeLocs.dbc: these are the exact")
    print("# teleport targets the source server used, so they are on the floor and")
    print("# inside the arena by construction. Orientations face the two points at")
    print("# each other, which is what every arena here does.")
    for mid in sorted(MAP_IDS):
        bg, directory, name = MAP_IDS[mid]
        rows = found.get(mid, [])
        pair = pick_teams(rows)
        if not pair:
            print("    # %-5d %-22s no usable team rows" % (bg, directory))
            continue
        a, h = pair
        ao = math.atan2(h["y"] - a["y"], h["x"] - a["x"]) % (2 * math.pi)
        ho = (ao + math.pi) % (2 * math.pi)
        gy = [r for r in rows if "graveyard" in r["name"].lower() or r["name"].lower().endswith(" - gy")]
        print("    %d: {  # %s, map %d  (WSL %d/%d)" % (bg, name, mid, a["id"], h["id"]))
        print('        "alliance": (%.6f, %.6f, %.6f, %.6f),' % (a["x"], a["y"], a["z"], ao))
        print('        "horde":    (%.6f, %.6f, %.6f, %.6f),' % (h["x"], h["y"], h["z"], ho))
        if gy:
            g = gy[0]
            print('        # graveyard %d: (%.3f, %.3f, %.3f)' % (g["id"], g["x"], g["y"], g["z"]))
        print("    },")


if __name__ == "__main__":
    main()
