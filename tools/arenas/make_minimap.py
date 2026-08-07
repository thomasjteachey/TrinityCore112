"""Render minimap tiles for arenas that have no terrain to draw.

Guardian's Hall (1007) and Spark of Creator (1008) are a single WMO floating in
empty space -- zero ground textures, zero doodads. A normal minimap generator
renders an ADT's terrain, so for these it had nothing to draw, which is why
Ascension shipped no minimap for either.

The geometry is still there though, just in the WMO rather than the terrain, so
it can be rendered directly: project the mesh straight down, shade it, and slice
the result onto the ADT tile grid.

Two details that matter:

* Height clipping. Keeping the topmost surface at each pixel would draw the
  roof, not the floor. Geometry is clipped to a band around the known floor
  height so the render shows the fighting surface and its walls.
* Placement. Both arenas sit at rotY = 0, so the WMO transform is a pure
  translation and the fiddly rotated-placement case does not arise. The tool
  refuses to guess if it meets a rotated one.

Usage:
    python make_minimap.py --preview          # PNGs only, for eyeballing
    python make_minimap.py --preview 877      # just one arena
"""

import argparse
import os
import struct
import sys

import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
SCRATCH = (r"C:\Users\broki\AppData\Local\Temp\claude"
           r"\C--Ascension-Launcher-resources-ascension-live"
           r"\b4c02561-8768-40b5-abb1-376e8e1417b6\scratchpad")
sys.path.insert(0, HERE)
sys.path.insert(0, SCRATCH)

from adt_probe import TILE, ORIGIN                 # noqa: E402
from wmo_floor import extract, group_triangles     # noqa: E402
from gen_arena_sql import ARENAS, MEASURED         # noqa: E402
from blp import write_blp2_dxt1, read_blp2         # noqa: E402
import json                                        # noqa: E402

# 512, not 256: every minimap tile already in this client is 512x512 DXT1 with
# no mipmaps, and matching what the client already ships beats picking a size on
# theory. At 533 yards per tile that is ~0.96 px/yard.
TILE_PX = 512
OUT = os.path.join(HERE, "_minimap")

# arenas this applies to: WMO-only maps, with the WMO and the floor to clip around
JOBS = {
    877: {"wmo": "world\\wmo\\Dungeon\\pvp\\karazhan_arena.wmo",
          "dir": "karazhanarena"},
    878: {"wmo": "world\\wmo\\Dungeon\\pvp\\ulduar_arena.wmo",
          "dir": "ulduararena"},
}


def wmo_world_pos(directory):
    """The arena WMO's placement, from the geometry probe."""
    with open(os.path.join(HERE, "arena_geometry.json"), encoding="utf-8") as fh:
        geom = json.load(fh)
    g = geom[directory]["arena_wmo"]
    return g["pos"], g["rot_deg"]


def load_triangles(root_in_mpq, wmo_pos):
    """All triangles of a WMO, in world space. Pure translation (rotY == 0)."""
    with open(os.path.join(SCRATCH, "wmo_index.json"), encoding="utf-8") as fh:
        index = {k.upper(): v for k, v in json.load(fh).items()}
    key = os.path.basename(root_in_mpq).rsplit(".", 1)[0].upper()
    archive, files = next(iter(index[key].items()))
    groups = [f for f in files if not f.upper().endswith(key + ".WMO")]
    paths = extract(archive, groups)

    wx, wy, wz = wmo_pos
    tris = []
    for gp in paths.values():
        raw, _verts = group_triangles(gp)
        for v0, v1, v2, _collides in raw:
            # WMO group vertices are Z-UP, unlike the MODF position triple which
            # is Y-up. Getting that wrong rotates the model onto its side: the
            # first attempt mapped the 117-yard vertical extent onto world X and
            # drew the arena edge-on with a hollow middle.
            #
            # Confirmed by extent: local vx/vy/vz span 112.3/133.6/117.0 against
            # the placement bounding box's world X/Y/Z of 112.6/133.6/117.0.
            tris.append((
                (wx + v0[0], wy + v0[1], wz + v0[2]),
                (wx + v1[0], wy + v1[1], wz + v1[2]),
                (wx + v2[0], wy + v2[1], wz + v2[2]),
            ))
    return tris


