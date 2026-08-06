"""Generate the DBC-mirror and world-DB scripts for the ported arenas.

Hand-writing these would be 14 map rows, 14 battlemaster rows, 224 PvP difficulty
rows, 28 start locations, 28 world-state rows and 14 templates. Generating them
keeps the ids arithmetic instead of typed, and keeps this file as the one place
a coordinate is corrected.

Run:  python gen_arena_sql.py
Writes into sql/custom/dbc/ and sql/custom/world/.

Coordinates
-----------
centre_x / centre_y / floor_z were measured from the client terrain by
adt_probe.py + wmo_floor.py, not surveyed in-game. The method was validated
against the two arenas already live: Tol'viron's measured centre lands 1.2 yards
from the midpoint of its real start locations and its floor within 1.7 yards.

`confidence` records how good the evidence is per arena:
  high   - a collision volume, gate doodads or arena pillars fixed the position
  medium - the arena's own WMO mesh, which the two validations agree with
  low    - no arena-shaped object could be identified; this is the centre of the
           most plausible structure and WILL need .gps confirmation

Start positions live in WorldSafeLocs and gates in battleground_custom_arena_object,
both server-side tables, so correcting any of this is an UPDATE plus
`.reload battleground_template` -- never a rebuild and never a client patch.
"""

import os

OUT_DBC = r"C:\Projects\Gamedev\wow\servers\tc-lplus\sql\custom\dbc"
OUT_WORLD = r"C:\Projects\Gamedev\wow\servers\tc-lplus\sql\custom\world"
STAMP = "2026_08_06_02"

# id blocks, chosen clear of everything currently in use (checked against the
# live dbc schema). AreaTable is deliberately absent -- see the header note in
# the generated DBC file.
WSL_BASE = 52600        # WorldSafeLocs, 2 per arena
PVPDIFF_BASE = 93000    # PvpDifficulty, 16 per arena
WSUI_BASE = 90100       # WorldStateUI, 2 per arena

DOOR_GO = 185483        # "Gate", the same model the Obsidian Colosseum uses
DOOR_OFFSET = 45.0      # gates, along X from centre
SPAWN_OFFSET = 57.0     # players start behind their gate
SPAWN_Z_LIFT = 2.0      # measured floors are the mesh; stand slightly above it

LANG_MASK = 16712190
DESC_MASK = 16712188

# bg_type_id, map_id, directory, display name, centre x, centre y, floor z, confidence, evidence
ARENAS = [
    (872,  982, "coliseumarena",      "Coliseum of Past Echoes",        8077.39, -2749.79, 1242.32, "low",    "PVP_TITAN_ARENA.WMO; floor sits outside the placement bbox, so treat Z as provisional"),
    (873,  983, "nerubianarena",      "Imperial Arena of Thakraj",       544.03,  -909.11,   85.22, "medium", "NERUBIAN_ARENA.WMO mesh"),
    (874,  984, "maldraxxuscoliseum", "Maldraxxus Coliseum",            2833.00,  2253.00, 3260.30, "high",   "9NC_MALDRAXXUSARENA_LARGEPILLAR M2 bases ring this point at Z 3260.2-3260.4"),
    (875,  985, "nagrandarena2",      "Nagrand Arena (Remastered)",    -2043.54,  6654.35,   70.94, "medium", "7NG_NAGRANDARENA_PVPSTADIUM.WMO mesh"),
    (876,  986, "bladesedgearena2b",  "Blade's Edge Arena (Remastered)",2785.61,  6066.08,   84.13, "medium", "7OG_BLADESEDGEARENA_PVPSTADIUM.WMO mesh"),
    (877, 1007, "karazhanarena",      "Guardian's Hall",                 536.49,   805.68,   64.25, "medium", "KARAZHAN_ARENA.WMO, the map's only WMO"),
    (878, 1008, "ulduararena",        "Spark of Creator",                533.42,   800.12,  131.40, "medium", "ULDUAR_ARENA.WMO, the map's only WMO"),
    (879, 1401, "BaradinHoldArena",   "Baradin Hold Arena",            -1232.87,   981.33,  119.58, "low",    "no arena-named WMO; TB_TOWER is the closest structure to the tile centre and the densest doodad cluster"),
    (880, 1402, "obeliskofthestarts", "Obelisk of the Stars",          -9261.33, -1550.22,  100.44, "high",   "14 PVP_COLLISIONPANE WMOs fence a 43x34 yard box; Z is the pane base"),
    (881, 1403, "thetwistingnether",  "The Twisting Nether",            4500.37, -1273.10,  386.80, "high",   "two 7DU_TOMBOFSARGERAS_FIREDOOR01 gates at Z 386.7/386.8 either side of centre"),
    (882, 1504, "BlackrookHoldArena", "Black Rook Hold Arena",          1458.69,  1458.06,  247.18, "medium", "7VS_BLACKROOKHOLD_ARENA.WMO mesh"),
    (883, 1552, "valsharaharena",     "Ashamane's Fall",                3548.97,  5542.23,  378.48, "medium", "CUSTOM_VALSHARAH_ARENA.WMO mesh"),
    (884, 1683, "ulduaroutarena",     "The Inventor's Library",         8859.22,  -904.74,  884.09, "low",    "no arena-named WMO; ULDUAR_EXT03 origin used as a placeholder"),
    (885, 1684, "gundrakarena",       "Amphitheater of Anguish",        5798.49, -3047.38,  307.96, "medium", "ZD_COLISEUM.WMO mesh; the WMO is the whole Gundrak coliseum shell"),
]

# ---------------------------------------------------------------- area ids
#
# The zone name a player sees does NOT come from Map.dbc. The client reads the
# `areaid` baked into each MCNK terrain chunk and looks that up in AreaTable.dbc.
# Ported terrain keeps the ids it was authored with, and none of these existed
# here -- so map 982 announced itself as whatever zone the player came from.
#
# Because the ids are absent rather than taken, rows can simply be added under
# the same ids and no terrain has to be rewritten. Read out of the ADTs by
# tools/arenas/area_ids.py; first id listed covers most of the map, the rest are
# slivers along the edges.
AREA_IDS = {
    872:  [10026, 10027],
    873:  [10028, 10029],
    874:  [10032, 10033],
    875:  [10040, 10041],
    876:  [10038, 10039],
    877:  [6137],
    878:  [6138],
    879:  [10109],
    880:  [8463],     # the rest of this map is area 0 -- see the caveat below
    881:  [10051, 10052],
    882:  [10057, 10058],
    883:  [10053, 10054],
    884:  [8443],     # ditto
    885:  [8442],     # ditto, plus real Zul'Drak ids that already resolve
}

# Caveat for 880/884/885: most of their chunks carry area id 0, which is "no
# area" and cannot be given a row. Standing on those the client still shows a
# stale name. The listed ids cover the built-up part, which is where the arena
# is, so this is expected to be enough -- if not, the fix is rewriting the MCNK
# areaid fields, which area_ids.py already knows how to find.

# 3715/3716 are Tol'viron and Tiger's Peak; the highest in use is 3718, taken by
# the Violet Hold work that is live in this database right now. Starting at 3800
# leaves that room to grow. The client's explored-zone bitfield is 4096 bits.
AREABIT_BASE = 3800

# Copied from AreaTable 6296 (Tol'viron) so the arenas sound and light like the
# arenas already here.
AREA_FLAGS       = 65664
AREA_AMBIENCE    = 369
AREA_ZONEMUSIC   = 243
AREA_INTROSOUND  = 512
AREA_AMBIENT_MUL = 0.6


