"""The Gamesman's Hall arena's navmesh: carve the doors and statues into map 1532's hall tile.

    python chess_nav.py carve <work>                   # add them to a SCRATCH copy of the vmaps
    python chess_nav.py check <old.mmtile> <new.mmtile>

<work> holds the four M2s (KarazahnPortcullis, Karazahn_chessroomdoors, statueHMpaladin,
statueHMpriest from the client) and vmaps/ with copies of 1532.vmtree and 1532_35_52.vmtile
(the hall's tile; 532's own, renamed) plus each model's <name>_nav.vmo from
tools/eoe_arena/pillars/build_nav_vmo.py at the scale it is spawned at. The generator then runs in
a server dir holding these vmaps, maps/1532*.map and dbc/:
    mmaps_generator 1532 --tile 35,52 --allowDebug
(the realm's binary; it skips a tile whose .mmtile already exists - delete it first). A
gameobject is invisible to the navmesh, so without this a bot or pet walks straight through a
closed portcullis or a statue. ANY later regen of 1532's mmaps drops the carve.
"""
import math
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "..", "eoe_arena", "pillars"))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tanaris"))
sys.path.insert(0, os.path.join(HERE, "..", "topdown"))
import chess_layout as L  # noqa: E402
import vmtile as V  # noqa: E402

TILE = "%d_35_52.vmtile" % L.MAP_ID
ZEROPOINT = 32.0 * 533.3333


def items():
    """[(label, x, y, z, o, model, scale)] for everything carved."""
    out = []
    for label, entry, x, y, z, o in L.statues():
        model = "statueHMpaladin" if entry == L.KNIGHT_ENTRY else "statueHMpriest"
        out.append((label + " statue", x, y, z, o, model, L.STATUE_SCALE))
    for label, x, y, z, o in L.portcullises():
        out.append((label + " portcullis", x, y, z, o, "KarazahnPortcullis", 1.0))
    for label, x, y, z, o, rot in L.HALL_DOORS:
        out.append((label, x, y, z, o, "Karazahn_chessroomdoors", L.HALL_DOOR_SIZE))
    return out


def cmd_carve(work):
    import build_go_vmo as B
    tile = os.path.join(work, "vmaps", TILE)
    tree = os.path.join(work, "vmaps", "%d.vmtree" % L.MAP_ID)
    spawns = V.read_vmtile(tile)
    ours = set(L.MAP_ID * 1000 + 101 + i for i in range(len(items())))
    if any(s["id"] in ours or s["name"].endswith("_nav") for s in spawns):
        raise SystemExit("%s already holds the carve or uses one of the ids %s - start from the realm's copy" % (tile, sorted(ours)))
    tiled, (lo, hi, nodes, objs), rest = V.read_vmtree(tree)
    # The generator collects every loaded instance and never walks the tree: the new instances
    # only need slots in the object list.
    lo, hi = list(lo), list(hi)
    boxes = {}
    for i, (label, x, y, z, o, model, scale) in enumerate(items()):
        nav = model + "_nav"
        if not os.path.exists(os.path.join(work, "vmaps", nav + ".vmo")):
            raise SystemExit("put %s.vmo (build_nav_vmo.py %s.m2 <out> %s) in %s/vmaps" % (nav, model, scale, work))
        if model not in boxes:
            verts, tris = B.read_collision(os.path.join(work, model + ".m2"))
            boxes[model] = [min(v[k] for v in verts) for k in range(3)] + [max(v[k] for v in verts) for k in range(3)]
        box = boxes[model]
        yaw = (math.degrees(o) - 180.0) % 360.0
        pos = (ZEROPOINT - x, ZEROPOINT - y, z)
        c, s = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
        corners = [(pos[0] + scale * (lx * c - ly * s), pos[1] + scale * (lx * s + ly * c), pos[2] + scale * lz)
                   for lx in (box[0], box[3]) for ly in (box[1], box[4]) for lz in (box[2], box[5])]
        blo = tuple(min(p[k] for p in corners) for k in range(3))
        bhi = tuple(max(p[k] for p in corners) for k in range(3))
        ref = len(objs)
        spawns.append(dict(flags=V.MOD_HAS_BOUND, adt=0, id=L.MAP_ID * 1000 + 101 + i, pos=pos, rot=(0.0, yaw, 0.0),
                           scale=scale, bound=(blo, bhi), name=nav, ref=ref))
        objs.append(ref)
        lo = [min(lo[k], blo[k]) for k in range(3)]
        hi = [max(hi[k], bhi[k]) for k in range(3)]
        print("%-42s (%.2f, %.2f, %.2f) yaw %.1f %s x%.2f -> ref %d" % (label, x, y, z, yaw, nav, scale, ref))
    V.write_vmtile(tile, spawns)
    V.write_vmtree(tree, tiled, (tuple(lo), tuple(hi), nodes, objs), rest)
    print("%s now %d spawns; tree objects %d" % (TILE, len(V.read_vmtile(tile)), len(V.read_vmtree(tree)[1][3])))


