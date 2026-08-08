"""Append the Violet Hold survival battleground (map 1608 / BG type 105) rows
to the binary DBCs that describe it.

Runs identically against a server data/dbc directory and a local workspace, so
the same rows land everywhere instead of drifting (the SQL mirrors and the
binaries are separate sources of truth and only agree if both are written).
Pairs with sql/custom/dbc/2026_08_06_02_dbc_violet_hold_battleground.sql;
the rows here and there must match.

Engine copied from tools/tanaris/tanaris_dbc.py (see its header for the WDBC
format notes), with the Violet Hold row set and two extra specs: DungeonMap
and DungeonMapChunk, which an indoor instance needs for its floor map and a
continent clone did not.

Idempotent: a row whose id is already present is left alone, while the outdoor
classification fields on existing AreaTable/WMOAreaTable rows are normalized
in place. The original is copied to <name>.bak-vhr once, and never overwritten
by a later run.
"""
import os
import shutil
import struct
import sys

# ---------------------------------------------------------------- field specs
# One character per field, in DBC column order: i = int32, f = float,
# s = string (offset into the string block). patch() cross-checks each spec
# against the binary header, so a wrong count fails loudly instead of writing
# a corrupt file.

LOC = "s" * 16 + "i"            # 16 locale slots + the locale mask

SPECS = {
    "Map.dbc": {
        "fields": "is" + "ii" + "i" + LOC + "i" + LOC + LOC + "ififfiiii",
        "count": 66,
    },
    "AreaTable.dbc": {
        "fields": "iiiii" + "iiiiii" + LOC + "i" + "iiii" + "ffi",
        "count": 36,
    },
    "BattlemasterList.dbc": {
        "fields": "i" + "iiiiiiii" + "ii" + LOC + "iiii",
        "count": 32,
    },
    "PvpDifficulty.dbc": {
        "fields": "iiiiii",
        "count": 6,
    },
    "WorldSafeLocs.dbc": {
        "fields": "ii" + "fff" + LOC,
        "count": 22,
    },
    "WorldMapArea.dbc": {
        # AreaName here is a plain string, not a 17-field locale block.
        "fields": "iii" + "s" + "ffff" + "iii",
        "count": 11,
    },
    "DungeonMap.dbc": {
        # ID, MapID, FloorIndex, MinX, MaxX, MinY, MaxY, ParentWorldMapID
        "fields": "iii" + "ffff" + "i",
        "count": 8,
    },
    "DungeonMapChunk.dbc": {
        # ID, MapID, WmoGroupID, DungeonMapID, MinZ
        "fields": "iiii" + "f",
        "count": 5,
    },
    "WorldStateUI.dbc": {
        # ID, MapID, AreaID, PhaseShift, Icon, String(loc), Tooltip(loc),
        # StateVariable, Type, DynamicIcon, DynamicTooltip(loc), ExtendedUI,
        # ExtendedUIStateVariable_1..3
        "fields": "iiii" + "s" + LOC + LOC + "ii" + "s" + LOC + "s" + "iii",
        "count": 63,
    },
    "WMOAreaTable.dbc": {
        # ID, WMOID, NameSetID, WMOGroupID, five sound fields, Flags,
        # AreaTableID, AreaName(loc).
        "fields": "i" * 11 + LOC,
        "count": 28,
    },
}


def loc(text, mask=16712190):
    """A localized string block: enUS populated, the other 15 locales empty."""
    return [text] + [""] * 15 + [mask]


# ------------------------------------------------------------------ the rows
MAP_ID = 1608
BG_TYPE_ID = 105
AREA_ID = 30608
AREA_BIT = 3718                 # 3717 (Tanaris) is the highest in use
STOCK_AREA_ID = 4415            # baked into all 6,400 DalaranPrison MCNKs
AREA_FLAG_INSIDE = 0x02000000
AREA_FLAG_OUTSIDE = 0x04000000
WMO_ROOT_ID = 5282
WMO_AREA_FLAG_INSIDE = 2
WMO_AREA_FLAG_OUTSIDE = 4

BG_NAME = "The Violet Hold"
MAP_NAME = "The Violet Hold Gauntlet"
DESC = "Hold the line against wave after wave of dark reflections. Every wave adds one more."

# 52520: the party's start on the entrance landing inside the prison seal.
# 52521: the enemy team's nominal start at the chamber centre - clones stand
# there for one frame before the wave driver moves them into their cells.
WSL_DEFENDERS, WSL_ASSAULT = 52520, 52521
DEFENDERS_POS = (1848.03, 804.62, 44.07)
ASSAULT_POS = (1886.25, 803.07, 38.42)

