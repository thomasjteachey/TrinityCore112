"""Static scenery in the Amphitheater of Anguish (BG 885, map 1684): two Drakkari columns that
break line of sight, and six Drakkari walls that close the pit's ways out.

    python aoa_pillars.py layout                     # positions, orientations, heights
    python aoa_pillars.py sql > pillars.sql          # templates 300406/300407 + eight spawns, both Centurion worlds
    python aoa_pillars.py dbc <dbc-dir> [--dry-run]  # GameObjectDisplayInfo 11971/11972 + MapDifficulty 761
    python aoa_pillars.py carve <work>               # add them to a SCRATCH copy of 1684's vmaps
    python aoa_pillars.py check <old.mmtile> <new.mmtile>

The columns are zuldrak_archpost_05 - the coliseum's own (zd_coliseum.wmo places 14 of them,
scale 1) - at scale 1.3: a 6.5 yd square, 15 yd tall. In the arena's team-axis frame (u from the
Horde start to the Alliance start, v square to it, origin midway between the starts) they stand
on the team axis in front of each team's alcove, u +-13, v 0, so the starts open out of each
other's sight, and both face the middle of the open floor (u 0, v 7.5; the floor runs v -30 to
v +48). The pit is a mirror image of itself across the v axis (to 1.6 yd) and not across the
other axis or by a half turn, so a fair layout has to be symmetric across v - which this pair
is. A third column on the long axis (v +22.5) stood for one build; owner: "only have 2".

The walls are zuldrak_ruin_15, a block of the troll masonry the coliseum is built of (the yellow
frieze), at scale 0.42: 10.9 x 2.6 x 5.8 yd with plain box collision, nothing on it to climb.
  * The stairs. Each team's alcove has a ramp either side, 7.9 yd wide, cut into the inside of
    the ring wall and climbing 0.8 yd per yard from the pit floor (273.5) to the top of the ring
    (286.5): wall on one side, open edge over the pit on the other. The start pens' stair doors
    only part each ramp's foot from the alcove, and the foot is flush with the pit floor, so they
    never kept anyone off the stairs. A wall stands across each ramp where its surface reaches
    277.5, about 5 yd up; from there on the open edge is 4+ yd above the pit floor, out of reach
    of a jump (apex about 1.6 yd). Its +X end is 0.15 yd past the open edge and 2.85 yd of it is
    buried in the ring wall; its bottom is 0.1 below the ramp at its downhill face.
  * The tunnels. The pit's short end (v -32) has a corridor under the ring at each corner, both
    leading east and down out of the bounds outline (the leash pulled anyone who went in). A wall
    plugs each along the line of the east block's west face, from its corner to the pit's long
    wall (9.0 and 8.8 yd), its front face flush with that line, 0.9-1.0 yd into the walls at
    each end.
Measured from the server collision on 2026-10-15: the ramps' steepest-ascent axes and
cross-sections every 0.05 yd, and the corridor mouths from the navmesh edges that cross the
bounds outline. A first try put the Black Temple gate across the ramps; the owner found it
jarring.

Why each piece:
  * GameObjectDisplayInfo 11971 (column) and 11972 (wall) - no display row used either model.
    Client (patch-enUS-A) and both servers.
  * MapDifficulty 761 (1684, difficulty 0) - ObjectMgr allows a map only the spawn modes its
    MapDifficulty rows list, so without it the spawns are skipped at startup. Server only.
  * vmaps/<model>_GO.m2.vmo + a GameObjectModels.dtree entry per display
    (tools/tanaris/build_go_vmo.py --doodad) - spell line of sight, Blink and charges go through
    the dynamic tree, and the server knows a gameobject's shape only through that list.
  * the navmesh carve - a gameobject is invisible to the navmesh, so the tile under them is
    rebuilt with them added to a scratch copy of the vmaps. ANY later regen of map 1684's mmaps
    drops it. The nav models (build_nav_vmo.py) are each model's collision plus horizontal
    plugs, or Recast leaves a walkable floor inside them.

The guids are fixed, never AUTO_INCREMENT. A worldserver numbers the spawns it creates itself
from MAX(guid)+1 at startup, and Outdoor PvP's objects among them are DELETEd from `gameobject`
by guid when it shuts down (stock OPvPCapturePoint::DelObject -> GameObject::DeleteFromDB). A
row inserted above a running server's MAX therefore lands on those numbers and is deleted at its
next restart: on 2026-10-15 the Hellfire towers' three flags (counter+1, +3, +5) took two of
these columns and one of the Eye of Eternity's. Safe: a guid at or below MAX as it was when the
running server started (live, 5530585), or far beyond what it will count up to before it
restarts (CenturionDev started near 5331214). Once a restart has loaded a row, MAX covers it.
"""
import math
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "eoe_arena"))
sys.path.insert(0, os.path.join(HERE, "..", "..", "eoe_arena", "pillars"))
import eoe_dbc as D  # noqa: E402
import vmtile as V  # noqa: E402
from dtile import sections  # noqa: E402

