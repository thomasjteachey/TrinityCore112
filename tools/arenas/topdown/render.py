"""Top-down render of an arena's real geometry (terrain + WMOs + M2 collision) beside its minimap.

Output: mm/geo_<bg>.png  (left: minimap composite, right: geometry render, same window).
"""
import io, math, struct, sys
import numpy as np
from PIL import Image, ImageDraw
import cl, geo
from mmcomp import ARENAS, load_common

BS = "\\"
PX = 0.5   # yards per pixel


class Raster:
    def __init__(self, cx, cy, R, zclip):
        self.cx, self.cy, self.R = cx, cy, R
        self.n = int(2 * R / PX)
        self.z = np.full((self.n, self.n), -1e9)
        self.kind = np.zeros((self.n, self.n), dtype=np.uint8)   # 1 terrain, 2 wmo, 3 m2
        self.zclip = zclip

    def uv(self, wx, wy):
        # image right = east = -Y ; image down = south = -X
        u = (self.cy + self.R - wy) / PX
        v = (self.cx + self.R - wx) / PX
        return u, v

    def tris(self, P, kind):
        """P: (M,3,3) world triangles."""
        if len(P) == 0:
            return
        u = (self.cy + self.R - P[:, :, 1]) / PX
        v = (self.cx + self.R - P[:, :, 0]) / PX
        z = P[:, :, 2]
        keep = (u.max(1) >= 0) & (u.min(1) < self.n) & (v.max(1) >= 0) & (v.min(1) < self.n) & (z.min(1) < self.zclip)
        u, v, z = u[keep], v[keep], z[keep]
        for k in range(len(u)):
            self._tri(u[k], v[k], z[k], kind)

    def _tri(self, u, v, z, kind):
        u0, u1 = max(int(math.floor(u.min())), 0), min(int(math.ceil(u.max())), self.n - 1)
        v0, v1 = max(int(math.floor(v.min())), 0), min(int(math.ceil(v.max())), self.n - 1)
        if u1 < u0 or v1 < v0:
            return
        uu, vv = np.meshgrid(np.arange(u0, u1 + 1) + 0.5, np.arange(v0, v1 + 1) + 0.5)
        d = (v[1] - v[2]) * (u[0] - u[2]) + (u[2] - u[1]) * (v[0] - v[2])
        if abs(d) < 1e-9:
            return
        a = ((v[1] - v[2]) * (uu - u[2]) + (u[2] - u[1]) * (vv - v[2])) / d
        b = ((v[2] - v[0]) * (uu - u[2]) + (u[0] - u[2]) * (vv - v[2])) / d
        c = 1 - a - b
        e = -0.02
        m = (a >= e) & (b >= e) & (c >= e)
        if not m.any():
            return
        zz = a * z[0] + b * z[1] + c * z[2]
        m &= zz < self.zclip
        sub = self.z[v0:v1 + 1, u0:u1 + 1]
        ks = self.kind[v0:v1 + 1, u0:u1 + 1]
        upd = m & (zz > sub)
        sub[upd] = zz[upd]
        ks[upd] = kind

    def image(self, zlo, zhi):
        z = self.z.copy()
        empty = z < -1e8
        zn = np.clip((z - zlo) / max(zhi - zlo, 1), 0, 1)
        # hillshade
        gy, gx = np.gradient(np.where(empty, zlo, z), PX)
        shade = np.clip(0.75 + (gx * -0.7 + gy * 0.7) * 0.25, 0.35, 1.2)
        steep = np.hypot(gx, gy) > 2.0
        base = np.stack([60 + 150 * zn, 60 + 150 * zn, 70 + 160 * zn], -1)
        tint = {1: (0.85, 1.0, 0.75), 2: (1.0, 0.95, 0.85), 3: (0.8, 0.9, 1.1), 4: (1.6, 0.3, 0.3)}
        img = np.zeros(z.shape + (3,))
        for k, t in tint.items():
            sel = self.kind == k
            img[sel] = base[sel] * np.array(t)
        img *= shade[..., None]
        img[steep] = img[steep] * 0.45
        img[empty] = (0, 0, 0)
        return Image.fromarray(np.clip(img, 0, 255).astype(np.uint8))