# ------------------------------------------------------------ surveyed data
#
# Positions measured in-game with .gps, which replace the ones derived from the
# client terrain. Filled in one arena at a time as each is walked; any arena not
# listed here still uses the derived fallback (centre +- SPAWN_OFFSET on X).
#
#   alliance/horde : (x, y, z, orientation)
#   gates          : (goEntry, x, y, z, o, rot0, rot1, rot2, rot3)
#   buffs          : (goEntry, x, y, z, o)   -- rotation derived from o
#
# Everything except 872 comes from Ascension's own WorldSafeLocs.dbc, read by
# tools/arenas/ascension_starts.py -- the exact teleport targets the source
# server used, so they are on the floor and inside the arena by construction.
# That beats anything derived from geometry, and it checks out: Ascension's 982
# starts land 1.1 yards from the values surveyed in-game here, with orientations
# agreeing to 0.02 rad.
#
# 872 keeps the in-game survey because it was measured in this client.
MEASURED = {
    874: {  # Maldraxxus Coliseum, map 984
        # starts from Ascension's WorldSafeLocs 6041/6042; the rest surveyed.
        "alliance": (2853.540039, 2185.810059, 3259.969971, 1.565400),
        "horde":    (2854.270020, 2321.080078, 3259.739990, 4.706993),
        "gates": [
            (185483, 2849.806152, 2201.163330, 3260.175049, 0.281295, 0.0, 0.0, -0.140184, -0.990125),
            (185483, 2849.402100, 2304.351318, 3259.939453, 5.991170, 0.0, 0.0, -0.145491,  0.989360),
        ],
        # Re-surveyed. The first pass had both of these 4.9 yards apart on the
        # same side of the arena; these are 88.9 apart and mirrored about
        # X 2831.9, which lines up with Ascension's own arena-centre graveyard
        # (6043, at 2832.28/2254.64). Note the centre is NOT the midpoint of the
        # team starts -- both starts sit at X ~2854, 22 yards off it.
        "buffs": [
            (184663, 2876.318359, 2252.889648, 3260.397705, 3.081219),
            (184664, 2787.438477, 2254.387939, 3260.434570, 0.002457),
        ],
        # Permanent arena furniture, not part of the match lifecycle: spawned as
        # ordinary gameobject rows rather than battleground objects, because
        # they never open, never respawn and want no code to manage them. They
        # are emitted into this script so a dev wipe does not lose them.
        "static_gameobjects": [
            (2136050, 193612, 2768.251953, 2252.592041, 3260.246338, 0.081026,
             0.0, 0.0, -0.040502, -0.999179, "Maldraxxus barricade 1"),
            (2136051, 193612, 2768.198975, 2258.288818, 3260.260254, 5.908680,
             0.0, 0.0, -0.186161,  0.982519, "Maldraxxus barricade 2"),
        ],
        "drop_gameobject_guids": [2136052, 2136053],
    },
    875: {  # Nagrand Arena (Remastered), map 985
        # starts from Ascension's WorldSafeLocs 6044/6045; the rest surveyed.
        # Gates use cloned templates 300400/300401 so they can be scaled to
        # 1.25 without touching the real Nagrand Arena's doors.
        "alliance": (-2070.389893, 6704.709961, 12.052200, 5.199065),
        "horde":    (-2016.709961, 6603.259766, 12.373800, 2.057472),
        "gates": [
            (300400, -2019.264404, 6609.552246, 12.717039, 0.400356, 0.0, 0.0, -0.198844, -0.980031),
            (300401, -2067.859375, 6699.410645, 12.658770, 3.632270, 0.0, 0.0, -0.970055,  0.242886),
        ],
        "buffs": [
            (184663, -1995.345825, 6679.221191, 13.071568, 3.616559),
            (184664, -2090.863037, 6629.114746, 12.869463, 0.722366),
        ],
        "drop_gameobject_guids": [2136054, 2136055],
    },
    876: {  # Blade's Edge Arena (Remastered), map 986
        # starts from Ascension's WorldSafeLocs 6047/6048; the rest surveyed.
        # Gates reuse the source arena's two inner doors (183971/183973) at
        # stock size, so no cloned template is needed here.
        "alliance": (2772.129883, 6060.359863, -3.068170, 4.973402),
        "horde":    (2802.149902, 5947.970215, -3.098460, 1.831809),
        "gates": [
            (183971, 2778.054688, 6053.388672, -3.145406, 2.169530, 0.0, 0.0, -0.884193, -0.467121),
            (183973, 2796.864746, 5954.017578, -3.068224, 2.322670, 0.0, 0.0, -0.917336, -0.398114),
        ],
        "buffs": [
            (184663, 2768.117432, 5986.673828, -4.405022, 0.775437),
            (184664, 2808.445801, 6020.854492, -4.124462, 3.803169),
        ],
        "drop_gameobject_guids": [2136075, 2136076],
    },
    877: {  # Guardian's Hall, map 1007
        # starts from Ascension's WorldSafeLocs 4852/4853; the rest surveyed.
        # Gate order follows the starts: gate 1 is the Alliance side (Y ~752,
        # start at Y 746), gate 2 the Horde side (Y ~859, start at Y 866).
        "alliance": (536.510010, 745.909973, 0.500000, 1.570629),
        "horde":    (536.530029, 865.880005, 0.500000, 4.712222),
        "gates": [
            (185483, 536.768921, 752.416748, 0.505373, 3.141500, 0.0, 0.0, -1.0,      -0.0000487129),
            (185483, 536.463623, 858.864014, 0.504717, 3.204290, 0.0, 0.0, -0.999509,  0.0313427),
        ],
        "buffs": [
            (184663, 564.335449, 805.874634, -0.059970, 3.188655),
            (184664, 508.639801, 805.705750, -0.059966, 6.279174),
        ],
        "drop_gameobject_guids": [2136077, 2136078],
    },
    878: {  # Spark of Creator, map 1008
        # starts from Ascension's WorldSafeLocs 4855/4856; the rest surveyed.
        # Unusual shape: both teams enter from the west wall (X ~498) at the
        # north and south ends, and both buffs sit against the east wall
        # (X ~557). Each pair is symmetric about Y ~802, and the arena WMO's
        # centre (533.4) falls between the two walls, so this is the layout, not
        # a mis-measurement.
        "alliance": (497.869995, 853.880005, 0.928000, 4.712774),
        "horde":    (497.910004, 750.059998, 0.930000, 1.571182),
        "gates": [
            (185483, 505.719635, 838.528198, 0.872084, 0.522682, 0.0, 0.0, -0.258376, -0.966044),
            (185483, 507.537842, 764.469116, 0.872084, 2.501910, 0.0, 0.0, -0.949285, -0.314417),
        ],
        "buffs": [
            (184663, 557.732117, 837.495300, 0.870770, 3.864655),
            (184664, 556.419800, 768.008728, 0.871984, 2.388106),
        ],
        "drop_gameobject_guids": [2136081, 2136082],
    },
    879: {  # Baradin Hold Arena, map 1401  (WSL 2033/2034)
        "alliance": (-1176.300049, 1043.319946, 121.000000, 3.948866),
        "horde":    (-1288.290039, 926.320007, 121.000000, 0.807273),
    },
    880: {  # Obelisk of the Stars, map 1402  (WSL 2035/2036)
        "alliance": (-9254.530273, -1473.119995, 68.000000, 4.650473),
        "horde":    (-9264.080078, -1627.160034, 68.000000, 1.508880),
    },
    881: {  # The Twisting Nether, map 1403  (WSL 2037/2038)
        "alliance": (4566.959961, -1428.469971, 387.000000, 3.136064),
        "horde":    (4431.299805, -1427.719971, 387.000000, 6.277657),
    },
    882: {  # Black Rook Hold Arena, map 1504
        # starts from Ascension's WorldSafeLocs 5121/5122; the rest surveyed.
        # FOUR gates, two per side -- the table takes any number, so this needs
        # nothing special beyond listing them. Alliance side is the pair at
        # X ~1384, Horde the pair at X ~1450-1464.
        "alliance": (1372.130005, 1247.189941, 33.004799, 0.271261),
        "horde":    (1472.699951, 1275.160034, 32.110401, 3.412853),
        "gates": [
            (185483, 1383.606689, 1258.042847, 33.250454, 5.007230, 0.0, 0.0, -0.595572,  0.803302),
            (185483, 1384.172729, 1232.460693, 33.254791, 4.426049, 0.0, 0.0, -0.800763,  0.598981),
            (185483, 1450.317993, 1277.049927, 33.233463, 4.971893, 0.0, 0.0, -0.609672,  0.792654),
            (185483, 1464.198730, 1254.328491, 33.238369, 2.442888, 0.0, 0.0, -0.939595, -0.342289),
        ],
        # Both of these sit south of the arena centre rather than either side of
        # it -- see the note in the survey summary. Applied as measured.
        "buffs": [
            (184663, 1429.629395, 1238.815552, 34.107357, 2.329053),
            (184664, 1424.596191, 1202.647827, 32.094311, 1.669335),
        ],
        "drop_gameobject_guids": [2136085, 2136086, 2136088, 2136089],
    },
    883: {  # Ashamane's Fall, map 1552
        # starts from Ascension's WorldSafeLocs 2020/2021; the rest surveyed.
        # Gates are entry 300002 "Chapel Door", an existing custom template, not
        # the 185483 gate the other arenas use. Each doorway is flanked by two
        # barricades, which are permanent scenery and stay as plain spawns.
        "alliance": (3548.389893, 5601.629883, 327.000000, 4.640550),
        "horde":    (3539.080078, 5472.259766, 327.000000, 1.498958),
        "gates": [
            (300002, 3539.142334, 5482.908203, 325.489502, 4.774114, 0.0, 0.0, -0.684951, 0.728590),
            (300002, 3548.853760, 5591.532715, 325.513580, 4.683782, 0.0, 0.0, -0.717148, 0.696921),
        ],
        "buffs": [
            (184663, 3493.611572, 5540.871094, 323.020508, 6.160314),
            (184664, 3604.439697, 5532.029785, 325.363953, 3.042288),
        ],
        "static_gameobjects": [
            (2136097, 193612, 3544.428467, 5481.986816, 325.499939, 1.334063,
             0.0, 0.0, -0.618657, -0.785662, "Ashamane's Fall barricade, Horde gate E"),
            (2136098, 193612, 3533.850098, 5481.982910, 325.499786, 1.813156,
             0.0, 0.0, -0.787399, -0.616444, "Ashamane's Fall barricade, Horde gate W"),
            (2136100, 193612, 3553.842529, 5591.891602, 325.507080, 5.017581,
             0.0, 0.0, -0.591407,  0.806373, "Ashamane's Fall barricade, Alliance gate E"),
            (2136101, 193612, 3543.508789, 5591.613281, 325.506836, 4.589548,
             0.0, 0.0, -0.749177,  0.662370, "Ashamane's Fall barricade, Alliance gate W"),
        ],
        "drop_gameobject_guids": [2136096, 2136099],
    },
    884: {  # The Inventor's Library, map 1683  (WSL 2013/2014)
        "alliance": (8115.540039, -960.760010, 958.000000, 1.318947),
        "horde":    (8165.160156, -767.919983, 958.000000, 4.460540),
    },
    885: {  # Amphitheater of Anguish, map 1684  (WSL 2010/2011)
        "alliance": (5812.209961, -2977.669922, 274.000000, 3.576462),
        "horde":    (5748.359863, -3007.330078, 274.000000, 0.434869),
    },
    873: {  # Imperial Arena of Thakraj, map 983
        # starts from Ascension's WorldSafeLocs 6066/6067; gates and buffs
        # surveyed in-game. The gates sit just inside the starts, which is the
        # arrangement every arena here uses.
        "alliance": (483.440002, -878.169983, 27.520000, 5.810376),
        "horde":    (604.260010, -939.969971, 27.520000, 2.668784),
        "gates": [
            (185483, 488.070007, -878.305847, 27.709145, 4.750093, 0.0, 0.0, -0.693652, 0.720311),
            (185483, 599.510742, -941.209656, 27.702806, 4.714780, 0.0, 0.0, -0.706261, 0.707952),
        ],
        "buffs": [
            (184663, 544.370667, -884.355469, 26.796488, 4.675490),
            (184664, 544.132263, -931.261658, 26.033279, 4.710832),
        ],
        "drop_gameobject_guids": [2136044, 2136045],
    },
    872: {  # Coliseum of Past Echoes, map 982
        "alliance": (8017.729004, -2750.085205, 1134.563477, 6.263565),
        "horde":    (8138.802246, -2750.745605, 1134.562378, 3.114111),
        # Entry 185483 is the same gate the Obsidian Colosseum uses, so it
        # behaves the same: closed on spawn, opened on the start countdown.
        # Rotations are the ones the placed spawns carried, not recomputed.
        "gates": [
            (185483, 8026.25, -2749.97, 1133.77, 4.63387, 0.0, 0.0, -0.734317, 0.678807),
            (185483, 8128.83, -2748.27, 1133.85, 4.68099, 0.0, 0.0, -0.71812,  0.695919),
        ],
        "buffs": [
            (184663, 8074.241699, -2712.755615, 1134.076294, 4.708468),
            (184664, 8076.928223, -2788.572266, 1134.222046, 1.610051),
        ],
        # Placed as static gameobject rows to survey them; the battleground
        # spawns its own, so these have to go or every match starts with a
        # second, permanently shut gate in the same doorway.
        "drop_gameobject_guids": [2136044, 2136045],
    },
}


