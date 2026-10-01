"""Path across one TrinityCore .mmtile (Detour navmesh tile) between world points.

  python3 mmpath.py <tile.mmtile> x1 y1 z1 x2 y2 z2 [x3 y3 z3 ...]

For each leg: which polygon each end lands on (and how far off the mesh it is), then an
A* over polygon adjacency inside the tile, printed as walkable waypoints (portal-edge
midpoints, collinear points dropped). Used to lay Var'jun's escort route over the cave.

Detour layout (TC v4 mmaps, DT_VERTS_PER_POLYGON 6): 20-byte MmapTileHeader, then
dtMeshHeader (100 bytes), verts (float[3] each, Recast order y,z,x of WoW), polys (32 bytes).
"""
import heapq, math, struct, sys


def load(path):
    d = open(path, 'rb').read()
    off = 20
    h = struct.unpack_from('<15i10f', d, off)
    poly_count, vert_count = h[6], h[7]
    off += 100
    verts = []
    for i in range(vert_count):
        ry, rz, rx = struct.unpack_from('<3f', d, off + i * 12)   # recast (y, z, x) of WoW
        verts.append((rx, ry, rz))
    off += vert_count * 12
    polys = []
    for i in range(poly_count):
        first, *rest = struct.unpack_from('<I6H6HHBB', d, off + i * 32)
        pv, neis, flags, vc, area = rest[0:6], rest[6:12], rest[12], rest[13], rest[14]
        if area >> 6 == 1:          # off-mesh connection
            polys.append(None)
            continue
        polys.append((pv[:vc], neis[:vc]))
    return verts, polys


def poly_pts(verts, poly):
    return [verts[i] for i in poly[0]]


def centroid(pts):
    n = len(pts)
    return (sum(p[0] for p in pts) / n, sum(p[1] for p in pts) / n, sum(p[2] for p in pts) / n)


def contains2d(pts, x, y):
    inside = False
    j = len(pts) - 1
    for i in range(len(pts)):
        xi, yi = pts[i][0], pts[i][1]
        xj, yj = pts[j][0], pts[j][1]
        if (yi > y) != (yj > y) and x < (xj - xi) * (y - yi) / (yj - yi + 1e-12) + xi:
            inside = not inside
        j = i
    return inside


def locate(verts, polys, p):
    best, bestd = None, 1e18
    for idx, poly in enumerate(polys):
        if poly is None:
            continue
        pts = poly_pts(verts, poly)
        c = centroid(pts)
        if contains2d(pts, p[0], p[1]):
            dz = abs(c[2] - p[2])
            if dz < bestd:
                best, bestd = idx, dz
    if best is not None and bestd < 6:
        return best, bestd, 'inside'
    best, bestd = None, 1e18
    for idx, poly in enumerate(polys):
        if poly is None:
            continue
        c = centroid(poly_pts(verts, poly))
        dd = math.dist(c, p)
        if dd < bestd:
            best, bestd = idx, dd
    return best, bestd, 'nearest'


def edge_mid(verts, poly, k):
    a = verts[poly[0][k]]
    b = verts[poly[0][(k + 1) % len(poly[0])]]
    return ((a[0] + b[0]) / 2, (a[1] + b[1]) / 2, (a[2] + b[2]) / 2)


def astar(verts, polys, s, g):
    goal = centroid(poly_pts(verts, polys[g]))
    openq = [(0.0, s)]
    came = {s: (None, None)}
    cost = {s: 0.0}
    cent = {}

    def c(i):
        if i not in cent:
            cent[i] = centroid(poly_pts(verts, polys[i]))
        return cent[i]
    while openq:
        _, cur = heapq.heappop(openq)
        if cur == g:
            break
        poly = polys[cur]
        for k, n in enumerate(poly[1]):
            if n == 0 or n & 0x8000:
                continue
            nb = n - 1
            if polys[nb] is None:
                continue
            nc = cost[cur] + math.dist(c(cur), c(nb))
            if nb not in cost or nc < cost[nb]:
                cost[nb] = nc
                came[nb] = (cur, k)
                heapq.heappush(openq, (nc + math.dist(c(nb), goal), nb))
    if g not in came:
        return None
    path = []
    cur = g
    while came[cur][0] is not None:
        prev, k = came[cur]
        path.append(edge_mid(verts, polys[prev], k))
        cur = prev
    return list(reversed(path))


def simplify(points, tol=1.5):
    if len(points) < 3:
        return points
    out = [points[0]]
    for i in range(1, len(points) - 1):
        a, b, c = out[-1], points[i], points[i + 1]
        # keep b when it bends the line a->c by more than tol yards or changes height
        ax, ay, cx, cy = a[0], a[1], c[0], c[1]
        seg = math.hypot(cx - ax, cy - ay) or 1e-9
        dev = abs((cx - ax) * (ay - b[1]) - (ax - b[0]) * (cy - ay)) / seg
        if dev > tol or abs(b[2] - (a[2] + c[2]) / 2) > 2.0:
            out.append(b)
    out.append(points[-1])
    return out


def main():
    verts, polys = load(sys.argv[1])
    nums = list(map(float, sys.argv[2:]))
    pts = [tuple(nums[i:i + 3]) for i in range(0, len(nums), 3)]
    print('tile: %d verts, %d polys' % (len(verts), sum(1 for p in polys if p)))
    for a, b in zip(pts, pts[1:]):
        sa, da, ka = locate(verts, polys, a)
        sb, db, kb = locate(verts, polys, b)
        print('\nleg %s -> %s' % (a, b))
        print('  start poly %d (%s, %.1f yd off)   end poly %d (%s, %.1f yd off)' % (sa, ka, da, sb, kb, db))
        path = astar(verts, polys, sa, sb)
        if path is None:
            print('  NO PATH inside this tile')
            continue
        way = simplify([a] + path + [b])
        print('  %d portals -> %d waypoints:' % (len(path), len(way)))
        for p in way:
            print('    { %.1ff, %.1ff, %.1ff },' % p)


if __name__ == '__main__':
    main()
