"""The Eye of Eternity arena: every id and coordinate, in one place.

The terrain builder, the DBC rows and the world SQL all import this, so a
spawn platform cannot move in the client without its start point moving with it.

Coordinates are world (X north, Y west, Z up) unless named otherwise. The ADT
placement format stores a WMO's position as (17066.666 - Y, Z, 17066.666 - X)
and, at rotation 0, a WMO-local vertex (lx, ly, lz) lands at world
(X - lx, Y - ly, Z + lz): local X/Y are SUBTRACTED. Checked against the stock
placement's own extents box in NexusRaid_29_30.adt (to 0.01 yd).
"""

import math

# ------------------------------------------------------------------ ids
MAP_ID = 1616                  # cloned from 616, the way 1615 is 615 and 1608 is 608
SOURCE_MAP_ID = 616
SOURCE_DIR = "NexusRaid"       # Map.dbc Directory of 616
DIRECTORY = "EyeOfEternityArena"
BG_TYPE_ID = 886               # BATTLEGROUND_EOE, next after BATTLEGROUND_AOA (885)
NAME = "The Eye of Eternity"

# The terrain keeps the source's baked area id. Nothing on the arena map keys off
# it except the zone name, which is the one we want; the Coldarra graveyard that
# graveyard_zone ties to it is kept out by BattlegroundCustomArena's
# GetClosestGraveyard (null: a ghost stays in the arena, like every arena).
AREA_ID = 4500

LOADING_SCREEN = 236           # the Eye of Eternity's own

WSL_ALLIANCE = 52628           # next after the ported arenas' 52600-52627
WSL_HORDE = 52629
PVPDIFF_FIRST = 93244          # 16 brackets, next after Temple of Kotmogu's 93243
WSUI_GREEN = 90128             # next after the ported arenas' 90100-90127
WSUI_GOLD = 90129
LIGHT_ID = 3616                # free in patch-enUS-A (max 3566), world-terrain and both servers
LIGHT_SOURCE = 1773            # the Eye's own map-wide light: NexusRaid_StarrySky

# ---------------------------------------------------------- the client files
PLATFORM_WMO = "world\\wmo\\dungeon\\nexus\\platforms\\nexus_raid_floating_platform.wmo"
PAD_WMO = "World\\wmo\\Dungeon\\Nexus\\Platforms\\nexus_floating_platform_01_small.wmo"

# The Eye's floor: the stock placement, read out of NexusRaid_29_30.adt.
PLATFORM_ADT_POS = (15765.7958984375, 256.24853515625, 16312.3203125)
ZEROPOINT = 32.0 * 533.3333


def adt_to_world(px, py, pz):
    return ZEROPOINT - pz, ZEROPOINT - px, py


def world_to_adt(x, y, z):
    return ZEROPOINT - y, z, ZEROPOINT - x


CENTRE_X, CENTRE_Y, PLATFORM_ORIGIN_Z = adt_to_world(*PLATFORM_ADT_POS)

# Heights of the Eye's floor in its own WMO frame, measured off the collision
# mesh (README.md): 9.92 out to r 28, 10.99 from r 33 to 45, 11.98 from r 48 to
# the rim at r 55. An invisible collision wall (18 sides, 54-56 yd out) rises
# 32 yd above the floor under a sloped invisible lid: nobody leaves the floor.
FLOOR_CENTRE_Z = PLATFORM_ORIGIN_Z + 9.92
FLOOR_RING_Z = PLATFORM_ORIGIN_Z + 10.99
WALL_RADIUS = 54.0             # the wall's nearest point

# ------------------------------------------------------------- the two pads
# nexus_floating_platform_01_small.wmo, a Coldarra pad: flat floor at local
# z 5.535 out to r 15 (r 17 on the diagonals), four pylon ramps out to r 22 on
# its own axes. Placed unrotated, so the pylons point along world X and Y and the
# view to the Eye, along a diagonal, runs between two of them.
PAD_FLOOR_LOCAL_Z = 5.535
PAD_DISTANCE = 115.0           # pad centre from the Eye's centre: clear of the
                               # diagonal stubs (r 110, z <= -5 in the Eye's frame)
