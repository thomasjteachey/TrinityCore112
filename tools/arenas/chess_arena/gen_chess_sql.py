"""World DB rows for The Gamesman's Hall arena (BG type 888, map 1532).

    python gen_chess_sql.py <database> > chess_world.sql
    python gen_chess_sql.py <database> --rollback > chess_world_rollback.sql

Idempotent: every block deletes this arena's rows first. The battleground_* rows are read by
BattlegroundMgr::LoadBattlegroundTemplates (`.reload battleground_template`); a binary without
BATTLEGROUND_GMH skips the template. The gameobject templates and the static spawns are read
once at startup; the spawns load only because map 1532 has MapDifficulty 762 (chess_dbc.py).
The spawn guids are fixed, never AUTO_INCREMENT (tools/arenas/aoa_pillars/README.md).

The gates are battleground objects, not static spawns: BattlegroundCustomArena spawns them
shut for the preparation and opens them when the match begins. Their templates have to be
loaded before a `.reload battleground_template` can name them, so a realm takes this whole file
at a restart, never at a reload.
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import chess_layout as L  # noqa: E402


def quat(o):
    return 0.0, 0.0, math.sin(o / 2.0), math.cos(o / 2.0)


def esc(s):
    return s.replace("\\", "\\\\").replace("'", "''")


TEMPLATES = [
    # entry, type, display, name, size, faction, addon flags (0x10 not selectable | 0x20 never
    # despawns; the arena code makes its gates unselectable itself, as on the other arenas' gates)
    (L.HALL_DOOR_ENTRY, 0, 6628, "Gamesman's Hall Door", L.HALL_DOOR_SIZE, 114, 48),
    (L.KNIGHT_ENTRY, 5, 6820, "Marble Knight", L.STATUE_SCALE, 0, 48),
    (L.BISHOP_ENTRY, 5, 1527, "Marble Bishop", L.STATUE_SCALE, 0, 48),
    (L.WINDOW_GATE_ENTRY, 0, L.GATE_DISPLAY, "Arena Gate (Gamesman's Hall windows)", L.WINDOW_GATE_SCALE, 114, 32),
    (L.STAIR_GATE_ENTRY, 0, L.GATE_DISPLAY, "Arena Gate (Gamesman's Hall stairways)", L.STAIR_GATE_SCALE, 114, 32),
]
# Every entry this arena has ever had, so a re-run also clears what an earlier layout left.
ALL_ENTRIES = sorted(set([t[0] for t in TEMPLATES] + list(L.OLD_ENTRIES)))


def spawns():
    """[(guid, entry, x, y, z, o, (r0, r1, r2, r3), comment)]"""
    out = []
    for guid, (label, x, y, z, o, rot) in zip(L.HALL_DOOR_GUIDS, L.HALL_DOORS):
        out.append((guid, L.HALL_DOOR_ENTRY, x, y, z, o, rot, label))
    for i, (label, entry, x, y, z, o) in enumerate(L.statues()):
        out.append((L.STATUE_GUID_FIRST + i, entry, x, y, z, o, quat(o), label + " statue"))
    assert len(out) == 2 + 4 == len(set(g[0] for g in out))
    return out


def world_sql(db):
    bg = L.BG_TYPE_ID
    ax, ay, az, ao = L.ALLIANCE_START
    hx, hy, hz, ho = L.HORDE_START
    lines = ["-- The Gamesman's Hall arena: battleground %d on map %d (tools/arenas/chess_arena)." % (bg, L.MAP_ID),
             "USE `%s`;" % db, ""]
    lines += [
        "-- Each team waits on a balcony at an end of the hall, its windows and the stairways past",
        "-- its side arches shut by gates; the leash holds it there until the match begins.",
        "DELETE FROM battleground_template WHERE ID = %d;" % bg,
        "INSERT INTO battleground_template (ID, MinPlayersPerTeam, MaxPlayersPerTeam, MinLvl, MaxLvl,",
        "  AllianceStartLoc, AllianceStartO, HordeStartLoc, HordeStartO, StartMaxDist, Weight, ScriptName, Comment)",
        "VALUES (%d, 0, 5, 10, 80, %d, %.5f, %d, %.5f, %g, 1, 'BGReplayBGScript', '%s');"
        % (bg, L.WSL_ALLIANCE, ao, L.WSL_HORDE, ho, L.START_MAX_DIST, esc(L.NAME)),
        "",
        "DELETE FROM battleground_random_pool WHERE PoolBgTypeId = 6 AND MemberBgTypeId = %d;" % bg,
        "INSERT INTO battleground_random_pool (PoolBgTypeId, MemberBgTypeId, Weight, Enabled, Comment)",
        "VALUES (6, %d, 1, 1, '%s');" % (bg, esc(L.NAME)),
        "",
        "-- No start exits: when the match begins the gates sink and everyone goes down on foot.",
        "DELETE FROM battleground_start_exit WHERE BgTypeId = %d;" % bg,
        "",
    ]

    sx, sy = L.CX, L.CY
    outline = ", ".join("%.1f %.1f" % p for p in L.bounds_outline())
    lines += [
        "-- The hall floor, and at each end the balcony and what is behind it up to Karazhan's door,",
        "-- the long stairways and the corner landings: anyone outside that once the match is on, or",
        "-- below MinZ, goes to the middle of the board (ProbeZ 0 - the ends stack three floors).",
        "DELETE FROM battleground_bounds WHERE BgTypeId = %d;" % bg,
        "INSERT INTO battleground_bounds (BgTypeId, MinZ, ProbeZ, KeepXY, SafeX, SafeY, SafeZ, GraceMs, Boundary, Comment)",
        "VALUES (%d, %g, %g, 0, %.3f, %.3f, %.3f, 0, '%s', '%s');"
        % (bg, L.BOUNDS_MIN_Z, L.BOUNDS_PROBE_Z, sx, sy, L.BOARD_Z + 0.5, outline,
           esc("%s: off the hall, its balconies and stairways - back onto the middle of the board" % L.NAME)),
        "",
        "-- The start gates (ObjectType 0, shut for the preparation, sunk at the start): one in each",
        "-- of a balcony's three windows and one across the top of each long stairway past its side",
        "-- arches. Then Shadow Sight at the middle of the board's two side edges (ObjectType 1).",
        "DELETE FROM battleground_custom_arena_object WHERE BgTypeId = %d;" % bg,
        "INSERT INTO battleground_custom_arena_object",
        "  (BgTypeId, GoEntry, X, Y, Z, Orientation, Rotation0, Rotation1, Rotation2, Rotation3, ObjectType, Comment) VALUES",
    ]
    rows = []
    for entry, gates in ((L.WINDOW_GATE_ENTRY, L.window_gates()), (L.STAIR_GATE_ENTRY, L.stair_gates())):
        for label, x, y, z, o in gates:
            r0, r1, r2, r3 = quat(o)
            rows.append("  (%d, %d, %.3f, %.3f, %.3f, %.5f, %g, %g, %.6f, %.6f, 0, '%s')"
                        % (bg, entry, x, y, z, o, r0, r1, r2, r3, esc("gamesmans hall gate, " + label)))
    for i, (entry, x, y, z) in enumerate(L.BUFFS, 1):
        rows.append("  (%d, %d, %.3f, %.3f, %.3f, 0, 0, 0, 0, 1, 1, 'gamesmans hall shadowsight %d')" % (bg, entry, x, y, z, i))
    lines.append(",\n".join(rows) + ";")
    lines.append("")

    lines += ["-- Doors that never open (the far rooms' ways on into Karazhan) and four marble chess pieces",
              "-- that break line of sight on the board; the gates' templates. Earlier layouts' entries",
              "-- (%s) go with this arena's own." % ", ".join(str(e) for e in L.OLD_ENTRIES)]
    entries = ", ".join(str(e) for e in ALL_ENTRIES)
    lines.append("DELETE FROM gameobject WHERE id IN (%s);" % entries)
    lines.append("DELETE FROM gameobject_template_addon WHERE entry IN (%s);" % entries)
    lines.append("DELETE FROM gameobject_template WHERE entry IN (%s);" % entries)
    for entry, kind, display, name, size, faction, flags in TEMPLATES:
        lines.append("INSERT INTO gameobject_template (entry, type, displayId, name, IconName, castBarCaption, unk1, size, "
                     "AIName, ScriptName, VerifiedBuild) VALUES (%d, %d, %d, '%s', '', '', '', %g, '', '', 0);"
                     % (entry, kind, display, esc(name), size))
        lines.append("INSERT INTO gameobject_template_addon (entry, faction, flags, mingold, maxgold) VALUES (%d, %d, %d, 0, 0);"
                     % (entry, faction, flags))
    for guid, entry, x, y, z, o, (r0, r1, r2, r3), comment in spawns():
        lines.append("INSERT INTO gameobject (guid, id, map, zoneId, areaId, spawnMask, phaseMask, position_x, position_y, "
                     "position_z, orientation, rotation0, rotation1, rotation2, rotation3, spawntimesecs, animprogress, state, "
                     "ScriptName, VerifiedBuild) VALUES (%d, %d, %d, 0, 0, 1, 1, %.4f, %.4f, %.4f, %.6f, %g, %g, %.6f, %.6f, "
                     "120, 255, 1, '', 0); -- %s" % (guid, entry, L.MAP_ID, x, y, z, o, r0, r1, r2, r3, esc(comment)))
    lines.append("")
    return "\n".join(lines)


def rollback_sql(db):
    bg = L.BG_TYPE_ID
    entries = ", ".join(str(e) for e in ALL_ENTRIES)
    return "\n".join([
        "USE `%s`;" % db,
        "DELETE FROM battleground_template WHERE ID = %d;" % bg,
        "DELETE FROM battleground_random_pool WHERE PoolBgTypeId = 6 AND MemberBgTypeId = %d;" % bg,
        "DELETE FROM battleground_start_exit WHERE BgTypeId = %d;" % bg,
        "DELETE FROM battleground_bounds WHERE BgTypeId = %d;" % bg,
        "DELETE FROM battleground_custom_arena_object WHERE BgTypeId = %d;" % bg,
        "DELETE FROM gameobject WHERE id IN (%s);" % entries,
        "DELETE FROM gameobject_template_addon WHERE entry IN (%s);" % entries,
        "DELETE FROM gameobject_template WHERE entry IN (%s);" % entries,
        ""])


if __name__ == "__main__":
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    db = sys.argv[1]
    sys.stdout.write(rollback_sql(db) if "--rollback" in sys.argv else world_sql(db))
