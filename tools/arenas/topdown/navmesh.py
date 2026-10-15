"""Read TrinityCore .mmtile navmesh tiles and answer "where can a bot walk from here".

Recast coords in TC tiles are (worldY, worldZ, worldX).
"""
import glob, os, struct
from collections import defaultdict, deque
import numpy as np

DT_EXT_LINK = 0x8000


class Tile:
    def __init__(self, path):
        b = open(path, "rb").read()
        magic, dtv, mmv, size, liq = struct.unpack_from("<IIIIB", b, 0)
        o = 20
        h = struct.unpack_from("<6i I 9i 3f 3f 3f f", b, o)
        (self.magic, self.version, self.tx, self.ty, self.layer) = h[0:5]
        self.userId = h[5]
        (self.polyCount, self.vertCount, self.maxLinkCount, self.detailMeshCount,
         self.detailVertCount, self.detailTriCount, self.bvNodeCount,
         self.offMeshConCount, self.offMeshBase) = h[6:15]
        self.walkableHeight, self.walkableRadius, self.walkableClimb = h[15:18]
        self.bmin = h[18:21]
        self.bmax = h[21:24]
        p = o + 100
        self.verts = np.frombuffer(b, dtype="<f4", count=self.vertCount * 3, offset=p).reshape(-1, 3).astype(np.float64)
        p += (self.vertCount * 12 + 3) & ~3
        self.polys = []
        for i in range(self.polyCount):
            q = p + i * 32
            firstLink = struct.unpack_from("<I", b, q)[0]
            verts = struct.unpack_from("<6H", b, q + 4)
            neis = struct.unpack_from("<6H", b, q + 16)
            flags, vc, at = struct.unpack_from("<HBB", b, q + 28)
            self.polys.append((verts[:vc], neis[:vc], flags, at & 0x3F, at >> 6))
        p += self.polyCount * 32
        p += self.maxLinkCount * 16
        self.detail = []
        for i in range(self.detailMeshCount):
            vb, tb, vc_, tc_ = struct.unpack_from("<IIBB", b, p + i * 12)
            self.detail.append((vb, vc_))
        p += self.detailMeshCount * 12
        self.dverts = np.frombuffer(b, dtype="<f4", count=self.detailVertCount * 3, offset=p).reshape(-1, 3).astype(np.float64)
        p += (self.detailVertCount * 12 + 3) & ~3
        p += (self.detailTriCount * 4 + 3) & ~3
        p += self.bvNodeCount * 16
        self.offmesh = []
        for i in range(self.offMeshConCount):
            q = p + i * 36
            pos = struct.unpack_from("<6f", b, q)
            rad = struct.unpack_from("<f", b, q + 24)[0]
            poly, fl, side = struct.unpack_from("<HBB", b, q + 28)
            self.offmesh.append((pos, rad, poly, fl, side))
        self.path = path


def world(v):
    """recast (y, z, x) -> world (x, y, z)."""
    return (v[2], v[0], v[1])


