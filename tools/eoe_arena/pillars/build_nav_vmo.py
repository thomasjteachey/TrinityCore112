"""Navmesh-only collision for a hollow M2 obstacle: its own triangles plus horizontal plugs.

Recast rasterises surfaces, not volumes. A pillar's walls cut the floor around it, but the floor
INSIDE keeps its clearance up to the first overhead face (the capital, metres up) and survives
as a walkable island enclosed by the walls - unreachable, yet a nearest-poly lookup can land in
it. Plugs (the convex hull of the mesh's cross-section) every STEP model units up the column
leave no floor or plug inside with the 1.6 yd agent height, so nothing inside is walkable.

usage: python build_nav_vmo.py <model.m2> <out.vmo> <scale>
"""
import math, os, struct, sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tanaris"))
import build_go_vmo as B  # noqa: E402

m2, out, scale = sys.argv[1], sys.argv[2], float(sys.argv[3])
verts, tris = B.read_collision(m2)
zmin = min(v[2] for v in verts); zmax = max(v[2] for v in verts)
STEP = 1.2 / scale            # 1.2 yd in the world, under the 1.6 yd agent height


def hull(points):
    pts = sorted(set((round(p[0], 5), round(p[1], 5)) for p in points))
    if len(pts) < 3:
        return pts
    def half(seq):
        h = []
        for p in seq:
            while len(h) >= 2 and (h[-1][0] - h[-2][0]) * (p[1] - h[-2][1]) - (h[-1][1] - h[-2][1]) * (p[0] - h[-2][0]) <= 0:
                h.pop()
            h.append(p)
        return h
    lo, up = half(pts), half(reversed(pts))
    return lo[:-1] + up[:-1]


verts = [tuple(v) for v in verts]
tris = list(tris)
levels = []
h = zmin + 0.6 / scale        # first plug 0.6 yd above the base
while h < zmax - 0.2 / scale:
    cut = []
    for a, b, c in tris:
        for p, q in ((verts[a], verts[b]), (verts[b], verts[c]), (verts[c], verts[a])):
            if (p[2] - h) * (q[2] - h) < 0:
                t = (h - p[2]) / (q[2] - p[2])
                cut.append((p[0] + t * (q[0] - p[0]), p[1] + t * (q[1] - p[1])))
    poly = hull(cut)
    if len(poly) >= 3:
        base = len(verts)
        cx = sum(p[0] for p in poly) / len(poly); cy = sum(p[1] for p in poly) / len(poly)
        verts.append((cx, cy, h))
        for x, y in poly:
            verts.append((x, y, h))
        n = len(poly)
        for i in range(n):
            tris.append((base, base + 1 + i, base + 1 + (i + 1) % n))
        levels.append((round(h * scale, 2), n))
    h += STEP

vmo, lo, hi = B.build_vmo(verts, tris)
open(out, "wb").write(vmo)
print("%s: %d verts, %d tris, %d plugs at world heights %s" % (out, len(verts), len(tris), len(levels),
      [l[0] for l in levels]))
