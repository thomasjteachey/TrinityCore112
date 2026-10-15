"""Server-side collision as the worldserver sees it: vmtile spawns + .vmo meshes + .map terrain."""
import math, os
import numpy as np
import geo, vmapread
from gridmap import MapData

# A copy of a realm's data/{maps,vmaps,mmaps} for the maps in question, plus the
# .vmo/.m2 models their vmtiles name, in vmo/ - see README.md for the tar line.
SRV = os.environ.get("ARENA_SRV_DATA", os.path.join(os.path.dirname(os.path.abspath(__file__)), "srv"))
MID = geo.MID
_vmo = {}


def vmo(name):
    n = name.rstrip("\x00")
    if n in _vmo:
        return _vmo[n]
    p = os.path.join(SRV, "vmo", n + ".vmo")
    if not os.path.exists(p):
        p = os.path.join(SRV, "vmo", n)
    if not os.path.exists(p):
        _vmo[n] = None
        return None
    _vmo[n] = vmapread.read_vmo(p)
    return _vmo[n]


def spawn_tris(s):
    mdl = vmo(s["name"])
    if not mdl:
        return []
    root, groups = mdl
    ipos = np.array(s["pos"])
    rot = s["rot"]
    R = geo.euler_zyx(math.radians(rot[1]), math.radians(rot[0]), math.radians(rot[2]))
    out = []
    for g in groups:
        if len(g["tris"]) == 0:
            continue
        internal = (g["verts"] * s["scale"]) @ R.T + ipos
        w = np.empty_like(internal)
        w[:, 0] = MID - internal[:, 0]
        w[:, 1] = MID - internal[:, 1]
        w[:, 2] = internal[:, 2]
        out.append(w[g["tris"]])
    return out


def collect(mapid, cx, cy, R):
    tris = []
    for s in vmapread.map_spawns(os.path.join(SRV, "vmaps"), mapid):
        for P in spawn_tris(s):
            keep = ((P[:, :, 0].max(1) >= cx - R) & (P[:, :, 0].min(1) <= cx + R) &
                    (P[:, :, 1].max(1) >= cy - R) & (P[:, :, 1].min(1) <= cy + R))
            if keep.any():
                tris.append(P[keep])
    return np.concatenate(tris) if tris else np.zeros((0, 3, 3))


def terrain(mapid, x, y):
    return MapData(os.path.join(SRV, "maps"), mapid).height(x, y)
