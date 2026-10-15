"""Build the client terrain for map 1616, The Eye of Eternity arena.

    python eoe_terrain.py <out-dir>

writes

    <out>/World/Maps/EyeOfEternityArena/EyeOfEternityArena.wdt / .wdl
    <out>/World/Maps/EyeOfEternityArena/EyeOfEternityArena_<x>_<y>.adt   (25 tiles)
    <out>/md5translate-block.txt      the minimap lines, for the art base's md5translate.trs
    <out>/manifest.json               md5 of every file written

from the highest-priority client copy of the Eye's own map (616, NexusRaid).

The Eye is 25 ADTs of holes and one WMO, nexus_raid_floating_platform.wmo. Its
placement carries MODF flag 0x1 (destroyable): on map 616 the floor is the
gameobject 193070 that Malygos breaks in phase three, so vmap4extractor skips the
placement ("destructible wmo, do not dump") and the server has no static floor
and no navmesh there at all. The clone clears the flag, so the floor is ordinary
static geometry for the client, the vmaps and the mmaps alike.

It also places two copies of Coldarra's nexus_floating_platform_01_small.wmo off
the Eye's diagonals (layout.py), the pads the two teams start on. Every new
placement is referenced from the MCRF list of each terrain chunk its extents box
covers, which is how Blizzard references the platform (chunks 5..12 x 5..13 of
tile 29_30): a placement no chunk references is never drawn and never collided
with by the client. The vmap extractor reads MODF directly and does not care.

Everything else is copied byte for byte, area ids (4500) included; the rewrite is
verified against the source before anything is written.
"""

import hashlib
import json
import os
import struct
import sys

sys.path.insert(0, r"C:\Projects\Gamedev\wow\tools\mpqpy")
from mpqread import MPQ  # noqa: E402

import layout as L  # noqa: E402

DATA = r"C:\Projects\Gamedev\wow\clients\centurion\Data"
TILE = 533.3333
CHUNK = TILE / 16.0
SRC_TILES = [(x, y) for y in range(28, 33) for x in range(27, 32)]   # <Map>_<x>_<y>.adt

# MCNK header fields that are offsets from the start of the MCNK chunk.
MCNK_OFFSET_FIELDS = (0x14, 0x18, 0x1C, 0x20, 0x24, 0x2C, 0x58, 0x60, 0x74, 0x78)


# ---------------------------------------------------------------- archives
def _rank(path):
    name = os.path.basename(path)[:-4].lower()
    base = ["common", "common-2", "expansion", "lichking", "locale-enus", "expansion-locale-enus",
            "lichking-locale-enus", "patch", "patch-enus"]
    if name in base:
        return (0, base.index(name))
    c = name[-1].upper()
    return (1, (10 + int(c)) if c.isdigit() else (20 + ord(c) - 64))


_ARCHIVES = None


def archives():
    global _ARCHIVES
    if _ARCHIVES is None:
        paths = [os.path.join(DATA, f) for f in os.listdir(DATA) if f.lower().endswith(".mpq")]
        enus = os.path.join(DATA, "enUS")
        paths += [os.path.join(enus, f) for f in os.listdir(enus)
                  if f.lower().endswith(".mpq") and "speech" not in f.lower() and "backup" not in f.lower()]
        _ARCHIVES = [(os.path.basename(p), MPQ(p)) for p in sorted(paths, key=_rank)]
    return _ARCHIVES


def resolve(rel):
    """The copy the client actually loads: the last archive in load order holding it."""
    hit = None
    for name, mpq in archives():
        if mpq.find(rel) is not None:
            hit = (name, mpq)
    if not hit:
        raise SystemExit("not in any archive: " + rel)
    return hit[0], hit[1].extract(rel)


# ------------------------------------------------------------------- chunks
def read_chunks(blob):
    out, pos = [], 0
    while pos + 8 <= len(blob):
        magic = blob[pos:pos + 4][::-1].decode("latin1")
        size = struct.unpack_from("<I", blob, pos + 4)[0]
        out.append([magic, bytearray(blob[pos + 8:pos + 8 + size])])
        pos += 8 + size
    if pos != len(blob):
        raise ValueError("trailing bytes after the last chunk")
    return out


def write_chunks(chunks):
    out = bytearray()
    for magic, data in chunks:
        out += magic[::-1].encode("latin1") + struct.pack("<I", len(data)) + data
    return bytes(out)


# ------------------------------------------------------------------- WMO box
def wmo_bbox(rel):
    _arc, blob = resolve(rel)
    for magic, data in read_chunks(blob):
        if magic == "MOHD":
            return struct.unpack_from("<6f", data, 36)
    raise SystemExit("no MOHD in " + rel)


