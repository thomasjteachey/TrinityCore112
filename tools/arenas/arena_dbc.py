"""Write the ported-arena rows into the binary DBCs.

The SQL mirrors in dbc.*_lplus and the binary .dbc files are two separate sources
of truth. They only agree if both are written, and they drift silently if they
are not -- so this exists to write the same rows generated into
sql/custom/dbc/*_dbc_ported_arenas.sql into the binaries.

The arena list is imported from gen_arena_sql rather than restated here, so the
SQL and the binaries cannot disagree about a coordinate.

Targets (see the handoff, section 1.4):
    <server>/data/dbc/                        server-side, one per realm
    /home/brokilodeluxe/itemforge/dbc/        staging for the client patch

Usage:
    python arena_dbc.py --dry-run <dbc-dir> [<dbc-dir> ...]
    python arena_dbc.py           <dbc-dir> [<dbc-dir> ...]
    python arena_dbc.py --restore <dbc-dir>       # back to .bak-arenas

Idempotent: a row whose id is already present is left alone, so re-running after
a partial failure is safe. Note that means changing a value needs --restore
first, then a fresh apply -- rows are appended, never rewritten in place.

Which DBCs, and which not:
    Map, BattlemasterList, PvpDifficulty, WorldSafeLocs, WorldStateUI.
    No AreaTable  -- arenas carry AreaTableID 0 (map 980 and 1134 both do), and
                     an area row no ADT references would never be selected.
    No WorldMapArea -- arenas have no world map to draw a player arrow on.
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "tanaris"))

from tanaris_dbc import SPECS, loc, patch, restore  # noqa: E402
from gen_arena_sql import (ARENAS, AREA_AMBIENCE, AREA_AMBIENT_MUL,  # noqa: E402
                           AREA_FLAGS, AREA_IDS, AREA_INTROSOUND, AREA_ZONEMUSIC,
                           AREABIT_BASE, DESC_MASK, LANG_MASK, PVPDIFF_BASE,
                           SPAWN_OFFSET, SPAWN_Z_LIFT, WSL_BASE, WSUI_BASE,
                           measured, start_positions)

BACKUP_SUFFIX = ".bak-arenas"
FILES = ["Map.dbc", "BattlemasterList.dbc", "PvpDifficulty.dbc",
         "WorldSafeLocs.dbc", "WorldStateUI.dbc", "AreaTable.dbc"]


def upsert(path, spec, rows, dry_run=False, backup_suffix=BACKUP_SUFFIX):
    """Append rows that are new, and rewrite rows whose id is already there.

    tanaris_dbc.patch() only appends, and leaves an id it already sees alone.
    That is right for a first apply but wrong for a correction: when a surveyed
    coordinate replaces a derived one, the record has to actually change.

    Numeric fields are overwritten in place, which is exact -- records are fixed
    width. A string field is only touched when its text differs, and then the new
    text is APPENDED to the string block and the record repointed at it, never
    patched over: the replacement can be longer, and every other record's offset
    has to stay valid.
    """
    import shutil
    import struct

    name = os.path.basename(path)
    with open(path, "rb") as f:
        blob = f.read()

    magic, rec_count, field_count, rec_size, str_size = struct.unpack_from("<4sIIII", blob, 0)
    if magic != b"WDBC":
        raise ValueError("%s: not a WDBC file" % name)
    if field_count != spec["count"] or len(spec["fields"]) != field_count:
        raise ValueError("%s: field spec does not match the file" % name)

    rec_start = 20
    rec_bytes = bytearray(blob[rec_start:rec_start + rec_count * rec_size])
    str_block = bytearray(blob[rec_start + rec_count * rec_size:])

    index = {}
    for i in range(rec_count):
        index[struct.unpack_from("<i", rec_bytes, i * rec_size)[0]] = i

    def read_str(off):
        if off == 0:
            return ""
        end = str_block.index(b"\x00", off)
        return str_block[off:end].decode("utf-8", "replace")

    added = changed = same = 0
    for row in rows:
        if len(row) != field_count:
            raise ValueError("%s: row has %d values, need %d" % (name, len(row), field_count))

        if row[0] not in index:
            packed = bytearray()
            for kind, value in zip(spec["fields"], row):
                if kind == "i":
                    packed += struct.pack("<i", int(value))
                elif kind == "f":
                    packed += struct.pack("<f", float(value))
                else:
                    if value == "":
                        packed += struct.pack("<I", 0)
                    else:
                        packed += struct.pack("<I", len(str_block))
                        str_block += value.encode("utf-8") + b"\x00"
            rec_bytes += packed
            index[row[0]] = rec_count
            rec_count += 1
            added += 1
            continue

        base = index[row[0]] * rec_size
        dirty = False
        for fi, (kind, value) in enumerate(zip(spec["fields"], row)):
            off = base + fi * 4
            if kind == "i":
                if struct.unpack_from("<i", rec_bytes, off)[0] != int(value):
                    struct.pack_into("<i", rec_bytes, off, int(value))
                    dirty = True
            elif kind == "f":
                # Compare the encoded float32 bytes, not the Python float64
                # values.  Large coordinates such as -9235.11 cannot be
                # represented within 1e-4 by a float32, so a tolerance check
                # incorrectly marked an already-canonical record dirty on
                # every run.
                encoded = struct.pack("<f", float(value))
                if rec_bytes[off:off + 4] != encoded:
                    rec_bytes[off:off + 4] = encoded
                    dirty = True
            else:
                cur_off = struct.unpack_from("<I", rec_bytes, off)[0]
                if read_str(cur_off) != value:
                    if value == "":
                        struct.pack_into("<I", rec_bytes, off, 0)
                    else:
                        struct.pack_into("<I", rec_bytes, off, len(str_block))
                        str_block += value.encode("utf-8") + b"\x00"
                    dirty = True
        if dirty:
            changed += 1
        else:
            same += 1

    print("      %d appended, %d updated, %d already correct" % (added, changed, same))
    if not (added or changed):
        return 0

    out = bytearray(struct.pack("<4sIIII", b"WDBC", rec_count, field_count,
                                rec_size, len(str_block)))
    out += rec_bytes
    out += str_block

    if dry_run:
        print("      (dry run, %d bytes not written)" % len(out))
        return added + changed

    backup = path + backup_suffix
    if not os.path.exists(backup):
        shutil.copyfile(path, backup)
    tmp = path + ".arena-tmp"
    with open(tmp, "wb") as f:
        f.write(out)
    os.replace(tmp, path)
    return added + changed


_areabit_counter = [AREABIT_BASE]


def _next_areabit():
    b = _areabit_counter[0]
    _areabit_counter[0] += 1
    return b


def build_rows():
    _areabit_counter[0] = AREABIT_BASE
    rows = {name: [] for name in FILES}

    for i, (bg, mid, directory, name, cx, cy, cz, conf, ev) in enumerate(ARENAS):
        # ---- Map.dbc: shape copied from map 980 (Tol'viron)
        rows["Map.dbc"].append(
            [mid, directory,
             4,             # InstanceType 4 = arena
             1,             # Flags
             0,             # PVP: 0, as every existing arena has
             ] + loc(name, LANG_MASK) + [
             # AreaTableID: the arena's own area. Tol'viron and Tiger's Peak
             # carry 0 here, but pointing it at a real row gives the server a
             # sane answer when something asks the map what area it is.
             (AREA_IDS.get(bg) or [0])[0],
             ] + loc("", DESC_MASK) + loc("", DESC_MASK) + [
             319,           # LoadingScreenID: the arena loading screen
             0.0,           # MinimapIconScale
             0,             # CorpseMapID
             0.0, 0.0,      # CorpseX / CorpseY
             -1,            # TimeOfDayOverride
             0,             # ExpansionID
             0,             # RaidOffset
             0])            # MaxPlayers

        # ---- BattlemasterList.dbc: ID must equal the BattlegroundTypeId
        rows["BattlemasterList.dbc"].append(
            [bg,
             mid, -1, -1, -1, -1, -1, -1, -1,
             4,             # InstanceType 4 = arena
             1,             # GroupsAllowed
             ] + loc(name, LANG_MASK) + [
             5,             # MaxGroupSize
             0,             # HolidayWorldState: arenas have no weekend holiday
             10,            # Minlevel
             80])           # Maxlevel

        # ---- PvpDifficulty.dbc: 16 brackets, 10-89. Miss these and the queue
        # refuses everyone with nothing in the log.
        for r in range(16):
            rows["PvpDifficulty.dbc"].append(
                [PVPDIFF_BASE + i * 16 + r, mid, r, 10 + 5 * r, 14 + 5 * r, 0])

        # ---- WorldSafeLocs.dbc: team starts, surveyed where we have them
        (ax, ay, az, _ao), (hx, hy, hz, _ho) = start_positions(bg, cx, cy, cz)
        rows["WorldSafeLocs.dbc"].append(
            [WSL_BASE + i * 2, mid, ax, ay, az]
            + loc("%s - Alliance Start" % name, LANG_MASK))
        rows["WorldSafeLocs.dbc"].append(
            [WSL_BASE + i * 2 + 1, mid, hx, hy, hz]
            + loc("%s - Horde Start" % name, LANG_MASK))

        # ---- AreaTable.dbc: the ids the terrain already references, so the
        # arena names itself instead of inheriting the last zone's name.
        for area_id in AREA_IDS.get(bg, []):
            rows["AreaTable.dbc"].append(
                [area_id,
                 mid,               # ContinentID -> this arena's map
                 0,                 # ParentAreaID: each stands alone
                 _next_areabit(),
                 AREA_FLAGS,
                 0, 0,              # SoundProviderPref / underwater
                 AREA_AMBIENCE, AREA_ZONEMUSIC, AREA_INTROSOUND,
                 0,                 # ExplorationLevel
                 ] + loc(name, LANG_MASK) + [
                 0,                 # FactionGroupMask
                 0, 0, 0, 0,        # LiquidTypeID_1..4
                 -500.0,            # MinElevation
                 AREA_AMBIENT_MUL,
                 0])                # Lightid

        # ---- WorldStateUI.dbc: the "N players remaining" pair, copied from
        # Tiger's Peak's 90006/90007. StateVariable 3610 gates whether the pair
        # is drawn; %3600w / %3601w substitute the live alive-player counts.
        rows["WorldStateUI.dbc"].append(
            [WSUI_BASE + i * 2, mid, 0, 0, ""]
            + loc("Green Team: %3600w Players Remaining", LANG_MASK)
            + loc("", DESC_MASK) + [3610, 0, ""]
            + loc("", DESC_MASK) + ["", 0, 0, 0])
        rows["WorldStateUI.dbc"].append(
            [WSUI_BASE + i * 2 + 1, mid, 0, 0, ""]
            + loc("Gold Team: %3601w Players Remaining", LANG_MASK)
            + loc("", DESC_MASK) + [3610, 0, ""]
            + loc("", DESC_MASK) + ["", 0, 0, 0])

    return rows


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    dry_run = "--dry-run" in sys.argv
    do_restore = "--restore" in sys.argv
    if not args:
        raise SystemExit(__doc__)

    rows = build_rows()

    # Fail before touching anything if a row is the wrong width -- a short row
    # would otherwise be caught halfway through a directory, leaving some files
    # written and some not.
    for name, rs in rows.items():
        want = SPECS[name]["count"]
        for r in rs:
            if len(r) != want:
                raise SystemExit("%s: built a row with %d values, spec wants %d"
                                 % (name, len(r), want))
    print("row widths OK: " + ", ".join("%s=%d" % (n, len(r)) for n, r in rows.items()))

    total = 0
    for d in args:
        print("=== %s ===" % d)
        if not os.path.isdir(d):
            print("    NOT A DIRECTORY, skipped")
            continue
        for name in FILES:
            path = os.path.join(d, name)
            if not os.path.exists(path):
                print("    %-24s missing, skipped" % name)
                continue
            print("    %s" % name)
            if do_restore:
                restore_path(path)
                continue
            # upsert, not patch: a re-run after a survey has to actually change
            # the coordinates it already wrote, not skip them as "present".
            total += upsert(path, SPECS[name], rows[name], dry_run, BACKUP_SUFFIX)

    print("\n%d rows %s" % (total, "would be appended" if dry_run else "appended"))


def restore_path(path):
    import shutil
    backup = path + BACKUP_SUFFIX
    if not os.path.exists(backup):
        print("      no %s backup, left alone" % BACKUP_SUFFIX)
        return
    shutil.copyfile(backup, path)
    print("      restored from %s" % os.path.basename(backup))


if __name__ == "__main__":
    main()
