"""Horizontal cross-sections of the server collision at player height, over the height map.

Every collision triangle that crosses the plane z = h contributes the segment where it
cuts it - which is exactly what blocks a walking player at that height. Drawn in white
over the colour relief from heightmap.py; navmesh reachable from the starts in purple.
"""
import sys
import numpy as np
from PIL import Image, ImageDraw
import heightmap, srvcol, render
from navmesh import NavMesh


def cut_segments(P, h):
    segs = []
    z = P[:, :, 2]
    above = z > h
    n_above = above.sum(1)
    sel = (n_above == 1) | (n_above == 2)
    for t in P[sel]:
        pts = []
        for i in range(3):
            a, b = t[i], t[(i + 1) % 3]
            if (a[2] > h) != (b[2] > h):
                u = (h - a[2]) / (b[2] - a[2])
                pts.append(a[:2] + u * (b[:2] - a[:2]))
        if len(pts) == 2:
            segs.append((pts[0], pts[1]))
    return segs


def run(mapid, cx, cy, R, zlo, zhi, zcap, slices, out, marks=(), px=0.4, nav=None, step=10):
    ras = heightmap.run(mapid, cx, cy, R, zlo, zhi, zcap, out, step=step, marks=marks, px=px)
    img = Image.open(out).convert("RGBA")
    scale = img.size[0] / ras.n
    ov = Image.new("RGBA", img.size, (0, 0, 0, 0))
    dr = ImageDraw.Draw(ov)
    if nav is not None:
        nm, reach = nav
        for g, v, area, fl in nm.iter_polys():
            if g not in reach:
                continue
            pts = [ras.uv(x, y) for x, y in zip(v[:, 2], v[:, 0])]
            pts = [(u * scale, w * scale) for u, w in pts]
            dr.polygon(pts, fill=(200, 0, 255, 60))
    P = srvcol.collect(mapid, cx, cy, R)
    cols = [(255, 255, 255, 255), (255, 255, 0, 255), (0, 255, 255, 255)]
    for k, h in enumerate(slices):
        for a, b in cut_segments(P, h):
            ua, va = ras.uv(a[0], a[1])
            ub, vb = ras.uv(b[0], b[1])
            dr.line((ua * scale, va * scale, ub * scale, vb * scale), fill=cols[k % 3], width=2)
    img = Image.alpha_composite(img, ov).convert("RGB")
    d2 = ImageDraw.Draw(img)
    d2.text((5, 18), "slices at z=%s (white/yellow/cyan); purple = navmesh reachable from the starts" % (slices,), fill=(255, 255, 255))
    img.save(out)
