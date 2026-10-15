"""M2 (v264) geometry + textures for the top-down renderer, and WMO doodad sets."""
import math, struct
import numpy as np
import cl, geo, texrender as tr

_m2 = {}


def m2_name(n):
    if n.lower().endswith((".mdx", ".mdl")):
        n = n[:-4] + ".m2"
    return n


def load_m2(name):
    """(verts Nx3, uv Nx2, [(tri indices Mx3, texture array or None, alphatest)]) or None."""
    name = m2_name(name)
    key = name.upper()
    if key in _m2:
        return _m2[key]
    res = None
    b = cl.read(name)
    if b is not None and b[:4] == b"MD20":
        try:
            res = _parse(name, b)
        except Exception as e:
            print("m2 parse failed", name, e)
    _m2[key] = res
    return res


def _arr(b, off):
    return struct.unpack_from("<II", b, off)


def _parse(name, b):
    nv, ov = _arr(b, 0x3C)
    nviews = struct.unpack_from("<I", b, 0x44)[0]
    ntex, otex = _arr(b, 0x50)
    nrf, orf = _arr(b, 0x70)
    ntl, otl = _arr(b, 0x80)
    raw = np.frombuffer(b, dtype=np.uint8, count=nv * 48, offset=ov).reshape(nv, 48)
    pos = raw[:, 0:12].copy().view("<f4").reshape(nv, 3).astype(np.float64)
    uv = raw[:, 32:40].copy().view("<f4").reshape(nv, 2).astype(np.float64)
    texnames = []
    for i in range(ntex):
        ttype, tflags, nlen, nofs = struct.unpack_from("<IIII", b, otex + i * 16)
        texnames.append(b[nofs:nofs + nlen].split(b"\0")[0].decode("latin1") if ttype == 0 and nlen else None)
    texlookup = list(struct.unpack_from("<%dH" % ntl, b, otl)) if ntl else []
    renderflags = [struct.unpack_from("<HH", b, orf + i * 4) for i in range(nrf)]
    if nviews == 0:
        return None
    skin = cl.read(name[:-3] + "00.skin")
    if skin is None or skin[:4] != b"SKIN":
        return None
    nind, oind = _arr(skin, 4)
    ntri, otri = _arr(skin, 12)
    nsub, osub = _arr(skin, 28)
    nbat, obat = _arr(skin, 36)
    lookup = np.frombuffer(skin, dtype="<u2", count=nind, offset=oind).astype(np.int64)
    tris = np.frombuffer(skin, dtype="<u2", count=ntri, offset=otri).astype(np.int64)
    subs = []
    for i in range(nsub):
        sid, level, vstart, vcount, tstart, tcount = struct.unpack_from("<6H", skin, osub + i * 48)
        tstart += level << 16
        subs.append((tstart, tcount))
    parts = []
    for i in range(nbat):
        flags, prio, shader, skinsec, geoset, color, matidx, matlayer, texcount, texcombo = struct.unpack_from("<BbHHHhHHHH", skin, obat + i * 24)
        if skinsec >= len(subs) or matlayer != 0:
            continue
        tstart, tcount = subs[skinsec]
        t = tris[tstart:tstart + tcount]
        t = lookup[t].reshape(-1, 3)
        tex = None
        if texcombo < len(texlookup):
            ti = texlookup[texcombo]
            if ti < len(texnames) and texnames[ti]:
                tex = tr.texture(texnames[ti])
        blend = renderflags[matidx][1] if matidx < len(renderflags) else 0
        parts.append((t, tex, blend))
    return pos, uv, parts


def quat_matrix(x, y, z, w):
    return np.array([
        [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
        [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
        [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)],
    ])


def draw_m2(ras, model, to_world):
    pos, uv, parts = model
    wv = to_world(pos)
    for t, tex, blend in parts:
        if blend >= 2:          # additive / blended glow cards: not part of the floor plan
            continue
        P = wv[t]
        n = np.cross(P[:, 1] - P[:, 0], P[:, 2] - P[:, 0])
        n = n / (np.linalg.norm(n, axis=1)[:, None] + 1e-12)
        n[n[:, 2] < 0] *= -1
        shade = 0.45 + 0.75 * np.clip(n @ tr.LIGHT, 0, 1)
        ras.draw(P, uv[t], tex, shade, alphatest=(blend == 1))


def render_adt_m2(ras, m):
    model = load_m2(m["name"])
    if not model:
        return
    draw_m2(ras, model, geo.placement_to_world(m["pos"], m["rot"], m["scale"]))


def render_wmo_doodads(ras, wmo_placement):
    root = cl.read(wmo_placement["name"])
    if root is None:
        return
    no, ns = geo.find(root, "MODN")
    modn = root[no:no + ns] if no is not None else b""
    so, ss = geo.find(root, "MODS")
    sets = [struct.unpack_from("<20sIII", root, so + i * 32) for i in range(ss // 32)] if so is not None else []
    do, ds = geo.find(root, "MODD")
    if do is None:
        return
    chosen = {0, wmo_placement.get("doodadset", 0)}
    idx = set()
    for si in chosen:
        if si < len(sets):
            start, count = sets[si][1], sets[si][2]
            idx.update(range(start, start + count))
    wmo_f = geo.placement_to_world(wmo_placement["pos"], wmo_placement["rot"])
    for i in sorted(idx):
        b = do + i * 40
        if b + 40 > do + ds:
            continue
        nameofs = struct.unpack_from("<I", root, b)[0] & 0xFFFFFF
        dpos = np.array(struct.unpack_from("<3f", root, b + 4))
        qx, qy, qz, qw = struct.unpack_from("<4f", root, b + 16)
        scale = struct.unpack_from("<f", root, b + 32)[0]
        if nameofs >= len(modn):
            continue
        nm = modn[nameofs:modn.index(b"\0", nameofs)].decode("latin1")
        model = load_m2(nm)
        if not model:
            continue
        Rq = quat_matrix(qx, qy, qz, qw)

        def to_world(v, Rq=Rq, dpos=dpos, scale=scale):
            local = (v * scale) @ Rq.T + dpos
            return wmo_f(local)
        draw_m2(ras, model, to_world)
