"""Bake world maps for the instanced custom battlegrounds.

WHY THIS EXISTS. Scarlet Chapel, Blackrock Throne and the Violet Hold Gauntlet
were all given *dungeon* maps - DungeonMap floor rects plus DungeonMapChunk
rows keyed on the WMO group the player stands in. None of the three has ever
drawn a map. In stock 3.3.5 **no InstanceType 3 (battleground) map has a single
DungeonMap row**: Alterac Valley, Warsong Gulch, Arathi Basin, Eye of the Storm,
Strand and Isle of Conquest all ship plain terrain-style art instead, named

    Interface\\WorldMap\\<AreaName>\\<AreaName>1.blp .. <AreaName>12.blp

with no floor suffix - which is exactly the name WorldMapFrame_Update builds
when GetCurrentMapDungeonLevel() is 0. Our two battlegrounds that DO work,
Tanaris (1620) and the Obsidian Colosseum (1615), are terrain-style too. The
three broken ones are precisely the three that went the dungeon route.

So this crops each arena out of the stock dungeon sheet it lives on and ships
it as that battleground's own terrain-style art. Two names are written for
every tile:

    <dst>1.blp .. <dst>12.blp      the terrain path (what stock battlegrounds use)
    <dst>1_1.blp .. <dst>1_12.blp  the dungeon path, floor 1, in case the client
                                   does resolve a floor after all

Identical images, and tools/dbc/fix_bg_world_maps.py points BOTH rects - the
WorldMapArea Loc box and the DungeonMap row - at the same crop, so whichever
path the client takes it draws the same picture with the arrow in the same
place. That is the whole point of doing both: no guessing about which branch
runs.

AXES, because this is the bug that started all of it: on a world map screen X
is world **Y** (increasing leftwards) and screen Y is world **X** (increasing
upwards). px()/unpx() are the only places that conversion lives.

A sheet is a 4x3 grid of 256x256 tiles - 1024x768 - of which the UI shows only
the top-left 1002x668, so every crop must be 3:2 and the last 22 columns / 100
rows are padding nobody sees. Tiles are BLP2 / DXT1, no mipmaps, the same shape
as Blizzard's own; the writer came from tools/tanaris/build_worldmap_art.py,
which has the notes on why pixel data starts at 1172. Extraction goes through
mpqtool.exe rather than mpqpy because the files in patch-enUS-A are
MPQ-encrypted and mpqread cannot read those.

    python build_bg_worldmap_art.py [name ...]      (default: all of them)

Output lands in data/patch-staging/BGWorldMaps/<name>/, ready to pack into a
LOCALE archive - everything under Interface/ lives in the locale MPQs.
"""
import io
import math
import os
import struct
import subprocess
import sys

from PIL import Image

DATA = r"C:\Projects\Gamedev\wow\clients\centurion\Data"
MPQTOOL = r"C:\Projects\Gamedev\wow\tools\mpqtool\mpqtool.exe"
ARCHIVE = os.path.join(DATA, "enUS", "patch-enUS-A.MPQ")
OUT_ROOT = r"C:\Projects\Gamedev\wow\data\patch-staging\BGWorldMaps"

COLS, ROWS, TILE = 4, 3, 256
SHEET_W, SHEET_H = COLS * TILE, ROWS * TILE      # 1024 x 768
VIS_W, VIS_H = 1002, 668                         # the part the UI shows

# src_rect is what the SOURCE sheet covers, taken from the stock DungeonMap row
# it was drawn for. crop is (x, y, w, h) in that sheet's visible pixels; it must
# be 3:2 and contain the arena.
BGS = {
    # Scarlet Chapel (map 1189). Source: Scarlet Monastery floor 4, the
    # Cathedral (DungeonMap row 179). Arena from WorldSafeLocs 51890-51895,
    # which lands at x 409..572, y 294..588 - taller than wide, so the 3:2 crop
    # is driven by its height.
    "ScarletChapel": dict(
        src_dir="ScarletMonastery", src_floor=4,
        src_rect=dict(minY=1040.68994140625, maxY=1743.989990234375,
                      minX=812.4240112304688, maxX=1281.2900390625),
        # crop=None -> UNCROPPED: Blizzard's Cathedral sheet copied through
        # pixel for pixel, padding and all, and the rect is that sheet's own.
        # Nothing is resampled, so nothing can smear.
        crop=None,
        arena=dict(minX=868.24, maxX=1074.78, minY=1342.21, maxY=1456.92),
    ),
    # Blackrock Throne (map 1230). Source: Blackrock Depths floor 2 (DungeonMap
    # row 201), whose top-right corner is the Imperial Seat - the throne room is
    # the entire arena. Arena from WorldSafeLocs 52300-52305.
    "BlackrockThrone": dict(
        src_dir="BlackrockDepths", src_floor=2,
        src_rect=dict(minY=-934.7239990234375, maxY=572.3369750976562,
                      minX=495.3030090332031, maxX=1500.010009765625),
        crop=(762, 0, 240, 160),
        arena=dict(minX=1338.35, maxX=1424.53, minY=-843.91, maxY=-697.52),
    ),
}


