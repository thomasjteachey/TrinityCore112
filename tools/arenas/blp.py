"""BLP2 reader and writer, enough for minimap tiles.

BLP2 header (1172 bytes):
    char   magic[4]        "BLP2"
    uint32 type            1 = direct colour
    uint8  encoding        1 = palettised, 2 = DXT, 3 = raw BGRA
    uint8  alphaDepth      0 / 1 / 4 / 8
    uint8  alphaEncoding   0 = DXT1, 1 = DXT3, 7 = DXT5
    uint8  hasMips
    uint32 width, height
    uint32 mipOffsets[16]
    uint32 mipSizes[16]
    uint32 palette[256]    BGRA; present even when unused

The writer matches whatever the client's own minimap tiles use rather than
picking a format on theory -- see --inspect.
"""

import os
import struct
import sys

HEADER_SIZE = 4 + 4 + 4 + 4 + 4 + 64 + 64 + 1024   # 1172

ENCODING = {1: "palettised", 2: "DXT", 3: "raw BGRA"}
ALPHA_ENC = {0: "DXT1", 1: "DXT3", 7: "DXT5"}


def inspect(blob, label=""):
    if blob[:4] != b"BLP2":
        return {"error": "not BLP2 (%r)" % blob[:4]}
    (_magic, btype, encoding, alpha_depth, alpha_enc, has_mips,
     width, height) = struct.unpack_from("<4sIBBBBII", blob, 0)
    offs = struct.unpack_from("<16I", blob, 20)
    sizes = struct.unpack_from("<16I", blob, 84)
    mips = sum(1 for s in sizes if s)
    return {
        "label": label, "type": btype, "encoding": encoding,
        "encoding_name": ENCODING.get(encoding, "?"),
        "alpha_depth": alpha_depth, "alpha_enc": alpha_enc,
        "alpha_enc_name": ALPHA_ENC.get(alpha_enc, "?"),
        "has_mips": has_mips, "width": width, "height": height,
        "mips": mips, "mip0_size": sizes[0], "bytes": len(blob),
    }


