"""Repair the world-map rows for the custom instanced battlegrounds.

All client-side: TrinityCore never reads WorldMapArea, DungeonMap or
DungeonMapChunk.

THE ROOT CAUSE, found 2026-09-18 after a first pass that fixed real defects but
not the actual failure. Scarlet Chapel, Blackrock Throne and the Violet Hold
Gauntlet were all built as *dungeon* maps - a DungeonMap floor rect plus
DungeonMapChunk rows keyed on the WMO group the player is inside. In stock
3.3.5 **no InstanceType 3 (battleground) map has a single DungeonMap row**.
Alterac Valley, Warsong Gulch, Arathi Basin, Eye of the Storm, Strand and Isle
of Conquest all ship plain terrain-style art, `<AreaName>1.blp..12.blp`, with no
floor suffix - the name WorldMapFrame_Update builds when the dungeon level is 0.
Our two battlegrounds that DO draw a map, Tanaris (1620) and the Obsidian
Colosseum (1615), are terrain-style as well. The three broken ones are exactly
the three that went the dungeon route.

So each battleground now gets its own terrain-style art directory, baked by
tools/bgmaps/build_bg_worldmap_art.py, and this script points BOTH rects - the
WorldMapArea Loc box (terrain path) and the DungeonMap row (dungeon path) - at
that same crop. The baker writes both tile namings. Whichever branch the client
takes, it draws the same picture with the arrow in the same place, so nothing
depends on being right about which one runs.

The defects fixed along the way, all real and all still fixed here:

1. BLACKROCK THRONE (map 1230) - WorldMapArea row 1763 and DungeonMap row 1100
   have their X and Y ranges TRANSPOSED.

   Both tables store the world **Y** range first and the **X** range second.
   WorldMapArea is LocLeft = max Y, LocRight = min Y, LocTop = max X,
   LocBottom = min X; DungeonMap is (minY, maxY, minX, maxX), ascending.
   Verified against stock Violet Hold (row 536 / DungeonMap 52, interior at
   X~1830 Y~803) and Tanaris (row 161, Gadgetzan at X -7176 Y -3785). The
   field NAMES in tools/violet_hold/vhr_dbc.py say "MinX, MaxX, MinY, MaxY",
   which is where the mistake came from - that comment is wrong, and the rows
   it wrote are only correct because they were copied verbatim from stock.

   The arena is X 1338.4..1424.5, Y -843.9..-697.5 (WorldSafeLocs 52300-52305),
   so the intended box was X 1330..1435, Y -860..-690 - the right numbers in
   the wrong slots. Transposed, the client maps the player's position outside
   the rect and the arrow never lands on the map.

2. SCARLET CHAPEL (map 1189) - no fallback floor.

   The floor shown inside an instance comes from DungeonMapChunk, keyed on the
   WMO group the client thinks it is standing in. Map 1189's 89 chunk rows are
   a verbatim clone of stock Scarlet Monastery's, and they inherit its two
   holes: cathedral groups 7902 and 7909 have no row, and neither does the
   WMO's root row. Land in one of those and GetCurrentMapDungeonLevel() is 0,
   so WorldMapFrame_Update asks for "ScarletMonastery1".."12" with no floor
   suffix - names that do not exist - and the map frame draws nothing.

   Fixed from both ends: the two missing chunk rows are added, and row 1762's
   DefaultDungeonFloor is set so there is a default even when the lookup
   misses. DefaultDungeonFloor is a DungeonMap ROW ID, not a floor index
   (stock precedent: Dalaran, WorldMapArea 504 -> DungeonMap 27); 1003 is the
   Cathedral floor, the one the arena at X 868..1075 Y 1342..1457 sits in.

3. VIOLET HOLD GAUNTLET (map 1608) - NOT fixable here, see the note at the
   bottom of this file. It needs art that the client does not ship.

None of this touches mountability. That comes from WMOAreaTable Flags & 4,
which the client reads only in CGUnit_C::IsOutdoors and which has no bearing on
"am I inside a WMO" - so the outdoors behaviour of all three battlegrounds is
left exactly as it is.

Idempotent: a row already holding the wanted values is reported and skipped, so
re-running after a partial failure is safe. The original is copied to
<name>.bak-bgmaps once, and never overwritten by a later run.

    python fix_bg_world_maps.py [--dry-run] <dbc-dir> [<dbc-dir> ...]

Run it against EVERY copy - data/dbc/lplus, data/dbc/bplus, data/mapdata/dbc
and clients/centurion/dbc all carry these files - and then repack the client
patch, or the binaries drift from each other again.
"""
import os
import shutil
import struct
import sys