def px(rect, wy, wx):
    """World (Y, X) -> pixel (x, y) in a sheet's visible area."""
    x = (rect["maxY"] - wy) / (rect["maxY"] - rect["minY"]) * VIS_W
    y = (rect["maxX"] - wx) / (rect["maxX"] - rect["minX"]) * VIS_H
    return x, y


def unpx(rect, x, y):
    wy = rect["maxY"] - (x / VIS_W) * (rect["maxY"] - rect["minY"])
    wx = rect["maxX"] - (y / VIS_H) * (rect["maxX"] - rect["minX"])
    return wy, wx


def full_crop(spec):
    """The source box to scale onto the WHOLE 1024x768 sheet.

    `crop` describes the part the rect refers to - the visible 1002x668 - so
    this widens it by the same ratios to reach the sheet's 4:3 shape. Keeping
    the origin fixed means the visible region still starts at the sheet's
    top-left corner, which is what the coordinate mapping assumes.
    """
    if spec["crop"] is None:
        return 0, 0, SHEET_W, SHEET_H
    x, y, w, h = spec["crop"]
    fw = int(math.ceil(w * SHEET_W / float(VIS_W)))
    fh = int(math.ceil(h * SHEET_H / float(VIS_H)))
    if x + fw > VIS_W or y + fh > VIS_H:
        raise SystemExit("  full-bleed crop (%d,%d,%d,%d) runs past the source's own "
                         "content area %dx%d - move `crop` up/left" % (x, y, fw, fh, VIS_W, VIS_H))
    return x, y, fw, fh


def crop_rect(spec):
    """The world rect the baked sheet covers, in DungeonMap's field order."""
    if spec["crop"] is None:
        return dict(spec["src_rect"])
    x, y, w, h = spec["crop"]
    wy_left, wx_top = unpx(spec["src_rect"], x, y)
    wy_right, wx_bottom = unpx(spec["src_rect"], x + w, y + h)
    return dict(minY=wy_right, maxY=wy_left, minX=wx_bottom, maxX=wx_top)


HEADER_SIZE = 148
PALETTE_SIZE = 1024
DATA_OFFSET = HEADER_SIZE + PALETTE_SIZE     # 1172