# ------------------------------------------------------------------ DXT1
def _dxt1_block(px):
    """Encode one 4x4 RGBA block (list of 16 (r,g,b,a)) to 8 DXT1 bytes.

    Endpoints are the extremes along the block's principal luminance spread,
    which is cheap and perfectly adequate for a minimap: these are low-contrast
    shaded surfaces, not photographic detail.
    """
    def to565(c):
        return ((c[0] >> 3) << 11) | ((c[1] >> 2) << 5) | (c[2] >> 3)

    lum = [0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2] for p in px]
    lo_i = min(range(16), key=lambda i: lum[i])
    hi_i = max(range(16), key=lambda i: lum[i])
    c0, c1 = px[hi_i][:3], px[lo_i][:3]
    v0, v1 = to565(c0), to565(c1)
    if v0 == v1:
        # flat block: one colour, all indices 0
        return struct.pack("<HHI", v0, v1, 0)
    if v0 < v1:
        v0, v1 = v1, v0
        c0, c1 = c1, c0

    # the four representable colours
    def expand(v):
        r = ((v >> 11) & 0x1F) << 3
        g = ((v >> 5) & 0x3F) << 2
        b = (v & 0x1F) << 3
        return (r, g, b)
    e0, e1 = expand(v0), expand(v1)
    pal = [e0, e1,
           tuple((2 * e0[k] + e1[k]) // 3 for k in range(3)),
           tuple((e0[k] + 2 * e1[k]) // 3 for k in range(3))]

    bits = 0
    for i, p in enumerate(px):
        best, bd = 0, 1 << 30
        for j, q in enumerate(pal):
            d = (p[0] - q[0]) ** 2 + (p[1] - q[1]) ** 2 + (p[2] - q[2]) ** 2
            if d < bd:
                best, bd = j, d
        bits |= best << (2 * i)
    return struct.pack("<HHI", v0, v1, bits)


def encode_dxt1(img):
    """PIL RGB(A) image -> DXT1 bytes. Dimensions must be multiples of 4."""
    w, h = img.size
    px = img.convert("RGBA").load()
    out = bytearray()
    for by in range(0, h, 4):
        for bx in range(0, w, 4):
            block = []
            for y in range(4):
                for x in range(4):
                    block.append(px[min(bx + x, w - 1), min(by + y, h - 1)])
            out += _dxt1_block(block)
    return bytes(out)


def write_blp2_dxt1(img, with_mips=True):
    """Encode a PIL image as a BLP2/DXT1 texture with a full mip chain."""
    from PIL import Image

    levels = []
    cur = img.convert("RGBA")
    while True:
        levels.append(cur)
        if not with_mips or min(cur.size) <= 4:
            break
        cur = cur.resize((max(cur.width // 2, 4), max(cur.height // 2, 4)), Image.LANCZOS)

    datas = [encode_dxt1(l) for l in levels]

    offs = [0] * 16
    sizes = [0] * 16
    pos = HEADER_SIZE
    for i, d in enumerate(datas[:16]):
        offs[i] = pos
        sizes[i] = len(d)
        pos += len(d)

    hdr = bytearray()
    hdr += b"BLP2"
    hdr += struct.pack("<I", 1)                     # direct colour
    hdr += struct.pack("<BBBB", 2, 0, 0, 1 if with_mips else 0)   # DXT, no alpha, DXT1
    hdr += struct.pack("<II", img.width, img.height)
    hdr += struct.pack("<16I", *offs)
    hdr += struct.pack("<16I", *sizes)
    hdr += b"\x00" * 1024                           # unused palette
    assert len(hdr) == HEADER_SIZE, len(hdr)

    return bytes(hdr) + b"".join(datas[:16])


def decode_dxt1(data, w, h):
    """DXT1 -> PIL RGB image. Exists so an encode can be checked, not trusted."""
    from PIL import Image

    out = Image.new("RGB", (w, h))
    px = out.load()
    i = 0
    for by in range(0, h, 4):
        for bx in range(0, w, 4):
            v0, v1, bits = struct.unpack_from("<HHI", data, i)
            i += 8

            def expand(v):
                return (((v >> 11) & 0x1F) << 3, ((v >> 5) & 0x3F) << 2, (v & 0x1F) << 3)
            e0, e1 = expand(v0), expand(v1)
            if v0 > v1:
                pal = [e0, e1,
                       tuple((2 * e0[k] + e1[k]) // 3 for k in range(3)),
                       tuple((e0[k] + 2 * e1[k]) // 3 for k in range(3))]
            else:
                pal = [e0, e1,
                       tuple((e0[k] + e1[k]) // 2 for k in range(3)),
                       (0, 0, 0)]
            for y in range(4):
                for x in range(4):
                    if bx + x < w and by + y < h:
                        px[bx + x, by + y] = pal[(bits >> (2 * (y * 4 + x))) & 3]
    return out


def read_blp2(blob):
    """Decode a BLP2/DXT1 texture back to a PIL image (mip 0 only)."""
    info = inspect(blob)
    if info.get("error"):
        raise ValueError(info["error"])
    if info["encoding"] != 2:
        raise ValueError("only DXT is handled, got %s" % info["encoding_name"])
    off = struct.unpack_from("<16I", blob, 20)[0]
    size = struct.unpack_from("<16I", blob, 84)[0]
    return decode_dxt1(blob[off:off + size], info["width"], info["height"])


def main():
    if "--inspect" not in sys.argv:
        raise SystemExit(__doc__)
    SCRATCH = (r"C:\Users\broki\AppData\Local\Temp\claude"
               r"\C--Ascension-Launcher-resources-ascension-live"
               r"\b4c02561-8768-40b5-abb1-376e8e1417b6\scratchpad")
    sys.path.insert(0, SCRATCH)
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from deps import Index

    idx = Index()
    samples = [
        "textures\\minimap\\coliseumarena_35_16.blp",
        "textures\\minimap\\BladesEdgeArena2b18_24.blp",
        "textures\\minimap\\nerubianarena_32_30.blp",
    ]
    for s in samples:
        blob = idx.read(s)
        if blob is None:
            print("%-52s NOT FOUND" % s)
            continue
        info = inspect(blob, s)
        print("%-52s %s" % (os.path.basename(s), info))
    idx.close()


if __name__ == "__main__":
    main()
