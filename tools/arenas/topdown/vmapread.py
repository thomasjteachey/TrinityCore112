"""TrinityCore VMAP_4.8 reader: vmtree global spawns + vmtile spawns + .vmo group meshes."""
import glob, os, struct
import numpy as np


def read_spawn(b, p):
    flags, = struct.unpack_from("<I", b, p); p += 4
    adtId, = struct.unpack_from("<H", b, p); p += 2
    ID, = struct.unpack_from("<I", b, p); p += 4
    pos = struct.unpack_from("<3f", b, p); p += 12
    rot = struct.unpack_from("<3f", b, p); p += 12
    scale, = struct.unpack_from("<f", b, p); p += 4
    bound = None
    if flags & 4:
        bound = (struct.unpack_from("<3f", b, p), struct.unpack_from("<3f", b, p + 12)); p += 24
    nl, = struct.unpack_from("<I", b, p); p += 4
    name = b[p:p + nl].decode("latin1"); p += nl
    return dict(flags=flags, adt=adtId, id=ID, pos=pos, rot=rot, scale=scale, bound=bound, name=name), p


def tile_spawns(path):
    b = open(path, "rb").read()
    assert b[:8] == b"VMAP_4.8", b[:8]
    n, = struct.unpack_from("<I", b, 8)
    p = 12
    out = []
    for i in range(n):
        s, p = read_spawn(b, p)
        ref, = struct.unpack_from("<I", b, p); p += 4
        s["ref"] = ref
        out.append(s)
    return out


def map_spawns(vdir, mapid):
    res = {}
    for f in glob.glob(os.path.join(vdir, "%03d_*.vmtile" % mapid)):
        for s in tile_spawns(f):
            res[(s["id"], s["name"])] = s
    return list(res.values())


def read_vmo(path):
    b = open(path, "rb").read()
    assert b[:8] == b"VMAP_4.8", (path, b[:8])
    p = 8
    assert b[p:p + 4] == b"WMOD"; p += 4
    sz, root = struct.unpack_from("<II", b, p); p += 8
    groups = []
    if b[p:p + 4] == b"GMOD":
        p += 4
        cnt, = struct.unpack_from("<I", b, p); p += 4
        for i in range(cnt):
            lo = struct.unpack_from("<3f", b, p); hi = struct.unpack_from("<3f", b, p + 12); p += 24
            mflags, gid = struct.unpack_from("<II", b, p); p += 8
            assert b[p:p + 4] == b"VERT", b[p:p + 4]; p += 4
            csz, vc = struct.unpack_from("<II", b, p); p += 8
            if vc == 0:
                # GroupModel::readFromFile stops here for a group with no collision geometry
                groups.append(dict(flags=mflags, id=gid, verts=np.zeros((0, 3)), tris=np.zeros((0, 3), dtype=np.int64), lo=lo, hi=hi))
                continue
            verts = np.frombuffer(b, dtype="<f4", count=vc * 3, offset=p).reshape(-1, 3).astype(np.float64); p += vc * 12
            assert b[p:p + 4] == b"TRIM"; p += 4
            csz, tc = struct.unpack_from("<II", b, p); p += 8
            tris = np.frombuffer(b, dtype="<u4", count=tc * 3, offset=p).reshape(-1, 3).astype(np.int64); p += tc * 12
            assert b[p:p + 4] == b"MBIH"; p += 4
            # BIH: lo(3f) hi(3f) treeSize tree[] count objects[]
            p += 24
            ts, = struct.unpack_from("<I", b, p); p += 4 + ts * 4
            oc, = struct.unpack_from("<I", b, p); p += 4 + oc * 4
            assert b[p:p + 4] == b"LIQU", b[p:p + 4]; p += 4
            lsz, = struct.unpack_from("<I", b, p); p += 4 + lsz
            groups.append(dict(flags=mflags, id=gid, verts=verts, tris=tris, lo=lo, hi=hi))
    return root, groups
