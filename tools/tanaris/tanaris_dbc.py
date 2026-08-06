"""Append the Tanaris battleground (map 1620 / BG type 104) rows to the five
binary DBCs that describe it.

Runs identically against a server data/dbc directory and a local workspace, so
the same rows land everywhere instead of drifting (the SQL mirrors and the
binaries are separate sources of truth and only agree if both are written).

WDBC layout: a 20-byte header (magic, recordCount, fieldCount, recordSize,
stringBlockSize), then fixed-size records, then the string block. Strings are
stored as byte offsets into that block, so a new string is APPENDED to the end
of the block and the record points at it -- existing offsets stay valid and
nothing has to be rewritten. Offset 0 is the empty string by convention, which
is what every unused locale slot points at.

Idempotent: a row whose id is already present is left alone, so re-running
after a partial failure is safe. The original is copied to <name>.bak-tanaris
once, and never overwritten by a later run.
"""
import os
import shutil
import struct
import sys

# ---------------------------------------------------------------- field specs
# One character per field, in DBC column order: i = int32, f = float,
# s = string (offset into the string block). These were checked against both
# the binary headers (fieldCount/recordSize) and the dbc.*_lplus column lists;
# all five agree exactly, which is why the SQL mirror and the binary can be
# written from one description.

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
    "WorldMapOverlay.dbc": {
        # ID, MapAreaID, AreaID_1..4, MapPointX, MapPointY, TextureName,
        # TextureWidth, TextureHeight, OffsetX, OffsetY, HitRect{Top,Left,Bottom,Right}
        "fields": "iiiiii" + "ii" + "s" + "iiiiiiii",
        "count": 17,
    },
    "WorldStateUI.dbc": {
        # ID, MapID, AreaID, PhaseShift, Icon, String(loc), Tooltip(loc),
        # StateVariable, Type, DynamicIcon, DynamicTooltip(loc), ExtendedUI,
        # ExtendedUIStateVariable_1..3
        "fields": "iiii" + "s" + LOC + LOC + "ii" + "s" + LOC + "s" + "iii",
        "count": 63,
    },
}


def loc(text, mask=16712190):
    """A localized string block: enUS populated, the other 15 locales empty."""
    return [text] + [""] * 15 + [mask]


# ------------------------------------------------------------------ the rows
MAP_ID = 1620
BG_TYPE_ID = 104
AREA_ID = 30234
AREA_BIT = 3717                 # max in use is 3716; the client field is 4096 bits

BG_NAME = "Tanaris"
MAP_NAME = "Tanaris Deathmatch"
DESC = "A deathmatch in the deep desert of Tanaris. First team to the kill limit wins."

# Start / graveyard positions. Southmoon Ruins for the Alliance, Gadgetzan for
# the Horde -- about 2200 yards apart, which is RTS scale rather than arena
# scale. Each team's spirit healer stands on its own spawn point.
#
# Z is the measured ground height plus a yard so nobody spawns inside terrain.
A_START = (-9235.11, -3009.99, 17.19)   # ground 16.19
H_START = (-7166.45, -3760.62, 9.40)    # ground 8.40
A_GRAVE = (-9235.11, -3009.99, 17.19)   # spirit healer stands on the spawn
H_GRAVE = (-7166.45, -3760.62, 9.40)

WSL_A_START, WSL_H_START, WSL_A_GRAVE, WSL_H_GRAVE = 52500, 52501, 52502, 52503