MAP_ID = 1684
BG_TYPE_ID = 885
ENTRY = 300406
DISPLAY = 11971
MAPDIFF_ID = 761
NAME = "Drakkari Pillar"
SCALE = 1.3
GODI_MODEL = "World\\Expansion02\\Doodads\\ZulDrak\\TrollRuins\\ZulDrak_ArchPost_05.mdx"
GEO_BOX = (-2.561, -2.846, 0.031, 2.577, 2.905, 11.805)       # render bounds, model units
COLL_BOX = (-2.529, -2.534, 0.0, 2.491, 2.452, 9.871)         # the M2's collision box
NAV_NAME = "ZulDrak_ArchPost_05_nav"                          # no ".m2": keep the M2's own winding
NAV_TILE = "1684_37_21.vmtile"                                # holds the whole pit
GUIDS = {"alliance side": 5530584, "horde side": 5530585}

WALL_ENTRY = 300407
WALL_DISPLAY = 11972
WALL_NAME = "Drakkari Wall"
WALL_SCALE = 0.42
WALL_MODEL = "World\\Expansion02\\Doodads\\ZulDrak\\TrollRuins\\ZulDrak_Ruin_15.mdx"
WALL_BOX = (-13.003, -3.098, -1.759, 12.939, 3.070, 12.000)   # render bounds = collision box, model units
WALL_NAV = "ZulDrak_Ruin_15_nav"
RAMP_SURFACE = 277.5                                          # where each ramp wall stands
# (label, guid, x, y, z, o, uphill bearing of the ramp in degrees, or None for a tunnel plug).
# Ramp walls: local +X toward the open edge. Tunnel plugs: local +Y into the corridor.
WALLS = (
    ("Alliance stairs-left", 5530570, 5809.628, -2964.233, 277.063, 3.58631, 115.5),
    ("Alliance stairs-right", 5530571, 5820.891, -2987.817, 277.074, 3.56582, -65.7),
    ("Horde stairs-left", 5530572, 5750.715, -3021.177, 277.024, 0.44028, -64.8),
    ("Horde stairs-right", 5530573, 5739.427, -2997.602, 277.136, 0.44534, 115.5),
    ("Tunnel north-east", 5530574, 5815.730, -3011.305, 273.377, 3.59532, None),
    ("Tunnel south-east", 5530575, 5772.345, -3032.040, 273.505, 3.58645, None),
)

# WorldSafeLocs 52626/52627 (Ascension's 2010/2011)
ALLIANCE = (5812.209961, -2977.669922)
HORDE = (5748.359863, -3007.330078)
BUFFS = ((5787.87, -3008.82, 272.86), (5767.65, -2965.29, 273.99))
SIDE = 26.0
# battleground_bounds.Boundary of 885 - the leash. The check wants nothing reachable outside it.
BOUNDARY = ("5824.5 -2988.5, 5817.5 -2993.0, 5820.5 -3000.0, 5817.0 -3003.5, 5822.0 -3011.0, "
            "5814.5 -3014.5, 5811.5 -3013.0, 5803.5 -3017.5, 5801.0 -3014.0, 5791.0 -3015.5, "
            "5783.5 -3022.5, 5784.5 -3026.5, 5777.0 -3029.5, 5776.0 -3033.0, 5768.0 -3036.5, "
            "5765.5 -3028.0, 5763.5 -3027.0, 5760.0 -3029.0, 5756.5 -3021.5, 5749.0 -3024.5, "
            "5744.5 -3015.0, 5747.0 -3011.5, 5744.0 -3006.5, 5741.0 -3007.5, 5736.0 -2997.0, "
            "5742.5 -2992.0, 5739.5 -2985.0, 5743.5 -2982.0, 5740.5 -2977.0, 5736.0 -2978.5, "
            "5728.5 -2961.5, 5789.0 -2933.0, 5797.0 -2949.5, 5793.0 -2952.5, 5794.5 -2956.5, "
            "5796.0 -2958.0, 5800.0 -2956.5, 5804.0 -2963.5, 5811.0 -2960.5, 5816.0 -2971.0, "
            "5813.5 -2974.5, 5816.5 -2979.5, 5819.5 -2978.0")