# ------------------------------------------------- cloned gameobject templates
#
# A gate that needs a different scale gets its own template rather than a change
# to the stock one. `gameobject_template.size` is per-entry, and these entries
# are not ours: 183977 and 183979 are the real Nagrand Arena's front doors, so
# resizing them there would resize map 559's doors too.
#
# Cloned with a temporary table rather than a spelled-out column list, so every
# column comes across whatever the schema happens to be -- there are 36 of them,
# and a hand-written list silently drops whatever it forgets.
#
# (new_entry, source_entry, size, name)
GO_TEMPLATE_CLONES = [
    (300400, 183977, 1.25, "Nagrand Arena HD Gate 1"),
    (300401, 183979, 1.25, "Nagrand Arena HD Gate 2"),
]


# ------------------------------------------------------------------ disabled
#
# Held back for now. Disabling is done through `disables` rather than by
# deleting anything: it is the mechanism the server already has, every guard
# added for Dalaran Sewers and the Ring of Valor already honours it, and
# re-enabling one is a single DELETE. All their data -- DBC rows, terrain,
# gates, minimaps -- stays in place.
#
# 880/884/885 are also the three whose terrain is mostly area id 0, so they show
# a stale zone name in the open; 881 is a pure-WMO map with no terrain textures
# at all. Worth knowing if the question of why comes up later.
DISABLED = {
    879: "Baradin Hold Arena",
    880: "Obelisk of the Stars",
    881: "The Twisting Nether",
    884: "The Inventor's Library",
    885: "Amphitheater of Anguish",
}


