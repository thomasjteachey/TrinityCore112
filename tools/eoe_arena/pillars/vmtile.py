"""Read/write VMAP_4.8 .vmtree and .vmtile files (TrinityCore 3.3.5 layout, MapTree.cpp / ModelInstance.cpp)."""
import struct

MAGIC = b"VMAP_4.8"
MOD_M2, MOD_WORLDSPAWN, MOD_HAS_BOUND = 1, 2, 4


def read_bih(d, p):
    lo = struct.unpack_from("<3f", d, p); hi = struct.unpack_from("<3f", d, p + 12); p += 24
    n, = struct.unpack_from("<I", d, p); p += 4
    tree = list(struct.unpack_from("<%dI" % n, d, p)); p += 4 * n
    c, = struct.unpack_from("<I", d, p); p += 4
    objs = list(struct.unpack_from("<%dI" % c, d, p)); p += 4 * c
    return (lo, hi, tree, objs), p


def write_bih(b):
    lo, hi, tree, objs = b
    return (struct.pack("<3f", *lo) + struct.pack("<3f", *hi) + struct.pack("<I", len(tree)) +
            struct.pack("<%dI" % len(tree), *tree) + struct.pack("<I", len(objs)) + struct.pack("<%dI" % len(objs), *objs))


def read_vmtree(path):
    d = open(path, "rb").read()
    assert d[:8] == MAGIC
    tiled = d[8]
    assert d[9:13] == b"NODE"
    bih, p = read_bih(d, 13)
    assert d[p:p + 4] == b"GOBJ"
    return tiled, bih, d[p + 4:]


def write_vmtree(path, tiled, bih, rest):
    open(path, "wb").write(MAGIC + bytes([tiled]) + b"NODE" + write_bih(bih) + b"GOBJ" + rest)


def read_spawn(d, p):
    flags, adt, sid = struct.unpack_from("<IHI", d, p); p += 10
    pos = struct.unpack_from("<3f", d, p); p += 12
    rot = struct.unpack_from("<3f", d, p); p += 12
    scale, = struct.unpack_from("<f", d, p); p += 4
    bound = None
    if flags & MOD_HAS_BOUND:
        bound = (struct.unpack_from("<3f", d, p), struct.unpack_from("<3f", d, p + 12)); p += 24
    n, = struct.unpack_from("<I", d, p); p += 4
    name = d[p:p + n].decode("latin1"); p += n
    return dict(flags=flags, adt=adt, id=sid, pos=pos, rot=rot, scale=scale, bound=bound, name=name), p


def write_spawn(s):
    out = struct.pack("<IHI", s["flags"], s["adt"], s["id"]) + struct.pack("<3f", *s["pos"]) + struct.pack("<3f", *s["rot"])
    out += struct.pack("<f", s["scale"])
    if s["flags"] & MOD_HAS_BOUND:
        out += struct.pack("<3f", *s["bound"][0]) + struct.pack("<3f", *s["bound"][1])
    nb = s["name"].encode("latin1")
    return out + struct.pack("<I", len(nb)) + nb


def read_vmtile(path):
    d = open(path, "rb").read()
    assert d[:8] == MAGIC
    n, = struct.unpack_from("<I", d, 8)
    p, spawns = 12, []
    for _ in range(n):
        s, p = read_spawn(d, p)
        s["ref"], = struct.unpack_from("<I", d, p); p += 4
        spawns.append(s)
    assert p == len(d), (p, len(d))
    return spawns


def write_vmtile(path, spawns):
    out = MAGIC + struct.pack("<I", len(spawns))
    for s in spawns:
        out += write_spawn(s) + struct.pack("<I", s["ref"])
    open(path, "wb").write(out)


if __name__ == "__main__":
    import sys
    for f in sys.argv[1:]:
        if f.endswith(".vmtree"):
            tiled, (lo, hi, tree, objs), rest = read_vmtree(f)
            print(f, "tiled", tiled, "bounds", [round(v, 2) for v in lo + hi], "tree", tree, "objects", objs, "rest", len(rest))
        else:
            for s in read_vmtile(f):
                print(f, {k: (tuple(round(x, 3) for x in v) if isinstance(v, tuple) and v and isinstance(v[0], float) else v)
                          for k, v in s.items()})
