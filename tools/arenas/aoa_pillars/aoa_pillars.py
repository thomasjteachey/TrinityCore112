"""Three Drakkari columns that break line of sight in the Amphitheater of Anguish (BG 885, map 1684).

    python aoa_pillars.py layout                     # positions, orientations, heights
    python aoa_pillars.py sql > pillars.sql          # template 300406 + three spawns, both Centurion worlds
    python aoa_pillars.py dbc <dbc-dir> [--dry-run]  # GameObjectDisplayInfo 11971 + MapDifficulty 761
    python aoa_pillars.py carve <work>               # add them to a SCRATCH copy of 1684's vmaps
    python aoa_pillars.py check <old.mmtile> <new.mmtile>

The column is zuldrak_archpost_05 - the coliseum's own (zd_coliseum.wmo places 14 of them,
scale 1) - at scale 1.3: a 6.5 yd square, 15 yd tall. Layout, in the arena's team-axis frame
(u from the Horde start to the Alliance start, v square to it, origin midway between the starts):
an equilateral triangle of side 26 with one column on the team axis in front of each team's
alcove (u +-13, v 0: the starts open out of each other's sight) and the third on the long axis
at v +22.5, between the centre and Shadow Sight 2. The pit is a mirror image of itself across
the v axis (to 1.6 yd) and not across the other axis or by a half turn, so a fair layout has to
be symmetric across v - which this triangle is. Its centroid (v 7.5) is the pit's own centre:
the floor runs from v -30 to v +48.

Why each piece:
  * GameObjectDisplayInfo 11971 - no display row used this model. Client (patch-enUS-A) and
    both servers.
  * MapDifficulty 761 (1684, difficulty 0) - ObjectMgr allows a map only the spawn modes its
    MapDifficulty rows list, so without it the spawns are skipped at startup. Server only.
  * vmaps/ZulDrak_ArchPost_05_GO.m2.vmo + a GameObjectModels.dtree entry for 11971
    (tools/tanaris/build_go_vmo.py --doodad) - spell line of sight goes through the dynamic
    tree, and the server knows a gameobject's shape only through that list.
  * the navmesh carve - a gameobject is invisible to the navmesh, so the tile under the
    columns is rebuilt with them added to a scratch copy of the vmaps. ANY later regen of map
    1684's mmaps drops it. The nav model (build_nav_vmo.py) is the column's collision box plus
    horizontal plugs, or Recast leaves a walkable floor inside it.
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

# WorldSafeLocs 52626/52627 (Ascension's 2010/2011)
ALLIANCE = (5812.209961, -2977.669922)
HORDE = (5748.359863, -3007.330078)
BUFFS = ((5787.87, -3008.82, 272.86), (5767.65, -2965.29, 273.99))
SIDE = 26.0

# Lowest ground under each column's footprint (server .map terrain, measured 2026-10-15 with
# tools/arenas/topdown/gridmap.py); the column is set 5 cm into it so no slope shows a gap.
GROUND = {"alliance side": 273.367, "horde side": 272.704, "long axis": 273.253}

CX, CY = (ALLIANCE[0] + HORDE[0]) / 2, (ALLIANCE[1] + HORDE[1]) / 2
_A = math.atan2(ALLIANCE[1] - HORDE[1], ALLIANCE[0] - HORDE[0])
U = (math.cos(_A), math.sin(_A))
VV = (-math.sin(_A), math.cos(_A))
ZEROPOINT = 32.0 * 533.3333


def xy(u, v):
    return CX + u * U[0] + v * VV[0], CY + u * U[1] + v * VV[1]


def pillars():
    """[(label, x, y, z, orientation)]: each column's local +x toward the triangle's centroid."""
    corners = (("alliance side", SIDE / 2, 0.0), ("horde side", -SIDE / 2, 0.0),
               ("long axis", 0.0, SIDE * math.sqrt(3) / 2))
    gu, gv = 0.0, sum(c[2] for c in corners) / 3
    gx, gy = xy(gu, gv)
    out = []
    for label, u, v in corners:
        x, y = xy(u, v)
        o = math.atan2(gy - y, gx - x) % (2 * math.pi)
        out.append((label, x, y, GROUND[label] - 0.05, o))
    return out


def cmd_layout():
    print("centre (%.3f, %.3f), team axis %.2f deg" % (CX, CY, math.degrees(_A)))
    for label, x, y, z, o in pillars():
        print("%-14s (%.3f, %.3f, %.3f) o %.4f (%.1f deg)" % (label, x, y, z, o, math.degrees(o)))
    p = pillars()
    for i in range(3):
        for j in range(i + 1, 3):
            print("  %s - %s: %.2f yd" % (p[i][0], p[j][0], math.hypot(p[i][1] - p[j][1], p[i][2] - p[j][2])))


def cmd_sql():
    out = []
    for db in ("centurionworld", "centuriondevworld"):
        out.append("-- %s" % db)
        out.append("DELETE FROM %s.gameobject WHERE id = %d;" % (db, ENTRY))
        out.append("DELETE FROM %s.gameobject_template_addon WHERE entry = %d;" % (db, ENTRY))
        out.append("DELETE FROM %s.gameobject_template WHERE entry = %d;" % (db, ENTRY))
        out.append("INSERT INTO %s.gameobject_template (entry, type, displayId, name, IconName, castBarCaption, unk1, size, "
                   "AIName, ScriptName, VerifiedBuild) VALUES (%d, 5, %d, '%s', '', '', '', %s, '', '', 0);"
                   % (db, ENTRY, DISPLAY, NAME, SCALE))
        # 0x10 not selectable (scenery, no hand cursor) | 0x20 never despawns
        out.append("INSERT INTO %s.gameobject_template_addon (entry, faction, flags, mingold, maxgold) VALUES (%d, 0, 48, 0, 0);"
                   % (db, ENTRY))
        for label, x, y, z, o in pillars():
            out.append("INSERT INTO %s.gameobject (id, map, zoneId, areaId, spawnMask, phaseMask, position_x, position_y, "
                       "position_z, orientation, rotation0, rotation1, rotation2, rotation3, spawntimesecs, animprogress, "
                       "state, ScriptName, VerifiedBuild) VALUES (%d, %d, 0, 0, 1, 1, %.4f, %.4f, %.4f, %.6f, 0, 0, %.6f, %.6f, "
                       "120, 255, 1, '', 0); -- %s" % (db, ENTRY, MAP_ID, x, y, z, o, math.sin(o / 2), math.cos(o / 2), label))
    print("\n".join(out))


def dbc_rows():
    D.SPECS.setdefault("GameObjectDisplayInfo", (19, [1]))
    return {
        "GameObjectDisplayInfo": [[DISPLAY, GODI_MODEL] + [0] * 10 + [float(v) for v in GEO_BOX] + [0]],
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
    if not os.path.exists(os.path.join(work, "vmaps", NAV_NAME + ".vmo")):
        raise SystemExit("put %s.vmo (build_nav_vmo.py zuldrak_archpost_05.m2 <out> %s) in %s/vmaps" % (NAV_NAME, SCALE, work))
    spawns = V.read_vmtile(tile)
    if any(s["name"] == NAV_NAME for s in spawns):
        raise SystemExit("%s already holds the columns" % tile)
    tiled, (lo, hi, nodes, objs), rest = V.read_vmtree(tree)
    # The generator collects every loaded instance and never walks the tree, so the new
    # instances only need slots: grow the tree's object list, leave its nodes alone.
    lo, hi = list(lo), list(hi)
    for i, (label, x, y, z, o) in enumerate(pillars()):
        yaw = (math.degrees(o) - 180.0) % 360.0
        pos = (ZEROPOINT - x, ZEROPOINT - y, z)
        c, s = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
        corners = [(pos[0] + SCALE * (lx * c - ly * s), pos[1] + SCALE * (lx * s + ly * c), pos[2] + SCALE * lz)
                   for lx in (COLL_BOX[0], COLL_BOX[3]) for ly in (COLL_BOX[1], COLL_BOX[4]) for lz in (COLL_BOX[2], COLL_BOX[5])]
        blo = tuple(min(p[k] for p in corners) for k in range(3))
        bhi = tuple(max(p[k] for p in corners) for k in range(3))
        ref = len(objs)
        spawns.append(dict(flags=V.MOD_HAS_BOUND, adt=0, id=MAP_ID * 1000 + 101 + i, pos=pos, rot=(0.0, yaw, 0.0),
                           scale=SCALE, bound=(blo, bhi), name=NAV_NAME, ref=ref))
        objs.append(ref)
        lo = [min(lo[k], blo[k]) for k in range(3)]
        hi = [max(hi[k], bhi[k]) for k in range(3)]
        print("%-14s world (%.2f, %.2f, %.2f) yaw %.1f -> ref %d" % (label, x, y, z, yaw, ref))
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
    for lab, (x, y) in (("alliance start", ALLIANCE), ("horde start", HORDE), ("centre", (CX, CY))):
        pts.append((x, y, 273.5, lab, True))
    for i, (x, y, z) in enumerate(BUFFS, 1):
        pts.append((x, y, z, "shadow sight %d" % i, True))
    bad = 0
    for x, y, z, n, want in pts:
        b, a = on_mesh(before, x, y, z), on_mesh(after, x, y, z)
        ok = a == want
        bad += not ok
        if not ok or "middle" in n or "start" in n or "shadow" in n or "centre" in n:
            print("%-40s before %-5s after %-5s %s" % (n, b, a, "" if ok else "<-- UNEXPECTED"))
    print("polys before %d after %d; unexpected %d of %d points" % (len(before), len(after), bad, len(pts)))
    return 1 if bad else 0


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
