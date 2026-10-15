"""The Gamesman's Hall arena (Karazhan's chess hall): every id and coordinate, in one place.

The DBC rows, the world SQL and the navmesh carve all import this.

Hall frame (measured 2026-10-15 off the live realm's map 532 collision; scratchpad survey):
    a - along the hall, toward the SW balcony (world orientation 2.243)
    b - across it, toward the SE side (world orientation 3.814)
    x = CX - 0.6225 a - 0.7826 b,   y = CY + 0.7826 a - 0.6225 b
The hall is the mirror image of itself across b = 0 (none of 3,760 floor samples differ) and,
but for 14 small props, across a = 0 - the mirror that swaps the teams. The two ends match
floor for floor out to the balconies' side arches, the long stairways and the corner landings.

    board      44.3 x 44.3 yd, 8 squares of 5.54, floor 220.67 (0.40 below the hall's 221.07)
    hall       |a| <= 38.2, |b| <= 38.8; two rows of nine 2.1 yd pillars at |b| 28.45
    corners    a short flight at each corner, |b| 21.18-25.94, bottom step at |a| 32.38, up
               8 steps to 223.66 at |a| 37.38 and through an arch in the end wall (|a| 38.25,
               3.95 yd clear) onto a corner landing (|a| 38.5-44.4, |b| 21-26.2, 223.66)
    stairways  from each corner landing a long stairway climbs along the end wall, |a|
               40.74-44.53, from 223.66 to 226.69 at |b| 7.75; arches on its hall side open
               onto the hall, 3-6 yd above the floor
    balconies  one at each end, floor 229.62, |a| 38.2-47.3, |b| <= 3.5 (7.0 past |a| 44.5).
               The front at |a| 38.2 is an arcade of three windows open to the floor (|b| < 1.12
               and 2.60-4.39; 1.08 yd column plinths between them) over an 8.55 yd drop. A
               short stair at each side goes down through an arch (the doorway |a| 40.74-44.53,
               |b| 6.9-7.75) onto the top of a long stairway
    beyond     behind the SW balcony a stair climbs to Karazhan's Gamesman's Hall Door (184276,
               a 60.7), behind the NE one a room ends at its Exit Door (184277, a -52.9)
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
BOOKCASE_DISPLAY = 11973       # GameObjectDisplayInfo, next after the Amphitheater's 11972

# gameobject_template entries and the fixed guids of the static spawns (never AUTO_INCREMENT:
# see tools/arenas/aoa_pillars/README.md - every guid here is below every running server's
# start MAX). 300408 was the corner portcullis, removed with the start teleport.
OLD_PORTCULLIS_ENTRY = 300408
HALL_DOOR_ENTRY = 300409       # display 6628 Karazahn_chessroomdoors, as 184276 / 184277
KNIGHT_ENTRY = 300410          # display 6820 statueHMpaladin
BISHOP_ENTRY = 300411          # display 1527 statueHMpriest
BOOKCASE_ENTRY = 300412        # display 11973 KarazahnBookshelfXtralarge, size 0.42 (middle window)
SIDE_DOOR_ENTRY = 300413       # display 6633 Karazahn_stageDoor, the start gates
BOOKCASE_SIDE_ENTRY = 300414   # display 11973 again, size 0.335 (the two side windows)
HALL_DOOR_GUIDS = (5530564, 5530565)
STATUE_GUID_FIRST = 5530566    # 5530566-69
BOOKCASE_GUIDS = (5530560, 5530561, 5530562, 5530563, 5530576, 5530577)   # the portcullises' four + two free

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
# Each team waits on a balcony, 4.8 yd back from its front, facing down the hall. Bookcases fill
# its three windows and a shut door each of its two side arches, so until the match begins the
# balcony and the short stairs down to those doors are all there is; the leash (StartMaxDist)
# covers both (a door is 7.9 yd from the start) and stops anyone wandering up the stair or into
# the room behind. When the match begins the doors open onto the long stairways, which lead down
# the end wall to the corner landings and the corner flights to the hall floor - or anyone can
# drop into the hall through the stairways' arches. Nothing teleports anyone.
START_A = 43.0
ALLIANCE_START = xy(START_A, 0.0) + (BALCONY_Z, facing(START_A, 0.0))       # SW balcony
HORDE_START = xy(-START_A, 0.0) + (BALCONY_Z, facing(-START_A, 0.0))        # NE balcony
START_MAX_DIST = 9.5

# The middle of each back rank, 19.39 yd from the centre. Shadow Sight sits level with it at the
# middle of the board's two side edges, halfway between the teams.
BACK_RANK = 3.5 * SQUARE
BUFFS = [(184663,) + xy(0.0, BACK_RANK) + (BOARD_Z + 0.1,),
         (184664,) + xy(0.0, -BACK_RANK) + (BOARD_Z + 0.1,)]

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


# ------------------------------------------------------------------ bookcases
# Karazhan's tall library bookcase stood in each of a balcony's three windows, against their
# inner face (the window bays are 2.6 yd deep, the side ones with a 0.76 yd rail at |a|
# 39.25-39.85), so nobody jumps or casts from a balcony down into the hall. Books on both faces:
# the hall sees them too. Each fills its window's width (2.24 and 1.79 yd); with the column
# plinths and the slender columns above them that leaves no gap wider than 0.46 yd. Local X
# along +b; the model's origin is at mid-height (z -6.66..6.64), so z is the floor plus 6.66 x
# the scale. 5.6 and 4.5 yd tall: over anyone's head and eyes, under the window arches.
BOOKCASE_MODEL = "World\\AZEROTH\\KARAZAHN\\PASSIVEDOODADS\\BookShelves\\KarazahnBookshelfXtralarge.mdx"
BOOKCASE_GEO_BOX = (-2.68, -0.76, -6.66, 2.65, 0.80, 6.64)     # the model's bounds, for the GODI row
BOOKCASE_SLOTS = [  # (label, entry, b, a, scale)
    ("middle", BOOKCASE_ENTRY, 0.0, 40.10, 0.42),
    ("+b", BOOKCASE_SIDE_ENTRY, 3.495, 40.12, 0.335),
    ("-b", BOOKCASE_SIDE_ENTRY, -3.495, 40.12, 0.335),
]


def bookcases():
    """[(label, entry, x, y, z, o, scale)]"""
    out = []
    for side, sa in (("SW", 1), ("NE", -1)):
        for label, entry, b, a, scale in BOOKCASE_SLOTS:
            x, y = xy(sa * a, b)
            out.append(("%s balcony, %s window" % (side, label), entry, x, y, BALCONY_Z + 6.66 * scale, B_ORIENT, scale))
    return out


# ------------------------------------------------------------------ side doors
# The start gates: Karazhan's own stage door (Karazahn_stageDoor, display 6633) in each of a
# balcony's two side arches. The doorway is |a| 40.74-44.53 (3.79 yd) by |b| 6.9-7.75, its floor
# 226.69 at the outer face (the foot of the short stair down from the balcony), its round head
# springing at 232.0 and peaking at 233.4-233.9. At scale 1.15 the leaf is 4.29 x 6.66: its
# edges tucked 0.25 into the jambs, its top at the crown. Local +X points out of the balcony,
# along the long stairway, so the door swings open onto that stairway's flat top (the Opened
# pose lies 4.6 yd along +X, flat against the side the hinge is on) and never into the stair
# behind it. The leaf spans local Y -1.73..2.00, so the origin sits 0.155 off the doorway's
# middle. ObjectType 0 in battleground_custom_arena_object: shut through the preparation and
# opened when the match begins.
SIDE_DOOR_DISPLAY = 6633
SIDE_DOOR_SCALE = 1.15
DOOR_A = (40.74 + 44.53) / 2
DOOR_B = 7.32
DOOR_FLOOR_Z = 226.69


def side_doors():
    """[(label, x, y, z, o)]"""
    out = []
    for side, sa in (("SW", 1), ("NE", -1)):
        for edge, sb in (("+b", 1), ("-b", -1)):
            o = B_ORIENT if sb > 0 else (B_ORIENT - math.pi) % (2 * math.pi)
            # local +Y is -a when local +X is +b, and +a when it is -b
            a = sa * DOOR_A + (0.135 if sb > 0 else -0.135) * SIDE_DOOR_SCALE
            x, y = xy(a, sb * DOOR_B)
            out.append(("%s balcony, %s arch" % (side, edge), x, y, DOOR_FLOOR_Z + 1.99 * SIDE_DOOR_SCALE, o))
    return out


# The two doors at the back of the balconies' far rooms, where Karazhan's own stand on map 532
# (guids 28214 / 28215), kept shut: on the clone nothing else closes the way on into the castle.
HALL_DOORS = [
    ("SW balcony, Gamesman's Hall Door", -11120.0, -1826.89, 241.807, 2.24275, (0.0, 0.0, 0.900699, 0.434444)),
    ("NE balcony, Gamesman's Hall Exit Door", -11048.2, -1917.13, 235.777, 2.24275, (0.0, 0.0, 0.900699, 0.434444)),
]
HALL_DOOR_SIZE = 1.02

# ------------------------------------------------------------------ bounds
# Everywhere a player can stand: the hall floor and, at each end, the balcony, the stair or room
# behind it as far as Karazhan's door, the long stairways and the corner landings. Anyone outside
# it once the match is on, or below MinZ, goes to the middle of the board (ProbeZ 0): with three
# floors stacked at the ends there is no one height to look down from for "just inside".
BOUNDS_A, BOUNDS_B = 37.8, 38.3        # the hall
END_A, END_B = 44.8, 26.3              # an end's balcony front, long stairways and corner landings
BACK_B = 7.3                           # the balcony and the stair or room behind it
BACK_A = {1: 61.2, -1: 53.4}           # as far as the hall doors (SW 60.73, NE 52.93)
BOUNDS_MIN_Z = 216.0
BOUNDS_PROBE_Z = 0.0


def bounds_outline():
    ab = [(BOUNDS_A, BOUNDS_B), (BOUNDS_A, END_B), (END_A, END_B), (END_A, BACK_B), (BACK_A[1], BACK_B),
          (BACK_A[1], -BACK_B), (END_A, -BACK_B), (END_A, -END_B), (BOUNDS_A, -END_B), (BOUNDS_A, -BOUNDS_B),
          (-BOUNDS_A, -BOUNDS_B), (-BOUNDS_A, -END_B), (-END_A, -END_B), (-END_A, -BACK_B), (-BACK_A[-1], -BACK_B),
          (-BACK_A[-1], BACK_B), (-END_A, BACK_B), (-END_A, END_B), (-BOUNDS_A, END_B), (-BOUNDS_A, BOUNDS_B)]
    return [xy(a, b) for a, b in ab]


if __name__ == "__main__":
    print("centre      (%.2f, %.2f), board %.2f, hall %.2f" % (CX, CY, BOARD_Z, HALL_Z))
    print("starts      A %s\n            H %s" % (tuple(round(v, 3) for v in ALLIANCE_START), tuple(round(v, 3) for v in HORDE_START)))
    print("buffs       %s" % [tuple(round(v, 3) for v in b) for b in BUFFS])
    for s in statues():
        print("statue      %-10s entry %d (%.3f, %.3f, %.3f) o %.4f" % s)
    for s in bookcases():
        print("bookcase    %-26s entry %d (%.3f, %.3f, %.3f) o %.4f x%.3f" % s)
    for d in side_doors():
        print("side door   %-20s (%.3f, %.3f, %.3f) o %.4f" % d)
    for d in HALL_DOORS:
        print("hall door   %s (%.2f, %.2f, %.2f)" % d[:4])
    print("bounds      %s" % ", ".join("%.1f %.1f" % p for p in bounds_outline()))