def placement_extents(adt_pos, bbox):
    """MODF extents box for an unrotated WMO (see layout.py for the frame)."""
    px, py, pz = adt_pos
    x0, y0, z0, x1, y1, z1 = bbox
    return (px + y0, py + z0, pz + x0, px + y1, py + z1, pz + x1)


# --------------------------------------------------------------- the rewrite
def pad_placements():
    bbox = wmo_bbox(L.PAD_WMO)
    out = []
    for (x, y), uid in zip((L.ALLIANCE_PAD, L.HORDE_PAD), L.PAD_UNIQUE_IDS):
        pos = L.world_to_adt(x, y, L.PAD_ORIGIN_Z)
        out.append({"uid": uid, "pos": pos, "ext": placement_extents(pos, bbox)})
    return out


def tile_rect(tx, ty):
    return tx * TILE, ty * TILE, (tx + 1) * TILE, (ty + 1) * TILE


def chunks_covered(tx, ty, ext):
    """MCNK indices (iy * 16 + ix) of tile (tx, ty) whose square meets the box."""
    x0, _y0, z0, x1, _y1, z1 = ext
    ax, az, bx, bz = tile_rect(tx, ty)
    if x1 < ax or x0 > bx or z1 < az or z0 > bz:
        return []
    ix0 = max(0, int((x0 - ax) // CHUNK)); ix1 = min(15, int((x1 - ax) // CHUNK))
    iz0 = max(0, int((z0 - az) // CHUNK)); iz1 = min(15, int((z1 - az) // CHUNK))
    return [iz * 16 + ix for iz in range(iz0, iz1 + 1) for ix in range(ix0, ix1 + 1)]


def add_mapobj_refs(mcnk, refs):
    """Append MODF indices to an MCNK's MCRF list, shifting everything after it."""
    if not refs:
        return mcnk
    data = bytearray(mcnk)
    ofs_refs = struct.unpack_from("<I", data, 0x20)[0]
    # offsets in the header are from the MCNK chunk start, i.e. 8 bytes before data
    pos = ofs_refs - 8
    if data[pos:pos + 4][::-1] != b"MCRF":
        raise ValueError("MCRF not where the header says")
    rsize = struct.unpack_from("<I", data, pos + 4)[0]
    insert_at = pos + 8 + rsize          # end of the existing refs, data-relative
    extra = struct.pack("<%dI" % len(refs), *refs)
    shift_from = insert_at + 8           # the same point, chunk-start relative
    for f in MCNK_OFFSET_FIELDS:
        v = struct.unpack_from("<I", data, f)[0]
        if v and v >= shift_from:
            struct.pack_into("<I", data, f, v + len(extra))
    struct.pack_into("<I", data, 0x38, struct.unpack_from("<I", data, 0x38)[0] + len(refs))
    struct.pack_into("<I", data, pos + 4, rsize + len(extra))
    data[insert_at:insert_at] = extra
    return data


def rebuild_adt(src, tx, ty, pads):
    chunks = read_chunks(src)
    names = [c[0] for c in chunks]
    get = lambda m: chunks[names.index(m)][1]  # noqa: E731

    mwmo, mwid, modf = get("MWMO"), get("MWID"), get("MODF")
    n_modf = len(modf) // 64
    cleared = 0
    for i in range(n_modf):
        fl = struct.unpack_from("<H", modf, i * 64 + 56)[0]
        if fl & 1:
            struct.pack_into("<H", modf, i * 64 + 56, fl & ~1)
            cleared += 1

    mine = [p for p in pads if chunks_covered(tx, ty, p["ext"])]
    refs_by_chunk = {}
    if mine:
        # a blank MWMO is a single NUL; the first real name replaces it
        if bytes(mwmo) == b"\0" and not mwid:
            mwmo[:] = b""
        name_off = len(mwmo)
        mwmo += L.PAD_WMO.lower().encode("latin1") + b"\0"
        name_id = len(mwid) // 4
        mwid += struct.pack("<I", name_off)
        for p in mine:
            index = len(modf) // 64
            modf += struct.pack("<II3f3f6fHHHH", name_id, p["uid"], *p["pos"], 0.0, 0.0, 0.0, *p["ext"], 0, 0, 0, 0)
            for c in chunks_covered(tx, ty, p["ext"]):
                refs_by_chunk.setdefault(c, []).append(index)

    k = 0
    for c in chunks:
        if c[0] == "MCNK":
            if k in refs_by_chunk:
                c[1] = add_mapobj_refs(c[1], refs_by_chunk[k])
            k += 1
    if k != 256:
        raise ValueError("expected 256 MCNK, found %d" % k)

    # offsets: MHDR's are from the start of its data, MCIN's are absolute
    blob = bytearray(write_chunks(chunks))
    starts, pos = {}, 0
    mcnk_at = []
    for magic, data in chunks:
        if magic == "MCNK":
            mcnk_at.append((pos, 8 + len(data)))
        else:
            starts.setdefault(magic, pos)
        pos += 8 + len(data)
    mhdr_data = starts["MHDR"] + 8
    order = ["MCIN", "MTEX", "MMDX", "MMID", "MWMO", "MWID", "MDDF", "MODF", "MFBO", "MH2O", "MTXF"]
    for i, magic in enumerate(order):
        old = struct.unpack_from("<I", blob, mhdr_data + 4 + i * 4)[0]
        if magic in starts:
            struct.pack_into("<I", blob, mhdr_data + 4 + i * 4, starts[magic] - mhdr_data)
        elif old:
            raise ValueError("MHDR names a %s the file does not have" % magic)
    mcin = starts["MCIN"] + 8
    for i, (ofs, size) in enumerate(mcnk_at):
        struct.pack_into("<II", blob, mcin + i * 16, ofs, size)
    return bytes(blob), cleared, [p["uid"] for p in mine], refs_by_chunk


# --------------------------------------------------------------- verification
def parse_adt(blob):
    chunks = read_chunks(blob)
    mhdr = [d for m, d in chunks if m == "MHDR"][0]
    mcnk = [d for m, d in chunks if m == "MCNK"]
    return chunks, mhdr, mcnk


def mcrf(mcnk):
    ofs = struct.unpack_from("<I", mcnk, 0x20)[0] - 8
    size = struct.unpack_from("<I", mcnk, ofs + 4)[0]
    nd = struct.unpack_from("<I", mcnk, 0x10)[0]
    nm = struct.unpack_from("<I", mcnk, 0x38)[0]
    refs = struct.unpack_from("<%dI" % (size // 4), mcnk, ofs + 8)
    if len(refs) != nd + nm:
        raise ValueError("MCRF holds %d refs, header says %d + %d" % (len(refs), nd, nm))
    return list(refs[:nd]), list(refs[nd:])


def subchunks(mcnk):
    """Every sub-chunk the header points at, as bytes, keyed by header field."""
    out = {}
    for f in MCNK_OFFSET_FIELDS:
        v = struct.unpack_from("<I", mcnk, f)[0]
        if v and f != 0x20:
            out[f] = bytes(mcnk[v - 8:v - 8 + 8])   # magic + size of what it points at
    return out


def verify(src, new, tx, ty, pads, expect_refs):
    s_chunks, _, s_mcnk = parse_adt(src)
    n_chunks, n_mhdr, n_mcnk = parse_adt(new)
    if [m for m, _ in s_chunks] != [m for m, _ in n_chunks]:
        raise ValueError("chunk order changed")
    for (sm, sd), (nm, nd) in zip(s_chunks, n_chunks):
        if sm in ("MCNK", "MHDR", "MCIN", "MWMO", "MWID", "MODF"):
            continue
        if sd != nd:
            raise ValueError("%s changed" % sm)
    # MHDR / MCIN point where they claim
    base = 12 + 8
    order = ["MCIN", "MTEX", "MMDX", "MMID", "MWMO", "MWID", "MDDF", "MODF", "MFBO", "MH2O", "MTXF"]
    for i, magic in enumerate(order):
        off = struct.unpack_from("<I", n_mhdr, 4 + i * 4)[0]
        if off and new[base + off:base + off + 4][::-1].decode("latin1") != magic:
            raise ValueError("MHDR %s offset wrong" % magic)
    mcin = [d for m, d in n_chunks if m == "MCIN"][0]
    for i in range(256):
        ofs, size = struct.unpack_from("<II", mcin, i * 16)
        if new[ofs:ofs + 4] != b"KNCM" or struct.unpack_from("<I", new, ofs + 4)[0] + 8 != size:
            raise ValueError("MCIN %d wrong" % i)
    # MCNKs: identical apart from the refs; every sub-chunk still where the header says
    for i, (a, b) in enumerate(zip(s_mcnk, n_mcnk)):
        da, ma = mcrf(a)
        db, mb = mcrf(b)
        added = expect_refs.get(i, [])
        if db != da or mb != ma + added:
            raise ValueError("MCNK %d refs %s -> %s, expected +%s" % (i, ma, mb, added))
        if subchunks(a).keys() != subchunks(b).keys():
            raise ValueError("MCNK %d sub-chunk set changed" % i)
        for f, head in subchunks(b).items():
            if head[:4] != subchunks(a)[f][:4]:
                raise ValueError("MCNK %d field %#x points at %r" % (i, f, head[:4]))
        if a[:0x10] != b[:0x10] or a[0x34:0x38] != b[0x34:0x38] or a[0x3C:0x58] != b[0x3C:0x58] or a[0x68:0x74] != b[0x68:0x74]:
            raise ValueError("MCNK %d header changed" % i)
        if not added and a != b:
            raise ValueError("MCNK %d changed without new refs" % i)
    # MODF: originals with flag 1 cleared, pads appended
    s_modf = [d for m, d in s_chunks if m == "MODF"][0]
    n_modf = [d for m, d in n_chunks if m == "MODF"][0]
    for i in range(len(s_modf) // 64):
        a, b = bytearray(s_modf[i * 64:i * 64 + 64]), n_modf[i * 64:i * 64 + 64]
        struct.pack_into("<H", a, 56, struct.unpack_from("<H", a, 56)[0] & ~1)
        if bytes(a) != bytes(b):
            raise ValueError("MODF %d changed beyond its flag" % i)
    return len(n_modf) // 64


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else r"C:\Projects\Gamedev\wow\data\patch-staging\EyeOfEternityArena"
    rel_dir = "World\\Maps\\%s" % L.DIRECTORY
    dest = os.path.join(out, "World", "Maps", L.DIRECTORY)
    os.makedirs(dest, exist_ok=True)
    pads = pad_placements()
    for p in pads:
        print("pad uid %d  adt pos %s  ext %s" % (p["uid"], [round(v, 3) for v in p["pos"]], [round(v, 2) for v in p["ext"]]))

    manifest = {}

    def emit(name, data):
        path = os.path.join(dest, name)
        with open(path, "wb") as fh:
            fh.write(data)
        manifest["%s\\%s" % (rel_dir, name)] = hashlib.md5(data).hexdigest()

    for ext in ("wdt", "wdl"):
        arc, blob = resolve("World\\Maps\\%s\\%s.%s" % (L.SOURCE_DIR, L.SOURCE_DIR, ext))
        emit("%s.%s" % (L.DIRECTORY, ext), blob)
        print("%s from %s, %d bytes, copied" % (ext, arc, len(blob)))

    placed = {p["uid"]: 0 for p in pads}
    for (tx, ty) in SRC_TILES:
        arc, src = resolve("World\\Maps\\%s\\%s_%d_%d.adt" % (L.SOURCE_DIR, L.SOURCE_DIR, tx, ty))
        new, cleared, uids, added = rebuild_adt(src, tx, ty, pads)
        # what the refs must be, worked out independently of rebuild_adt
        n_src_modf = len([d for m, d in read_chunks(src) if m == "MODF"][0]) // 64
        expect = {}
        for j, p in enumerate([p for p in pads if chunks_covered(tx, ty, p["ext"])]):
            placed[p["uid"]] += 1
            for c in chunks_covered(tx, ty, p["ext"]):
                expect.setdefault(c, []).append(n_src_modf + j)
        if expect != added:
            raise SystemExit("tile %d_%d: refs written %s, expected %s" % (tx, ty, added, expect))
        n_modf = verify(src, new, tx, ty, pads, expect)
        emit("%s_%d_%d.adt" % (L.DIRECTORY, tx, ty), new)
        if cleared or uids:
            print("tile %d_%d from %s: %d destroyable flag(s) cleared, %d placements, pads %s in chunks %s"
                  % (tx, ty, arc, cleared, n_modf, uids, sorted(expect)))
    for uid, n in placed.items():
        if not n:
            raise SystemExit("pad %d landed in no tile" % uid)

    # minimap: the source's own lines under the new directory (art is shared by md5)
    _arc, trs = resolve("textures\\Minimap\\md5translate.trs")
    lines, cur = [], None
    for ln in trs.decode("latin1").splitlines():
        if ln.startswith("dir:"):
            cur = ln[4:].strip()
            continue
        if cur and cur.lower() == L.SOURCE_DIR.lower() and "\t" in ln:
            src, dst = ln.split("\t", 1)
            lines.append("%s\\%s\t%s" % (L.DIRECTORY, src.split("\\", 1)[1], dst))
    with open(os.path.join(out, "md5translate-block.txt"), "w", newline="\r\n") as fh:
        fh.write("dir: %s\n" % L.DIRECTORY)
        fh.write("\n".join(lines) + "\n")
    print("md5translate block: %d lines" % len(lines))

    with open(os.path.join(out, "manifest.json"), "w") as fh:
        json.dump(manifest, fh, indent=1, sort_keys=True)
    print("wrote %d files under %s" % (len(manifest), dest))


if __name__ == "__main__":
    main()