class NavMesh:
    def __init__(self, mmap_dir, mapid):
        self.tiles = []
        for f in sorted(glob.glob(os.path.join(mmap_dir, "%03d????.mmtile" % mapid))):
            name = os.path.basename(f)
            if len(name) != len("%03d" % mapid) + 4 + 7:
                continue
            self.tiles.append(Tile(f))
        self.n = sum(t.polyCount for t in self.tiles)
        self.base = []
        acc = 0
        for t in self.tiles:
            self.base.append(acc)
            acc += t.polyCount
        self.adj = defaultdict(set)
        self._build()

    def gid(self, ti, pi):
        return self.base[ti] + pi

    def poly_verts(self, ti, pi):
        t = self.tiles[ti]
        return t.verts[list(t.polys[pi][0])]

    def _build(self):
        border = defaultdict(list)   # (tx,ty,side) -> [(gid, a, b)]
        for ti, t in enumerate(self.tiles):
            for pi, (vs, ns, fl, area, typ) in enumerate(t.polys):
                g = self.gid(ti, pi)
                if typ == 1:
                    continue
                for j, nb in enumerate(ns):
                    if nb == 0:
                        continue
                    if nb & DT_EXT_LINK:
                        side = nb & 0xFF
                        a = t.verts[vs[j]]
                        bb = t.verts[vs[(j + 1) % len(vs)]]
                        border[(t.tx, t.ty, side)].append((g, a, bb))
                    else:
                        self.adj[g].add(self.gid(ti, nb - 1))
        # cross-tile portals: side 0 = +x, 2 = +z, 4 = -x, 6 = -z (recast axes)
        off = {0: (1, 0), 2: (0, 1), 4: (-1, 0), 6: (0, -1)}
        climb = max((t.walkableClimb for t in self.tiles), default=1.0)
        for (tx, ty, side), edges in border.items():
            if side not in off:
                continue
            dx, dy = off[side]
            other = border.get((tx + dx, ty + dy, (side + 4) % 8), [])
            if not other:
                continue
            axis = 2 if side in (0, 4) else 0     # coordinate along the border
            for g, a, b in edges:
                lo, hi = sorted((a[axis], b[axis]))
                for g2, a2, b2 in other:
                    lo2, hi2 = sorted((a2[axis], b2[axis]))
                    ov_lo, ov_hi = max(lo, lo2), min(hi, hi2)
                    if ov_hi - ov_lo < 0.01:
                        continue
                    # heights at overlap midpoint
                    m = 0.5 * (ov_lo + ov_hi)

                    def hat(p, q):
                        if abs(q[axis] - p[axis]) < 1e-6:
                            return 0.5 * (p[1] + q[1])
                        u = (m - p[axis]) / (q[axis] - p[axis])
                        return p[1] + u * (q[1] - p[1])
                    if abs(hat(a, b) - hat(a2, b2)) <= climb * 2 + 0.5:
                        self.adj[g].add(g2)
                        self.adj[g2].add(g)
        # off-mesh connections
        for ti, t in enumerate(self.tiles):
            for (pos, rad, poly, fl, side) in t.offmesh:
                g = self.gid(ti, poly)
                for end in (pos[0:3], pos[3:6]):
                    h = self.find_poly_recast(end, maxdz=3.0)
                    if h is not None:
                        self.adj[g].add(h)
                        if fl & 1 or end is pos[0:3]:
                            self.adj[h].add(g)

    def zrange(self, ti, pi):
        t = self.tiles[ti]
        v = t.verts[list(t.polys[pi][0])]
        lo, hi = v[:, 1].min(), v[:, 1].max()
        if pi < len(t.detail):
            vb, vc = t.detail[pi]
            if vc:
                d = t.dverts[vb:vb + vc]
                lo, hi = min(lo, d[:, 1].min()), max(hi, d[:, 1].max())
        return lo, hi

    def find_poly_recast(self, p, maxdz=4.0):
        best, bestd = None, 1e9
        for ti, t in enumerate(self.tiles):
            if not (t.bmin[0] - 1 <= p[0] <= t.bmax[0] + 1 and t.bmin[2] - 1 <= p[2] <= t.bmax[2] + 1):
                continue
            for pi, (vs, ns, fl, area, typ) in enumerate(t.polys):
                if typ == 1:
                    continue
                v = t.verts[list(vs)]
                if not point_in_poly(p[0], p[2], v[:, 0], v[:, 2]):
                    continue
                lo, hi = self.zrange(ti, pi)
                dz = 0.0 if lo <= p[1] <= hi else min(abs(p[1] - lo), abs(p[1] - hi))
                if dz < bestd and dz <= maxdz:
                    best, bestd = self.gid(ti, pi), dz
        return best

    def find_poly_world(self, x, y, z, maxdz=4.0):
        return self.find_poly_recast((y, z, x), maxdz)

    def nearest_poly_world(self, x, y, z, rad=6.0):
        """Closest poly centroid within rad (for far-from-poly starts)."""
        best, bd = None, 1e9
        for ti, t in enumerate(self.tiles):
            for pi, (vs, ns, fl, area, typ) in enumerate(t.polys):
                if typ == 1:
                    continue
                c = t.verts[list(vs)].mean(0)
                d = (c[2] - x) ** 2 + (c[0] - y) ** 2 + 4 * (c[1] - z) ** 2
                if d < bd:
                    best, bd = self.gid(ti, pi), d
        return best if bd <= rad * rad * 4 else None

    def reach(self, start):
        seen = {start}
        dq = deque([start])
        while dq:
            g = dq.popleft()
            for h in self.adj[g]:
                if h not in seen:
                    seen.add(h)
                    dq.append(h)
        return seen

    def iter_polys(self):
        for ti, t in enumerate(self.tiles):
            for pi, (vs, ns, fl, area, typ) in enumerate(t.polys):
                if typ == 1:
                    continue
                yield self.gid(ti, pi), t.verts[list(vs)], area, fl


def point_in_poly(x, z, xs, zs):
    inside = False
    n = len(xs)
    j = n - 1
    for i in range(n):
        if ((zs[i] > z) != (zs[j] > z)) and (x < (xs[j] - xs[i]) * (z - zs[i]) / (zs[j] - zs[i] + 1e-12) + xs[i]):
            inside = not inside
        j = i
    return inside