# Lowest ground under each column's footprint (server .map terrain, measured 2026-10-15 with
# tools/arenas/topdown/gridmap.py); the column is set 5 cm into it so no slope shows a gap.
GROUND = {"alliance side": 273.367, "horde side": 272.704}

CX, CY = (ALLIANCE[0] + HORDE[0]) / 2, (ALLIANCE[1] + HORDE[1]) / 2
_A = math.atan2(ALLIANCE[1] - HORDE[1], ALLIANCE[0] - HORDE[0])
U = (math.cos(_A), math.sin(_A))
VV = (-math.sin(_A), math.cos(_A))
ZEROPOINT = 32.0 * 533.3333
FACE = (0.0, SIDE * math.sqrt(3) / 6)                         # (u, v): the middle of the open floor
REMOVED = ("long axis (removed)", 0.0, SIDE * math.sqrt(3) / 2, 273.2)


def xy(u, v):
    return CX + u * U[0] + v * VV[0], CY + u * U[1] + v * VV[1]


def pillars():
    """[(label, x, y, z, orientation)]: each column's local +x toward FACE."""
    gx, gy = xy(*FACE)
    out = []
    for label, u, v in (("alliance side", SIDE / 2, 0.0), ("horde side", -SIDE / 2, 0.0)):
        x, y = xy(u, v)
        o = math.atan2(gy - y, gx - x) % (2 * math.pi)
        out.append((label, x, y, GROUND[label] - 0.05, o))
    return out


def boundary():
    return [tuple(map(float, p.split())) for p in BOUNDARY.split(",")]


def inside_boundary(x, y, poly=None):
    poly = poly or boundary()
    c, n = False, len(poly)
    for i in range(n):
        x1, y1 = poly[i]; x2, y2 = poly[(i + 1) % n]
        if (y1 > y) != (y2 > y) and x < (x2 - x1) * (y - y1) / (y2 - y1) + x1:
            c = not c
    return c


def cmd_layout():
    print("centre (%.3f, %.3f), team axis %.2f deg" % (CX, CY, math.degrees(_A)))
    p = pillars()
    for label, x, y, z, o in p:
        print("%-14s (%.3f, %.3f, %.3f) o %.4f (%.1f deg)" % (label, x, y, z, o, math.degrees(o)))
    for i in range(len(p)):
        for j in range(i + 1, len(p)):
            print("  %s - %s: %.2f yd" % (p[i][0], p[j][0], math.hypot(p[i][1] - p[j][1], p[i][2] - p[j][2])))
    for label, guid, x, y, z, o, up, (x1, y1), (x2, y2) in wall_lines():
        print("%-22s guid %d (%.3f, %.3f, %.3f) o %.5f, long side (%.2f, %.2f) - (%.2f, %.2f), top %.2f"
              % (label, guid, x, y, z, o, x1, y1, x2, y2, z + WALL_SCALE * WALL_BOX[5]))


def wall_lines():
    """[(label, guid, x, y, z, o, uphill or None, end1, end2)]: each wall, its ramp's uphill bearing
    (radians) and the ends of its collision box's long side (local x) in world X/Y."""
    out = []
    for label, guid, x, y, z, o, up in WALLS:
        c, s = math.cos(o), math.sin(o)
        ends = [(x + WALL_SCALE * lx * c, y + WALL_SCALE * lx * s) for lx in (WALL_BOX[0], WALL_BOX[3])]
        out.append((label, guid, x, y, z, o, None if up is None else math.radians(up), ends[0], ends[1]))
    return out


