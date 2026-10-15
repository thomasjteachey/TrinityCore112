"""Arena outline from the navmesh: the walkable floor reachable from the team starts within a
height band, traced and simplified into a polygon for `battleground_bounds.Boundary`."""
import math
from collections import deque
import numpy as np
from PIL import Image, ImageDraw
from navmesh import NavMesh, point_in_poly

CELL = 0.5


def region_grid(nm, starts, zlo, zhi, box):
    """Rasterise navmesh polys whose height lies in [zlo,zhi]; flood from the starts."""
    x0, x1, y0, y1 = box
    W = int((y1 - y0) / CELL) + 1   # columns along Y
    H = int((x1 - x0) / CELL) + 1   # rows along X
    grid = np.zeros((H, W), dtype=bool)
    for g, v, area, fl in nm.iter_polys():
        zs = v[:, 1]
        if not (zlo <= zs.mean() <= zhi):
            continue
        wx, wy = v[:, 2], v[:, 0]
        if wx.max() < x0 or wx.min() > x1 or wy.max() < y0 or wy.min() > y1:
            continue
        r0 = max(int((wx.min() - x0) / CELL), 0); r1 = min(int((wx.max() - x0) / CELL) + 1, H - 1)
        c0 = max(int((wy.min() - y0) / CELL), 0); c1 = min(int((wy.max() - y0) / CELL) + 1, W - 1)
        for r in range(r0, r1 + 1):
            for c in range(c0, c1 + 1):
                if point_in_poly(y0 + (c + 0.5) * CELL, x0 + (r + 0.5) * CELL, wy, wx):
                    grid[r, c] = True
    # flood fill from the starts
    reach = np.zeros_like(grid)
    dq = deque()
    for (sx, sy) in starts:
        r = int((sx - x0) / CELL); c = int((sy - y0) / CELL)
        best = None
        for dr in range(-6, 7):
            for dc in range(-6, 7):
                rr, cc = r + dr, c + dc
                if 0 <= rr < H and 0 <= cc < W and grid[rr, cc]:
                    d = dr * dr + dc * dc
                    if best is None or d < best[0]:
                        best = (d, rr, cc)
        if best:
            reach[best[1], best[2]] = True
            dq.append((best[1], best[2]))
    while dq:
        r, c = dq.popleft()
        for dr, dc in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            rr, cc = r + dr, c + dc
            if 0 <= rr < H and 0 <= cc < W and grid[rr, cc] and not reach[rr, cc]:
                reach[rr, cc] = True
                dq.append((rr, cc))
    # close one-cell cracks between navmesh polygons, then fill holes (pillars are walls anyway)
    reach = close(reach, 2)
    reach = fill_holes(reach)
    return reach, (x0, y0)


def close(m, n):
    for _ in range(n):
        m = dilate(m)
    for _ in range(n):
        m = erode(m)
    return m


def dilate(m):
    o = m.copy()
    o[1:] |= m[:-1]; o[:-1] |= m[1:]; o[:, 1:] |= m[:, :-1]; o[:, :-1] |= m[:, 1:]
    return o


def erode(m):
    o = m.copy()
    o[1:] &= m[:-1]; o[:-1] &= m[1:]; o[:, 1:] &= m[:, :-1]; o[:, :-1] &= m[:, 1:]
    o[0, :] = o[-1, :] = False; o[:, 0] = o[:, -1] = False
    return o


def fill_holes(m):
    H, W = m.shape
    outside = np.zeros_like(m)
    dq = deque()
    for r in range(H):
        for c in (0, W - 1):
            if not m[r, c] and not outside[r, c]:
                outside[r, c] = True; dq.append((r, c))
    for c in range(W):
        for r in (0, H - 1):
            if not m[r, c] and not outside[r, c]:
                outside[r, c] = True; dq.append((r, c))
    while dq:
        r, c = dq.popleft()
        for dr, dc in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            rr, cc = r + dr, c + dc
            if 0 <= rr < H and 0 <= cc < W and not m[rr, cc] and not outside[rr, cc]:
                outside[rr, cc] = True; dq.append((rr, cc))
    return ~outside


def trace(m):
    """Outer boundary of the largest blob as a loop of cell-corner points (r, c), corners at
    integer coordinates. Built from directed boundary edges chained into loops."""
    H, W = m.shape
    pad = np.zeros((H + 2, W + 2), dtype=bool)
    pad[1:-1, 1:-1] = m
    nxt = {}
    for r in range(1, H + 1):
        for c in range(1, W + 1):
            if not pad[r, c]:
                continue
            # corners of cell (r,c): (r-1,c-1) top-left .. using corner coords in unpadded space
            R, C = r - 1, c - 1
            if not pad[r - 1, c]:
                nxt[(R, C)] = (R, C + 1)          # top edge, left->right
            if not pad[r, c + 1]:
                nxt[(R, C + 1)] = (R + 1, C + 1)  # right edge, down
            if not pad[r + 1, c]:
                nxt[(R + 1, C + 1)] = (R + 1, C)  # bottom edge, right->left
            if not pad[r, c - 1]:
                nxt[(R + 1, C)] = (R, C)          # left edge, up
    seen = set()
    best = []
    for s in list(nxt):
        if s in seen:
            continue
        loop = [s]
        seen.add(s)
        cur = nxt.get(s)
        while cur is not None and cur != s and cur not in seen:
            loop.append(cur)
            seen.add(cur)
            cur = nxt.get(cur)
        if len(loop) > len(best):
            best = loop
    return best


def dp(points, tol):
    if len(points) < 3:
        return points
    a, b = np.array(points[0]), np.array(points[-1])
    ab = b - a
    L = np.hypot(*ab)
    best, idx = -1, 0
    for i in range(1, len(points) - 1):
        p = np.array(points[i])
        if L < 1e-9:
            d = np.hypot(*(p - a))
        else:
            d = abs(ab[0] * (a[1] - p[1]) - ab[1] * (a[0] - p[0])) / L
        if d > best:
            best, idx = d, i
    if best > tol:
        return dp(points[:idx + 1], tol)[:-1] + dp(points[idx:], tol)
    return [points[0], points[-1]]


def outline(nm, starts, zlo, zhi, box, tol=1.5):
    m, (x0, y0) = region_grid(nm, starts, zlo, zhi, box)
    path = trace(m)
    pts = [(x0 + r * CELL, y0 + c * CELL) for r, c in path]
    # simplify a closed ring: split at the farthest point from the first
    far = max(range(len(pts)), key=lambda i: (pts[i][0] - pts[0][0]) ** 2 + (pts[i][1] - pts[0][1]) ** 2)
    poly = dp(pts[:far + 1], tol)[:-1] + dp(pts[far:] + [pts[0]], tol)[:-1]
    return poly, m, (x0, y0)


def inside(poly, x, y):
    xs = [p[1] for p in poly]; ys = [p[0] for p in poly]
    return point_in_poly(y, x, xs, ys)


def boundary_text(poly):
    return ", ".join("%.1f %.1f" % (x, y) for x, y in poly)
