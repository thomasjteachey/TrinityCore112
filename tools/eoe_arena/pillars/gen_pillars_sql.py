"""SQL for the Eye of Eternity arena's three line-of-sight pillars (both Centurion worlds).

Nexus_PillarA (GameObjectDisplayInfo 11970, the Nexus's own pillar - same JLO_NEX_* textures as
the Eye's floor) at scale 2.5, static `gameobject` spawns on map 1616. Layout, from
tools/eoe_arena/layout.py (PILLAR_BEARINGS / PILLAR_RADIUS) so it moves with the arena: an
equilateral triangle at r 20 round the centre, one corner on the square axis (315) and the
other two mirrored across it (75 and 195).

Fair by the mirror across the 135/315 axis, the one that swaps the teams: the model is
symmetric only across its own front-back plane, so every pillar faces the centre - the corner
on the axis has its plane on it, and the other two are each other's mirror images. Spawns put
the column (base and waist, centred 0.06 model units behind the origin) on the intended point;
only the capital and spire lean forward, well above head height.

The rows only load because map 1616 has a MapDifficulty row (../eoe_dbc.py): a map with none
accepts no static spawn ("wrong spawn mask ... unsupported difficulty modes").
"""
import math, os, sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import layout as L  # noqa: E402

ENTRY = 300405
DISPLAY = 11970
SCALE = 2.5
NAME = "Nexus Pillar"
BBOX = (-1.6639766693115234, -1.1887866258621216, -0.05296637490391731,
        1.663975715637207, 1.80129873752594, 5.183651924133301)

cx, cy, cz = L.CENTRE_X, L.CENTRE_Y, L.FLOOR_CENTRE_Z
lcx = 0.0
lcy = -0.06 * SCALE


def spawn(px, py, orientation_deg, comment):
    o = math.radians(orientation_deg) % (2 * math.pi)
    # world = origin + R(o) * local; put the column centre on (px, py)
    ox = px - (lcx * math.cos(o) - lcy * math.sin(o))
    oy = py - (lcx * math.sin(o) + lcy * math.cos(o))
    return ox, oy, cz, o, math.sin(o / 2), math.cos(o / 2), comment


# The model's front is its +y, so orientation = bearing + 90 turns it to the centre.
spawns = [spawn(x, y, b + 90.0, "corner at bearing %d, front to the centre" % (b % 360))
          for x, y, b in L.pillar_points()]

out = []
for db in ("centurionworld", "centuriondevworld"):
    out.append("-- %s" % db)
    out.append("DELETE FROM %s.gameobject WHERE id = %d;" % (db, ENTRY))
    out.append("DELETE FROM %s.gameobject_template_addon WHERE entry = %d;" % (db, ENTRY))
    out.append("DELETE FROM %s.gameobject_template WHERE entry = %d;" % (db, ENTRY))
    out.append("INSERT INTO %s.gameobject_template (entry, type, displayId, name, IconName, castBarCaption, unk1, size, "
               "AIName, ScriptName, VerifiedBuild) VALUES (%d, 5, %d, '%s', '', '', '', %s, '', '', 0);"
               % (db, ENTRY, DISPLAY, NAME, SCALE))
    # 0x10 not selectable (decoration, no hand cursor) | 0x20 never despawns
    out.append("INSERT INTO %s.gameobject_template_addon (entry, faction, flags, mingold, maxgold) VALUES (%d, 0, 48, 0, 0);"
               % (db, ENTRY))
    for x, y, z, o, r2, r3, c in spawns:
        out.append("INSERT INTO %s.gameobject (id, map, zoneId, areaId, spawnMask, phaseMask, position_x, position_y, position_z, "
                   "orientation, rotation0, rotation1, rotation2, rotation3, spawntimesecs, animprogress, state, ScriptName, VerifiedBuild) "
                   "VALUES (%d, %d, 0, 0, 1, 1, %.4f, %.4f, %.4f, %.6f, 0, 0, %.6f, %.6f, 120, 255, 1, '', 0); -- %s"
                   % (db, ENTRY, L.MAP_ID, x, y, z, o, r2, r3, c))
print("\n".join(out))
for x, y, z, o, r2, r3, c in spawns:
    print("-- check: %-40s origin (%.2f, %.2f, %.2f) o=%.1f deg, r from centre %.2f" %
          (c, x, y, z, math.degrees(o), math.hypot(x - cx, y - cy)), file=sys.stderr)