def cmd_sql():
    out = []
    for db in ("centurionworld", "centuriondevworld"):
        out.append("-- %s" % db)
        for entry, kind, display, name, scale in ((ENTRY, 5, DISPLAY, NAME, SCALE),
                                                  (WALL_ENTRY, 5, WALL_DISPLAY, WALL_NAME, WALL_SCALE)):
            out.append("DELETE FROM %s.gameobject WHERE id = %d;" % (db, entry))
            out.append("DELETE FROM %s.gameobject_template_addon WHERE entry = %d;" % (db, entry))
            out.append("DELETE FROM %s.gameobject_template WHERE entry = %d;" % (db, entry))
            out.append("INSERT INTO %s.gameobject_template (entry, type, displayId, name, IconName, castBarCaption, unk1, size, "
                       "AIName, ScriptName, VerifiedBuild) VALUES (%d, %d, %d, '%s', '', '', '', %s, '', '', 0);"
                       % (db, entry, kind, display, name, scale))
            # 0x10 not selectable (scenery, no hand cursor) | 0x20 never despawns
            out.append("INSERT INTO %s.gameobject_template_addon (entry, faction, flags, mingold, maxgold) VALUES (%d, 0, 48, 0, 0);"
                       % (db, entry))
        for label, x, y, z, o in pillars():
            out.append(spawn_sql(db, GUIDS[label], ENTRY, x, y, z, o, label))
        for label, guid, x, y, z, o, up in WALLS:
            out.append(spawn_sql(db, guid, WALL_ENTRY, x, y, z, o, label + " wall"))
    print("\n".join(out))


def spawn_sql(db, guid, entry, x, y, z, o, comment):
    return ("INSERT INTO %s.gameobject (guid, id, map, zoneId, areaId, spawnMask, phaseMask, position_x, position_y, "
            "position_z, orientation, rotation0, rotation1, rotation2, rotation3, spawntimesecs, animprogress, "
            "state, ScriptName, VerifiedBuild) VALUES (%d, %d, %d, 0, 0, 1, 1, %.4f, %.4f, %.4f, %.6f, 0, 0, %.6f, %.6f, "
            "120, 255, 1, '', 0); -- %s" % (db, guid, entry, MAP_ID, x, y, z, o, math.sin(o / 2), math.cos(o / 2), comment))


def dbc_rows():
    D.SPECS.setdefault("GameObjectDisplayInfo", (19, [1]))
    return {
        "GameObjectDisplayInfo": [[DISPLAY, GODI_MODEL] + [0] * 10 + [float(v) for v in GEO_BOX] + [0],
                                  [WALL_DISPLAY, WALL_MODEL] + [0] * 10 + [float(v) for v in WALL_BOX] + [0]],
        "MapDifficulty": [D.client_row("MapDifficulty", 112, f0=MAPDIFF_ID, f1=MAP_ID)],
    }


def cmd_dbc(dbc_dir, dry_run):
    rows = dbc_rows()
    print(dbc_dir)
    for name, rs in rows.items():
        path = os.path.join(dbc_dir, name + ".dbc")
        if not os.path.exists(path):
            print("    %-22s not present here, skipped" % (name + ".dbc"))
            continue
        D.upsert(path, name, rs, dry_run)
        if not dry_run:
            D.verify(path, name, rs)


