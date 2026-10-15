"""Composite each arena's ADT minimap tiles around its play area and mark the team starts."""
import io, math, sys
from PIL import Image, ImageDraw
import cl
T = 1600.0 / 3
ARENAS = {  # bg: (map, (allyWSL, hordeWSL))
    4: (559, (929, 936)), 5: (562, (939, 940)), 8: (572, (1258, 1259)),
    10: (617, (1362, 1363)), 11: (618, (1364, 1365)), 103: (1572, (52410, 52411)),
    870: (980, (4136, 4137)), 871: (1134, (4535, 4534)),
}
for i, m in enumerate([982, 983, 984, 985, 986, 1007, 1008, 1401, 1402, 1403, 1504, 1552, 1683, 1684]):
    ARENAS[872 + i] = (m, (52600 + 2 * i, 52601 + 2 * i))


def load_common():
    mp = cl.DBC(cl.read(r"DBFilesClient\Map.dbc"))
    dirs = {r[0]: mp.s(r[1]) for r in mp.rows()}
    wsl = cl.DBC(cl.read(r"DBFilesClient\WorldSafeLocs.dbc"))
    locs = {r[0]: (r[1], cl.f32(r[2]), cl.f32(r[3]), cl.f32(r[4])) for r in wsl.rows()}
    return dirs, locs


def main(only=None):
    dirs, locs = load_common()
    trs = cl.trs_map()
    for bg, (m, (a, h)) in sorted(ARENAS.items()):
        if only and bg not in only:
            continue
        d = dirs[m]
        A = locs.get(a)
        H = locs.get(h)
        cx = (A[1] + H[1]) / 2
        cy = (A[2] + H[2]) / 2
        R = max(90, 0.5 * math.hypot(A[1] - H[1], A[2] - H[2]) + 60)
        S = None
        x0 = int((32 - (cy + R) / T)); x1 = int((32 - (cy - R) / T))
        y0 = int((32 - (cx + R) / T)); y1 = int((32 - (cx - R) / T))
        tiles = {}
        for tx in range(x0, x1 + 1):
            for ty in range(y0, y1 + 1):
                key = ("%s" + cl.BS + "map%02d_%02d.blp") % (d, tx, ty)
                hsh = trs.get(key.lower())
                if not hsh:
                    tiles[(tx, ty)] = None
                    continue
                b = cl.read("textures" + cl.BS + "Minimap" + cl.BS + hsh)
                if b is None:
                    tiles[(tx, ty)] = 'missing'
                    continue
                im = Image.open(io.BytesIO(b)); im.load(); im = im.convert('RGB')
                S = im.size[0]
                tiles[(tx, ty)] = im
        S = S or 256
        big = Image.new('RGB', ((x1 - x0 + 1) * S, (y1 - y0 + 1) * S), (255, 0, 255))
        for (tx, ty), im in tiles.items():
            if isinstance(im, Image.Image):
                big.paste(im.resize((S, S)), ((tx - x0) * S, (ty - y0) * S))

        def px(wx, wy):
            return ((32 - wy / T - x0) * S, (32 - wx / T - y0) * S)
        u0, v0 = px(cx + R, cy + R)
        u1, v1 = px(cx - R, cy - R)
        crop = big.crop((int(u0), int(v0), int(u1), int(v1)))
        scale = 600 / crop.size[0]
        crop = crop.resize((600, int(crop.size[1] * scale)), Image.LANCZOS)
        dr = ImageDraw.Draw(crop)
        for (L, col) in ((A, (60, 120, 255)), (H, (255, 60, 60))):
            u, v = px(L[1], L[2]); u = (u - u0) * scale; v = (v - v0) * scale
            dr.ellipse((u - 6, v - 6, u + 6, v + 6), outline=col, width=3)
        status = {k: ('ok' if isinstance(v, Image.Image) else v) for k, v in tiles.items()}
        print(bg, m, d, 'tilepx', S, 'R', round(R), 'tiles', status)
        crop.save('mm/mm_%d_%d.png' % (bg, m))


if __name__ == '__main__':
    main(set(int(x) for x in sys.argv[1:]) or None)