def y_max_of(col_lo):
    """World Y at the left (west) edge of the rendered block."""
    return (32 - col_lo) * TILE


def x_max_of(row_lo):
    """World X at the top (north) edge of the rendered block."""
    return (32 - row_lo) * TILE


def tiles_covering(tris):
    xs = [p[0] for t in tris for p in t]
    ys = [p[1] for t in tris for p in t]
    # tile (col,row): worldY in [(31-col)T,(32-col)T], worldX in [(31-row)T,(32-row)T]
    col_lo = int(32 - max(ys) / TILE)
    col_hi = int(32 - min(ys) / TILE)
    row_lo = int(32 - max(xs) / TILE)
    row_hi = int(32 - min(xs) / TILE)
    return col_lo, col_hi, row_lo, row_hi


def render(tris, col_lo, col_hi, row_lo, row_hi, z_lo, z_hi):
    """Orthographic top-down raster of the whole arena, then sliced by caller."""
    ncol = col_hi - col_lo + 1
    nrow = row_hi - row_lo + 1
    W, H = ncol * TILE_PX, nrow * TILE_PX

    # world bounds of the whole rendered block
    y_max = (32 - col_lo) * TILE      # image left edge  (west)
    y_min = (31 - col_hi) * TILE      # image right edge (east)
    x_max = (32 - row_lo) * TILE      # image top edge   (north)
    x_min = (31 - row_hi) * TILE      # image bottom edge(south)

    zbuf = np.full((H, W), -1e9, dtype=np.float32)
    shade = np.zeros((H, W), dtype=np.float32)
    hit = np.zeros((H, W), dtype=bool)

    light = np.array([0.35, 0.25, 0.90])
    light /= np.linalg.norm(light)

    kept = 0
    for (a, b, c) in tris:
        if max(a[2], b[2], c[2]) < z_lo or min(a[2], b[2], c[2]) > z_hi:
            continue

        # world -> pixel. east is -Y so image x grows as Y falls; south is -X so
        # image y grows as X falls. North ends up at the top, as a map wants.
        px = [(y_max - p[1]) / (y_max - y_min) * W for p in (a, b, c)]
        py = [(x_max - p[0]) / (x_max - x_min) * H for p in (a, b, c)]
        pz = [p[2] for p in (a, b, c)]

        min_x, max_x = int(np.floor(min(px))), int(np.ceil(max(px)))
        min_y, max_y = int(np.floor(min(py))), int(np.ceil(max(py)))
        if max_x < 0 or min_x >= W or max_y < 0 or min_y >= H:
            continue
        min_x, max_x = max(min_x, 0), min(max_x, W - 1)
        min_y, max_y = max(min_y, 0), min(max_y, H - 1)
        if min_x > max_x or min_y > max_y:
            continue

        denom = ((py[1] - py[2]) * (px[0] - px[2]) + (px[2] - px[1]) * (py[0] - py[2]))
        if abs(denom) < 1e-9:
            continue

        gx, gy = np.meshgrid(np.arange(min_x, max_x + 1) + 0.5,
                             np.arange(min_y, max_y + 1) + 0.5)
        w0 = ((py[1] - py[2]) * (gx - px[2]) + (px[2] - px[1]) * (gy - py[2])) / denom
        w1 = ((py[2] - py[0]) * (gx - px[2]) + (px[0] - px[2]) * (gy - py[2])) / denom
        w2 = 1.0 - w0 - w1
        inside = (w0 >= -1e-6) & (w1 >= -1e-6) & (w2 >= -1e-6)
        if not inside.any():
            continue

        z = w0 * pz[0] + w1 * pz[1] + w2 * pz[2]
        inside &= (z >= z_lo) & (z <= z_hi)
        if not inside.any():
            continue

        # world-space normal, for lighting
        e1 = np.array([b[0] - a[0], b[1] - a[1], b[2] - a[2]])
        e2 = np.array([c[0] - a[0], c[1] - a[1], c[2] - a[2]])
        n = np.cross(e1, e2)
        ln = np.linalg.norm(n)
        if ln < 1e-9:
            continue
        n /= ln
        if n[2] < 0:
            n = -n                      # face upward for shading purposes
        lit = 0.35 + 0.65 * max(0.0, float(np.dot(n, light)))

        sub_z = zbuf[min_y:max_y + 1, min_x:max_x + 1]
        upd = inside & (z > sub_z)
        if not upd.any():
            continue
        sub_z[upd] = z[upd]
        shade[min_y:max_y + 1, min_x:max_x + 1][upd] = lit
        hit[min_y:max_y + 1, min_x:max_x + 1][upd] = True
        kept += 1

    return zbuf, shade, hit, kept, (W, H)


