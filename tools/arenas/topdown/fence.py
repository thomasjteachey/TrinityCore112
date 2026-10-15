"""Offline replay of BattlegroundFence::BuildFence: will the bots-only fence exist for
these two team starts, and how much room is left before it refuses?

The fence refuses an arena when fewer than 60% of its 72 rays find a wall, or when a
start sits more than 1.4x the 75th-percentile wall distance from the midpoint of the
starts. Moving a start outward (Dalaran Sewers, 2026-10-15) can trip the second rule
without anything else changing, so check here before editing WorldSafeLocs.

    python fence.py 617 1353.0 817.25 15.4 1230.5 765.1 15.4

Uses the server collision in srv/ (see README.md), WMOs and M2s alike, as the vmap
tree does. Gameobjects are not in that tree, so gates do not count here either.
"""
import math, sys
import numpy as np
import srvcol

SECTORS = 72
HEIGHTS = (1.5, 6.0, 14.0)


class Rays:
    def __init__(self, mapid, cx, cy, radius):
        T = srvcol.collect(mapid, cx, cy, radius)
        self.A = T[:, 0]
        self.E1 = T[:, 1] - self.A
        self.E2 = T[:, 2] - self.A

    def first(self, o, d, maxd):
        o = np.asarray(o, float); d = np.asarray(d, float)
        p = np.cross(d, self.E2); det = (self.E1 * p).sum(1)
        ok = np.abs(det) > 1e-9
        inv = np.where(ok, 1.0 / np.where(ok, det, 1), 0)
        s = o - self.A
        u = (s * p).sum(1) * inv
        q = np.cross(s, self.E1)
        v = (q * d).sum(1) * inv
        t = (self.E2 * q).sum(1) * inv
        m = ok & (u >= 0) & (v >= 0) & (u + v <= 1) & (t > 0) & (t <= maxd)
        return t[m].min() if m.any() else None


def replay(mapid, alliance, horde):
    cx, cy = (alliance[0] + horde[0]) / 2, (alliance[1] + horde[1]) / 2
    span = math.dist(alliance, horde)
    probe = min(max(span * 1.5, 60.0), 400.0)
    rays = Rays(mapid, cx, cy, probe + 10)
    top = max(alliance[2], horde[2]) + 10
    t = rays.first((cx, cy, top), (0, 0, -1), 200)
    floor = top - t if t is not None else (alliance[2] + horde[2]) / 2
    hits = []
    for s in range(SECTORS):
        ang = s * 2 * math.pi / SECTORS
        best = 0.0
        for h in HEIGHTS:
            t = rays.first((cx, cy, floor + h), (math.cos(ang), math.sin(ang), 0), probe)
            if t is not None and t < probe * 0.95:
                best = max(best, t)
        if best > 0:
            hits.append(best)
    enclosed = len(hits) / SECTORS
    ref = sorted(hits)[min(len(hits) - 1, int(0.75 * len(hits)))] if hits else 0.0
    reach = max(math.dist((cx, cy), alliance[:2]), math.dist((cx, cy), horde[:2]))
    ok = enclosed >= 0.60 and ref >= 1.0 and reach <= ref * 1.4
    print("map %d centre (%.1f, %.1f) floor %.2f: %.0f%% of rays walled, reference %.1f yd, "
          "starts reach %.1f of a %.1f yd limit -> %s"
          % (mapid, cx, cy, floor, enclosed * 100, ref, reach, ref * 1.4, "fence" if ok else "NO FENCE"))
    return ok


if __name__ == "__main__":
    a = [float(v) for v in sys.argv[2:8]]
    replay(int(sys.argv[1]), tuple(a[0:3]), tuple(a[3:6]))
