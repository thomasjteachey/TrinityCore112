"""Minimap tiles for a WMO-only arena from the textured top-down render.

For each ADT tile (col,row) the client looks up "<dir>\\map<col>_<row>.blp" in
md5translate.trs. Tile (col,row) covers world Y in [(31-col)T,(32-col)T] (image
left = larger Y) and X in [(31-row)T,(32-row)T] (image top = larger X).
"""
import hashlib, os, sys
import numpy as np
from PIL import Image
import geo, texrender as tr, m2render as mr

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from blp import write_blp2_dxt1, read_blp2   # noqa: E402

T = geo.T
OUT = "tiles"
TILE_PX = 512
SS = 4          # supersampling


def build(directory, tiles, floor, clip_above=12.0, gain=1.3, gamma=0.85):
    """tiles: list of (col,row). Returns {(col,row): (png path, blp bytes, md5)}."""
    os.makedirs(OUT, exist_ok=True)
    flags, gw, wtiles = geo.wdt_tiles(directory)
    wmos, m2s = {}, {}
    for (c, r) in wtiles:
        a = geo.read_adt(directory, c, r)
        if not a:
            continue
        for w in a["wmo"]:
            wmos[w["uid"]] = w
        for m in a["m2"]:
            m2s[m["uid"]] = m
    cols = sorted(set(c for c, r in tiles))
    rows = sorted(set(r for c, r in tiles))
    c0, c1, r0, r1 = cols[0], cols[-1], rows[0], rows[-1]
    y_left = (32 - c0) * T
    x_top = (32 - r0) * T
    width = (c1 - c0 + 1) * T
    height = (r1 - r0 + 1) * T
    px = T / (TILE_PX * SS)
    ras = tr.TexRaster(x_top, y_left, width, height, px, floor + clip_above)
    for w in wmos.values():
        tr.render_wmo(ras, w["name"], w["pos"], w["rot"])
        mr.render_wmo_doodads(ras, w)
    for m in m2s.values():
        mr.render_adt_m2(ras, m)
    img, mask = ras.image()
    arr = np.asarray(img, dtype=np.float32) / 255.0
    arr = np.clip((arr ** gamma) * gain, 0, 1)
    big = Image.fromarray((arr * 255).astype(np.uint8))
    big = big.resize(((c1 - c0 + 1) * TILE_PX, (r1 - r0 + 1) * TILE_PX), Image.LANCZOS)
    out = {}
    for (c, r) in tiles:
        box = ((c - c0) * TILE_PX, (r - r0) * TILE_PX, (c - c0 + 1) * TILE_PX, (r - r0 + 1) * TILE_PX)
        t = big.crop(box)
        png = os.path.join(OUT, "%s_map%02d_%02d.png" % (directory, c, r))
        t.save(png)
        blob = write_blp2_dxt1(t.convert("RGB"), with_mips=False)
        back = read_blp2(blob)
        md5 = hashlib.md5(blob).hexdigest()
        open(os.path.join(OUT, md5 + ".blp"), "wb").write(blob)
        out[(c, r)] = (png, blob, md5)
        print(directory, (c, r), "->", md5, len(blob), "readback", getattr(back, "size", None))
    big.save(os.path.join(OUT, "%s_whole.png" % directory))
    return out


if __name__ == "__main__":
    build("karazhanarena", [(30, 30), (30, 31)], floor=0.5)
    build("ulduararena", [(30, 30), (30, 31)], floor=0.9)