def colourise(zbuf, shade, hit, z_lo, z_hi):
    """Height ramp modulated by the lighting term; transparent where nothing hit.

    The ramp is anchored to the heights actually drawn, not to the clip band.
    Anchoring it to the band left the floor near the dark end -- the band is
    deliberately wider than the geometry, so the arena floor sat at 15% of the
    range and came out nearly black.
    """
    if hit.any():
        lo = float(np.percentile(zbuf[hit], 2))
        hi = float(np.percentile(zbuf[hit], 98))
        if hi - lo < 1.0:
            lo, hi = lo - 0.5, hi + 0.5
    else:
        lo, hi = z_lo, z_hi
    h = np.clip((zbuf - lo) / max(hi - lo, 1e-6), 0.0, 1.0)
    # keep the floor off the bottom of the ramp so it reads as stone, not shadow
    h = 0.25 + 0.75 * h
    # low = cool slate, high = warm stone. Reads like a relief map.
    low = np.array([54, 58, 70], dtype=np.float32)
    high = np.array([196, 184, 160], dtype=np.float32)
    rgb = low[None, None, :] + (high - low)[None, None, :] * h[:, :, None]
    rgb *= np.clip(shade, 0, 1)[:, :, None]
    out = np.zeros(rgb.shape[:2] + (4,), dtype=np.uint8)
    out[:, :, :3] = np.clip(rgb, 0, 255).astype(np.uint8)
    out[:, :, 3] = np.where(hit, 255, 0)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("bgs", nargs="*", type=int)
    ap.add_argument("--preview", action="store_true")
    ap.add_argument("--blp", action="store_true", help="also encode BLP2/DXT1 tiles")
    ap.add_argument("--band-below", type=float, default=6.0)
    ap.add_argument("--band-above", type=float, default=32.0)
    args = ap.parse_args()

    os.makedirs(OUT, exist_ok=True)
    names = {a[0]: a[3] for a in ARENAS}

    for bg, job in JOBS.items():
        if args.bgs and bg not in args.bgs:
            continue
        directory = job["dir"]
        pos, rot = wmo_world_pos(directory)
        if abs(rot[1]) > 0.01 or abs(rot[0]) > 0.01 or abs(rot[2]) > 0.01:
            print("!! %s WMO is rotated %s -- this tool only handles rotY == 0" % (directory, rot))
            continue

        floor = MEASURED[bg]["gates"][0][3] if MEASURED.get(bg, {}).get("gates") else pos[2]
        z_lo, z_hi = floor - args.band_below, floor + args.band_above

        print("=" * 70)
        print("%s (%s)" % (names[bg], directory))
        print("   WMO at (%.2f, %.2f, %.2f), rot %s" % (pos[0], pos[1], pos[2], rot))
        print("   floor %.2f -> clipping geometry to Z %.1f .. %.1f" % (floor, z_lo, z_hi))

        tris = load_triangles(job["wmo"], pos)
        print("   %d triangles" % len(tris))

        col_lo, col_hi, row_lo, row_hi = tiles_covering(tris)
        print("   covers tiles col %d..%d, row %d..%d" % (col_lo, col_hi, row_lo, row_hi))

        zbuf, shade, hit, kept, (W, H) = render(tris, col_lo, col_hi, row_lo, row_hi, z_lo, z_hi)
        print("   %d triangles inside the band, %.1f%% of pixels covered"
              % (kept, 100.0 * hit.mean()))

        rgba = colourise(zbuf, shade, hit, z_lo, z_hi)
        img = Image.fromarray(rgba, "RGBA")
        whole = os.path.join(OUT, "%s_whole.png" % directory)
        img.save(whole)
        print("   wrote %s (%dx%d)" % (os.path.basename(whole), W, H))

        # Overlay the surveyed positions. This is the check that the render is
        # actually aligned: the team starts and gates were measured in-game, so
        # if the geometry is placed correctly they land on the arena floor and
        # in its doorways. A rotated or offset model puts them in empty space.
        if args.preview:
            marked = img.copy()
            d = ImageDraw.Draw(marked)
            m = MEASURED.get(bg, {})
            pts = []
            if m.get("alliance"):
                pts.append((m["alliance"][:3], (80, 160, 255, 255), "A"))
                pts.append((m["horde"][:3], (255, 90, 90, 255), "H"))
            for g in m.get("gates", []):
                pts.append((g[1:4], (255, 220, 60, 255), "gate"))
            for b in m.get("buffs", []):
                pts.append((b[1:4], (140, 255, 140, 255), "buff"))
            for (wxp, wyp, _wzp), colr, _lbl in pts:
                px = (y_max_of(col_lo) - wyp) / ((col_hi - col_lo + 1) * TILE) * W
                py = (x_max_of(row_lo) - wxp) / ((row_hi - row_lo + 1) * TILE) * H
                d.ellipse([px - 4, py - 4, px + 4, py + 4], fill=colr)
            marked.save(os.path.join(OUT, "%s_marked.png" % directory))
            print("   wrote %s_marked.png (surveyed positions overlaid)" % directory)

        # a zoomed crop of just the drawn area, purely so the detail can be
        # judged by eye -- a tile is 533 yards and an arena is ~110, so at real
        # minimap density the arena is only ~50px and hard to assess
        bbox = img.getchannel("A").getbbox()
        if bbox:
            crop = img.crop(bbox)
            scale = max(1, int(512 / max(crop.size)))
            crop = crop.resize((crop.width * scale, crop.height * scale), Image.NEAREST)
            zoom = os.path.join(OUT, "%s_zoom.png" % directory)
            crop.save(zoom)
            print("   wrote %s (%dx%d, %dx magnification)"
                  % (os.path.basename(zoom), crop.width, crop.height, scale))

        n = 0
        for ri, row in enumerate(range(row_lo, row_hi + 1)):
            for ci, col in enumerate(range(col_lo, col_hi + 1)):
                tile = img.crop((ci * TILE_PX, ri * TILE_PX,
                                 (ci + 1) * TILE_PX, (ri + 1) * TILE_PX))
                if tile.getchannel("A").getbbox() is None:
                    continue          # nothing drawn on this tile
                base = "%s_%d_%d" % (directory, col, row)
                tile.save(os.path.join(OUT, base + ".png"))
                n += 1

                if args.blp:
                    # DXT1 has no usable alpha, so composite the transparent
                    # background onto black first -- otherwise unwritten pixels
                    # come out as whatever the RGB happened to be under alpha 0.
                    flat = Image.new("RGB", tile.size, (0, 0, 0))
                    flat.paste(tile, mask=tile.getchannel("A"))
                    blob = write_blp2_dxt1(flat, with_mips=False)

                    # read it back rather than trust the encoder
                    back = read_blp2(blob)
                    diff = np.abs(np.asarray(back, dtype=np.int16)
                                  - np.asarray(flat, dtype=np.int16))
                    print("      %-28s %6d bytes, round-trip max/mean error %d/%.2f"
                          % (base + ".blp", len(blob), diff.max(), diff.mean()))
                    with open(os.path.join(OUT, base + ".blp"), "wb") as fh:
                        fh.write(blob)
        print("   wrote %d non-empty tiles" % n)


if __name__ == "__main__":
    main()