def cmd_carve(work):
    tile = os.path.join(work, "vmaps", NAV_TILE)
    tree = os.path.join(work, "vmaps", "%d.vmtree" % MAP_ID)
    for nav, m2, scale in ((NAV_NAME, "zuldrak_archpost_05.m2", SCALE), (WALL_NAV, "zuldrak_ruin_15.m2", WALL_SCALE)):
        if not os.path.exists(os.path.join(work, "vmaps", nav + ".vmo")):
            raise SystemExit("put %s.vmo (build_nav_vmo.py %s <out> %s) in %s/vmaps" % (nav, m2, scale, work))
    spawns = V.read_vmtile(tile)
    ours = set(MAP_ID * 1000 + 101 + i for i in range(3)) | set(MAP_ID * 1000 + 201 + i for i in range(len(WALLS)))
    if any(s["name"] in (NAV_NAME, WALL_NAV) or s["id"] in ours for s in spawns):
        raise SystemExit("%s already holds the columns or the walls, or uses one of the ids %s - "
                         "start from the realm's own copy" % (tile, sorted(ours)))
    tiled, (lo, hi, nodes, objs), rest = V.read_vmtree(tree)
    # The generator collects every loaded instance and never walks the tree, so the new
    # instances only need slots: grow the tree's object list, leave its nodes alone.
    lo, hi = list(lo), list(hi)
    # (label, x, y, z, o, model, scale, collision box, instance id)
    items = [(label, x, y, z, o, NAV_NAME, SCALE, COLL_BOX, MAP_ID * 1000 + 101 + i)
             for i, (label, x, y, z, o) in enumerate(pillars())]
    items += [(label, x, y, z, o, WALL_NAV, WALL_SCALE, WALL_BOX, MAP_ID * 1000 + 201 + i)
              for i, (label, guid, x, y, z, o, up) in enumerate(WALLS)]
    for label, x, y, z, o, name, scale, box, iid in items:
        yaw = (math.degrees(o) - 180.0) % 360.0
        pos = (ZEROPOINT - x, ZEROPOINT - y, z)
        c, s = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
        corners = [(pos[0] + scale * (lx * c - ly * s), pos[1] + scale * (lx * s + ly * c), pos[2] + scale * lz)
                   for lx in (box[0], box[3]) for ly in (box[1], box[4]) for lz in (box[2], box[5])]
        blo = tuple(min(p[k] for p in corners) for k in range(3))
        bhi = tuple(max(p[k] for p in corners) for k in range(3))
        ref = len(objs)
        spawns.append(dict(flags=V.MOD_HAS_BOUND, adt=0, id=iid, pos=pos, rot=(0.0, yaw, 0.0),
                           scale=scale, bound=(blo, bhi), name=name, ref=ref))
        objs.append(ref)
        lo = [min(lo[k], blo[k]) for k in range(3)]
        hi = [max(hi[k], bhi[k]) for k in range(3)]
        print("%-22s world (%.2f, %.2f, %.2f) yaw %.1f %s x%.2f -> ref %d" % (label, x, y, z, yaw, name, scale, ref))
    V.write_vmtile(tile, spawns)
    V.write_vmtree(tree, tiled, (tuple(lo), tuple(hi), nodes, objs), rest)
    print("%s now %d spawns; tree objects %d" % (NAV_TILE, len(V.read_vmtile(tile)), len(V.read_vmtree(tree)[1][3])))


def load_polys(path):
    d = open(path, "rb").read()
    m, secs, _ = sections(d)
    s = dict((n, (a, b)) for n, a, b in secs)
    verts = [struct.unpack_from("<3f", d, s["verts"][0] + 12 * i) for i in range(m["vertCount"])]
    polys = []
    for i in range(m["polyCount"]):
        p = s["polys"][0] + 32 * i
        vs = struct.unpack_from("<6H", d, p + 4)
        if (d[p + 31] >> 6) == 1:      # off-mesh connection
            continue
        polys.append([(verts[k][2], verts[k][0], verts[k][1]) for k in vs[:d[p + 30]]])
    return polys


def on_mesh(polys, x, y, z, dz=2.5):
    for poly in polys:
        if min(v[2] for v in poly) - dz <= z <= max(v[2] for v in poly) + dz:
            inside, n = False, len(poly)
            for i in range(n):
                x1, y1, _ = poly[i]; x2, y2, _ = poly[(i + 1) % n]
                if (y1 > y) != (y2 > y) and x < (x2 - x1) * (y - y1) / (y2 - y1) + x1:
                    inside = not inside
            if inside:
                return True
    return False


def cmd_check(old, new):
    before, after = load_polys(old), load_polys(new)
    pts = []
    for label, x, y, z, o in pillars():
        pts.append((x, y, z, "%s column - middle" % label, False))
        for k in range(8):
            a = k * math.pi / 4
            pts.append((x + 2.0 * math.cos(a), y + 2.0 * math.sin(a), z, "%s column - 2 yd in, dir %d" % (label, k * 45), False))
            pts.append((x + 6.5 * math.cos(a), y + 6.5 * math.sin(a), z, "%s column - 6.5 yd out, dir %d" % (label, k * 45), True))
    rx, ry = xy(REMOVED[1], REMOVED[2])
    pts.append((rx, ry, REMOVED[3], "%s - middle" % REMOVED[0], True))
    for lab, (x, y) in (("alliance start", ALLIANCE), ("horde start", HORDE), ("centre", (CX, CY))):
        pts.append((x, y, 273.5, lab, True))
    for i, (x, y, z) in enumerate(BUFFS, 1):
        pts.append((x, y, z, "shadow sight %d" % i, True))
    for label, guid, x, y, z, o, up, e1, e2 in wall_lines():
        mz = RAMP_SURFACE if up is not None else 273.5
        pts.append((x, y, mz, "%s wall - middle" % label, False))
    bad = 0
    for x, y, z, n, want in pts:
        b, a = on_mesh(before, x, y, z), on_mesh(after, x, y, z)
        ok = a == want
        bad += not ok
        if not ok or "middle" in n or "start" in n or "shadow" in n or "centre" in n:
            print("%-40s before %-5s after %-5s %s" % (n, b, a, "" if ok else "<-- UNEXPECTED"))
    print("polys before %d after %d; unexpected %d of %d points" % (len(before), len(after), bad, len(pts)))
    bad += check_walls(old, new)
    return 1 if bad else 0


