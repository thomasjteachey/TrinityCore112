"""Build a fully-revealed world map image for the Tanaris battleground.

The battleground's world map draws Blizzard's Tanaris base art but none of the
zone detail, because that detail lives in WorldMapOverlay textures which the
client only reveals for explored areas -- and it refuses to match overlays to
map 1620 at all (GetNumMapOverlays() returns 0 there).

Rather than fight that lookup, this bakes the overlays INTO the base image and
ships the result as the battleground's own map. The whole zone is then visible
permanently, which is what an RTS wants anyway, and it needs no overlay rows,
no exploration and no fog.

    Interface/WorldMap/TanarisBG/TanarisBG1.blp .. TanarisBG12.blp

Pair it with WorldMapArea 9532's AreaName set to "TanarisBG": that field is what
picks the art directory.

Note this must be packed into a LOCALE archive (patch-enUS-8), not patch-Z --
everything under Interface/ lives in the locale MPQs.

Pillow reads BLP2/DXT fine but its encoder only writes palettized BLP, which
would posterise the map, so the writer here emits BLP2 colorEncoding 3
(uncompressed BGRA). The stock tiles carry no mipmaps, so neither does this.
"""
import io
import os
import struct
import sys

sys.path.insert(0, r"C:\Projects\Gamedev\wow\tools\mpqpy")
from mpqread import MPQ
from PIL import Image

DATA = r"C:\Projects\Gamedev\wow\clients\centurion\Data"
OUT_ROOT = r"C:\Projects\Gamedev\wow\data\patch-staging\TanarisBG-worldmap"

SRC_DIR = "Tanaris"
DST_DIR = "TanarisBG"

# World map art is a 4x3 grid of 256x256 tiles, numbered left to right then top
# to bottom, giving a 1024x768 sheet of which the UI shows the top-left
# 1002x668. Overlay offsets are in that same pixel space.
COLS, ROWS, TILE = 4, 3, 256
SHEET_W, SHEET_H = COLS * TILE, ROWS * TILE

ORDER = ["common.MPQ", "common-2.MPQ", "expansion.MPQ", "lichking.MPQ",
         "patch.MPQ", "patch-2.MPQ", "patch-3.MPQ", "patch-Y.MPQ", "patch-Z.MPQ"]
LOCALE = ["locale-enUS.MPQ", "expansion-locale-enUS.MPQ", "lichking-locale-enUS.MPQ",
          "base-enUS.MPQ", "patch-enUS.MPQ", "patch-enUS-2.MPQ", "patch-enUS-3.MPQ",
          "patch-enUS-6.MPQ", "patch-enUS-7.MPQ", "patch-enUS-8.MPQ"]


def load_archives():
    out = []
    for n in ORDER:
        p = os.path.join(DATA, n)
        if os.path.exists(p):
            try:
                out.append((n, MPQ(p)))
            except Exception:
                pass
    for n in LOCALE:
        p = os.path.join(DATA, "enUS", n)
        if os.path.exists(p):
            try:
                out.append((n, MPQ(p)))
            except Exception:
                pass
    return out


def resolve(archives, rel):
    hit = None
    for name, m in archives:          # last match wins = highest priority
        if m.find(rel) is not None:
            hit = (name, m)
    return hit


def read_blp(archives, rel):
    hit = resolve(archives, rel)
    if not hit:
        return None
    blob = hit[1].extract(rel)
    if blob is None:
        return None
    return Image.open(io.BytesIO(blob)).convert("RGBA")


HEADER_SIZE = 148
PALETTE_SIZE = 1024
DATA_OFFSET = HEADER_SIZE + PALETTE_SIZE     # 1172