# ---------------------------------------------------------------- field specs
# One character per field in column order: i = int32, f = float, s = string
# offset. patch() cross-checks each against the file header, so a wrong count
# fails loudly instead of writing a corrupt DBC.
LOC = "s" * 16 + "i"            # 16 locale string slots + the locale mask

SPECS = {
    "AreaTable.dbc": {
        # ID, ContinentID, ParentAreaID, AreaBit, Flags, SoundProviderPref,
        # ..Underwater, AmbienceID, ZoneMusic, IntroSound, ExplorationLevel,
        # AreaName(loc), FactionGroupMask, LiquidTypeID_1..4, MinElevation,
        # Ambient_Multiplier, Lightid
        "fields": "iiiii" + "iiiiii" + LOC + "i" + "iiii" + "ffi",
        "count": 36,
    },
    "WorldMapArea.dbc": {
        # ID, MapID, AreaID, AreaName, LocLeft, LocRight, LocTop, LocBottom,
        # DisplayMapID, DefaultDungeonFloor, ParentWorldMapID
        "fields": "iii" + "s" + "ffff" + "iii",
        "count": 11,
    },
    "DungeonMap.dbc": {
        # ID, MapID, FloorIndex, MinY, MaxY, MinX, MaxX, ParentWorldMapID
        # (note the axis order - see the header)
        "fields": "iii" + "ffff" + "i",
        "count": 8,
    },
    "DungeonMapChunk.dbc": {
        # ID, MapID, WmoGroupID, DungeonMapID, MinZ
        "fields": "iiii" + "f",
        "count": 5,
    },
}

# ------------------------------------------------------------------- the work
# {file: {row id: {field index: wanted value}}} - in-place field edits.
EDITS = {
    # ---- THE ONE THAT ACTUALLY MAKES THE MAP APPEAR ----
    # AreaBit is the index into the client's explored-zone bitfield, and its
    # zone table is keyed by it. Stock 3.3.5 never duplicates one: 2306 zones,
    # 2306 distinct bits. Our data duplicated five, and the custom row is the
    # one that loses the lookup - so the client reads the zone NAME straight
    # off the AreaTable row (GetRealZoneText() said "Scarlet Chapel" correctly)
    # but cannot resolve the zone's MAP, and SetMapToCurrentZone falls back to
    # a continent. That is exactly the reported symptom.
    #
    # New bits start past the highest in use (3821); the field holds 4096.
    # Fixing all five also stops the exploration cross-talk they caused -
    # walking into Scarlet Chapel was marking the Ruby Sanctum explored.
    "AreaTable.dbc": {
        30189: {3: 3822},   # Scarlet Chapel          was 3617, clashed 4987 Ruby Sanctum
        30230: {3: 3823},   # Blackrock Throne        was  698, clashed 1584 Blackrock Depths
        30231: {3: 3824},   # Nefarian's Arena        was  968, clashed 2677 Blackwing Lair
        30232: {3: 3825},   # The Battle Ring         was  819, clashed 2177 Battle Ring
        30233: {3: 3826},   # The Obsidian Colosseum  was 2349, clashed 4493 Obsidian Sanctum
    },
    "WorldMapArea.dbc": {
        # Blackrock Throne: its own art directory, and the rect that art
        # covers - Y into LocLeft/LocRight (max first), X into LocTop/Bottom.
        # The numbers come from tools/bgmaps/build_brt_worldmap_art.py, which
        # prints them; they must match the DungeonMap row below.
        1763: {3: "BlackrockThrone",
               4: -573.7513106111994, 5: -934.7239990234375,
               6: 1500.010009765625, 7: 1259.3616263566616,
               9: 1100},
        # Scarlet Chapel: its own terrain-style art and the rect that art
        # covers - a 2x crop of ScarletMonastery4 around the chapel and its
        # gardens. Numbers from tools/bgmaps/build_bg_worldmap_art.py.
        1762: {3: "ScarletChapel",
               4: 1743.989990234375, 5: 1040.68994140625,
               6: 1281.2900390625, 7: 812.4240112304688,
               9: 1003},
    },
    "DungeonMap.dbc": {
        # Blackrock Throne: become a copy of Blackrock Depths' floor 2 (row
        # 201) rather than a box drawn round the arena.
        #
        # The rect is not just the arrow's coordinate space, it is what the ART
        # SHEET covers. A rect the size of the arena stretches a whole
        # 1002x668 floor image across 105x170 yards of dungeon, so the picture
        # shown has nothing to do with what is on screen however right the
        # arrow is. Copying the floor's own rect draws BlackrockDepths2 at its
        # native scale with the arena in its true position, exactly as standing
        # in real Blackrock Depths does.
        #
        # The full Blackrock Depths floor 2 sheet would be correct but useless:
        # the Imperial Seat is a 97x57 pixel speck in its top-right corner. So
        # build_brt_worldmap_art.py crops that corner out and ships it as
        # BlackrockThrone1_1..12, and this is the world rect that crop covers.
        # FloorIndex stays 1 because the art is named <dir><floor>_<tile> and
        # the baked set is floor 1 of its own directory.
        1100: {2: 1,
               3: -934.7239990234375, 4: -573.7513106111994,
               5: 1259.3616263566616, 6: 1500.010009765625,
               7: 28},
        # Scarlet Chapel's own floor row, realigned onto the same crop as its
        # WorldMapArea box above and moved to FloorIndex 1 so the dungeon-path
        # name is ScarletChapel1_<tile>, which the baker also writes. Keeping
        # the two rects identical is what makes it not matter which path the
        # client takes.
        1003: {2: 1,
               3: 1040.68994140625, 4: 1743.989990234375,
               5: 812.4240112304688, 6: 1281.2900390625},
    },
}