def terrain_tris(adt):
    out = []
    U = T / 128.0
    for c in adt["mcnk"]:
        px, py, pz = c["pos"]
        h = c["h"]
        holes = c["holes"]
        # outer 9x9 at rows 0,2,..16 ; inner 8x8 at odd rows
        def outer(r, k):
            return (px - r * 2 * U, py - k * 2 * U, pz + h[r * 17 + k])

        def inner(r, k):
            return (px - (r * 2 + 1) * U, py - (k * 2 + 1) * U, pz + h[r * 17 + 9 + k])
        for r in range(8):
            for k in range(8):
                if holes and (holes >> ((r // 2) * 4 + (k // 2))) & 1:
                    continue
                a, b, cc, d, e = outer(r, k), outer(r, k + 1), outer(r + 1, k + 1), outer(r + 1, k), inner(r, k)
                out += [(a, b, e), (b, cc, e), (cc, d, e), (d, a, e)]
    return out


T = geo.T
_m2cache = {}


def m2_collision(name):
    key = name.upper()
    if key in _m2cache:
        return _m2cache[key]
    n = name
    if n.lower().endswith(".mdx") or n.lower().endswith(".mdl"):
        n = n[:-4] + ".m2"
    b = cl.read(n)
    res = None
    if b is not None and b[:4] == b"MD20":
        nt, ot = struct.unpack_from("<II", b, 0xD8)
        nv, ov = struct.unpack_from("<II", b, 0xE0)
        if nt and nv:
            idx = np.frombuffer(b, dtype="<u2", count=nt, offset=ot).reshape(-1, 3).astype(np.int64)
            vs = np.frombuffer(b, dtype="<f4", count=nv * 3, offset=ov).reshape(-1, 3).astype(np.float64)
            res = (vs, idx)
    _m2cache[key] = res
    return res


def collect(d, tiles, cx, cy, R, zclip, want_m2=True, want_doodads=True, mode='collide'):
    ras_tris = {1: [], 2: [], 3: []}
    c0, r0 = geo.adt_index(cx + R, cy + R)
    c1, r1 = geo.adt_index(cx - R, cy - R)
    wmos, m2s = {}, {}
    for c in range(c0 - 1, c1 + 2):
        for r in range(r0 - 1, r1 + 2):
            if (c, r) not in tiles:
                continue
            adt = geo.read_adt(d, c, r)
            if not adt:
                continue
            if c0 <= c <= c1 and r0 <= r <= r1:
                ras_tris[1] += terrain_tris(adt)
            for w in adt["wmo"]:
                wmos[w["uid"]] = w
            for m in adt["m2"]:
                m2s[m["uid"]] = m
    for w in wmos.values():
        res = geo.wmo_groups(w["name"])
        if not res:
            continue
        f = geo.placement_to_world(w["pos"], w["rot"])
        pane = 'COLLISIONPANE' in w["name"].upper()
        for g in res[1]:
            if g is None or g[2] is None:
                continue
            wv = f(g[2])
            sel = g[4] if mode == 'collide' else g[5]
            if pane:
                sel = g[4] | g[5]
            P = wv[g[3][sel]]
            ras_tris.setdefault(4 if pane else 2, []).append(P)
    if want_m2:
        for m in m2s.values():
            col = m2_collision(m["name"])
            if not col:
                continue
            f = geo.placement_to_world(m["pos"], m["rot"], m["scale"])
            wv = f(col[0])
            ras_tris[3].append(wv[col[1]])
    return ras_tris, wmos, m2s


def run(bg, zpad=18.0, Rmin=None, out=None, mode='collide', zbelow=15.0):
    dirs, locs = load_common()
    m, (a, h) = ARENAS[bg]
    d = dirs[m]
    A, H = locs[a], locs[h]
    cx, cy = (A[1] + H[1]) / 2, (A[2] + H[2]) / 2
    R = max(90, 0.5 * math.hypot(A[1] - H[1], A[2] - H[2]) + 60) if Rmin is None else Rmin
    zfloor = (A[3] + H[3]) / 2
    flags, gwmo, tiles = geo.wdt_tiles(d)
    tri, wmos, m2s = collect(d, set(tiles), cx, cy, R, zfloor + zpad, mode=mode)
    ras = Raster(cx, cy, R, zfloor + zpad)
    if tri[1]:
        ras.tris(np.array(tri[1]), 1)
    for P in tri[2]:
        ras.tris(P, 2)
    for P in tri[3]:
        ras.tris(P, 3)
    for P in tri.get(4, []):
        ras.tris(P, 4)
    img = ras.image(zfloor - zbelow, zfloor + zpad)
    dr = ImageDraw.Draw(img)
    for (L, col) in ((A, (60, 120, 255)), (H, (255, 60, 60))):
        u, v = ras.uv(L[1], L[2])
        dr.ellipse((u - 8, v - 8, u + 8, v + 8), outline=col, width=3)
    img = img.resize((600, 600), Image.LANCZOS)
    mm = Image.open("mm/mm_%d_%d.png" % (bg, m)).resize((600, 600))
    both = Image.new("RGB", (1210, 630), (30, 30, 30))
    both.paste(mm, (0, 30))
    both.paste(img, (610, 30))
    ImageDraw.Draw(both).text((5, 5), "bg %d map %d %s  | left: client minimap | right: geometry (terrain green, WMO tan, M2 blue), z clip %.0f..%.0f, R=%d" % (bg, m, d, zfloor - 15, zfloor + zpad, R), fill=(255, 255, 255))
    both.save(out or "mm/geo_%d.png" % bg)
    print(bg, "terrain tris", len(tri[1]), "wmos", len(wmos), "m2", len(m2s))


if __name__ == "__main__":
    for x in sys.argv[1:]:
        run(int(x))