ROWS = {
    "Map.dbc": [
        [MAP_ID,
         # The stock instance's own directory: the client resolves terrain by
         # Directory, not map id (the 1615 -> 615 trick), and unlike Tanaris
         # there is no plan to edit this terrain, so borrowing is safe and no
         # client map files are needed at all.
         "DalaranPrison",
         3,             # InstanceType 3 = battleground
         1,             # Flags: matches the other custom BG maps, not 608's 29
         1,             # PVP
         ] + loc(MAP_NAME) + [
         AREA_ID,
         ] + loc(DESC, 16712188) + loc(DESC, 16712188) + [
         235,           # LoadingScreenID: the Violet Hold's own
         1.0,           # MinimapIconScale
         -1,            # CorpseMapID: corpses stay put; the mode has no graveyards
         0.0,           # CorpseX
         0.0,           # CorpseY
         -1,            # TimeOfDayOverride
         0,             # ExpansionID: never let expansion gating refuse entry
         0,             # RaidOffset
         50],           # MaxPlayers: 10 humans + the full 40-clone final wave
    ],
    "AreaTable.dbc": [
        # Only referenced as Map.dbc.AreaTableID. Players actually report the
        # stock zone 4415 - the WMO area data in the byte-copied map files
        # still says so - and the server code accounts for that.
        [AREA_ID,
         MAP_ID,        # ContinentID -> the new map
         0,             # ParentAreaID
         AREA_BIT,
         AREA_FLAG_OUTSIDE,
                        # Explicit outside verdict. The stock 4415 row is
                        # normalized below because every DalaranPrison ADT
                        # MCNK references it directly.
         0,             # SoundProviderPref            )
         0,             # SoundProviderPrefUnderwater  ) all copied from the
         0,             # AmbienceID                   ) stock Violet Hold
         0,             # ZoneMusic                    ) (area 4415)
         0,             # IntroSound
         0,             # ExplorationLevel
         ] + loc(MAP_NAME) + [
         0,             # FactionGroupMask
         0, 0, 0, 0,    # LiquidTypeID_1..4
         -500.0,        # MinElevation
         0.0,           # Ambient_Multiplier
         0],            # Lightid
    ],
    "BattlemasterList.dbc": [
        # ID must equal the BattlegroundTypeId. MaxGroupSize 10 is the
        # "party/raid of up to 10" cap the group-queue check reads directly.
        [BG_TYPE_ID,
         MAP_ID, -1, -1, -1, -1, -1, -1, -1,
         3,             # InstanceType
         1,             # GroupsAllowed
         ] + loc(BG_NAME) + [
         10,            # MaxGroupSize
         0,             # HolidayWorldState
         10,            # Minlevel
         80],           # Maxlevel
    ],
    "PvpDifficulty.dbc": [
        # Without a bracket row the queue silently refuses everyone --
        # GetBattlegroundBracketByLevel just fails and logs nothing.
        # Id follows the existing 9<mapid> convention (91620 for Tanaris).
        # One 1-80 bracket: 60 is this server's level cap, so a 60-69 bracket
        # (the older custom BGs' shape) locks out every leveling character.
        [90000 + MAP_ID, MAP_ID, 0, 1, 80, 0],
    ],
    "WorldSafeLocs.dbc": [
        # Deliberately NO graveyard entries: death in the gauntlet is final
        # for the run. BattlegroundVHR::GetClosestGraveyard returns nothing.
        [WSL_DEFENDERS, MAP_ID] + list(DEFENDERS_POS) + loc("Violet Hold - Defenders"),
        [WSL_ASSAULT, MAP_ID] + list(ASSAULT_POS) + loc("Violet Hold - Assault"),
    ],
    "WorldMapArea.dbc": [
        # Stock row 536 declares the Violet Hold map for MapID 608; without a
        # 1608 twin the client falls back to the Northrend continent map and
        # never draws the player arrow. Same bounds, same AreaID (the client
        # disambiguates on MapID, as OBC proved with its duplicated 4493),
        # same "VioletHold" art directory - the stock art is reused untouched.
        [9533, MAP_ID, 4415, "VioletHold",
         983.333,      # LocLeft
         600.0,        # LocRight
         2006.25,      # LocTop
         1750.0,       # LocBottom
         -1,           # DisplayMapID
         0,            # DefaultDungeonFloor
         0],           # ParentWorldMapID
    ],
    "DungeonMap.dbc": [
        # The dungeon-floor image drawn inside an instance. Stock row 52
        # covers map 608 floor 1; without a 1608 twin the floor view is blank.
        # 1101 is the next free id after the stock table's 1100.
        [1101, MAP_ID, 1, 665.347, 921.576, 1813.35, 1984.17, 504],
    ],
    "DungeonMapChunk.dbc": [
        # Binds the hold's WMO group (25154) to the floor above so the client
        # knows which floor a player inside the building is on. One stock row
        # exists for map 608; this is its 1608 twin (51001 was the max id).
        [51002, MAP_ID, 25154, 1101, -10000.0],
    ],
    "WorldStateUI.dbc": [
        # The top-frame readout: party members left, enemies left, and the
        # current wave, each shown as "X / Y". The denominators are world
        # states too - 9405 is the party size the run started with, 9404 is
        # this wave's clone count.
        # current wave. "%9401w" substitutes the live value of that world state - see
        # BG_VHR_WorldStates in BattlegroundVHR.h. The format copies the
        # custom arenas' rows ("Green Team: %3600w Players Remaining"): plain
        # labelled text, no icon. StateVariable 9400 is this battleground's
        # own show flag, sent as 1 for the whole match. AreaID 0 = anywhere on
        # the map, which matters because the hold reports stock area 4415
        # rather than the custom id.
        [90025, MAP_ID, 0, 0, ""]
        + loc("%9401w / %9405w Players Remaining") + loc("") + [
         9400,         # StateVariable: the battleground's show flag
         0,            # Type
         ""]           # DynamicIcon
        + loc("", 16712188) + [
         "",           # ExtendedUI
         0, 0, 0],     # ExtendedUIStateVariable_1..3

        [90026, MAP_ID, 0, 0, ""]
        + loc("%9402w / %9404w Memories Remaining") + loc("") + [
         9400,
         0,
         ""]
        + loc("", 16712188) + [
         "",
         0, 0, 0],

        [90027, MAP_ID, 0, 0, ""]
        + loc("Wave: %9403w") + loc("") + [
         9400,
         0,
         ""]
        + loc("", 16712188) + [
         "",
         0, 0, 0],
    ],
    # These are stock rows and are rewritten in place by normalize_outdoors;
    # no new WMOAreaTable records are appended.
    "WMOAreaTable.dbc": [],
}


