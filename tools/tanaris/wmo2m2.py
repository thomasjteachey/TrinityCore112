"""Convert a WMO building into a static M2, so creatures can wear it.

Creature displays can only reference M2 models -- the client's creature
pipeline will not load a WMO -- so a "creature twin" of a WMO building (the
GO-for-collision + same-model-creature-for-targeting pattern) needs the
building converted. This does that conversion directly, no Blender involved.

What carries over: all render geometry, per-material textures (the M2 simply
references the same BLP paths the WMO does, so no texture work), two-sided /
unlit material flags, blend modes, and the collision mesh (built from every
triangle, including the WMO's invisible collision-only faces).

What is knowingly dropped: doodad-set decorations (crates, lamps -- separate
models placed inside the WMO), baked vertex lighting (MOCV), portals and
liquids. For a building viewed from outside none of these read.

Every convention the client is fussy about -- bone/sequence layout, geoset-0
submeshes, batch flags, lookup-table shapes -- is copied verbatim from a
known-good static doodad (OrcTent01.m2), because those were dumped from a
model the client demonstrably renders. The one deliberate deviation: the
donor's transparency keyframe value is written as 32767 (fully opaque), the
value virtually every static doodad uses.

    python wmo2m2.py [--wmo <path-in-mpq>] [--name WgWorkshopBG]

Output lands in wow/data/patch-staging/TanarisBG-models/ as
World\\TanarisBG\\<name>.m2 + <name>00.skin, plus a wireframe preview PNG.
This is NON-LOCALE content: it packs into patch-Z / patch-Y, unlike the
Interface/ art which must ride a locale patch.
"""
import os
import struct
import sys

sys.path.insert(0, r"C:\Projects\Gamedev\wow\tools\mpqpy")
from mpqread import MPQ

DATA = r"C:\Projects\Gamedev\wow\clients\centurion\Data"
OUT_ROOT = r"C:\Projects\Gamedev\wow\data\patch-staging\TanarisBG-models"

ORDER = ["common.MPQ", "common-2.MPQ", "expansion.MPQ", "lichking.MPQ",
         "patch.MPQ", "patch-2.MPQ", "patch-3.MPQ", "patch-Y.MPQ", "patch-Z.MPQ"]


def load_archives():
    out = []
    for n in ORDER:
        p = os.path.join(DATA, n)
        if os.path.exists(p):
            try:
                out.append(MPQ(p))
            except Exception:
                pass
    return out


def fetch(arcs, rel):
    hit = None
    for m in arcs:
        if m.find(rel) is not None:
            hit = m
    return hit.extract(rel) if hit else None


def chunks(blob, base=0, end=None):
    out = {}
    pos = base
    stop = len(blob) if end is None else end
    while pos + 8 <= stop:
        magic = blob[pos:pos + 4][::-1].decode("ascii", "replace")
        size = struct.unpack_from("<I", blob, pos + 4)[0]
        out[magic] = (pos + 8, size)
        pos += 8 + size
    return out


