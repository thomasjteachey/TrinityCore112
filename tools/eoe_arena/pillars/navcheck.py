"""Is a world (x, y) on a walkable navmesh polygon of a .mmtile (at about floor height z)?

Detour verts are (game y, game z, game x). Compares tiles side by side over a point set.
"""
import math, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dtile import sections  # noqa: E402

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import layout as L  # noqa: E402


def load(path):
    d = open(path, "rb").read()
    m, secs, _ = sections(d)
    s = dict((n, (a, b)) for n, a, b in secs)
    verts = [struct.unpack_from("<3f", d, s["verts"][0] + 12 * i) for i in range(m["vertCount"])]
    polys = []
    for i in range(m["polyCount"]):
        p = s["polys"][0] + 32 * i
        vs = struct.unpack_from("<6H", d, p + 4)
        nv = d[p + 30]
        polys.append([(verts[v][2], verts[v][0], verts[v][1]) for v in vs[:nv]])  # -> (game x, y, z)
    return polys


def inside(poly, x, y):
    c = False
    n = len(poly)
    for i in range(n):
        x1, y1, _ = poly[i]; x2, y2, _ = poly[(i + 1) % n]
        if (y1 > y) != (y2 > y) and x < (x2 - x1) * (y - y1) / (y2 - y1) + x1:
            c = not c
    return c


def on_mesh(polys, x, y, z, dz=2.5):
    return any(inside(p, x, y) and min(v[2] for v in p) - dz <= z <= max(v[2] for v in p) + dz for p in polys)


before, after = load(sys.argv[1]), load(sys.argv[2])
cx, cy = L.CENTRE_X, L.CENTRE_Y
pts = []
centres = [(cx, cy, "centre pillar")]
for b in (135.0, 315.0):
    a = math.radians(b)
    centres.append((cx + 20 * math.cos(a), cy + 20 * math.sin(a), "flank pillar %d" % b))
for x, y, n in centres:
    pts.append((x, y, L.FLOOR_CENTRE_Z, n + " - middle", False))
    for r in (2.0, 3.0):
        for k in range(8):
            a = k * math.pi / 4
            pts.append((x + r * math.cos(a), y + r * math.sin(a), L.FLOOR_CENTRE_Z, "%s - %g yd in, dir %d" % (n, r, k * 45), False))
    for k in range(8):
        a = k * math.pi / 4
        pts.append((x + 7.5 * math.cos(a), y + 7.5 * math.sin(a), L.FLOOR_CENTRE_Z, "%s - 7.5 yd out, dir %d" % (n, k * 45), True))
for lab, p in (("alliance landing", L.ALLIANCE_LANDING), ("horde landing", L.HORDE_LANDING)):
    pts.append((p[0], p[1], p[2], lab, True))
for b in L.BUFFS:
    pts.append((b[1], b[2], b[3], "shadow sight %d" % b[0], True))
for k in range(12):
    a = k * math.pi / 6
    pts.append((cx + 12 * math.cos(a), cy + 12 * math.sin(a), L.FLOOR_CENTRE_Z, "ring r12 dir %d" % (k * 30), None))

bad = 0
for x, y, z, n, want in pts:
    b, a = on_mesh(before, x, y, z), on_mesh(after, x, y, z)
    ok = want is None or a == want
    bad += not ok
    if not ok or "middle" in n or "landing" in n or "shadow" in n or "r12" in n:
        print("%-40s before %-5s after %-5s %s" % (n, b, a, "" if ok else "<-- UNEXPECTED"))
print("polys before %d after %d; unexpected %d of %d points" % (len(before), len(after), bad, len(pts)))