def write_blp2_dxt1(img, path):
    """BLP2, colorEncoding 2 / DXT1, no mipmaps -- byte-for-byte the same shape
    as Blizzard's own map tiles.

    Two things are easy to get wrong here. A BLP2 always carries a 1024-byte
    palette block even when the image is DXT and the palette is unused, so the
    pixel data starts at 1172, not straight after the 148-byte header. And the
    stock tiles set hasMips=0, so a single mip level is correct rather than a
    full chain.

    Pillow will not write BLP in anything but palettised mode, but it will
    write DXT1 inside a DDS, whose header is a fixed 128 bytes -- so the block
    data is lifted straight out of that.
    """
    rgb = Image.new("RGB", img.size, (0, 0, 0))
    rgb.paste(img, (0, 0), img)                  # flatten: DXT1 here carries no alpha
    w, h = rgb.size

    buf = io.BytesIO()
    rgb.convert("RGBA").save(buf, format="DDS", pixel_format="DXT1")
    dxt = buf.getvalue()[128:]                   # skip the DDS header
    expected = max(1, w // 4) * max(1, h // 4) * 8
    if len(dxt) != expected:
        raise ValueError("DXT1 payload is %d bytes, expected %d" % (len(dxt), expected))

    header = bytearray()
    header += b"BLP2"
    header += struct.pack("<I", 1)                       # version
    header += struct.pack("<BBBB", 2, 0, 0, 0)           # DXT, no alpha, DXT1, no mips
    header += struct.pack("<II", w, h)
    header += struct.pack("<16I", *([DATA_OFFSET] + [0] * 15))
    header += struct.pack("<16I", *([len(dxt)] + [0] * 15))
    if len(header) != HEADER_SIZE:
        raise ValueError("header is %d bytes, expected %d" % (len(header), HEADER_SIZE))

    with open(path, "wb") as f:
        f.write(header)
        f.write(b"\x00" * PALETTE_SIZE)
        f.write(dxt)


def main():
    archives = load_archives()
    print("archives: %d" % len(archives))

    # --- base sheet -------------------------------------------------------
    sheet = Image.new("RGBA", (SHEET_W, SHEET_H), (0, 0, 0, 0))
    missing = []
    for i in range(1, COLS * ROWS + 1):
        rel = "Interface\\WorldMap\\%s\\%s%d.blp" % (SRC_DIR, SRC_DIR, i)
        tile = read_blp(archives, rel)
        if tile is None:
            missing.append(i)
            continue
        col = (i - 1) % COLS
        row = (i - 1) // COLS
        sheet.paste(tile, (col * TILE, row * TILE))
    print("base tiles pasted: %d/%d %s" % (COLS * ROWS - len(missing), COLS * ROWS,
                                           "missing %s" % missing if missing else ""))

    # --- overlays on top --------------------------------------------------
    # Read the overlay list straight out of the DBC so the bake always matches
    # what Blizzard actually places, rather than a hand-kept list.
    ov_path = r"C:/Projects/Gamedev/wow/data/dbc/lplus/WorldMapOverlay.dbc"
    blob = open(ov_path, "rb").read()
    _m, rc, fc, rs, _sb = struct.unpack_from("<4sIIII", blob, 0)
    recs = blob[20:20 + rc * rs]
    block = blob[20 + rc * rs:]

    def cstr(off):
        return "" if off == 0 else block[off:block.index(b"\x00", off)].decode("utf-8", "replace")

    placed = skipped = 0
    for i in range(rc):
        o = i * rs
        if struct.unpack_from("<i", recs, o + 1 * 4)[0] != 161:      # stock Tanaris
            continue
        tex = cstr(struct.unpack_from("<I", recs, o + 8 * 4)[0])
        tw = struct.unpack_from("<i", recs, o + 9 * 4)[0]
        th = struct.unpack_from("<i", recs, o + 10 * 4)[0]
        ox = struct.unpack_from("<i", recs, o + 11 * 4)[0]
        oy = struct.unpack_from("<i", recs, o + 12 * 4)[0]

        # Overlays wider or taller than one 256 tile are split the same way the
        # base sheet is; ours all fit in a single tile, but handle both.
        cols = (tw + TILE - 1) // TILE
        rows = (th + TILE - 1) // TILE
        n = 1
        pasted_any = False
        for r in range(rows):
            for c in range(cols):
                rel = "Interface\\WorldMap\\%s\\%s%d.blp" % (SRC_DIR, tex, n)
                part = read_blp(archives, rel)
                n += 1
                if part is None:
                    continue
                # each chunk carries only its own slice of the overlay
                cw = min(TILE, tw - c * TILE)
                ch = min(TILE, th - r * TILE)
                part = part.crop((0, 0, cw, ch))
                sheet.alpha_composite(part, (ox + c * TILE, oy + r * TILE))
                pasted_any = True
        if pasted_any:
            placed += 1
        else:
            skipped += 1
            print("   no art for overlay %s" % tex)
    print("overlays baked in: %d (%d without art)" % (placed, skipped))

    os.makedirs(OUT_ROOT, exist_ok=True)
    preview = os.path.join(OUT_ROOT, "TanarisBG_worldmap_preview.png")
    sheet.convert("RGB").save(preview)
    print("preview: %s" % preview)

    # --- slice back into 12 tiles ----------------------------------------
    dest = os.path.join(OUT_ROOT, "Interface", "WorldMap", DST_DIR)
    os.makedirs(dest, exist_ok=True)
    total = 0
    for i in range(1, COLS * ROWS + 1):
        col = (i - 1) % COLS
        row = (i - 1) // COLS
        tile = sheet.crop((col * TILE, row * TILE, (col + 1) * TILE, (row + 1) * TILE))
        out = os.path.join(dest, "%s%d.blp" % (DST_DIR, i))
        write_blp2_dxt1(tile, out)
        total += os.path.getsize(out)
    print("wrote 12 BLPs, %.2f MB -> %s" % (total / 1e6, dest))


if __name__ == "__main__":
    main()