def measured(bg):
    return MEASURED.get(bg)


def quat_from_o(o):
    """GO rotation for a yaw-only orientation."""
    import math
    return (0.0, 0.0, math.sin(o / 2.0), math.cos(o / 2.0))


def start_positions(bg, cx, cy, cz):
    """(alliance xyzo, horde xyzo) -- surveyed if we have it, derived if not."""
    m = measured(bg)
    if m:
        return m["alliance"], m["horde"]
    z = cz + SPAWN_Z_LIFT
    return ((cx + SPAWN_OFFSET, cy, z, 3.14159),
            (cx - SPAWN_OFFSET, cy, z, 0.0))


# Where a gate sits between the arena centre and a team's start, as a fraction
# of that distance. 0.85 and 0.8 yards below the start are what the surveyed
# gates on map 982 actually measure, so the same shape is assumed for arenas
# whose gates have not been walked yet.
GATE_FRACTION = 0.85
GATE_Z_DROP = 0.8


def gates_for(bg, cx, cy, cz):
    """[(entry, x, y, z, o, r0, r1, r2, r3)] -- surveyed if known, else derived
    from the team starts, which is far better than guessing from the terrain."""
    import math
    m = measured(bg)
    if m and m.get("gates"):
        return m["gates"]

    (ax, ay, az, _ao), (hx, hy, hz, _ho) = start_positions(bg, cx, cy, cz)
    mx, my = (ax + hx) / 2.0, (ay + hy) / 2.0
    out = []
    for (sx, sy, sz) in ((ax, ay, az), (hx, hy, hz)):
        gx = mx + GATE_FRACTION * (sx - mx)
        gy = my + GATE_FRACTION * (sy - my)
        gz = sz - GATE_Z_DROP
        # face the arena centre
        o = math.atan2(my - gy, mx - gx) % (2 * math.pi)
        r0, r1, r2, r3 = quat_from_o(o)
        out.append((DOOR_GO, gx, gy, gz, o, r0, r1, r2, r3))
    return out


def esc(s):
    return s.replace("'", "''")


def header(title, extra=""):
    return """-- %s
--
-- Generated by tools/arenas/gen_arena_sql.py -- edit that, not this.
--
-- Covers the %d arenas ported from the Ascension client data. Map ids are the
-- same ids the source data uses, which is what Tol'viron (980) and Tiger's Peak
-- (1134) already do, so a directory name never has to be reconciled against a
-- map id.
--
-- Apply to lplusdevworld/dbc while iterating; apply to prod when it ships. The
-- dbc.*_lplus mirrors are SHARED between dev and prod -- there is no *_lplusdev
-- flavour -- so these rows reach prod as soon as prod restarts. Every id here is
-- new and unused, so nothing existing is disturbed.
--
-- Replayable: every section deletes its own ids before inserting.
%s
""" % (title, len(ARENAS), extra)


