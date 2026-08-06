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
from gen_arena_sql import (ARENAS, DESC_MASK, LANG_MASK, PVPDIFF_BASE,  # noqa: E402
                           SPAWN_OFFSET, SPAWN_Z_LIFT, WSL_BASE, WSUI_BASE)

BACKUP_SUFFIX = ".bak-arenas"
FILES = ["Map.dbc", "BattlemasterList.dbc", "PvpDifficulty.dbc",
         "WorldSafeLocs.dbc", "WorldStateUI.dbc"]


def build_rows():
    rows = {name: [] for name in FILES}

    for i, (bg, mid, directory, name, cx, cy, cz, conf, ev) in enumerate(ARENAS):
        # ---- Map.dbc: shape copied from map 980 (Tol'viron)
        rows["Map.dbc"].append(
            [mid, directory,
             4,             # InstanceType 4 = arena
             1,             # Flags
             0,             # PVP: 0, as every existing arena has
             ] + loc(name, LANG_MASK) + [
             0,             # AreaTableID: arenas leave this at 0
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

        # ---- WorldSafeLocs.dbc: team starts, X axis either side of centre
        z = cz + SPAWN_Z_LIFT
        rows["WorldSafeLocs.dbc"].append(
            [WSL_BASE + i * 2, mid, cx + SPAWN_OFFSET, cy, z]
            + loc("%s - Alliance Start" % name, LANG_MASK))
        rows["WorldSafeLocs.dbc"].append(
            [WSL_BASE + i * 2 + 1, mid, cx - SPAWN_OFFSET, cy, z]
            + loc("%s - Horde Start" % name, LANG_MASK))

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
            total += patch(path, SPECS[name], rows[name], dry_run, BACKUP_SUFFIX)

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