PAD_FLOOR_Z = 282.0            # ~14 yd above the Eye's floor: they look down onto it
PAD_ORIGIN_Z = PAD_FLOOR_Z - PAD_FLOOR_LOCAL_Z

# Bearings in world space, counter-clockwise from +X (north). 45 = north-west.
ALLIANCE_BEARING = 45.0
HORDE_BEARING = 225.0


def _at(bearing_deg, distance):
    a = math.radians(bearing_deg)
    return CENTRE_X + distance * math.cos(a), CENTRE_Y + distance * math.sin(a)


def _facing_centre(x, y):
    o = math.atan2(CENTRE_Y - y, CENTRE_X - x)
    return o % (2 * math.pi)


ALLIANCE_PAD = _at(ALLIANCE_BEARING, PAD_DISTANCE)
HORDE_PAD = _at(HORDE_BEARING, PAD_DISTANCE)

# Unique ids for the two new placements (any value no other placement on this
# map uses; the client and the vmap extractor both de-duplicate by it).
PAD_UNIQUE_IDS = (3616001, 3616002)

# ----------------------------------------------------------- starts and exits
START_LIFT = 0.25
ALLIANCE_START = (ALLIANCE_PAD[0], ALLIANCE_PAD[1], PAD_FLOOR_Z + START_LIFT, _facing_centre(*ALLIANCE_PAD))
HORDE_START = (HORDE_PAD[0], HORDE_PAD[1], PAD_FLOOR_Z + START_LIFT, _facing_centre(*HORDE_PAD))

# Each team comes down on its own side of the Eye, facing the middle: 66 yd apart.
LANDING_DISTANCE = 33.0
_al = _at(ALLIANCE_BEARING, LANDING_DISTANCE)
_hl = _at(HORDE_BEARING, LANDING_DISTANCE)
ALLIANCE_LANDING = (_al[0], _al[1], FLOOR_RING_Z + START_LIFT, _facing_centre(*_al))
HORDE_LANDING = (_hl[0], _hl[1], FLOOR_RING_Z + START_LIFT, _facing_centre(*_hl))

START_MAX_DIST = 24.0          # the pad, pylon ramps included; off the edge is further
START_EXIT_RADIUS = 25.0

# Shadow Sight, on the axis square to the teams' so neither side is nearer.
BUFFS = [
    (184663,) + _at(ALLIANCE_BEARING + 90.0, LANDING_DISTANCE) + (FLOOR_RING_Z,),
    (184664,) + _at(HORDE_BEARING + 90.0, LANDING_DISTANCE) + (FLOOR_RING_Z,),
]

# battleground_bounds: below MinZ (well under both pads and the Eye's floor) or
# outside the circle, back onto the Eye. ProbeZ looks for the floor from under
# the invisible lid (which starts 32 yd above the floor at the wall).
BOUNDS_MIN_Z = 240.0
BOUNDS_PROBE_Z = 285.0
BOUNDS_RADIUS = 50.0
BOUNDS_POINTS = 36


def bounds_outline():
    pts = []
    for i in range(BOUNDS_POINTS):
        a = 2 * math.pi * i / BOUNDS_POINTS
        pts.append((CENTRE_X + BOUNDS_RADIUS * math.cos(a), CENTRE_Y + BOUNDS_RADIUS * math.sin(a)))
    return pts


if __name__ == "__main__":
    print("centre      %.3f %.3f  floor %.2f / %.2f" % (CENTRE_X, CENTRE_Y, FLOOR_CENTRE_Z, FLOOR_RING_Z))
    print("pads        A %.3f %.3f   H %.3f %.3f   floor %.2f (origin %.3f)" % (ALLIANCE_PAD + HORDE_PAD + (PAD_FLOOR_Z, PAD_ORIGIN_Z)))
    print("starts      A %s\n            H %s" % (tuple(round(v, 4) for v in ALLIANCE_START), tuple(round(v, 4) for v in HORDE_START)))
    print("landings    A %s\n            H %s" % (tuple(round(v, 4) for v in ALLIANCE_LANDING), tuple(round(v, 4) for v in HORDE_LANDING)))
    print("buffs       %s" % [tuple(round(v, 3) for v in b) for b in BUFFS])
