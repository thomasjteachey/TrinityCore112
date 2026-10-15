"""The Gamesman's Hall arena (Karazhan's chess hall): every id and coordinate, in one place.

The DBC rows, the world SQL and the navmesh carve all import this.

Hall frame (measured 2026-10-15 off the live realm's map 532 collision; scratchpad survey):
    a - along the hall, toward the SW balcony (world orientation 2.243)
    b - across it, toward the SE side (world orientation 3.814)
    x = CX - 0.6225 a - 0.7826 b,   y = CY + 0.7826 a - 0.6225 b
The hall is the mirror image of itself across b = 0 (none of 3,760 floor samples differ) and,
but for 14 small props, across a = 0 - the mirror that swaps the teams.

    board      44.3 x 44.3 yd, 8 squares of 5.54, floor 220.67 (0.40 below the hall's 221.07)
    hall       |a| <= 38.2, |b| <= 38.8; two rows of nine 2.1 yd pillars at |b| 28.45
    stairs     the only way up: a flight at each corner, |b| 21.18-25.94, bottom step at
               |a| 32.38, up 8 steps to 223.66 at |a| 37.38, then through an arch in the
               end wall at |a| 38.25 - 3.95 yd clear (|b| 21.58-25.53), 5.8 yd high
    balconies  one at each end, floor 229.62, |a| 38.2-47.3, |b| <= 3.5 (7.0 past |a| 44.5);
               the front at |a| 38.2 is a sheer 8.55 yd drop to the hall floor
    beyond     a door at the back of each balcony leads on into Karazhan (184276 / 184277)
"""
import math

# ------------------------------------------------------------------ ids
MAP_ID = 1532                  # cloned from 532, the way 1616 is 616 and 1608 is 608
SOURCE_MAP_ID = 532
DIRECTORY = "Karazahn"         # 532's own: the clone loads Karazhan's terrain and WMO as they are
BG_TYPE_ID = 888               # BATTLEGROUND_GMH, next after BATTLEGROUND_BHF (887)
NAME = "The Gamesman's Hall"
AREA_ID = 3457                 # Karazhan, 532's own
LOADING_SCREEN = 200           # Karazhan's

WSL_ALLIANCE = 52630           # next after the Eye of Eternity's 52628/52629
WSL_HORDE = 52631
PVPDIFF_FIRST = 93260          # 16 brackets, next after the Eye of Eternity's 93244-93259
WSUI_GREEN = 90130             # next after the Eye of Eternity's 90128/90129
WSUI_GOLD = 90131
MAPDIFF_ID = 762               # server only; next after the Amphitheater's 761

# gameobject_template entries and the fixed guids of the static spawns (never AUTO_INCREMENT:
# see tools/arenas/aoa_pillars/README.md - 5530560-69 is below every running server's start MAX)
PORTCULLIS_ENTRY = 300408      # display 6702 KarazahnPortcullis
HALL_DOOR_ENTRY = 300409       # display 6628 Karazahn_chessroomdoors, as 184276 / 184277
KNIGHT_ENTRY = 300410          # display 6820 statueHMpaladin
BISHOP_ENTRY = 300411          # display 1527 statueHMpriest
GUID_FIRST = 5530560

# ------------------------------------------------------------------ frame
CX, CY = -11080.71, -1876.25
BOARD_Z = 220.67
HALL_Z = 221.07
FLIGHT_TOP_Z = 223.66
BALCONY_Z = 229.62
SQUARE = 44.3 / 8

A_ORIENT = 2.243               # world orientation of +a
B_ORIENT = 3.814               # world orientation of +b


def xy(a, b):
    return CX - 0.6225 * a - 0.7826 * b, CY + 0.7826 * a - 0.6225 * b


def facing(a, b, ta=0.0, tb=0.0):
    """World orientation from (a, b) toward (ta, tb)."""
    x0, y0 = xy(a, b)
    x1, y1 = xy(ta, tb)
    return math.atan2(y1 - y0, x1 - x0) % (2 * math.pi)


# ------------------------------------------------------------------ starts
# Each team waits on a balcony, 4.8 yd back from its front, facing down the hall. The leash
# (StartMaxDist) covers the balcony and the arch into its stair; a jump down to the hall floor is
# 10+ yd from the start and is sent back. When the match begins, battleground_start_exit
# teleports everyone on a balcony onto their own back rank of the board.
START_A = 43.0
ALLIANCE_START = xy(START_A, 0.0) + (BALCONY_Z, facing(START_A, 0.0))       # SW balcony
HORDE_START = xy(-START_A, 0.0) + (BALCONY_Z, facing(-START_A, 0.0))        # NE balcony
START_MAX_DIST = 9.5
START_EXIT_RADIUS = 12.0

