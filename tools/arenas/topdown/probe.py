"""Surface heights (server collision: vmaps + terrain) at given points."""
import sys
import numpy as np
import srvcol
from gridmap import MapData


def surfaces(P, x, y):
    out = []
    xs, ys, zs = P[:, :, 0], P[:, :, 1], P[:, :, 2]
    d = (ys[:, 1] - ys[:, 2]) * (xs[:, 0] - xs[:, 2]) + (xs[:, 2] - xs[:, 1]) * (ys[:, 0] - ys[:, 2])
    ok = np.abs(d) > 1e-9
    d = np.where(ok, d, 1)
    a = ((ys[:, 1] - ys[:, 2]) * (x - xs[:, 2]) + (xs[:, 2] - xs[:, 1]) * (y - ys[:, 2])) / d
    b = ((ys[:, 2] - ys[:, 0]) * (x - xs[:, 2]) + (xs[:, 0] - xs[:, 2]) * (y - ys[:, 2])) / d
    c = 1 - a - b
    m = ok & (a >= -1e-6) & (b >= -1e-6) & (c >= -1e-6)
    z = a * zs[:, 0] + b * zs[:, 1] + c * zs[:, 2]
    return sorted(round(float(v), 2) for v in z[m])


class Probe:
    def __init__(self, mapid, cx, cy, R):
        self.P = srvcol.collect(mapid, cx, cy, R)
        self.md = MapData(srvcol.SRV + r"\maps", mapid)

    def at(self, x, y):
        return self.md.height(x, y), surfaces(self.P, x, y)


if __name__ == "__main__":
    m = int(sys.argv[1])
    cx, cy, R = float(sys.argv[2]), float(sys.argv[3]), float(sys.argv[4])
    p = Probe(m, cx, cy, R)
    pts = sys.argv[5:]
    for i in range(0, len(pts), 2):
        x, y = float(pts[i]), float(pts[i + 1])
        t, s = p.at(x, y)
        print("(%.1f, %.1f) terrain %s  vmap surfaces %s" % (x, y, None if t is None else round(t, 2), s))