def gen_dbc():
    L = []
    L.append(header("Ported arenas - DBC mirror rows", """--
-- NOTE ON AreaTable: there are deliberately no AreaTable rows here. Map 980
-- (Tol'viron) and 1134 (Tiger's Peak) both carry AreaTableID = 0, and the client
-- works out which area a player is standing in from the area ids baked into the
-- ADTs, not from Map.dbc. A new AreaTable row that no ADT references would
-- therefore never be selected, while still consuming an AreaBit -- and AreaBits
-- are contended right now, with other work in flight on this shared database.
-- The arena scoreboard does not depend on it: the world states are served from
-- C++ before the zone lookup happens (see Player::SendInitWorldStates).
--
-- The binary .dbc files are a SEPARATE source of truth and drift silently if
-- they are not written too -- see tools/arenas/README.md."""))

    L.append("-- Backups, taken once. IF NOT EXISTS keeps a replay from overwriting the")
    L.append("-- original snapshot with already-modified data.")
    for t in ("map", "battlemasterlist", "pvpdifficulty", "worldsafelocs", "worldstateui"):
        L.append("CREATE TABLE IF NOT EXISTS dbc.%s_lplus_bak_arenas AS SELECT * FROM dbc.%s_lplus;" % (t, t))
    L.append("")

    ids = ",".join(str(a[1]) for a in ARENAS)
    bgids = ",".join(str(a[0]) for a in ARENAS)

    # ---------------- Map.dbc
    L.append("-- ---------------------------------------------------------------- Map.dbc")
    L.append("-- Shape copied from map 980: InstanceType 4 (arena), Flags 1, PVP 0,")
    L.append("-- AreaTableID 0, LoadingScreenID 319, MaxPlayers 0.")
    L.append("DELETE FROM dbc.map_lplus WHERE ID IN (%s);" % ids)
    L.append("INSERT INTO dbc.map_lplus")
    L.append("  (ID, Directory, InstanceType, Flags, PVP,")
    L.append("   MapName_Lang_enUS, MapName_Lang_Mask, AreaTableID,")
    L.append("   MapDescription0_Lang_Mask, MapDescription1_Lang_Mask,")
    L.append("   LoadingScreenID, MinimapIconScale, CorpseMapID, CorpseX, CorpseY,")
    L.append("   TimeOfDayOverride, ExpansionID, RaidOffset, MaxPlayers)")
    L.append("VALUES")
    rows = []
    for bg, mid, d, name, cx, cy, cz, conf, ev in ARENAS:
        rows.append("  (%d, '%s', 4, 1, 0, '%s', %d, 0, %d, %d, 319, 0, 0, 0, 0, -1, 0, 0, 0)"
                    % (mid, esc(d), esc(name), LANG_MASK, DESC_MASK, DESC_MASK))
    L.append(",\n".join(rows) + ";")
    L.append("")

    # ---------------- BattlemasterList.dbc
    L.append("-- ---------------------------------------------------- BattlemasterList.dbc")
    L.append("-- ID must equal the BattlegroundTypeId. InstanceType 4 marks it an arena,")
    L.append("-- which is what makes the client offer it in the arena frame rather than")
    L.append("-- the battleground one.")
    L.append("--")
    L.append("-- MapID_2..8 stay -1. Listing several maps in one row is how Blizzard built")
    L.append("-- 'All Arenas' (row 6), and it is exactly the mechanism that caps a random")
    L.append("-- pool at eight, because the column only exists eight times. The random")
    L.append("-- rotation is served from `battleground_random_pool` instead.")
    L.append("DELETE FROM dbc.battlemasterlist_lplus WHERE ID IN (%s);" % bgids)
    L.append("INSERT INTO dbc.battlemasterlist_lplus")
    L.append("  (ID, MapID_1, MapID_2, MapID_3, MapID_4, MapID_5, MapID_6, MapID_7, MapID_8,")
    L.append("   InstanceType, GroupsAllowed, Name_Lang_enUS, Name_Lang_Mask,")
    L.append("   MaxGroupSize, HolidayWorldState, Minlevel, Maxlevel)")
    L.append("VALUES")
    rows = []
    for bg, mid, d, name, cx, cy, cz, conf, ev in ARENAS:
        rows.append("  (%d, %d, -1, -1, -1, -1, -1, -1, -1, 4, 1, '%s', %d, 5, 0, 10, 80)"
                    % (bg, mid, esc(name), LANG_MASK))
    L.append(",\n".join(rows) + ";")
    L.append("")

    # ---------------- PvpDifficulty.dbc
    L.append("-- ------------------------------------------------------- PvpDifficulty.dbc")
    L.append("-- The silent one. Miss these and the queue refuses everyone with no error:")
    L.append("-- GetBattlegroundBracketByLevel simply finds no bracket and fails.")
    L.append("--")
    L.append("-- Sixteen brackets per arena covering levels 10-89, which is what maps 980")
    L.append("-- and 1134 already carry. RangeIndex n spans levels 10+5n .. 14+5n.")
    L.append("DELETE FROM dbc.pvpdifficulty_lplus WHERE MapID IN (%s);" % ids)
    L.append("INSERT INTO dbc.pvpdifficulty_lplus (ID, MapID, RangeIndex, MinLevel, MaxLevel, Difficulty)")
    L.append("VALUES")
    rows = []
    for i, (bg, mid, d, name, cx, cy, cz, conf, ev) in enumerate(ARENAS):
        for r in range(16):
            rows.append("  (%d, %d, %d, %d, %d, 0)" % (
                PVPDIFF_BASE + i * 16 + r, mid, r, 10 + 5 * r, 14 + 5 * r))
    L.append(",\n".join(rows) + ";")
    L.append("")

    # ---------------- WorldSafeLocs.dbc
    L.append("-- ------------------------------------------------------- WorldSafeLocs.dbc")
    L.append("-- Team start positions. These are the coordinates most likely to need a")
    L.append("-- nudge, and they are deliberately here rather than in C++ so that fixing")
    L.append("-- one is an UPDATE and a `.reload battleground_template`.")
    L.append("--")
    L.append("-- Both teams are placed on the X axis either side of the measured centre,")
    L.append("-- %.0f yards out, facing each other -- the same arrangement Tol'viron uses" % SPAWN_OFFSET)
    L.append("-- (its teams sit at X centre +-72 with Y held constant). Z is the measured")
    L.append("-- floor plus %.0f yards so nobody spawns inside the mesh." % SPAWN_Z_LIFT)
    L.append("--")
    L.append("-- Verify each with .gps before considering an arena done; the 'low'")
    L.append("-- confidence ones below are guesses at the structure, not measurements.")
    L.append("DELETE FROM dbc.worldsafelocs_lplus WHERE ID BETWEEN %d AND %d;" % (
        WSL_BASE, WSL_BASE + len(ARENAS) * 2 - 1))
    L.append("INSERT INTO dbc.worldsafelocs_lplus")
    L.append("  (ID, Continent, LocX, LocY, LocZ, AreaName_Lang_enUS, AreaName_Lang_Mask)")
    L.append("VALUES")
    rows = []
    for i, (bg, mid, d, name, cx, cy, cz, conf, ev) in enumerate(ARENAS):
        (ax, ay, az, _ao), (hx, hy, hz, _ho) = start_positions(bg, cx, cy, cz)
        tag = "from Ascension WorldSafeLocs" if measured(bg) else ("derived, confidence: %s" % conf)
        rows.append(("  (%d, %d, %.6f, %.6f, %.6f, '%s - Alliance Start', %d)"
                     % (WSL_BASE + i * 2, mid, ax, ay, az, esc(name), LANG_MASK), tag))
        rows.append(("  (%d, %d, %.6f, %.6f, %.6f, '%s - Horde Start',    %d)"
                     % (WSL_BASE + i * 2 + 1, mid, hx, hy, hz, esc(name), LANG_MASK), ""))
    for i, (vals, note) in enumerate(rows):
        sep = "," if i < len(rows) - 1 else ";"
        L.append("%s%s%s" % (vals, sep, ("   -- %s" % note) if note else ""))
    L.append("")

    # ---------------- AreaTable.dbc
    L.append("-- ----------------------------------------------------------- AreaTable.dbc")
    L.append("-- Without these an arena shows the zone name of wherever the player last")
    L.append("-- was, because the client resolves the zone from the area id baked into the")
    L.append("-- terrain and these arenas' ids had no rows at all.")
    L.append("--")
    L.append("-- The ids are NOT chosen here -- they are read out of the ADTs. Adding rows")
    L.append("-- under the same ids is what makes the arenas name themselves without")
    L.append("-- touching a single terrain file.")
    L.append("--")
    L.append("-- Sound, light and flag values are copied from AreaTable 6296 (Tol'viron).")
    L.append("-- AreaBits start at %d: the highest in use is 3718 and other work is live" % AREABIT_BASE)
    L.append("-- in this table, so this leaves that room rather than crowding it.")
    all_area_ids = sorted(i for ids in AREA_IDS.values() for i in ids)
    L.append("DELETE FROM dbc.areatable_lplus WHERE ID IN (%s);"
             % ",".join(str(i) for i in all_area_ids))
    L.append("INSERT INTO dbc.areatable_lplus")
    L.append("  (ID, ContinentID, ParentAreaID, AreaBit, Flags,")
    L.append("   SoundProviderPref, SoundProviderPrefUnderwater, AmbienceID, ZoneMusic, IntroSound,")
    L.append("   ExplorationLevel, AreaName_Lang_enUS, AreaName_Lang_Mask, FactionGroupMask,")
    L.append("   LiquidTypeID_1, LiquidTypeID_2, LiquidTypeID_3, LiquidTypeID_4,")
    L.append("   MinElevation, Ambient_Multiplier, Lightid)")
    L.append("VALUES")
    rows = []
    bit = AREABIT_BASE
    for bg, mid, d, name, cx, cy, cz, conf, ev in ARENAS:
        for area_id in AREA_IDS.get(bg, []):
            rows.append("  (%d, %d, 0, %d, %d, 0, 0, %d, %d, %d, 0, '%s', %d, 0, 0, 0, 0, 0, -500, %s, 0)"
                        % (area_id, mid, bit, AREA_FLAGS, AREA_AMBIENCE, AREA_ZONEMUSIC,
                           AREA_INTROSOUND, esc(name), LANG_MASK, AREA_AMBIENT_MUL))
            bit += 1
    L.append(",\n".join(rows) + ";")
    L.append("")
    L.append("-- Point each map at its own area, replacing the 0 the arenas were created")
    L.append("-- with. The client still decides the zone from the terrain; this is the")
    L.append("-- server-side fallback for anything that asks the map what area it is.")
    for bg, mid, d, name, cx, cy, cz, conf, ev in ARENAS:
        ids = AREA_IDS.get(bg)
        if ids:
            L.append("UPDATE dbc.map_lplus SET AreaTableID = %d WHERE ID = %d;" % (ids[0], mid))
    L.append("")

    # ---------------- WorldStateUI.dbc
    L.append("-- -------------------------------------------------------- WorldStateUI.dbc")
    L.append("-- The 'N players remaining' readout above the arena frame.")
    L.append("--")
    L.append("-- Two rows per arena, which is exactly what every existing arena carries")
    L.append("-- (maps 559, 562, 572, 617, 618, 980 and 1134 all have precisely two).")
    L.append("-- Shape copied verbatim from Tiger's Peak's pair, 90006/90007.")
    L.append("--")
    L.append("-- %3600w and %3601w substitute the live values of the two alive-player")
    L.append("-- world states; StateVariable 3610 is the flag that decides whether the")
    L.append("-- pair is drawn at all. The server sends all three -- 3610 from")
    L.append("-- BattlegroundCustomArena::FillInitialWorldStates, 3600/3601 from")
    L.append("-- Arena::UpdateArenaWorldState -- so without these rows the client receives")
    L.append("-- the numbers and stores them but has nothing declared to display them.")
    L.append("--")
    L.append("-- AreaID 0 means 'anywhere on this map', which matters because these arenas")
    L.append("-- report whatever zone id their source ADTs baked in rather than a zone of")
    L.append("-- their own.")
    L.append("--")
    L.append("-- CLIENT-side: no effect until packed into the client patch.")
    L.append("DELETE FROM dbc.worldstateui_lplus WHERE ID BETWEEN %d AND %d;" % (
        WSUI_BASE, WSUI_BASE + len(ARENAS) * 2 - 1))
    L.append("INSERT INTO dbc.worldstateui_lplus")
    L.append("  (ID, MapID, AreaID, PhaseShift, Icon,")
    L.append("   String_Lang_enUS, String_Lang_Mask, Tooltip_Lang_Mask,")
    L.append("   StateVariable, Type, DynamicIcon, DynamicTooltip_Lang_Mask,")
    L.append("   ExtendedUI, ExtendedUIStateVariable_1, ExtendedUIStateVariable_2, ExtendedUIStateVariable_3)")
    L.append("VALUES")
    rows = []
    for i, (bg, mid, d, name, cx, cy, cz, conf, ev) in enumerate(ARENAS):
        rows.append("  (%d, %d, 0, 0, '', 'Green Team: %%3600w Players Remaining', %d, %d, 3610, 0, '', %d, '', 0, 0, 0)"
                    % (WSUI_BASE + i * 2, mid, LANG_MASK, DESC_MASK, DESC_MASK))
        rows.append("  (%d, %d, 0, 0, '', 'Gold Team: %%3601w Players Remaining',  %d, %d, 3610, 0, '', %d, '', 0, 0, 0)"
                    % (WSUI_BASE + i * 2 + 1, mid, LANG_MASK, DESC_MASK, DESC_MASK))
    L.append(",\n".join(rows) + ";")
    L.append("")

    return "\n".join(L)