ROWS = {
    "Map.dbc": [
        [MAP_ID,
         # A private copy of the Tanaris tiles rather than "Kalimdor" itself.
         # Borrowing the real continent works - the client resolves ADTs by
         # Directory, not by map id, which is how 1615 borrows 615's terrain -
         # but then any terrain edit for the battleground would also change the
         # live Tanaris zone. Built by build_tanaris_terrain.py.
         "TanarisBG",
         3,             # InstanceType 3 = battleground
         1,             # Flags: matches the other custom BG maps (1189/1230)
         1,             # PVP
         ] + loc(MAP_NAME) + [
         AREA_ID,
         ] + loc(DESC, 16712188) + loc(DESC, 16712188) + [
         3,             # LoadingScreenID: Kalimdor's
         1.0,           # MinimapIconScale
         1,             # CorpseMapID: ghosts resolve back onto Kalimdor
         -7177.0,       # CorpseX  (Gadgetzan)
         -3785.0,       # CorpseY
         -1,            # TimeOfDayOverride
         0,             # ExpansionID
         0,             # RaidOffset
         20],           # MaxPlayers
    ],
    "AreaTable.dbc": [
        [AREA_ID,
         MAP_ID,        # ContinentID -> the new map
         0,             # ParentAreaID
         AREA_BIT,
         0,             # Flags
         0,             # SoundProviderPref
         11,            # SoundProviderPrefUnderwater )
         39,            # AmbienceID                  ) copied from Tanaris (440)
         176,           # ZoneMusic                   ) so it sounds like Tanaris
         0,             # IntroSound
         0,             # ExplorationLevel
         ] + loc(MAP_NAME) + [
         0,             # FactionGroupMask
         0, 0, 0, 0,    # LiquidTypeID_1..4
         -500.0,        # MinElevation
         0.5,           # Ambient_Multiplier
         0],            # Lightid
    ],
    "BattlemasterList.dbc": [
        [BG_TYPE_ID,
         MAP_ID, -1, -1, -1, -1, -1, -1, -1,
         3,             # InstanceType
         1,             # GroupsAllowed
         ] + loc(BG_NAME) + [
         10,            # MaxGroupSize
         1942,          # HolidayWorldState: same as the other custom BGs
         10,            # Minlevel
         80],           # Maxlevel
    ],
    "PvpDifficulty.dbc": [
        # Without a bracket row the queue silently refuses everyone --
        # GetBattlegroundBracketByLevel just fails and logs nothing.
        # Id follows the existing 9<mapid> convention (91615 for OBC).
        [90000 + MAP_ID, MAP_ID, 0, 60, 69, 0],
    ],
    "WorldSafeLocs.dbc": [
        [WSL_A_START, MAP_ID] + list(A_START) + loc("Tanaris - Alliance Start"),
        [WSL_H_START, MAP_ID] + list(H_START) + loc("Tanaris - Horde Start"),
        [WSL_A_GRAVE, MAP_ID] + list(A_GRAVE) + loc("Tanaris - Alliance Graveyard"),
        [WSL_H_GRAVE, MAP_ID] + list(H_GRAVE) + loc("Tanaris - Horde Graveyard"),
    ],
    "WorldMapArea.dbc": [
        # Without this the world map draws Tanaris but never draws the player
        # arrow: the client only positions the blob when the displayed area's
        # MapID equals the map the player is actually standing on, and stock
        # Tanaris (row 161) says MapID 1.
        #
        # AreaID stays 440 rather than the custom area 30234, because the client
        # works out the current area from the Kalimdor ADTs it loaded, and those
        # say 440. That leaves two rows sharing AreaID 440, which is exactly what
        # the Obsidian Colosseum already does with 4493 (rows 531 on map 615 and
        # 9531 on map 1615) - the client disambiguates on MapID, so the real
        # Tanaris map is unaffected.
        #
        # The bounds are copied verbatim from row 161 so the arrow lands in the
        # right place on the same artwork. AreaName picks the artwork.
        [9532, MAP_ID, 440, "Tanaris",
         -218.75,      # LocLeft   (world Y max)
         -7118.75,     # LocRight  (world Y min)
         -5875.0,      # LocTop    (world X max)
         -10475.0,     # LocBottom (world X min)
         -1,           # DisplayMapID
         0,            # DefaultDungeonFloor
         0],           # ParentWorldMapID
    ],
    "WorldMapOverlay.dbc": [
        # The explored-area artwork. Without these the world map for 1620 draws
        # as blank parchment: the arrow sits in the right place, but no zone
        # detail appears however thoroughly the player has explored Tanaris.
        #
        # MapAreaID is a WorldMapArea ROW ID, not a map id. Stock Tanaris's 20
        # overlays all point at row 161; the client now displays row 9532 and so
        # finds none. These are those same 20 rows repointed at 9532 - identical
        # textures, offsets and hit rects, and identical AreaIDs so they light up
        # from the exploration the player already has.
        #
        # Generated from the MapAreaID=161 rows of the live binary; if those ever
        # change, regenerate rather than hand-editing.
        [1700, 9532, 980, 0, 0, 0, 0, 0, 'THISTLESHRUBVALLEY', 185, 250, 203, 286, 360, 245, 475, 370],
        [1701, 9532, 990, 0, 0, 0, 0, 0, 'VALLEYOFTHEWATCHERS', 150, 160, 291, 434, 490, 330, 585, 410],
        [1702, 9532, 992, 0, 0, 0, 0, 0, 'SOUTHMOONRUINS', 195, 210, 323, 359, 445, 365, 510, 440],
        [1703, 9532, 987, 0, 0, 0, 0, 0, 'LANDSENDBEACH', 205, 157, 445, 511, 565, 480, 660, 620],
        [1704, 9532, 984, 0, 0, 0, 0, 0, 'EASTMOONRUINS', 160, 150, 395, 346, 410, 435, 460, 500],
        [1705, 9532, 981, 0, 0, 0, 0, 0, 'THEGAPINGCHASM', 220, 210, 449, 372, 415, 490, 530, 600],
        [1706, 9532, 1940, 0, 0, 0, 0, 0, 'SOUTHBREAKSHORE', 215, 175, 499, 293, 365, 570, 430, 680],
        [1707, 9532, 983, 0, 0, 0, 0, 0, 'DUNEMAULCOMPOUND', 205, 145, 325, 289, 335, 380, 415, 460],
        [1708, 9532, 982, 0, 0, 0, 0, 0, 'THENOXIOUSLAIR', 180, 200, 252, 199, 255, 310, 355, 385],
        [1709, 9532, 1938, 0, 0, 0, 0, 0, 'BROKENPILLAR', 110, 180, 473, 234, 275, 500, 335, 565],
        [1710, 9532, 1939, 0, 0, 0, 0, 0, 'ABYSSALSANDS', 215, 180, 363, 194, 240, 410, 330, 505],
        [1711, 9532, 985, 0, 0, 0, 0, 0, 'WATERSPRINGFIELD', 165, 180, 509, 168, 210, 550, 295, 645],
        [1712, 9532, 1336, 0, 0, 0, 0, 0, 'LOSTRIGGERCOVE', 160, 190, 629, 220, 255, 675, 375, 760],
        [1713, 9532, 986, 0, 0, 0, 0, 0, 'ZALASHJISDEN', 110, 140, 611, 147, 190, 640, 255, 700],
        [1714, 9532, 977, 0, 0, 0, 0, 0, 'STEAMWHEEDLEPORT', 155, 150, 592, 75, 110, 630, 175, 710],
        [1715, 9532, 1937, 0, 0, 0, 0, 0, 'NOONSHADERUINS', 120, 135, 533, 104, 140, 570, 190, 630],
        [1716, 9532, 2300, 0, 0, 0, 0, 0, 'CAVERNSOFTIME', 155, 150, 561, 256, 295, 595, 365, 690],
        [1717, 9532, 976, 0, 0, 0, 0, 0, 'GADGETZAN', 175, 165, 421, 91, 140, 460, 225, 565],
        [1718, 9532, 979, 0, 0, 0, 0, 0, 'SANDSORROWWATCH', 195, 175, 299, 100, 155, 365, 225, 435],
        [1719, 9532, 978, 0, 0, 0, 0, 0, 'ZULFARRAK', 210, 175, 254, 0, 25, 300, 145, 450]
    ],
    "WorldStateUI.dbc": [
        # The top-frame score readout. Without these two rows the server still
        # sends the world states and the client still stores them, but nothing
        # is declared to display them, so the battleground shows no score at all.
        #
        # Shape copied from Scarlet Chapel's pair (90002/90003). The "%9300w"
        # syntax substitutes the live value of that world state into the string,
        # so these read "<team kills>/<kill limit>" - see BG_TRT_WorldStates.
        #
        # StateVariable 0 means "always visible", which is what a plain
        # deathmatch wants. The Obsidian Colosseum sets it instead, because its
        # rows are the mutually exclusive flag-carrier indicators.
        #
        # AreaID 0 means "anywhere on this map" - important here, since the
        # arena reports stock Tanaris (440) rather than a custom area.
        [90023, MAP_ID, 0, 0,
         r"Interface\TargetingFrame\UI-PVP-Alliance"]
        + loc("%9300w/%9306w") + loc("") + [
         0,            # StateVariable: always shown
         0,            # Type
         ""]           # DynamicIcon
        + loc("", 16712188) + [
         "",           # ExtendedUI
         0, 0, 0],     # ExtendedUIStateVariable_1..3

        [90024, MAP_ID, 0, 0,
         r"Interface\TargetingFrame\UI-PVP-Horde"]
        + loc("%9301w/%9306w") + loc("") + [
         0,
         0,
         ""]
        + loc("", 16712188) + [
         "",
         0, 0, 0],
    ],
}