def check_walls(old, new):
    """One walk over each tile from the Alliance start. After the carve: the Horde start, the
    centre and both Shadow Sights are in it; nothing in it lies outside the bounds outline (the
    ramps above their walls and both tunnels are cut off); on each ramp the stretch below its wall
    is still reachable (the foot is open to the pit floor) and 2.5, 4 and 7 yd above it is not;
    in front of each tunnel plug is reachable and 4 yd behind it is not."""
    import shutil
    import tempfile
    sys.path.insert(0, os.path.join(HERE, "..", "topdown"))
    from navmesh import NavMesh
    poly_b = boundary()
    bad = 0
    for tag, path in (("before", old), ("after", new)):
        tmp = tempfile.mkdtemp()
        try:
            shutil.copy(path, os.path.join(tmp, "%d%02d%02d.mmtile" % (MAP_ID, 21, 37)))
            nm = NavMesh(tmp, MAP_ID)
        finally:
            shutil.rmtree(tmp)
        cent = {}
        for g, v, area, fl in nm.iter_polys():
            cent[g] = (v[:, 2].mean(), v[:, 0].mean())

        def poly(x, y, z):
            return nm.find_poly_world(x, y, z, 3.0)

        reach = nm.reach(poly(ALLIANCE[0], ALLIANCE[1], 273.5))
        outside = [g for g in reach if g in cent and not inside_boundary(cent[g][0], cent[g][1], poly_b)]
        print("%-6s %5d polys reachable from the Alliance start, %d of them outside the bounds outline"
              % (tag, len(reach), len(outside)))
        if tag == "before":
            continue
        if outside:
            bad += 1
            print("    <-- UNEXPECTED: still a way out, e.g. poly at (%.1f, %.1f)" % cent[outside[0]])
        for name, (x, y, z) in [("horde start", HORDE + (273.5,)), ("centre", (CX, CY, 273.5))] + \
                [("shadow sight %d" % i, b) for i, b in enumerate(BUFFS, 1)]:
            if poly(x, y, z) not in reach:
                bad += 1
                print("    <-- UNEXPECTED: %s is cut off" % name)
        for label, guid, x, y, z, o, up, e1, e2 in wall_lines():
            if up is not None:
                ux, uy = math.cos(up), math.sin(up)
                probes = [(d, x + ux * d, y + uy * d, RAMP_SURFACE + 0.8 * d) for d in (-2.5, 2.5, 4.0, 7.0)]
            else:
                yx, yy = -math.sin(o), math.cos(o)          # local +Y, into the corridor
                probes = [(d, x + yx * d, y + yy * d, 273.5) for d in (-3.5, 4.0)]
            res = []
            for d, px, py, pz in probes:
                g = poly(px, py, pz)
                state = "off" if g is None else ("reach" if g in reach else "cut")
                ok = state == "reach" if d < 0 else state != "reach"
                bad += not ok
                res.append("%+.1f yd %s%s" % (d, state, "" if ok else " <-- UNEXPECTED"))
            print("    %-22s %s" % (label, ", ".join(res)))
    return bad


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "layout":
        cmd_layout()
    elif cmd == "sql":
        cmd_sql()
    elif cmd == "dbc":
        cmd_dbc(sys.argv[2], "--dry-run" in sys.argv)
    elif cmd == "carve":
        cmd_carve(sys.argv[2])
    elif cmd == "check":
        sys.exit(cmd_check(sys.argv[2], sys.argv[3]))
    else:
        raise SystemExit(__doc__)