def gen_world():
    L = []
    L.append(header("Ported arenas - world DB", """--
-- Pairs with:
--   sql/custom/dbc/%s_dbc_ported_arenas.sql
--   src/server/game/Battlegrounds/Zones/BattlegroundCustomArena.{h,cpp}
--
-- Creates two new tables. Both are read by BattlegroundMgr::LoadBattlegroundTemplates,
-- so `.reload battleground_template` picks up edits to either without a restart.""" % STAMP))

    # ---------------- cloned gameobject templates
    if GO_TEMPLATE_CLONES:
        L.append("-- ------------------------------------------- cloned gameobject templates")
        L.append("-- Gates that need a size other than the model's default get their own")
        L.append("-- template. `size` lives on the template, not the spawn, and these source")
        L.append("-- entries are not ours -- 183977/183979 are the real Nagrand Arena's front")
        L.append("-- doors on map 559, so editing them there would resize that arena too.")
        L.append("--")
        L.append("-- Copied through a temporary table rather than a written-out column list:")
        L.append("-- gameobject_template has 36 columns, and a hand-maintained list silently")
        L.append("-- drops whatever it forgets and whatever gets added later.")
        L.append("DELETE FROM gameobject_template WHERE entry IN (%s);"
                 % ",".join(str(c[0]) for c in GO_TEMPLATE_CLONES))
        for new_entry, src_entry, size, name in GO_TEMPLATE_CLONES:
            L.append("")
            L.append("DROP TEMPORARY TABLE IF EXISTS _arena_go_clone;")
            L.append("CREATE TEMPORARY TABLE _arena_go_clone AS SELECT * FROM gameobject_template WHERE entry = %d;"
                     % src_entry)
            L.append("UPDATE _arena_go_clone SET entry = %d, size = %s, name = '%s';"
                     % (new_entry, size, esc(name)))
            L.append("INSERT INTO gameobject_template SELECT * FROM _arena_go_clone;")
            L.append("DROP TEMPORARY TABLE _arena_go_clone;")
        L.append("")

    # ---------------- battleground_template
    L.append("-- --------------------------------------------------- battleground_template")
    L.append("-- MinPlayersPerTeam 0 / MaxPlayersPerTeam 5 matches Tol'viron. The real cap")
    L.append("-- comes from the arena type at creation time (2v2/3v3/4v4/5v5), which")
    L.append("-- BattlegroundMgr::CreateNewBattleground applies after the template is copied.")
    L.append("--")
    L.append("-- StartMaxDist 0 disables the leash, as every other arena here does.")
    L.append("-- Weight is only consulted for random selection, and the arena rotation now")
    L.append("-- comes from battleground_random_pool below, so it is left at 1.")
    L.append("DELETE FROM battleground_template WHERE ID BETWEEN %d AND %d;" % (
        ARENAS[0][0], ARENAS[-1][0]))
    L.append("INSERT INTO battleground_template")
    L.append("  (ID, MinPlayersPerTeam, MaxPlayersPerTeam, MinLvl, MaxLvl,")
    L.append("   AllianceStartLoc, AllianceStartO, HordeStartLoc, HordeStartO,")
    L.append("   StartMaxDist, Weight, ScriptName, Comment)")
    L.append("VALUES")
    rows = []
    for i, (bg, mid, d, name, cx, cy, cz, conf, ev) in enumerate(ARENAS):
        (_ax, _ay, _az, ao), (_hx, _hy, _hz, ho) = start_positions(bg, cx, cy, cz)
        rows.append("  (%d, 0, 5, 10, 80, %d, %.6f, %d, %.6f, 0, 1, '', '%s')"
                    % (bg, WSL_BASE + i * 2, ao, WSL_BASE + i * 2 + 1, ho, esc(name)))
    L.append(",\n".join(rows) + ";")
    L.append("")

    # ---------------- random pool
    L.append("-- ------------------------------------------------ battleground_random_pool")
    L.append("-- Replaces the eight-map ceiling on random selection.")
    L.append("--")
    L.append("-- BattlegroundMgr::GetRandomBG used to build its candidate list from")
    L.append("-- BattlemasterListEntry::MapID, which is a fixed `int32 MapID[8]` in the")
    L.append("-- client's DBC format. 'All Arenas' therefore could not roll a ninth arena")
    L.append("-- however many were installed -- and on this realm it was rolling only two,")
    L.append("-- because row 6 listed just Nagrand and Blade's Edge.")
    L.append("--")
    L.append("-- This table has no such limit and needs no DBC edit: change a row, run")
    L.append("-- `.reload battleground_template`, and the next queue pop uses it.")
    L.append("--")
    L.append("--   PoolBgTypeId   what the player queued for: 6 = All Arenas,")
    L.append("--                  32 = Random Battleground")
    L.append("--   MemberBgTypeId a candidate to roll")
    L.append("--   Weight         relative likelihood, any positive value")
    L.append("--   Enabled        0 takes an arena out of rotation without deleting the row")
    L.append("DROP TABLE IF EXISTS `battleground_random_pool`;")
    L.append("CREATE TABLE `battleground_random_pool` (")
    L.append("  `PoolBgTypeId`   int unsigned NOT NULL COMMENT 'BattlegroundTypeId players queue for (6=All Arenas, 32=Random BG)',")
    L.append("  `MemberBgTypeId` int unsigned NOT NULL COMMENT 'BattlegroundTypeId that may be rolled',")
    L.append("  `Weight`         double NOT NULL DEFAULT '1' COMMENT 'Relative selection weight; must be > 0',")
    L.append("  `Enabled`        tinyint unsigned NOT NULL DEFAULT '1' COMMENT '0 disables without deleting',")
    L.append("  `Comment`        varchar(64) NOT NULL DEFAULT '',")
    L.append("  PRIMARY KEY (`PoolBgTypeId`,`MemberBgTypeId`)")
    L.append(") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Random battleground/arena selection pools';")
    L.append("")
    L.append("-- All Arenas (6): the runnable stock arenas plus every ported one --")
    L.append("-- twenty entries, well past the eight the DBC route could express.")
    L.append("--")
    L.append("-- Dalaran Sewers (10) and the Ring of Valor (11) are seeded DISABLED. Both")
    L.append("-- are turned off in `disables` (sourceType 3), so neither has a")
    L.append("-- battleground template and neither can be created. They are kept as rows")
    L.append("-- rather than omitted so the reason is recorded here rather than being an")
    L.append("-- unexplained absence -- flip Enabled to 1 if they are ever re-enabled.")
    L.append("--")
    L.append("-- Enabled is not the only guard: BattlegroundMgr::IsPoolMemberSelectable")
    L.append("-- re-checks `disables` and the template at selection time, so a row turned")
    L.append("-- on by hand still cannot roll a battleground the server cannot build.")
    L.append("INSERT INTO `battleground_random_pool` (PoolBgTypeId, MemberBgTypeId, Weight, Enabled, Comment) VALUES")
    pool = [
        (4,  1, "Nagrand Arena"),
        (5,  1, "Blade's Edge Arena"),
        (8,  1, "Ruins of Lordaeron"),
        (10, 0, "Dalaran Sewers - DISABLED in `disables`"),
        (11, 0, "The Ring of Valor - DISABLED in `disables`"),
        (103, 1, "Nefarian's Arena"),
        (870, 1, "Tol'Viron Arena"),
        (871, 1, "The Tiger's Peak"),
    ]
    rows = ["  (6, %d, 1, %d, '%s')" % (m, en, esc(c)) for m, en, c in pool]
    for bg, mid, d, name, cx, cy, cz, conf, ev in ARENAS:
        if bg in DISABLED:
            rows.append("  (6, %d, 1, 0, '%s')" % (bg, esc(("%s - held back" % name)[:60])))
        else:
            rows.append("  (6, %d, 1, 1, '%s')" % (bg, esc(name[:60])))
    L.append(",\n".join(rows) + ";")
    L.append("")

    # ---------------- disables
    L.append("-- ------------------------------------------------------------------ disables")
    L.append("-- Arenas held back for now. sourceType 3 is DISABLE_TYPE_BATTLEGROUND, the")
    L.append("-- same switch Dalaran Sewers (10) and the Ring of Valor (11) already use.")
    L.append("--")
    L.append("-- Nothing is deleted: DBC rows, terrain, gates and minimaps all stay in")
    L.append("-- place. A disabled battleground gets no template, and every path that")
    L.append("-- could offer one -- the Chromie arena menu, SelectBattleground, the random")
    L.append("-- pool, the queue -- checks for a template or asks DisableMgr directly.")
    L.append("--")
    L.append("-- To bring one back:")
    L.append("--   DELETE FROM disables WHERE sourceType = 3 AND entry = <bgTypeId>;")
    L.append("--   UPDATE battleground_random_pool SET Enabled = 1")
    L.append("--     WHERE PoolBgTypeId = 6 AND MemberBgTypeId = <bgTypeId>;")
    L.append("--   then `.reload battleground_template` and restart, since templates are")
    L.append("--   built at startup.")
    ids = ",".join(str(b) for b in sorted(DISABLED))
    L.append("DELETE FROM disables WHERE sourceType = 3 AND entry IN (%s);" % ids)
    L.append("INSERT INTO disables (sourceType, entry, flags, params_0, params_1, comment) VALUES")
    rows = []
    by_bg = {a[0]: a[1] for a in ARENAS}
    for bg in sorted(DISABLED):
        rows.append("  (3, %d, 0, '', '', '%s (map %d) - held back')"
                    % (bg, esc(DISABLED[bg]), by_bg.get(bg, 0)))
    L.append(",\n".join(rows) + ";")
    L.append("")

    # ---------------- arena objects
    L.append("-- ------------------------------------------ battleground_custom_arena_object")
    L.append("-- Gates and buffs for the data-driven arenas, spawned by")
    L.append("-- BattlegroundCustomArena::SetupBattleground.")
    L.append("--")
    L.append("--   ObjectType 0 = door   spawned closed, opened on the start countdown")
    L.append("--   ObjectType 1 = buff   shadowsight nodes, spawned 60s after the gates open")
    L.append("--")
    L.append("-- Buff rows exist only for arenas that have been surveyed in-game;")
    L.append("-- shadowsight positions cannot be derived from the terrain. An arena with")
    L.append("-- no buff rows simply has no buffs and is otherwise fully playable. Add")
    L.append("-- them as each arena is walked, with:")
    L.append("--")
    L.append("--   INSERT INTO battleground_custom_arena_object")
    L.append("--     (BgTypeId, GoEntry, X, Y, Z, Orientation, Rotation0, Rotation1, Rotation2, Rotation3, ObjectType, Comment)")
    L.append("--   VALUES (872, 184663, <x>, <y>, <z>, <o>, 0, 0, 0, 1, 1, 'shadowsight');")
    L.append("--")
    L.append("-- then `.reload battleground_template`. GameObject 184663/184664 are the two")
    L.append("-- Shadow Sight nodes; 179469 below is the stock 'Arena Door'.")
    L.append("--")
    L.append("-- Rows marked (surveyed) were measured in-game and are correct. Rows marked")
    L.append("-- (derived) are placeholders: %.0f yards either side of the centre read off" % DOOR_OFFSET)
    L.append("-- the terrain, on the X axis, with the team spawns %.0f yards out so players" % SPAWN_OFFSET)
    L.append("-- start behind their own gate. Expect to replace every derived row as each")
    L.append("-- arena is walked -- add it to MEASURED in tools/arenas/gen_arena_sql.py.")
    L.append("DROP TABLE IF EXISTS `battleground_custom_arena_object`;")
    L.append("CREATE TABLE `battleground_custom_arena_object` (")
    L.append("  `Id`          int unsigned NOT NULL AUTO_INCREMENT,")
    L.append("  `BgTypeId`    int unsigned NOT NULL COMMENT 'BattlegroundTypeId, must be a data-driven arena',")
    L.append("  `GoEntry`     int unsigned NOT NULL COMMENT 'gameobject_template.entry',")
    L.append("  `X`           float NOT NULL DEFAULT '0',")
    L.append("  `Y`           float NOT NULL DEFAULT '0',")
    L.append("  `Z`           float NOT NULL DEFAULT '0',")
    L.append("  `Orientation` float NOT NULL DEFAULT '0',")
    L.append("  `Rotation0`   float NOT NULL DEFAULT '0',")
    L.append("  `Rotation1`   float NOT NULL DEFAULT '0',")
    L.append("  `Rotation2`   float NOT NULL DEFAULT '0',")
    L.append("  `Rotation3`   float NOT NULL DEFAULT '1',")
    L.append("  `ObjectType`  tinyint unsigned NOT NULL DEFAULT '0' COMMENT '0=door, 1=buff',")
    L.append("  `Comment`     varchar(64) NOT NULL DEFAULT '',")
    L.append("  PRIMARY KEY (`Id`),")
    L.append("  KEY `idx_bgtype` (`BgTypeId`,`ObjectType`)")
    L.append(") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Gates and buffs for data-driven custom arenas';")
    L.append("")
    L.append("INSERT INTO `battleground_custom_arena_object`")
    L.append("  (BgTypeId, GoEntry, X, Y, Z, Orientation, Rotation0, Rotation1, Rotation2, Rotation3, ObjectType, Comment)")
    L.append("VALUES")
    rows = []
    for bg, mid, d, name, cx, cy, cz, conf, ev in ARENAS:
        m = measured(bg) or {}
        surveyed_gates = bool(m.get("gates"))
        tag = "surveyed" if surveyed_gates else "derived from the team starts"
        for j, (entry, x, y, z, o, r0, r1, r2, r3) in enumerate(gates_for(bg, cx, cy, cz), 1):
            rows.append("  (%d, %d, %.6f, %.6f, %.6f, %.6f, %g, %g, %.6f, %.6f, 0, '%s gate %d (%s)')"
                        % (bg, entry, x, y, z, o, r0, r1, r2, r3, esc(d[:24]), j, tag))
        for j, (entry, x, y, z, o) in enumerate(m.get("buffs", []), 1):
            r0, r1, r2, r3 = quat_from_o(o)
            rows.append("  (%d, %d, %.6f, %.6f, %.6f, %.6f, %g, %g, %.6f, %.6f, 1, '%s shadowsight %d')"
                        % (bg, entry, x, y, z, o, r0, r1, r2, r3, esc(d[:30]), j))
    L.append(",\n".join(rows) + ";")
    L.append("")

    # ---------------- static spawns that the battleground now owns
    drops = [(bg, sorted(set(m.get("drop_gameobject_guids", []))))
             for bg, m in sorted(MEASURED.items())]
    drops = [(bg, gs) for bg, gs in drops if gs]
    if drops:
        L.append("-- Gates surveyed by placing them as static gameobject rows. The")
        L.append("-- battleground spawns its own copies from the table above, so the static")
        L.append("-- ones have to go -- otherwise every match starts with a second gate in")
        L.append("-- the same doorway that nothing ever opens.")
        L.append("--")
        L.append("-- Scoped by map, not by guid alone. Spawn guids are RECYCLED: deleting")
        L.append("-- 982's gates freed 2136044/2136045, and the next pair placed -- on 983 --")
        L.append("-- came back with the same two numbers. A bare `WHERE guid IN (...)` would")
        L.append("-- delete whatever holds that number today, on any map, which after a")
        L.append("-- second survey is somebody else's gate. The map id confines each")
        L.append("-- statement to its own arena and makes an already-applied one a no-op.")
        by_map = {a[0]: a[1] for a in ARENAS}
        by_name = {a[0]: a[3] for a in ARENAS}
        for bg, gs in drops:
            L.append("--   %-28s map %-5d guid%s %s"
                     % (by_name.get(bg, "?"), by_map.get(bg, 0),
                        "s" if len(gs) > 1 else " ", ", ".join(str(g) for g in gs)))
        for bg, gs in drops:
            L.append("DELETE FROM gameobject WHERE map = %d AND guid IN (%s);"
                     % (by_map.get(bg, 0), ",".join(str(g) for g in gs)))
        L.append("")

    # ---------------- permanent arena furniture
    statics = [(bg, m.get("static_gameobjects", [])) for bg, m in sorted(MEASURED.items())]
    statics = [(bg, s) for bg, s in statics if s]
    if statics:
        by_map = {a[0]: a[1] for a in ARENAS}
        by_name = {a[0]: a[3] for a in ARENAS}
        L.append("-- ------------------------------------------------ permanent arena props")
        L.append("-- Scenery that is always up: no opening, no respawn, no lifecycle. Plain")
        L.append("-- gameobject rows rather than battleground objects, because an instanced")
        L.append("-- map spawns its gameobject rows into every instance anyway, and routing")
        L.append("-- them through the battleground would need code to manage something that")
        L.append("-- never changes.")
        L.append("--")
        L.append("-- They are here at all because `lplusdevworld` gets wiped and re-cloned")
        L.append("-- from prod: a spawn placed by hand in-game does not survive that, and")
        L.append("-- this script is the only durable copy.")
        for bg, rows_ in statics:
            L.append("--   %s (map %d): %d prop(s)" % (by_name.get(bg, "?"), by_map.get(bg, 0), len(rows_)))
        all_guids = sorted({r[0] for _bg, rows_ in statics for r in rows_})
        for bg, rows_ in statics:
            L.append("DELETE FROM gameobject WHERE map = %d AND guid IN (%s);"
                     % (by_map.get(bg, 0), ",".join(str(r[0]) for r in sorted(rows_))))
        L.append("INSERT INTO gameobject")
        L.append("  (guid, id, map, zoneId, areaId, spawnMask, phaseMask,")
        L.append("   position_x, position_y, position_z, orientation,")
        L.append("   rotation0, rotation1, rotation2, rotation3,")
        L.append("   spawntimesecs, animprogress, state)")
        L.append("VALUES")
        out = []
        for bg, rows_ in statics:
            mid = by_map.get(bg, 0)
            for (guid, entry, x, y, z, o, r0, r1, r2, r3, note) in rows_:
                # note goes into a `--` comment, so it must NOT be SQL-escaped:
                # esc() would render an apostrophe as '' and just read wrong
                out.append(("  (%d, %d, %d, 0, 0, 1, 1, %.6f, %.6f, %.6f, %.6f, %g, %g, %.6f, %.6f, 300, 255, 1)"
                            % (guid, entry, mid, x, y, z, o, r0, r1, r2, r3), note))
        # separator BEFORE the trailing comment -- a comment placed first eats
        # the comma and silently fuses two rows into one syntax error
        for i, (vals, note) in enumerate(out):
            L.append("%s%s  -- %s" % (vals, "," if i < len(out) - 1 else ";", note))
        L.append("")

    # ---------------- provenance
    L.append("-- ------------------------------------------------------------- provenance")
    L.append("-- How each centre was established, and how much to trust it:")
    L.append("--")
    for bg, mid, d, name, cx, cy, cz, conf, ev in ARENAS:
        L.append("--   %-4d %-20s %-7s %s" % (bg, d[:20], conf, ev))
    L.append("")

    return "\n".join(L)


def main():
    dbc_path = os.path.join(OUT_DBC, "%s_dbc_ported_arenas.sql" % STAMP)
    world_path = os.path.join(OUT_WORLD, "%s_world_ported_arenas.sql" % STAMP)
    with open(dbc_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(gen_dbc())
    with open(world_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(gen_world())
    print("wrote %s" % dbc_path)
    print("wrote %s" % world_path)
    print("%d arenas, %d pvpdifficulty rows, %d worldsafelocs, %d worldstateui, %d gates" % (
        len(ARENAS), len(ARENAS) * 16, len(ARENAS) * 2, len(ARENAS) * 2, len(ARENAS) * 2))


if __name__ == "__main__":
    main()