# --------------------------------------------------------------------- engine
def patch(path, spec, rows, dry_run=False, backup_suffix=".bak-tanaris"):
    """Append rows to a binary DBC.

    backup_suffix lets another project reuse this engine and keep its own
    pre-change snapshot under its own name -- see tools/arenas/arena_dbc.py.
    An existing backup is never overwritten, so the oldest copy survives
    whichever project ran first.
    """
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

    if not added:
        return 0

    out = bytearray(struct.pack("<4sIIII", b"WDBC", rec_count, field_count,
                                rec_size, len(str_block)))
    out += rec_bytes
    out += str_block

    if dry_run:
        print("      (dry run, %d bytes not written)" % len(out))
        return added

    backup = path + backup_suffix
    if not os.path.exists(backup):
        shutil.copyfile(path, backup)

    # Write beside the target and rename in: a torn DBC is never visible under
    # the real name, even if the server happens to be starting up.
    tmp = path + ".dbc-tmp"
    with open(tmp, "wb") as f:
        f.write(out)
    os.replace(tmp, path)
    return added


def restore(path):
    """Put back the pre-Tanaris copy, so an apply with changed values starts
    from a clean file instead of trying to rewrite records in place."""
    backup = path + ".bak-tanaris"
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
        raise SystemExit("usage: tanaris_dbc.py [--dry-run] [--restore] <dbc-dir> [<dbc-dir> ...]")

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
    print("total rows added: %d%s" % (total, " (dry run)" if dry_run else ""))


if __name__ == "__main__":
    main()
