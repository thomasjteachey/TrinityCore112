"""Server-collision height map (vmaps + terrain) as a colour relief with a labelled grid.

The top-most surface below z_cap at each pixel, coloured by height. Walls show as
sharp colour steps; the labels give exact heights every `step` yards.
"""
import math, sys
import numpy as np
from PIL import Image, ImageDraw
import render, srvcol
from gridmap import MapData


def jet(t):
    t = np.clip(t, 0, 1)
    r = np.clip(1.5 - np.abs(4 * t - 3), 0, 1)
    g = np.clip(1.5 - np.abs(4 * t - 2), 0, 1)
    b = np.clip(1.5 - np.abs(4 * t - 1), 0, 1)
    return np.stack([r, g, b], -1)


def run(mapid, cx, cy, R, zlo, zhi, zcap, out, step=20, marks=(), include_terrain=True, px=0.5):
    render.PX = px
    ras = render.Raster(cx, cy, R, zcap)
    P = srvcol.collect(mapid, cx, cy, R)
    if include_terrain:
        md = MapData(srvcol.SRV + r"\maps", mapid)
        n = ras.n
        # terrain sampled per pixel centre
        for v in range(n):
            wx = cx + R - (v + 0.5) * px
            for u in range(n):
                wy = cy + R - (u + 0.5) * px
                h = md.height(wx, wy)
                if h is not None and h < zcap and h > ras.z[v, u]:
                    ras.z[v, u] = h
                    ras.kind[v, u] = 1
    ras.tris(P, 2)
    z = ras.z
    empty = z < -1e8
    t = (z - zlo) / max(zhi - zlo, 1e-6)
    col = jet(t) * 255
    gy, gx = np.gradient(np.where(empty, zlo, z), px)
    shade = np.clip(0.8 + (-gx * 0.6 + gy * 0.6) * 0.15, 0.4, 1.15)
    col = col * shade[..., None]
    col[ras.kind == 1] = col[ras.kind == 1] * 0.7 + 40
    col[empty] = 0
    img = Image.fromarray(np.clip(col, 0, 255).astype(np.uint8))
    scale = 900 / img.size[0]
    img = img.resize((900, 900), Image.NEAREST)
    dr = ImageDraw.Draw(img)
    gxs = np.arange(math.ceil((cx - R) / step) * step, cx + R, step)
    gys = np.arange(math.ceil((cy - R) / step) * step, cy + R, step)
    for wx in gxs:
        for wy in gys:
            u, v = ras.uv(wx, wy)
            ui, vi = int(u), int(v)
            if 0 <= vi < ras.n and 0 <= ui < ras.n:
                zz = z[vi, ui]
                lab = "%.0f" % zz if zz > -1e8 else "-"
                dr.text((u * scale - 6, v * scale - 5), lab, fill=(255, 255, 255))
    for (wx, wy, lab, c) in marks:
        u, v = ras.uv(wx, wy)
        dr.ellipse((u * scale - 7, v * scale - 7, u * scale + 7, v * scale + 7), outline=c, width=3)
        dr.text((u * scale + 9, v * scale - 6), lab, fill=c)
    dr.text((5, 5), "map %d  heights %.0f(blue)..%.0f(red), cap %.0f, grid %d yd; N up, W left" % (mapid, zlo, zhi, zcap, step), fill=(255, 255, 255))
    img.save(out)
    return ras
