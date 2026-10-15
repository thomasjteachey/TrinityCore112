"""World DB rows for The Eye of Eternity arena (BG type 886, map 1616).

    python gen_eoe_sql.py <database> > eoe_world.sql
    python gen_eoe_sql.py <database> --rollback > eoe_world_rollback.sql

Idempotent: every block deletes this arena's rows first. Read by
BattlegroundMgr::LoadBattlegroundTemplates, so `.reload battleground_template`
picks up an edit; a binary without BATTLEGROUND_EOE skips the template
(CreateBattleground's default case) and logs the two buff rows as "not a
data-driven arena".
"""

import math
import sys

import layout as L


def quat(o):
    return 0.0, 0.0, math.sin(o / 2.0), math.cos(o / 2.0)


def esc(s):
    return s.replace("\\", "\\\\").replace("'", "''")


def world_sql(db):
    bg = L.BG_TYPE_ID
    ax, ay, az, ao = L.ALLIANCE_START
    hx, hy, hz, ho = L.HORDE_START
    lines = ["-- The Eye of Eternity arena: battleground %d on map %d (tools/eoe_arena)." % (bg, L.MAP_ID),
             "USE `%s`;" % db, ""]

    lines += [
        "-- Both teams start on a pad floating off the Eye; the leash holds them",
        "-- there (pylon ramps included) until the gates open.",
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
        "-- When the gates open everyone still on a pad is teleported onto the Eye,",
        "-- each team on its own side, 66 yd apart, facing the middle.",
        "DELETE FROM battleground_start_exit WHERE BgTypeId = %d;" % bg,
        "INSERT INTO battleground_start_exit (BgTypeId, TeamId, X, Y, Z, O, Radius, DelayMs, BotsOnly, Comment) VALUES",
    ]
    rows = []
    for team, (x, y, z, o), side in ((0, L.ALLIANCE_LANDING, "north-west pad (Alliance)"),
                                     (1, L.HORDE_LANDING, "south-east pad (Horde)")):
        rows.append("  (%d, %d, %.4f, %.4f, %.4f, %.5f, %g, 0, 0, '%s')"
                    % (bg, team, x, y, z, o, L.START_EXIT_RADIUS, esc("%s: %s onto the Eye" % (L.NAME, side))))
    lines.append(",\n".join(rows) + ";")
    lines.append("")

    outline = ", ".join("%.1f %.1f" % p for p in L.bounds_outline())
    lines += [
        "-- An invisible wall and lid enclose the Eye, so this is a safety net: anyone",
        "-- below MinZ (off a pad after the gates) or outside the circle is put back",
        "-- on the floor just inside it.",
        "DELETE FROM battleground_bounds WHERE BgTypeId = %d;" % bg,
        "INSERT INTO battleground_bounds (BgTypeId, MinZ, ProbeZ, KeepXY, SafeX, SafeY, SafeZ, GraceMs, Boundary, Comment)",
        "VALUES (%d, %g, %g, 0, %.3f, %.3f, %.3f, 0, '%s', '%s');"
        % (bg, L.BOUNDS_MIN_Z, L.BOUNDS_PROBE_Z, L.CENTRE_X, L.CENTRE_Y, L.FLOOR_CENTRE_Z + 0.5, outline,
           esc("%s: off the Eye (or left on a pad) - back onto it at the nearest edge" % L.NAME)),
        "",
        "-- Shadow Sight on the axis square to the teams'. No gates: the void is the gate.",
        "DELETE FROM battleground_custom_arena_object WHERE BgTypeId = %d;" % bg,
        "INSERT INTO battleground_custom_arena_object",
        "  (BgTypeId, GoEntry, X, Y, Z, Orientation, Rotation0, Rotation1, Rotation2, Rotation3, ObjectType, Comment) VALUES",
    ]
    rows = []
    for i, (entry, x, y, z) in enumerate(L.BUFFS, 1):
        q = quat(0.0)
        rows.append("  (%d, %d, %.3f, %.3f, %.3f, 0, %g, %g, %g, %g, 1, 'eyeofeternity shadowsight %d')"
                    % (bg, entry, x, y, z, q[0], q[1], q[2], q[3], i))
    lines.append(",\n".join(rows) + ";")
    lines.append("")
    return "\n".join(lines)


def rollback_sql(db):
    bg = L.BG_TYPE_ID
    return "\n".join([
        "USE `%s`;" % db,
        "DELETE FROM battleground_template WHERE ID = %d;" % bg,
        "DELETE FROM battleground_random_pool WHERE PoolBgTypeId = 6 AND MemberBgTypeId = %d;" % bg,
        "DELETE FROM battleground_start_exit WHERE BgTypeId = %d;" % bg,
        "DELETE FROM battleground_bounds WHERE BgTypeId = %d;" % bg,
        "DELETE FROM battleground_custom_arena_object WHERE BgTypeId = %d;" % bg,
        ""])


if __name__ == "__main__":
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    db = sys.argv[1]
    sys.stdout.write(rollback_sql(db) if "--rollback" in sys.argv else world_sql(db))
