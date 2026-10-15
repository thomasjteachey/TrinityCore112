"""Client geometry: WDT/ADT terrain + WMO placements + WMO group meshes, in WORLD coordinates.

World frame: X north, Y west, Z up (server/.gps frame).
"""
import math
import struct
import numpy as np
import cl

T = 1600.0 / 3
MID = 32 * T
BS = "\\"


def chunks(data, start=0, end=None):
    end = len(data) if end is None else min(end, len(data))
    pos = start
    while pos + 8 <= end:
        magic = data[pos:pos + 4][::-1].decode("ascii", "replace")
        size = struct.unpack_from("<I", data, pos + 4)[0]
        yield magic, pos + 8, size
        pos += 8 + size


def find(data, want, start=0, end=None):
    for m, o, s in chunks(data, start, end):
        if m == want:
            return o, s
    return None, 0


def strtab(blob):
    """offset -> name, for MWMO/MMDX style blocks."""
    out = {}
    i = 0
    while i < len(blob):
        j = blob.find(b"\0", i)
        if j < 0:
            j = len(blob)
        if j > i:
            out[i] = blob[i:j].decode("latin1")
        i = j + 1
    return out


def wdt_tiles(directory):
    w = cl.read("World" + BS + "Maps" + BS + directory + BS + directory + ".wdt")
    if w is None:
        return None, None, []
    o, s = find(w, "MAIN")
    tiles = []
    for i in range(4096):
        if struct.unpack_from("<I", w, o + i * 8)[0] & 1:
            tiles.append((i % 64, i // 64))   # (col, row) = adt _<col>_<row>
    mo, ms = find(w, "MPHD")
    flags = struct.unpack_from("<I", w, mo)[0]
    gwmo = None
    no, ns = find(w, "MWMO")
    if no is not None and ns > 1:
        name = w[no:no + ns].split(b"\0")[0].decode("latin1")
        fo, fs = find(w, "MODF")
        if fo is not None and fs >= 64:
            gwmo = parse_modf(w[fo:fo + 64], {0: name}, [0])[0]
    return flags, gwmo, tiles


def parse_modf(blob, names, ids):
    out = []
    for i in range(len(blob) // 64):
        b = i * 64
        nid, uniq = struct.unpack_from("<II", blob, b)
        pos = struct.unpack_from("<3f", blob, b + 8)
        rot = struct.unpack_from("<3f", blob, b + 20)
        flags, dset, nset, scale = struct.unpack_from("<4H", blob, b + 56)
        name = names.get(ids[nid], "?") if nid < len(ids) else "?"
        out.append({"name": name, "uid": uniq, "pos": pos, "rot": rot, "flags": flags,
                    "doodadset": dset})
    return out


def parse_mddf(blob, names, ids):
    out = []
    for i in range(len(blob) // 36):
        b = i * 36
        nid, uniq = struct.unpack_from("<II", blob, b)
        pos = struct.unpack_from("<3f", blob, b + 8)
        rot = struct.unpack_from("<3f", blob, b + 20)
        scale, flags = struct.unpack_from("<HH", blob, b + 32)
        name = names.get(ids[nid], "?") if nid < len(ids) else "?"
        out.append({"name": name, "uid": uniq, "pos": pos, "rot": rot, "scale": scale / 1024.0,
                    "flags": flags})
    return out


def read_adt(directory, col, row):
    name = "World" + BS + "Maps" + BS + directory + BS + "%s_%d_%d.adt" % (directory, col, row)
    a = cl.read(name)
    if a is None:
        return None
    res = {"src": cl.src(name), "mcnk": [], "wmo": [], "m2": []}
    mo, ms = find(a, "MWMO")
    wnames = strtab(a[mo:mo + ms]) if mo is not None else {}
    io_, is_ = find(a, "MWID")
    wids = list(struct.unpack_from("<%dI" % (is_ // 4), a, io_)) if io_ is not None else []
    fo, fs = find(a, "MODF")
    if fo is not None:
        res["wmo"] = parse_modf(a[fo:fo + fs], wnames, wids)
    mo, ms = find(a, "MMDX")
    mnames = strtab(a[mo:mo + ms]) if mo is not None else {}
    io_, is_ = find(a, "MMID")
    mids = list(struct.unpack_from("<%dI" % (is_ // 4), a, io_)) if io_ is not None else []
    fo, fs = find(a, "MDDF")
    if fo is not None:
        res["m2"] = parse_mddf(a[fo:fo + fs], mnames, mids)
    for m, o, s in chunks(a):
        if m != "MCNK":
            continue
        flags, ix, iy = struct.unpack_from("<3I", a, o)
        area = struct.unpack_from("<I", a, o + 0x34)[0]
        holes = struct.unpack_from("<H", a, o + 0x3C)[0]
        ofs_mcvt = struct.unpack_from("<I", a, o + 0x14)[0]
        px, py, pz = struct.unpack_from("<3f", a, o + 0x68)
        # MCVT sub-chunk: offsets are relative to the MCNK chunk start (o - 8)
        hb = o - 8 + ofs_mcvt + 8
        h = struct.unpack_from("<145f", a, hb)
        res["mcnk"].append({"ix": ix, "iy": iy, "area": area, "holes": holes,
                            "pos": (px, py, pz), "h": h, "flags": flags})
    return res


def euler_zyx(y, p, r):
    cy, sy = math.cos(y), math.sin(y)
    cp, sp = math.cos(p), math.sin(p)
    cr, sr = math.cos(r), math.sin(r)
    Rz = np.array([[cy, -sy, 0], [sy, cy, 0], [0, 0, 1]])
    Ry = np.array([[cp, 0, sp], [0, 1, 0], [-sp, 0, cp]])
    Rx = np.array([[1, 0, 0], [0, cr, -sr], [0, sr, cr]])
    return Rz @ Ry @ Rx


def placement_to_world(pos, rot, scale=1.0):
    """Return f(local Nx3) -> world Nx3, following TC's vmap convention."""
    ipos = np.array([pos[2], pos[0], pos[1]])        # internal = (z, x, y)
    R = euler_zyx(math.radians(rot[1]), math.radians(rot[0]), math.radians(rot[2]))

    def f(v):
        internal = (v * scale) @ R.T + ipos
        out = np.empty_like(internal)
        out[:, 0] = MID - internal[:, 0]
        out[:, 1] = MID - internal[:, 1]
        out[:, 2] = internal[:, 2]
        return out
    return f


_wmo_cache = {}


def wmo_groups(root_name):
    """[(groupflags, verts Nx3 local, tris Mx3, collide mask M, render mask M)] for a WMO root."""
    key = root_name.upper()
    if key in _wmo_cache:
        return _wmo_cache[key]
    root = cl.read(root_name)
    if root is None:
        _wmo_cache[key] = None
        return None
    o, s = find(root, "MOHD")
    ngroups = struct.unpack_from("<I", root, o + 4)[0]
    go, gs = find(root, "MOGI")
    gi = []
    for i in range(ngroups):
        gi.append(struct.unpack_from("<I", root, go + i * 32)[0])
    out = []
    base = root_name[:-4]
    for i in range(ngroups):
        g = cl.read("%s_%03d.wmo" % (base, i))
        if g is None:
            out.append(None)
            continue
        po, ps = find(g, "MOGP")
        gflags = struct.unpack_from("<I", g, po + 8)[0]
        sub = po + 0x44
        verts = tris = mopy = None
        for m, so, ss in chunks(g, sub, po + ps):
            if m == "MOVT":
                verts = np.frombuffer(g, dtype="<f4", count=ss // 4, offset=so).reshape(-1, 3).astype(np.float64)
            elif m == "MOVI":
                tris = np.frombuffer(g, dtype="<u2", count=ss // 2, offset=so).reshape(-1, 3).astype(np.int64)
            elif m == "MOPY":
                mopy = np.frombuffer(g, dtype="u1", count=ss, offset=so).reshape(-1, 2)
        if verts is None or tris is None:
            out.append((gflags, gi[i], None, None, None, None))
            continue
        fl = mopy[:, 0] if mopy is not None else np.zeros(len(tris), dtype=np.uint8)
        mat = mopy[:, 1] if mopy is not None else np.zeros(len(tris), dtype=np.uint8)
        renderface = ((fl & 0x20) != 0) & ((fl & 0x04) == 0)
        collide = ((fl & 0x08) != 0) | renderface      # TC vmap extractor rule
        render = (mat != 0xFF)                           # visible to the player
        out.append((gflags, gi[i], verts, tris, collide, render))
    _wmo_cache[key] = (gi, out)
    return _wmo_cache[key]


def adt_index(wx, wy):
    """(col, row) of the ADT covering world (x, y)."""
    return int(32 - wy / T), int(32 - wx / T)