def write_blp2_dxt1(img, path):
    """BLP2, colorEncoding 2 / DXT1, no mipmaps. A BLP2 always carries a
    1024-byte palette block even when the image is DXT and the palette is
    unused, so pixel data starts at 1172, not straight after the header.
    Pillow will not write BLP except palettised, but it will write DXT1 inside
    a DDS whose header is a fixed 128 bytes, so the blocks are lifted out.
    """
    w, h = img.size

    buf = io.BytesIO()
    img.convert("RGBA").save(buf, format="DDS", pixel_format="DXT1")
    dxt = buf.getvalue()[128:]
    expected = max(1, w // 4) * max(1, h // 4) * 8
    if len(dxt) != expected:
        raise ValueError("DXT1 payload is %d bytes, expected %d" % (len(dxt), expected))

    header = bytearray()
    header += b"BLP2"
    header += struct.pack("<I", 1)
    header += struct.pack("<BBBB", 2, 1, 0, 0)   # DXT, 1-BIT ALPHA, DXT1, no mips
    header += struct.pack("<II", w, h)
    header += struct.pack("<16I", *([DATA_OFFSET] + [0] * 15))
    header += struct.pack("<16I", *([len(dxt)] + [0] * 15))
    if len(header) != HEADER_SIZE:
        raise ValueError("header is %d bytes, expected %d" % (len(header), HEADER_SIZE))

    with open(path, "wb") as f:
        f.write(header)
        f.write(b"\x00" * PALETTE_SIZE)
        f.write(dxt)


def load_source_sheet(spec, workdir):
    sheet = Image.new("RGBA", (SHEET_W, SHEET_H))
    for i in range(12):
        rel = "Interface\\WorldMap\\%s\\%s%d_%d.blp" % (
            spec["src_dir"], spec["src_dir"], spec["src_floor"], i + 1)
        dst = os.path.join(workdir, "src%02d.blp" % (i + 1))
        with open(dst, "wb") as f:
            rc = subprocess.run([MPQTOOL, "view", ARCHIVE, rel], stdout=f,
                                stderr=subprocess.PIPE)
        if rc.returncode != 0 or os.path.getsize(dst) == 0:
            raise SystemExit("could not read %s from %s" % (rel, os.path.basename(ARCHIVE)))
        sheet.paste(Image.open(dst).convert("RGBA"), ((i % COLS) * TILE, (i // COLS) * TILE))
    return sheet


def bake(name, spec):
    print("=== %s" % name)
    root = os.path.join(OUT_ROOT, name)
    workdir = os.path.join(root, "_src")
    outdir = os.path.join(root, "Interface", "WorldMap", name)
    os.makedirs(workdir, exist_ok=True)
    os.makedirs(outdir, exist_ok=True)

    if spec["crop"] is not None:
        x, y, w, h = spec["crop"]
        if x < 0 or y < 0 or x + w > VIS_W or y + h > VIS_H:
            raise SystemExit("  crop %r leaves the visible area %dx%d" % (spec["crop"], VIS_W, VIS_H))
        if abs(w / h - VIS_W / VIS_H) > 0.01:
            raise SystemExit("  crop aspect %.3f does not match the map's %.3f" % (w / h, VIS_W / VIS_H))

    r = crop_rect(spec)
    a = spec["arena"]
    if not (r["minX"] <= a["minX"] and a["maxX"] <= r["maxX"] and
            r["minY"] <= a["minY"] and a["maxY"] <= r["maxY"]):
        raise SystemExit("  the arena is not inside the cropped rect %r" % r)
    if spec["crop"] is None:
        print("  zoom  : 1.00x  (UNCROPPED - Blizzard's sheet copied verbatim)")
    else:
        print("  zoom  : %.2fx  (crop %dx%d -> %dx%d)" % (VIS_W / w, w, h, VIS_W, VIS_H))
    print("  margin: X %.1f / %.1f yd, Y %.1f / %.1f yd"
          % (a["minX"] - r["minX"], r["maxX"] - a["maxX"],
             a["minY"] - r["minY"], r["maxY"] - a["maxY"]))

    sheet = load_source_sheet(spec, workdir)
    if spec["crop"] is None:
        # UNCROPPED: copy the source tiles byte for byte. Nothing is decoded,
        # resampled or re-encoded, so the result is bit-identical to Blizzard's
        # own map - including the alpha in the overhang.
        for i in range(12):
            src = os.path.join(workdir, "src%02d.blp" % (i + 1))
            blob = open(src, "rb").read()
            for dst in ("%s%d.blp" % (name, i + 1), "%s1_%d.blp" % (name, i + 1)):
                with open(os.path.join(outdir, dst), "wb") as f:
                    f.write(blob)
        print("  wrote : 24 tiles, verbatim copies of %s%d_1..12"
              % (spec["src_dir"], spec["src_floor"]))
        out = sheet
        report(root, r, out)
        return

    # THE OVERHANG MUST BE TRANSPARENT. The sheet is 1024x768 but the frame is
    # 1002x668, and WoW frames do not clip their children - so the last 22
    # columns and 100 rows ARE drawn, outside the map frame, over the rest of
    # the UI. Blizzard's own tiles are DXT1 with 1-bit alpha and set that
    # region to alpha 0 so nothing shows. Fill it with anything opaque and it
    # spills: edge-replication smears into streaks, real art runs past the
    # frame. So: content in the top-left 1002x668, alpha 0 everywhere else.
    out = Image.new("RGBA", (SHEET_W, SHEET_H), (0, 0, 0, 0))
    x, y, w, h = spec["crop"]
    out.paste(sheet.crop((x, y, x + w, y + h)).resize((VIS_W, VIS_H), Image.LANCZOS), (0, 0))

    for i in range(12):
        tile = out.crop(((i % COLS) * TILE, (i // COLS) * TILE,
                         (i % COLS) * TILE + TILE, (i // COLS) * TILE + TILE))
        # terrain-style (what stock battlegrounds use) AND dungeon floor 1
        write_blp2_dxt1(tile, os.path.join(outdir, "%s%d.blp" % (name, i + 1)))
        write_blp2_dxt1(tile, os.path.join(outdir, "%s1_%d.blp" % (name, i + 1)))
    print("  wrote : 24 tiles (%s1..12 and %s1_1..1_12)" % (name, name))
    report(root, r, out)


def report(root, rect, sheet):
    """Save a preview and print the rect the DBC rows must carry.

    The preview is composited over mid-grey so the transparent overhang is
    obvious: anything that shows up outside the top-left 1002x668 is a bug that
    would be drawn over the UI in game.
    """
    bg = Image.new("RGB", sheet.size, (110, 110, 110))
    bg.paste(sheet, (0, 0), sheet)
    bg.save(os.path.join(root, "preview.png"))
    a = sheet.split()[3]
    over_r = a.crop((VIS_W, 0, SHEET_W, SHEET_H)).getextrema()
    over_b = a.crop((0, VIS_H, SHEET_W, SHEET_H)).getextrema()
    print("  overhang alpha: right %s  bottom %s   (both must be (0, 0))" % (over_r, over_b))
    print("  rect for DungeonMap / WorldMapArea:")
    for k in ("minY", "maxY", "minX", "maxX"):
        print("    %-5s %r" % (k, rect[k]))
    print("  preview: %s" % os.path.join(root, "preview.png"))


def main():
    names = sys.argv[1:] or sorted(BGS)
    for n in names:
        if n not in BGS:
            raise SystemExit("unknown battleground %r (have: %s)" % (n, ", ".join(sorted(BGS))))
        bake(n, BGS[n])


if __name__ == "__main__":
    main()
