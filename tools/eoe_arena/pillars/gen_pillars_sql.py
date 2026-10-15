"""SQL for the Eye of Eternity arena's four line-of-sight pillars (both Centurion worlds).

Nexus_PillarA (GameObjectDisplayInfo 11970, the Nexus's own pillar - same JLO_NEX_* textures as
the Eye's floor) at scale 2.5, static `gameobject` spawns on map 1616. Layout, from
tools/eoe_arena/layout.py (PILLAR_BEARINGS / PILLAR_RADIUS) so it moves with the arena: a
square at r 20 round the centre, each pillar 45 deg off the teams' line (0, 90, 180, 270).

Fair by the half turn and by both mirrors that swap the teams: the model is symmetric only
across its own front-back plane, so every pillar faces the centre and the square maps onto
itself. Spawns put the column (base and waist, centred 0.06 model units behind the origin) on
the intended point; only the capital and spire lean forward, well above head height.

The rows only load because map 1616 has a MapDifficulty row (../eoe_dbc.py): a map with none
accepts no static spawn ("wrong spawn mask ... unsupported difficulty modes").

The guids are fixed, never AUTO_INCREMENT. A worldserver numbers the spawns it creates itself
from MAX(guid)+1 at startup, and Outdoor PvP's objects among them are DELETEd from `gameobject`
by guid when it shuts down (stock OPvPCapturePoint::DelObject -> GameObject::DeleteFromDB). A
row inserted above a running server's MAX therefore lands on those numbers and is deleted at its
next restart: on 2026-10-15 the Hellfire towers' three flags took one pillar here and two in the
Amphitheater. Safe: a guid at or below MAX as it was when the running server started, or far
beyond what it will count up to before it restarts; once a restart has loaded a row, MAX covers
it. See tools/arenas/aoa_pillars/README.md.
"""
import math, os, sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import layout as L  # noqa: E402

ENTRY = 300405
GUIDS = (5530580, 5530581, 5530582, 5530583)     # one per PILLAR_BEARINGS entry, in order
assert len(GUIDS) == len(L.PILLAR_BEARINGS)
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
    for guid, (x, y, z, o, r2, r3, c) in zip(GUIDS, spawns):
        out.append("INSERT INTO %s.gameobject (guid, id, map, zoneId, areaId, spawnMask, phaseMask, position_x, position_y, position_z, "
                   "orientation, rotation0, rotation1, rotation2, rotation3, spawntimesecs, animprogress, state, ScriptName, VerifiedBuild) "
                   "VALUES (%d, %d, %d, 0, 0, 1, 1, %.4f, %.4f, %.4f, %.6f, 0, 0, %.6f, %.6f, 120, 255, 1, '', 0); -- %s"
                   % (db, guid, ENTRY, L.MAP_ID, x, y, z, o, r2, r3, c))
print("\n".join(out))
for x, y, z, o, r2, r3, c in spawns:
    print("-- check: %-40s origin (%.2f, %.2f, %.2f) o=%.1f deg, r from centre %.2f" %
          (c, x, y, z, math.degrees(o), math.hypot(x - cx, y - cy)), file=sys.stderr)
