"""Violet Hold runes, locked: a grey, see-through copy of a rune's model, shown while the
rune waits for the next wave (BattlegroundVHR::UpdateRuneDisplays).

  python make_dim_rune.py model <src.m2> <out.m2> <texture index>=<new BLP path> ...
  python make_dim_rune.py blp   <src.blp> <out.blp>

model: the copy keeps the mesh, bones, sequences and particles, and
  - repoints each listed texture at a greyed copy (made with `blp`); new names are
    appended at the end of the file and the texture record repointed, as
    make_power_crystal.py does,
  - turns every opaque or alpha-keyed material (blend 0/1) into alpha-blended (2), so
    the transparency below applies to the solid parts too,
  - greys every colour track and every particle colour (each key becomes
    GREY_LEVEL x its brightest channel),
  - scales every transparency (texture weight) key and every particle alpha key by
    FADE.
The skin is unchanged and is copied beside the model as <out>00.skin.

blp: a greyscale copy of a texture in the same format (palettized ones keep their
indices; only the palette changes) at LUMA_SCALE of its luminance.

M2 v264 particle records (0x1DC bytes) keep their colour key values at 0x10C and alpha
at 0x11C (M2FakeTrack: times array, then values array; colour as 3 floats 0-255,
alpha as int16 fixed point).
"""
import os
import shutil
import struct
import sys

sys.path.insert(0, r"C:\Projects\Gamedev\wow\tools\mpqpy")
sys.path.insert(0, r"C:\Projects\Gamedev\wow\tools\recolor")
import m2tracks as M

FADE = 0.4          # how much of the original transparency is left
GREY_LEVEL = 0.6    # colour keys: this much of the brightest channel, in grey
LUMA_SCALE = 0.85   # textures: this much of their luminance
PARTICLE_SIZE = 0x1DC
PARTICLE_COLOR_VALUES = 0x10C
PARTICLE_ALPHA_VALUES = 0x11C
MATERIALS = 0x70


def grey_key(r, g, b):
    v = max(r, g, b) * GREY_LEVEL
    return v, v, v


def build_model(src, retexture):
    b = bytearray(src)
    M.check_header(b)

    nt, ot = M.header_array(b, "textures")
    for index, path in retexture.items():
        assert 0 <= index < nt, index
        tex = ot + index * 16
        while len(b) % 16:
            b.append(0)
        name = path.encode() + b"\0"
        ofs = len(b)
        b.extend(name)
        struct.pack_into("<II", b, tex + 8, len(name), ofs)

    nm, om = struct.unpack_from("<II", b, MATERIALS)
    for i in range(nm):
        flags, blend = struct.unpack_from("<HH", b, om + i * 4)
        if blend in (0, 1):
            struct.pack_into("<HH", b, om + i * 4, flags, 2)

    for t in M.tracks(b):
        if t["kind"] == "colors" and t["label"] == "color":
            for s in range(t["nv"]):
                cnt, vo = M.entry(b, t["ov"], s)
                for k in range(cnt):
                    struct.pack_into("<3f", b, vo + k * 12, *grey_key(*struct.unpack_from("<3f", b, vo + k * 12)))
        elif t["kind"] == "tex_weights":
            for s in range(t["nv"]):
                cnt, vo = M.entry(b, t["ov"], s)
                for k in range(cnt):
                    w, = struct.unpack_from("<h", b, vo + k * 2)
                    struct.pack_into("<h", b, vo + k * 2, int(round(w * FADE)))

    npart, opart = M.header_array(b, "particles")
    for i in range(npart):
        p = opart + i * PARTICLE_SIZE
        cnt, vo = struct.unpack_from("<II", b, p + PARTICLE_COLOR_VALUES)
        for k in range(cnt):
            struct.pack_into("<3f", b, vo + k * 12, *grey_key(*struct.unpack_from("<3f", b, vo + k * 12)))
        cnt, vo = struct.unpack_from("<II", b, p + PARTICLE_ALPHA_VALUES)
        for k in range(cnt):
            a, = struct.unpack_from("<h", b, vo + k * 2)
            struct.pack_into("<h", b, vo + k * 2, int(round(a * FADE)))
    return bytes(b)


def verify_model(src, out, retexture):
    M.check_header(out)
    nt, ot = M.header_array(out, "textures")
    for index, path in retexture.items():
        _, _, ln, lo = struct.unpack_from("<IIII", out, ot + index * 16)
        assert out[lo:lo + ln].rstrip(b"\0").decode() == path
    nm, om = struct.unpack_from("<II", out, MATERIALS)
    assert all(struct.unpack_from("<HH", out, om + i * 4)[1] >= 2 for i in range(nm))
    for t in M.tracks(out):
        if t["kind"] == "colors" and t["label"] == "color":
            for s in range(t["nv"]):
                for raw in M.keys(out, t, s)[1]:
                    r, g, bb = struct.unpack("<3f", raw)
                    assert r == g == bb, (r, g, bb)
    # everything up to the appended names other than what was meant to change is the
    # same size: the copy only ever rewrites values in place
    assert len(out) >= len(src)


def grey_blp(data):
    import blp
    b = blp.Blp(data)

    def grey(r, g, bb):
        v = int(round((0.299 * r + 0.587 * g + 0.114 * bb) * LUMA_SCALE))
        return v, v, v

    if b.compression == blp.COMP_PAL:
        return b.with_palette([grey(r, g, bb) + (a,) for (r, g, bb, a) in b.palette()])
    im = b.image(0).convert("RGBA")
    px = im.load()
    for y in range(im.height):
        for x in range(im.width):
            r, g, bb, a = px[x, y]
            px[x, y] = grey(r, g, bb) + (a,)
    fmt = b.alphaType if b.alphaType in (blp.DXT1, blp.DXT3, blp.DXT5) else blp.DXT5
    return blp.write_dxt(im, fmt)


if __name__ == "__main__":
    if sys.argv[1] == "blp":
        data = grey_blp(open(sys.argv[2], "rb").read())
        open(sys.argv[3], "wb").write(data)
        print("ok: %s (%d bytes)" % (sys.argv[3], len(data)))
    elif sys.argv[1] == "model":
        src_path, out_path = sys.argv[2], sys.argv[3]
        retexture = {int(k): v for k, v in (a.split("=", 1) for a in sys.argv[4:])}
        src = open(src_path, "rb").read()
        out = build_model(src, retexture)
        verify_model(src, out, retexture)
        open(out_path, "wb").write(out)
        shutil.copyfile(os.path.splitext(src_path)[0] + "00.skin", os.path.splitext(out_path)[0] + "00.skin")
        print("ok: %s (%d -> %d bytes) + skin" % (out_path, len(src), len(out)))
    else:
        sys.exit(__doc__)