# {file: [row, ...]} - rows appended when their id is not already present.
APPENDS = {
    "DungeonMapChunk.dbc": [
        # The two cathedral groups stock Scarlet Monastery never covered, bound
        # to the same floor as their 24 neighbours. 51002 was the highest id in
        # use (the Violet Hold's), so these are the next two free.
        [51003, 1189, 7902, 1003, -10000.0],
        [51004, 1189, 7909, 1003, -10000.0],
    ],
}


def read_dbc(path, spec):
    with open(path, "rb") as f:
        blob = f.read()
    magic, rec_count, field_count, rec_size, str_size = struct.unpack_from("<4sIIII", blob, 0)
    name = os.path.basename(path)
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
    return rec_count, field_count, rec_size, rec_bytes, str_block


def read_string(str_block, off):
    if off == 0 or off >= len(str_block):
        return ""
    end = str_block.find(b"\x00", off)
    return str_block[off:end].decode("utf-8", "replace")


def get_field(rec_bytes, rec_size, index, field, kind):
    off = index * rec_size + field * 4
    if kind == "f":
        return struct.unpack_from("<f", rec_bytes, off)[0]
    return struct.unpack_from("<i", rec_bytes, off)[0]


def set_field(rec_bytes, rec_size, index, field, kind, value):
    off = index * rec_size + field * 4
    if kind == "f":
        struct.pack_into("<f", rec_bytes, off, float(value))
    else:
        struct.pack_into("<i", rec_bytes, off, int(value))


