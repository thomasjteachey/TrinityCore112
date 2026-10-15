"""Section offsets of a TrinityCore .mmtile (20-byte MmapTileHeader + Detour tile, 64-bit poly refs)."""
import struct, sys
def a4(x): return (x + 3) & ~3
def sections(d):
    h = 20
    f = struct.unpack_from("<4i i i i 7i", d, h)  # magic,version,x,y,layer,userId,polyCount,vertCount,maxLink,detailMesh,detailVert,detailTri,bvNode
    names = "magic version x y layer userId polyCount vertCount maxLinkCount detailMeshCount detailVertCount detailTriCount bvNodeCount".split()
    m = dict(zip(names, f))
    m["offMeshConCount"], = struct.unpack_from("<i", d, h + 13 * 4)
    hdr_size = a4(100)  # dtMeshHeader: 15 ints + 3 floats + 6 floats + 1 float = 100 bytes
    sizes = [("verts", 12 * m["vertCount"]), ("polys", 32 * m["polyCount"]), ("links", 16 * m["maxLinkCount"]),
             ("detailMeshes", 12 * m["detailMeshCount"]), ("detailVerts", 12 * m["detailVertCount"]),
             ("detailTris", 4 * m["detailTriCount"]), ("bvtree", 16 * m["bvNodeCount"]), ("offMesh", 36 * m["offMeshConCount"])]
    out, p = [], h + hdr_size
    for n, s in sizes:
        out.append((n, p, p + a4(s))); p += a4(s)
    return m, out, p
if __name__ == "__main__":
    a = open(sys.argv[1], "rb").read()
    m, secs, end = sections(a)
    print({k: m[k] for k in ("polyCount", "vertCount", "bvNodeCount", "offMeshConCount")}, "computed end", end, "file", len(a))
    if len(sys.argv) > 2:
        b = open(sys.argv[2], "rb").read()
        diff = [i for i in range(min(len(a), len(b))) if a[i] != b[i]]
        for n, s, e in secs:
            k = sum(1 for i in diff if s <= i < e)
            print("  %-13s %6d..%6d  differing bytes %d" % (n, s, e, k))