# --------------------------------------------------------------------- engine
def normalize_outdoors(name, rec_bytes, rec_count, rec_size):
    """Return the number of existing records whose outdoor fields changed.

    ADTs have no indoor/outdoor flag. Every DalaranPrison MCNK carries area id
    4415, so both that stock row and map 1608's fallback row must explicitly
    say OUTSIDE. WMO model flags must remain stock because they control client
    rendering/culling and must be changed as a coupled root/group/batch set by
    vhr_outdoors.py; WMOAreaTable is the client/server outdoor override. Its
    Violet Hold rows must also resolve to AreaTable 4415 instead of area 0, or
    the 3.3.5 client falls back to the stock WMO's indoor classification when
    deciding whether to enable outdoors-only action buttons.
    """
    changed = 0
    for i in range(rec_count):
        base = i * rec_size
        record_id = struct.unpack_from("<i", rec_bytes, base)[0]
        if name == "AreaTable.dbc" and record_id in (STOCK_AREA_ID, AREA_ID):
            field = base + 4 * 4
            old = struct.unpack_from("<I", rec_bytes, field)[0]
            new = (old | AREA_FLAG_OUTSIDE) & ~AREA_FLAG_INSIDE
        elif name == "WMOAreaTable.dbc":
            wmo_id = struct.unpack_from("<i", rec_bytes, base + 4)[0]
            if wmo_id != WMO_ROOT_ID:
                continue
            flags_field = base + 4 * 9
            area_field = base + 4 * 10
            old_flags = struct.unpack_from("<I", rec_bytes, flags_field)[0]
            old_area = struct.unpack_from("<I", rec_bytes, area_field)[0]
            new_flags = (old_flags | WMO_AREA_FLAG_OUTSIDE) & ~WMO_AREA_FLAG_INSIDE
            new_area = STOCK_AREA_ID
            if new_flags == old_flags and new_area == old_area:
                continue
            struct.pack_into("<I", rec_bytes, flags_field, new_flags)
            struct.pack_into("<I", rec_bytes, area_field, new_area)
            print("      id %-6s WMO outdoor flags 0x%08X -> 0x%08X, area %u -> %u" %
                  (record_id, old_flags, new_flags, old_area, new_area))
            changed += 1
            continue
        else:
            continue
        if new != old:
            struct.pack_into("<I", rec_bytes, field, new)
            print("      id %-6s outdoor flags 0x%08X -> 0x%08X" %
                  (record_id, old, new))
            changed += 1
    return changed