def patch(path, spec, edits, appends, dry_run=False):
    name = os.path.basename(path)
    rec_count, field_count, rec_size, rec_bytes, str_block = read_dbc(path, spec)
    fields = spec["fields"]

    index_of = {}
    for i in range(rec_count):
        index_of[struct.unpack_from("<i", rec_bytes, i * rec_size)[0]] = i

    changed = 0
    for row_id, wanted in sorted(edits.items()):
        if row_id not in index_of:
            print("      id %-6s NOT PRESENT, nothing to edit" % row_id)
            continue
        i = index_of[row_id]
        for field, value in sorted(wanted.items()):
            kind = fields[field]
            if kind == "s":
                # Strings are byte offsets into the block. An edited string is
                # APPENDED rather than overwritten in place: every other row's
                # offset stays valid, and the few bytes the old value leaves
                # behind are simply unreferenced.
                off = get_field(rec_bytes, rec_size, i, field, "i")
                was = read_string(str_block, off)
                if was == value:
                    print("      id %-6s field %-2d already %r" % (row_id, field, value))
                    continue
                set_field(rec_bytes, rec_size, i, field, "i", len(str_block))
                str_block += value.encode("utf-8") + b"\x00"
                print("      id %-6s field %-2d %r -> %r" % (row_id, field, was, value))
                changed += 1
                continue
            was = get_field(rec_bytes, rec_size, i, field, kind)
            same = (abs(was - value) < 1e-3) if kind == "f" else (was == value)
            if same:
                print("      id %-6s field %-2d already %s" % (row_id, field, value))
                continue
            set_field(rec_bytes, rec_size, i, field, kind, value)
            print("      id %-6s field %-2d %s -> %s" % (row_id, field, was, value))
            changed += 1

    for row in appends:
        if len(row) != field_count:
            raise ValueError("%s: row has %d values, need %d" % (name, len(row), field_count))
        if row[0] in index_of:
            print("      id %-6s already present, left alone" % row[0])
            continue
        packed = bytearray()
        for kind, value in zip(fields, row):
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
        print("      id %-6s appended" % row[0])
        changed += 1

    if not changed:
        return 0
    if dry_run:
        print("      (dry run, nothing written)")
        return changed

    out = bytearray(struct.pack("<4sIIII", b"WDBC", rec_count, field_count,
                                rec_size, len(str_block)))
    out += rec_bytes
    out += str_block

    backup = path + ".bak-bgmaps"
    if not os.path.exists(backup):
        shutil.copyfile(path, backup)
    # Write beside the target and rename in, so a torn DBC is never visible
    # under the real name even if something is reading the directory.
    tmp = path + ".bgmaps-tmp"
    with open(tmp, "wb") as f:
        f.write(out)
    os.replace(tmp, path)
    return changed


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    dry_run = "--dry-run" in sys.argv
    if not args:
        raise SystemExit("usage: fix_bg_world_maps.py [--dry-run] <dbc-dir> [<dbc-dir> ...]")

    total = 0
    for d in args:
        print("=== %s ===" % d)
        if not os.path.isdir(d):
            print("    NOT A DIRECTORY, skipped")
            continue
        for fname, spec in SPECS.items():
            path = os.path.join(d, fname)
            if not os.path.exists(path):
                print("    %-22s MISSING, skipped" % fname)
                continue
            print("    %s" % fname)
            total += patch(path, spec, EDITS.get(fname, {}),
                           APPENDS.get(fname, []), dry_run)
    print("total fields changed / rows added: %d%s" %
          (total, " (dry run)" if dry_run else ""))


# ---------------------------------------------------------------------------
# Why the Violet Hold Gauntlet (map 1608) is not in the table above.
#
# Its WorldMapArea row 9533 names the art directory "VioletHold", and
# Interface\WorldMap\VioletHold\ DOES NOT EXIST - not in the stock 3.3.5
# archives and not in any custom patch. Blizzard shipped DungeonMap row 52 for
# map 608 but never the tiles to go with it; the Scarlet Monastery and
# Blackrock Depths tiles that patch-enUS-A carries were ported in later, and
# Violet Hold was not among them. So no row change can make that map draw.
#
# On top of that the floor can never resolve there anyway: patch-X ships a
# rebuilt World\wmo\Dungeon\ND_DalaranPrison\DalaranPrison_000.wmo whose MOGP
# flags are 0x380D - EXTERIOR 0x8 set - so the client is never "inside" the
# hold, and DungeonMapChunk 51002 (group 25154) cannot fire. That rebuild is
# what makes the arena mountable and is deliberate.
#
# The fix is therefore art, not data: bake 12 tiles and ship them as the
# battleground's own map the way the Tanaris deathmatch does
# (tools/tanaris/build_worldmap_art.py), named WITHOUT a floor suffix -
#     Interface\WorldMap\VioletHoldBG\VioletHoldBG1.blp .. VioletHoldBG12.blp
# - and point row 9533's AreaName at "VioletHoldBG". Terrain-style naming is
# what WorldMapFrame_Update uses when the dungeon level is 0, which is exactly
# the state the exterior WMO leaves it in, so the map then works without
# touching the WMO or the mount behaviour at all.
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    main()
