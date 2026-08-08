"""Give a custom gameobject display SERVER-side collision + line of sight.

The client collides with whatever model patch-Z ships, but the SERVER only
collides/LoS-checks a gameobject if its displayId is in vmaps/
GameObjectModels.dtree with a matching .vmo model - both produced by the
vmap assembler at extraction time. A display minted after extraction is
invisible to the server: players walk "into" it fine (client blocks them)
but spells trace right through. This tool closes that gap without any
re-extraction, in the same spirit as clone_map_data.sh.

It reads the collision mesh straight out of a wmo2m2.py-built M2's bounding
arrays (nBoundingTriangles @216 counts INDICES, nBoundingVertices @224 -
v264 header, same offsets add_m2_attachment.py asserts on), then:

  * writes vmaps/<name>.vmo - VMAP_4.8, one GroupModel, no liquid;
  * rewrites GameObjectModels.dtree with the given displayId->name entries
    replaced-or-appended (the loader's emplace keeps the FIRST entry for a
    duplicate id, so blind appending would leave stale rows in charge).

The BIH trees are single-leaf: tree = [(3<<30)|0, N, 0], objects = 0..N-1,
exactly the shape BIH::init_empty() uses. Every ray that enters the model's
AABB tests all N triangles - the dynamic tree culls by AABB first, so only
rays actually crossing the building pay. If profiling ever shows this hot,
port BIH::buildHierarchy; the file format needs no change.

Usage:
  python build_go_vmo.py <model.m2> <name> <dtree> <outdir> <displayId> [<displayId>...]

  <name>   the model name to register, e.g. WgWorkshopBGCol.m2 - the vmo is
           written as <outdir>/<name>.vmo
  <dtree>  path to an existing GameObjectModels.dtree (rewritten in place)

isWmo is written as 1: converted buildings are WMO-class blockers, and the
flag also keeps area-info queries treating them as structures.
"""
import struct
import sys

VMAP_MAGIC = b"VMAP_4.8"

def read_collision(m2_path):
    with open(m2_path, "rb") as f:
        data = f.read()
    assert data[:4] == b"MD20" and struct.unpack_from("<I", data, 4)[0] == 264, m2_path
    n_idx, ofs_idx = struct.unpack_from("<2I", data, 216)
    n_vert, ofs_vert = struct.unpack_from("<2I", data, 224)
    assert n_idx % 3 == 0, n_idx
    verts = [struct.unpack_from("<3f", data, ofs_vert + i * 12) for i in range(n_vert)]
    idx = struct.unpack_from("<%dH" % n_idx, data, ofs_idx)
    tris = [(idx[i], idx[i + 1], idx[i + 2]) for i in range(0, n_idx, 3)]
    return verts, tris

def single_leaf_bih(bounds_lo, bounds_hi, count):
    out = struct.pack("<3f", *bounds_lo) + struct.pack("<3f", *bounds_hi)
    tree = [(3 << 30) | 0, count, 0]
    out += struct.pack("<I", len(tree)) + struct.pack("<%dI" % len(tree), *tree)
    out += struct.pack("<I", count) + struct.pack("<%dI" % count, *range(count))
    return out

def build_vmo(verts, tris):
    lo = [min(v[i] for v in verts) for i in range(3)]
    hi = [max(v[i] for v in verts) for i in range(3)]

    group = struct.pack("<6f", *lo, *hi)              # AABox
    group += struct.pack("<2I", 0, 0)                 # mogpFlags, groupWMOID
    group += b"VERT" + struct.pack("<2I", 4 + len(verts) * 12, len(verts))
    group += b"".join(struct.pack("<3f", *v) for v in verts)
    group += b"TRIM" + struct.pack("<2I", 4 + len(tris) * 12, len(tris))
    group += b"".join(struct.pack("<3I", *t) for t in tris)
    group += b"MBIH" + single_leaf_bih(lo, hi, len(tris))
    group += b"LIQU" + struct.pack("<I", 0)           # no liquid

    vmo = VMAP_MAGIC
    vmo += b"WMOD" + struct.pack("<2I", 8, 0)         # chunkSize, RootWMOID
    vmo += b"GMOD" + struct.pack("<I", 1) + group
    vmo += b"GBIH" + single_leaf_bih(lo, hi, 1)       # group tree: 1 object
    return vmo, lo, hi

def rewrite_dtree(dtree_path, entries):
    """entries: list of (displayId, isWmo, name, lo, hi)"""
    with open(dtree_path, "rb") as f:
        data = f.read()
    assert data[:8] == VMAP_MAGIC, dtree_path
    ours = {e[0] for e in entries}
    kept, pos, existing = [], 8, 0
    while pos < len(data):
        display_id, is_wmo, name_len = struct.unpack_from("<IBI", data, pos)
        rec_len = 9 + name_len + 24
        if display_id not in ours:
            kept.append(data[pos:pos + rec_len])
        pos += rec_len
        existing += 1
    out = VMAP_MAGIC + b"".join(kept)
    for display_id, is_wmo, name, lo, hi in entries:
        nb = name.encode()
        out += struct.pack("<IBI", display_id, is_wmo, len(nb)) + nb
        out += struct.pack("<6f", *lo, *hi)
    with open(dtree_path, "wb") as f:
        f.write(out)
    return existing, len(kept)

def main():
    m2_path, name, dtree, outdir = sys.argv[1:5]
    display_ids = [int(a) for a in sys.argv[5:]]
    assert display_ids, "need at least one displayId"

    verts, tris = read_collision(m2_path)
    vmo, lo, hi = build_vmo(verts, tris)
    out_path = f"{outdir}/{name}.vmo"
    with open(out_path, "wb") as f:
        f.write(vmo)
    print(f"{out_path}: {len(verts)} verts, {len(tris)} tris, "
          f"bounds ({lo[0]:.1f},{lo[1]:.1f},{lo[2]:.1f})..({hi[0]:.1f},{hi[1]:.1f},{hi[2]:.1f})")

    entries = [(d, 1, name, lo, hi) for d in display_ids]
    existing, kept = rewrite_dtree(dtree, entries)
    print(f"{dtree}: {existing} entries read, {kept} kept, {len(entries)} of ours written")

main()