def inside(x, y, poly):
    c, n = False, len(poly)
    for i in range(n):
        x1, y1 = poly[i]; x2, y2 = poly[(i + 1) % n]
        if (y1 > y) != (y2 > y) and x < (x2 - x1) * (y - y1) / (y2 - y1) + x1:
            c = not c
    return c


def load(path):
    from navmesh import NavMesh
    tmp = tempfile.mkdtemp()
    try:
        shutil.copy(path, os.path.join(tmp, "%d5235.mmtile" % L.MAP_ID))
        return NavMesh(tmp, L.MAP_ID)
    finally:
        shutil.rmtree(tmp)


def cmd_check(old, new):
    """One walk from the middle of the board over each tile. After the carve: both landings and
    both Shadow Sights are in it, the statues, portcullises and doors are off the mesh, nothing
    in it lies outside the bounds outline (the balconies and the rest of Karazhan are cut off),
    each corner flight is walkable to its portcullis and its landing beyond is not reachable."""
    poly = L.bounds_outline()
    bad = 0
    for tag, path in (("before", old), ("after", new)):
        nm = load(path)
        cent = {g: (v[:, 2].mean(), v[:, 0].mean()) for g, v, area, fl in nm.iter_polys()}

        def find(x, y, z, dz=2.0):
            return nm.find_poly_world(x, y, z, dz)

        reach = nm.reach(find(L.CX, L.CY, L.BOARD_Z))
        outside = [g for g in reach if g in cent and not inside(cent[g][0], cent[g][1], poly)]
        print("%-6s %5d polys reachable from the middle of the board, %d of them outside the bounds outline"
              % (tag, len(reach), len(outside)))
        if tag == "before":
            continue
        if outside:
            bad += 1
            print("    <-- UNEXPECTED: still a way out, e.g. poly at (%.1f, %.1f)" % cent[outside[0]])
        named = [("alliance landing", L.ALLIANCE_LANDING[:3]), ("horde landing", L.HORDE_LANDING[:3])] + \
                [("shadow sight %d" % b[0], b[1:4]) for b in L.BUFFS]
        for name, (x, y, z) in named:
            ok = find(x, y, z) in reach
            bad += not ok
            print("    %-36s %s" % (name, "reachable" if ok else "CUT OFF <-- UNEXPECTED"))
        for label, x, y, z, o, model, scale in items():
            g = find(x, y, z + 0.5, 1.0)
            ok = g is None or g not in reach
            bad += not ok
            print("    %-36s middle %s" % (label, "off the mesh" if g is None else ("cut off" if g not in reach else "REACHABLE <-- UNEXPECTED")))
        for side, a in (("SW", 1), ("NE", -1)):
            for edge, b in (("SE", 1), ("NW", -1)):
                below = L.xy(a * 34.5, b * L.ARCH_B)          # on the flight, 2 steps up
                beyond = L.xy(a * 40.0, b * L.ARCH_B)         # the corner landing behind the portcullis
                gb, gl = find(below[0], below[1], 222.3, 1.5), find(beyond[0], beyond[1], L.FLIGHT_TOP_Z, 1.5)
                ok = gb in reach and (gl is None or gl not in reach)
                bad += not ok
                print("    %s end %s corner flight: on the stair %s, beyond the portcullis %s%s" % (
                    side, edge, "reachable" if gb in reach else "CUT OFF", "off" if gl is None else
                    ("cut off" if gl not in reach else "REACHABLE"), "" if ok else "  <-- UNEXPECTED"))
    return 1 if bad else 0


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "carve":
        cmd_carve(sys.argv[2])
    elif cmd == "check":
        sys.exit(cmd_check(sys.argv[2], sys.argv[3]))
    else:
        raise SystemExit(__doc__)