LANDING_A = 3.5 * SQUARE       # the middle of each back rank: 19.39 yd from the centre
ALLIANCE_LANDING = xy(LANDING_A, 0.0) + (BOARD_Z + 0.1, facing(LANDING_A, 0.0))
HORDE_LANDING = xy(-LANDING_A, 0.0) + (BOARD_Z + 0.1, facing(-LANDING_A, 0.0))

# Shadow Sight at the middle of the two side edges of the board, halfway between the teams.
BUFFS = [(184663,) + xy(0.0, LANDING_A) + (BOARD_Z + 0.1,),
         (184664,) + xy(0.0, -LANDING_A) + (BOARD_Z + 0.1,)]

# ------------------------------------------------------------------ statues
# Four white marble figures on the board, each on the centre of a square 1.5 squares out from the
# middle on both axes: the knights (statueHMpaladin) on the +b side, the bishops (statueHMpriest)
# on the -b side. The team swap (a -> -a) maps each onto its twin; every one faces the centre.
STATUE_SCALE = 3.5
STATUE_AB = 1.5 * SQUARE       # 8.31


def statues():
    """[(label, entry, x, y, z, o)]"""
    out = []
    for label, entry, b in (("knight", KNIGHT_ENTRY, STATUE_AB), ("bishop", BISHOP_ENTRY, -STATUE_AB)):
        for side, a in (("SW", STATUE_AB), ("NE", -STATUE_AB)):
            x, y = xy(a, b)
            out.append(("%s %s" % (side, label), entry, x, y, BOARD_Z - 0.05, facing(a, b)))
    return out


# ------------------------------------------------------------------ doors
# A portcullis in each corner flight's arch at the top of its stair (the bottom is open at the
# sides, so a door there could be stepped round). Local +X along +a; it spans local Y -2.8..3.0,
# covering the 3.95 yd arch and 0.15 yd of the pillar faces either side. Its z min is -0.2.
ARCH_A = 38.25
ARCH_B = (21.58 + 25.53) / 2


def portcullises():
    """[(label, x, y, z, o)]"""
    out = []
    for side, a in (("SW", ARCH_A), ("NE", -ARCH_A)):
        for edge, b in (("SE", ARCH_B), ("NW", -ARCH_B)):
            x, y = xy(a, b)
            out.append(("%s end, %s corner" % (side, edge), x, y, FLIGHT_TOP_Z + 0.2 - 0.05, A_ORIENT))
    return out


# The two doors at the back of the balconies, where Karazhan's own stand on map 532 (guids
# 28214 / 28215), kept shut: on the clone nothing else closes the way on into the castle.
HALL_DOORS = [
    ("SW balcony, Gamesman's Hall Door", -11120.0, -1826.89, 241.807, 2.24275, (0.0, 0.0, 0.900699, 0.434444)),
    ("NE balcony, Gamesman's Hall Exit Door", -11048.2, -1917.13, 235.777, 2.24275, (0.0, 0.0, 0.900699, 0.434444)),
]
HALL_DOOR_SIZE = 1.02

# ------------------------------------------------------------------ bounds
# The hall floor. Anyone outside it once the match is on - left on a balcony, up a stair - is put
# back just inside; below MinZ is a fall through the floor.
BOUNDS_A, BOUNDS_B = 37.8, 38.3
BOUNDS_MIN_Z = 216.0
BOUNDS_PROBE_Z = 228.0          # under the balconies (229.62), over the flights' tops (223.66)


def bounds_outline():
    return [xy(a, b) for a, b in ((BOUNDS_A, BOUNDS_B), (BOUNDS_A, -BOUNDS_B), (-BOUNDS_A, -BOUNDS_B), (-BOUNDS_A, BOUNDS_B))]


if __name__ == "__main__":
    print("centre      (%.2f, %.2f), board %.2f, hall %.2f" % (CX, CY, BOARD_Z, HALL_Z))
    print("starts      A %s\n            H %s" % (tuple(round(v, 3) for v in ALLIANCE_START), tuple(round(v, 3) for v in HORDE_START)))
    print("landings    A %s\n            H %s" % (tuple(round(v, 3) for v in ALLIANCE_LANDING), tuple(round(v, 3) for v in HORDE_LANDING)))
    print("buffs       %s" % [tuple(round(v, 3) for v in b) for b in BUFFS])
    for s in statues():
        print("statue      %-10s entry %d (%.3f, %.3f, %.3f) o %.4f" % s)
    for p in portcullises():
        print("portcullis  %-16s (%.3f, %.3f, %.3f) o %.4f" % p)
    for d in HALL_DOORS:
        print("hall door   %s (%.2f, %.2f, %.2f)" % d[:4])
    print("bounds      %s" % ", ".join("%.1f %.1f" % p for p in bounds_outline()))