def patch(path, spec, rows, dry_run=False):
    name = os.path.basename(path)
    with open(path, "rb") as f:
        blob = f.read()

    magic, rec_count, field_count, rec_size, str_size = struct.unpack_from("<4sIIII", blob, 0)
    if magic != b"WDBC":
        raise ValueError("%s: not a WDBC file" % name)
    if field_count != spec["count"] or rec_size != spec["count"] * 4:
        raise ValueError("%s: expected %d fields / %d bytes, found %d / %d"
                         % (name, spec["count"], spec["count"] * 4, field_count, rec_size))
    if len(spec["fields"]) != field_count:
        raise ValueError("%s: field spec is %d chars, file has %d fields"
                         % (name, len(spec["fields"]), field_count))

    rec_start = 20
    rec_bytes = bytearray(blob[rec_start:rec_start + rec_count * rec_size])
    str_block = bytearray(blob[rec_start + rec_count * rec_size:])
    if len(str_block) != str_size:
        raise ValueError("%s: string block is %d bytes, header says %d"
                         % (name, len(str_block), str_size))
    if not str_block or str_block[0] != 0:
        raise ValueError("%s: string block does not start with the empty string" % name)

    existing = set()
    for i in range(rec_count):
        existing.add(struct.unpack_from("<i", rec_bytes, i * rec_size)[0])

    changed = normalize_outdoors(name, rec_bytes, rec_count, rec_size)
    added = 0
    for row in rows:
        if len(row) != field_count:
            raise ValueError("%s: row has %d values, need %d" % (name, len(row), field_count))
        if row[0] in existing:
            print("      id %-6s already present, left alone" % row[0])
            continue

        packed = bytearray()
        for kind, value in zip(spec["fields"], row):
            if kind == "i":
                packed += struct.pack("<i", int(value))
            elif kind == "f":
                packed += struct.pack("<f", float(value))
            elif kind == "s":
                if value == "":
                    packed += struct.pack("<I", 0)
                else:
                    packed += struct.pack("<I", len(str_block))
                    str_block += value.encode("utf-8") + b"\x00"
            else:
                raise ValueError("bad spec char %r" % kind)
        rec_bytes += packed
        rec_count += 1
        added += 1
        print("      id %-6s appended" % row[0])

    if not added and not changed:
        return 0

    out = bytearray(struct.pack("<4sIIII", b"WDBC", rec_count, field_count,
                                rec_size, len(str_block)))
    out += rec_bytes
    out += str_block

    if dry_run:
        print("      (dry run, %d bytes not written)" % len(out))
        return added + changed

    backup = path + ".bak-vhr"
    if not os.path.exists(backup):
        shutil.copyfile(path, backup)

    # Write beside the target and rename in: a torn DBC is never visible under
    # the real name, even if the server happens to be starting up.
    tmp = path + ".vhr-tmp"
    with open(tmp, "wb") as f:
        f.write(out)
    os.replace(tmp, path)
    return added + changed


def restore(path):
    """Put back the pre-VHR copy, so an apply with changed values starts from
    a clean file instead of trying to rewrite records in place."""
    backup = path + ".bak-vhr"
    if not os.path.exists(backup):
        print("      no backup, left alone")
        return
    shutil.copyfile(backup, path)
    print("      restored from %s" % os.path.basename(backup))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    dry_run = "--dry-run" in sys.argv
    do_restore = "--restore" in sys.argv
    if not args:
        raise SystemExit("usage: vhr_dbc.py [--dry-run] [--restore] <dbc-dir> [<dbc-dir> ...]")

    total = 0
    for d in args:
        print("=== %s ===" % d)
        if not os.path.isdir(d):
            print("    NOT A DIRECTORY, skipped")
            continue
        if do_restore:
            for name in SPECS:
                path = os.path.join(d, name)
                if os.path.exists(path):
                    print("    %s" % name)
                    restore(path)
        for name, spec in SPECS.items():
            path = os.path.join(d, name)
            if not os.path.exists(path):
                print("    %-22s MISSING, skipped" % name)
                continue
            print("    %s" % name)
            total += patch(path, spec, ROWS[name], dry_run)
    print("total records added/updated: %d%s" %
          (total, " (dry run)" if dry_run else ""))


if __name__ == "__main__":
    main()