# --------------------------------------------------------------- WMO reading
def read_wmo(arcs, root_rel):
    root = fetch(arcs, root_rel)
    if root is None:
        raise SystemExit("WMO not found: %s" % root_rel)
    ch = chunks(root)

    o, s = ch["MOHD"]
    nTex, nGroups = struct.unpack_from("<2I", root, o)[0:2]

    motx_o, motx_s = ch["MOTX"]
    motx = root[motx_o:motx_o + motx_s]

    def tex_at(ofs):
        return motx[ofs:motx.index(b"\x00", ofs)].decode("ascii", "replace")

    o, s = ch["MOMT"]
    materials = []
    for i in range(s // 64):
        flags, shader, blend, tex1 = struct.unpack_from("<4I", root, o + i * 64)
        materials.append({"flags": flags, "blend": blend, "tex": tex_at(tex1)})

    stem = root_rel[:-4]
    verts, normals, uvs = [], [], []
    tris = []                      # (i0, i1, i2, materialId)  materialId 0xFF = collision-only
    for g in range(nGroups):
        gb = fetch(arcs, "%s_%03d.wmo" % (stem, g))
        if gb is None:
            raise SystemExit("missing group file %d" % g)
        gch = chunks(gb)
        mogp_o, mogp_s = gch["MOGP"]
        sub = chunks(gb, mogp_o + 68, mogp_o + mogp_s)

        base = len(verts)
        vo, vs = sub["MOVT"]
        for k in range(vs // 12):
            verts.append(struct.unpack_from("<3f", gb, vo + k * 12))
        no, ns = sub["MONR"]
        for k in range(ns // 12):
            normals.append(struct.unpack_from("<3f", gb, no + k * 12))
        to, ts = sub["MOTV"]
        for k in range(ts // 8):
            uvs.append(struct.unpack_from("<2f", gb, to + k * 8))

        io, isz = sub["MOVI"]
        po, ps = sub["MOPY"]
        ntri = isz // 6
        assert ps // 2 == ntri, "MOPY/MOVI disagree in group %d" % g
        for t in range(ntri):
            i0, i1, i2 = struct.unpack_from("<3H", gb, io + t * 6)
            mat = gb[po + t * 2 + 1]
            tris.append((base + i0, base + i1, base + i2, mat))

    assert len(normals) == len(verts) and len(uvs) == len(verts)
    return materials, verts, normals, uvs, tris


# --------------------------------------------------------------- M2 assembly
class Blocks:
    """Sequential block allocator: append data, get its offset later."""

    def __init__(self, base):
        self.base = base
        self.chunks = []
        self.size = 0

    def add(self, data):
        if not data:
            return 0
        ofs = self.base + self.size
        pad = (-len(data)) % 16
        self.chunks.append(data + b"\x00" * pad)
        self.size += len(data) + pad
        return ofs

    def blob(self):
        return b"".join(self.chunks)


def norm_of(a, b, c):
    ux, uy, uz = b[0] - a[0], b[1] - a[1], b[2] - a[2]
    vx, vy, vz = c[0] - a[0], c[1] - a[1], c[2] - a[2]
    nx, ny, nz = uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx
    l = (nx * nx + ny * ny + nz * nz) ** 0.5 or 1.0
    return (nx / l, ny / l, nz / l)


def bbox_of(pts):
    xs = [p[0] for p in pts]; ys = [p[1] for p in pts]; zs = [p[2] for p in pts]
    mn = (min(xs), min(ys), min(zs)); mx = (max(xs), max(ys), max(zs))
    cx = [(a + b) / 2 for a, b in zip(mn, mx)]
    r = max(((p[0] - cx[0]) ** 2 + (p[1] - cx[1]) ** 2 + (p[2] - cx[2]) ** 2) ** 0.5 for p in pts)
    return mn, mx, cx, r


def convert(materials, verts, normals, uvs, tris, name):
    # ---- render geometry, re-bucketed per material so every submesh owns a
    # contiguous vertex and index range (the shape the skin format wants)
    by_mat = {}
    for i0, i1, i2, mat in tris:
        if mat != 0xFF:
            by_mat.setdefault(mat, []).append((i0, i1, i2))

    out_verts = []                # (pos, normal, uv)
    submeshes = []                # dicts
    out_indices = []
    used_mats = sorted(by_mat)
    for mat in used_mats:
        remap = {}
        v_start = len(out_verts)
        i_start = len(out_indices)
        for tri in by_mat[mat]:
            for gi in tri:
                if gi not in remap:
                    remap[gi] = len(out_verts)
                    out_verts.append((verts[gi], normals[gi], uvs[gi]))
                out_indices.append(remap[gi])
        pts = [out_verts[k][0] for k in range(v_start, len(out_verts))]
        _mn, _mx, center, radius = bbox_of(pts)
        submeshes.append({"mat": mat, "vstart": v_start, "vcount": len(out_verts) - v_start,
                          "istart": i_start, "icount": len(out_indices) - i_start,
                          "center": center, "radius": radius})

    if len(out_verts) > 0xFFFF:
        raise SystemExit("render vertices %d exceed u16" % len(out_verts))
    if len(out_indices) > 0xFFFF:
        raise SystemExit("render indices %d exceed u16" % len(out_indices))

    # ---- collision: every triangle, collision-only faces included
    cmap = {}
    cverts, cindices, cnormals = [], [], []
    for i0, i1, i2, _mat in tris:
        for gi in (i0, i1, i2):
            if gi not in cmap:
                cmap[gi] = len(cverts)
                cverts.append(verts[gi])
            cindices.append(cmap[gi])
        cnormals.append(norm_of(verts[i0], verts[i1], verts[i2]))
    if len(cverts) > 0xFFFF:
        raise SystemExit("collision vertices %d exceed u16" % len(cverts))

    # ---- material -> texture / renderflag tables
    tex_paths, tex_index = [], {}
    rf = []                        # (flags, blend) per used material
    mat_to_rf, mat_to_tex = {}, {}
    for n_, mat in enumerate(used_mats):
        m = materials[mat]
        path = m["tex"]
        if path not in tex_index:
            tex_index[path] = len(tex_paths)
            tex_paths.append(path)
        flags = 0
        if m["flags"] & 0x01:      # F_UNLIT
            flags |= 0x01
        if m["flags"] & 0x04:      # F_UNCULLED
            flags |= 0x04
        blend = m["blend"] if m["blend"] in (0, 1) else 2
        mat_to_rf[mat] = len(rf)
        mat_to_tex[mat] = tex_index[path]
        rf.append((flags, blend))

    render_pts = [v[0] for v in out_verts]
    vmn, vmx, _c, vrad = bbox_of(render_pts)
    cmn, cmx, _c2, crad = bbox_of(cverts)

    # ---------------------------------------------------------------- M2 file
    HDR = 304
    B = Blocks(HDR)
    name_b = name.encode("ascii") + b"\x00"
    ofs_name = B.add(name_b)

    # sequence: donor values (Stand, looping)
    seq = struct.pack("<HHIfIhHIII", 0, 0, 3333, 0.0, 0x20, 32767, 0, 0, 0, 150)
    seq += struct.pack("<6f", *(vmn + vmx)) + struct.pack("<f", vrad) + struct.pack("<hH", -1, 0)
    assert len(seq) == 64
    ofs_seq = B.add(seq)

    # one static bone, empty tracks (donor)
    track = struct.pack("<hh4I", 0, -1, 0, 0, 0, 0)
    bone = struct.pack("<iIhH", -1, 0, -1, 0) + struct.pack("<I", 0) + track * 3 + struct.pack("<3f", 0, 0, 0)
    assert len(bone) == 88
    ofs_bone = B.add(bone)

    ofs_keylk = B.add(struct.pack("<h", -1))

    vblob = bytearray()
    for pos, nrm, uv in out_verts:
        vblob += struct.pack("<3f4B4B3f2f2f", pos[0], pos[1], pos[2],
                             255, 0, 0, 0, 0, 0, 0, 0,
                             nrm[0], nrm[1], nrm[2], uv[0], uv[1], 0.0, 0.0)
    ofs_verts = B.add(bytes(vblob))

    # textures: type 0 (hardcoded path), flags 0x3 (wrap both) like the donor
    tex_entries = bytearray()
    tex_name_offsets = []
    for p in tex_paths:
        tex_name_offsets.append(B.add(p.encode("ascii") + b"\x00"))
    for i, p in enumerate(tex_paths):
        tex_entries += struct.pack("<4I", 0, 0x3, len(p) + 1, tex_name_offsets[i])
    ofs_tex = B.add(bytes(tex_entries))

    # transparency: one static track, opaque
    ofs_tts = B.add(struct.pack("<I", 0))
    ofs_tval = B.add(struct.pack("<h", 32767))
    ofs_transp = B.add(struct.pack("<hh4I", 0, -1, 1, ofs_tts, 1, ofs_tval))

    ofs_texrep = B.add(struct.pack("<h", -1))
    ofs_rf = B.add(b"".join(struct.pack("<HH", f, b) for f, b in rf))
    ofs_bonelk = B.add(struct.pack("<4h", 0, 0, 0, 0))
    ofs_texlk = B.add(struct.pack("<%dh" % len(tex_paths), *range(len(tex_paths))))
    ofs_texunits = B.add(struct.pack("<h", 0))
    ofs_translk = B.add(struct.pack("<h", 0))
    ofs_uvanimlk = B.add(struct.pack("<h", -1))

    ofs_btris = B.add(struct.pack("<%dH" % len(cindices), *cindices))
    ofs_bverts = B.add(b"".join(struct.pack("<3f", *v) for v in cverts))
    ofs_bnorms = B.add(b"".join(struct.pack("<3f", *n) for n in cnormals))

    hdr = struct.pack("<4sI", b"MD20", 264)
    hdr += struct.pack("<2I", len(name_b), ofs_name)
    # 0x10, not the donor's 0x80: the donor is only ever a doodad. Every stock
    # World-doodad model that Blizzard wired as a CREATURE display carries 0x8
    # or 0x10 here, and the creature renderer cubes on 0x80.
    hdr += struct.pack("<I", 0x10)
    hdr += struct.pack("<2I", 0, 0)                         # global sequences
    hdr += struct.pack("<2I", 1, ofs_seq)
    hdr += struct.pack("<2I", 0, 0)                         # animation lookup
    hdr += struct.pack("<2I", 1, ofs_bone)
    hdr += struct.pack("<2I", 1, ofs_keylk)
    hdr += struct.pack("<2I", len(out_verts), ofs_verts)
    hdr += struct.pack("<I", 1)                             # nViews
    hdr += struct.pack("<2I", 0, 0)                         # colors
    hdr += struct.pack("<2I", len(tex_paths), ofs_tex)
    hdr += struct.pack("<2I", 1, ofs_transp)
    hdr += struct.pack("<2I", 0, 0)                         # uv animations
    hdr += struct.pack("<2I", 1, ofs_texrep)
    hdr += struct.pack("<2I", len(rf), ofs_rf)
    hdr += struct.pack("<2I", 4, ofs_bonelk)
    hdr += struct.pack("<2I", len(tex_paths), ofs_texlk)
    hdr += struct.pack("<2I", 1, ofs_texunits)
    hdr += struct.pack("<2I", 1, ofs_translk)
    hdr += struct.pack("<2I", 1, ofs_uvanimlk)
    hdr += struct.pack("<6f", *(vmn + vmx)) + struct.pack("<f", vrad)
    hdr += struct.pack("<6f", *(cmn + cmx)) + struct.pack("<f", crad)
    hdr += struct.pack("<2I", len(cindices), ofs_btris)     # count is INDICES, donor-verified
    hdr += struct.pack("<2I", len(cverts), ofs_bverts)
    hdr += struct.pack("<2I", len(cnormals), ofs_bnorms)
    hdr += struct.pack("<2I", 0, 0) * 8                     # attach..particles
    assert len(hdr) == HDR, len(hdr)

    m2 = hdr + B.blob()

    # -------------------------------------------------------------- skin file
    SH = 48
    S = Blocks(SH)
    ofs_sv = S.add(struct.pack("<%dH" % len(out_verts), *range(len(out_verts))))
    ofs_si = S.add(struct.pack("<%dH" % len(out_indices), *out_indices))
    ofs_sb = S.add(b"\x00\x00\x00\x00" * len(out_verts))

    sub_blob = bytearray()
    for smx in submeshes:
        sub_blob += struct.pack("<10H", 0, 0, smx["vstart"], smx["vcount"],
                                smx["istart"], smx["icount"], 1, 0, 1, 0)
        sub_blob += struct.pack("<3f", *smx["center"])
        sub_blob += struct.pack("<3f", *smx["center"])
        sub_blob += struct.pack("<f", smx["radius"])
    ofs_sub = S.add(bytes(sub_blob))

    bat_blob = bytearray()
    for i, smx in enumerate(submeshes):
        bat_blob += struct.pack("<bb11H", 0x10, 0, 0, i, i, -1 & 0xFFFF,
                                mat_to_rf[smx["mat"]], 0, 1,
                                mat_to_tex[smx["mat"]], 0, 0, 0)
    ofs_bat = S.add(bytes(bat_blob))

    # boneCountMax 21, not the donor's 0: every stock skin paired with a
    # creature display says 21, and the creature renderer's bone-matrix setup
    # reads it -- 0 is another way to earn the checkered cube. Doodads don't care.
    skin = struct.pack("<4s10I", b"SKIN",
                       len(out_verts), ofs_sv, len(out_indices), ofs_si,
                       len(out_verts), ofs_sb, len(submeshes), ofs_sub,
                       len(submeshes), ofs_bat) + struct.pack("<I", 21)
    assert len(skin) == SH
    skin = skin + S.blob()
    # header was assembled before block offsets existed? no: Blocks(SH) already
    # accounted for the 48-byte header, and offsets were taken after adds.

    stats = {"verts": len(out_verts), "tris": len(out_indices) // 3,
             "submeshes": len(submeshes), "textures": len(tex_paths),
             "cverts": len(cverts), "ctris": len(cnormals),
             "bbox": (vmn, vmx)}
    return m2, skin, stats


def preview(verts_tris, path):
    """Crude wireframe (top + side) so a human can eyeball the geometry."""
    from PIL import Image, ImageDraw
    pts, tris = verts_tris
    W = H = 640
    img = Image.new("RGB", (W * 2, H), (18, 18, 22))
    d = ImageDraw.Draw(img)

    def project(axes, box):
        (x0, y0, x1, y1) = box
        a, b = axes
        av = [p[a] for p in pts]; bv = [p[b] for p in pts]
        amn, amx = min(av), max(av); bmn, bmx = min(bv), max(bv)
        sc = min((x1 - x0) / max(amx - amn, 1e-6), (y1 - y0) / max(bmx - bmn, 1e-6))
        return lambda p: (x0 + (p[a] - amn) * sc, y1 - (p[b] - bmn) * sc)

    for axes, box in (((0, 1), (20, 20, W - 20, H - 20)),
                      ((0, 2), (W + 20, 20, 2 * W - 20, H - 20))):
        pr = project(axes, box)
        for i0, i1, i2 in tris:
            a, b, c = pr(pts[i0]), pr(pts[i1]), pr(pts[i2])
            d.line([a, b, c, a], fill=(120, 160, 200), width=1)
    d.text((24, 4), "top (XY)", fill=(220, 220, 220))
    d.text((W + 24, 4), "side (XZ)", fill=(220, 220, 220))
    img.save(path)


def main():
    args = sys.argv[1:]
    wmo = r"world\wmo\Northrend\Wintergrasp\WG_Siege01.wmo"
    name = "WgWorkshopBG"
    i = 0
    while i < len(args):
        if args[i] == "--wmo":
            i += 1
            wmo = args[i]
        elif args[i] == "--name":
            i += 1
            name = args[i]
        i += 1

    arcs = load_archives()
    materials, verts, normals, uvs, tris = read_wmo(arcs, wmo)
    print("WMO: %d verts, %d tris, %d materials" % (len(verts), len(tris), len(materials)))

    m2, skin, st = convert(materials, verts, normals, uvs, tris, name)

    dest = os.path.join(OUT_ROOT, "World", "TanarisBG")
    os.makedirs(dest, exist_ok=True)
    m2_path = os.path.join(dest, "%s.m2" % name)
    with open(m2_path, "wb") as f:
        f.write(m2)
    with open(os.path.join(dest, "%s00.skin" % name), "wb") as f:
        f.write(skin)

    mn, mx = st["bbox"]
    print("M2: %d verts, %d tris in %d submeshes, %d textures" %
          (st["verts"], st["tris"], st["submeshes"], st["textures"]))
    print("collision: %d verts, %d tris" % (st["cverts"], st["ctris"]))
    print("bbox: %.1f x %.1f x %.1f yd" % (mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2]))
    print("wrote %s (%d KB) + skin (%d KB)" % (m2_path, len(m2) // 1024, len(skin) // 1024))

    prev = os.path.join(OUT_ROOT, "%s_preview.png" % name)
    tri_list = [(a, b, c) for a, b, c, m in tris if m != 0xFF]
    preview((verts, tri_list), prev)
    print("preview: %s" % prev)


if __name__ == "__main__":
    main()
