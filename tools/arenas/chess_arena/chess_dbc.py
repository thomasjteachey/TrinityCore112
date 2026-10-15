"""The Gamesman's Hall arena: its DBC rows.

    python chess_dbc.py show                          # print every row, no writes
    python chess_dbc.py apply <dbc-dir> [--only Map,WorldSafeLocs] [--dry-run]
    python chess_dbc.py verify <dbc-dir> [--only ...]

The writer, the field layouts and the templating are the Eye of Eternity's
(tools/eoe_arena/eoe_dbc.py): rows are copied from Centurion's own client copies and
changed field by field, so every layer gets the same bytes.

    Map 1532               532's (Karazhan) row as an arena: InstanceType 4, flags 1, no
                           corpse map, expansion 0 (the realm is expansion locked). It keeps
                           532's Directory, so the client loads Karazhan's own terrain and
                           castle with no new files, and 532's loading screen and zone.
    BattlemasterList 888   The Eye of Eternity's (886) shape.
    PvpDifficulty          sixteen 5-level brackets, 10-89, like every arena.
    WorldSafeLocs          the two balcony starts.
    WorldStateUI           the "N Players Remaining" pair, the Eye of Eternity's.
    MapDifficulty 762      server only: (1532, difficulty 0). Without it every static
                           `gameobject` row on 1532 (the doors and statues) is skipped as
                           "wrong spawn mask". chess_patch.py does not ship it.

`apply` only touches files present in the directory, backs each one up once as
<file>.bak-eoe (the shared writer's suffix), and is idempotent; an id already present with
other values stops the run.
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "eoe_arena"))
sys.path.insert(0, HERE)
import eoe_dbc as D  # noqa: E402
import chess_layout as L  # noqa: E402

TABLES = ("Map", "BattlemasterList", "PvpDifficulty", "WorldSafeLocs", "WorldStateUI", "MapDifficulty")


def build_rows():
    rows = {name: [] for name in TABLES}
    rows["Map"].append(D.client_row(
        "Map", L.SOURCE_MAP_ID,
        f0=L.MAP_ID, f1=L.DIRECTORY,
        f2=4,                 # InstanceType: arena (the client's arena UI keys off this)
        f3=1,                 # Flags, as every arena here
        f4=0,                 # PVP
        f5=L.NAME,            # MapName (enUS)
        f22=L.AREA_ID,
        f57=L.LOADING_SCREEN,
        f58=D.f32(1.0),       # MinimapIconScale
        f59=0xFFFFFFFF,       # CorpseMapID: none
        f60=0, f61=0,         # Corpse X/Y
        f62=0xFFFFFFFF,       # TimeOfDayOverride: none
        f63=0,                # ExpansionID
        f64=0, f65=0))        # RaidOffset, MaxPlayers

    rows["BattlemasterList"].append(D.client_row(
        "BattlemasterList", 886, f0=L.BG_TYPE_ID, f1=L.MAP_ID, f11=L.NAME))

    for bracket in range(16):
        low = 10 + 5 * bracket
        rows["PvpDifficulty"].append([L.PVPDIFF_FIRST + bracket, L.MAP_ID, bracket, low, low + 4, 0])

    for rid, team, start in ((L.WSL_ALLIANCE, "Alliance", L.ALLIANCE_START), (L.WSL_HORDE, "Horde", L.HORDE_START)):
        rows["WorldSafeLocs"].append(D.client_row(
            "WorldSafeLocs", 52628, f0=rid, f1=L.MAP_ID,
            f2=D.f32(start[0]), f3=D.f32(start[1]), f4=D.f32(start[2]),
            f5="%s - %s Start" % (L.NAME, team)))

    rows["WorldStateUI"].append(D.client_row("WorldStateUI", 90128, f0=L.WSUI_GREEN, f1=L.MAP_ID))
    rows["WorldStateUI"].append(D.client_row("WorldStateUI", 90129, f0=L.WSUI_GOLD, f1=L.MAP_ID))

    rows["MapDifficulty"].append(D.client_row("MapDifficulty", 112, f0=L.MAPDIFF_ID, f1=L.MAP_ID))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("command", choices=("show", "apply", "verify"))
    ap.add_argument("dir", nargs="?")
    ap.add_argument("--only", help="comma-separated table names")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    rows = build_rows()
    names = [n for n in TABLES if not args.only or n in args.only.split(",")]
    if args.command == "show":
        for name in names:
            for row in rows[name]:
                print(name, [v if not isinstance(v, str) else repr(v) for v in row if v not in ("", 0)])
        return 0
    if not args.dir or not os.path.isdir(args.dir):
        ap.error("need a DBC directory")
    print(args.dir)
    bad = 0
    for name in names:
        path = os.path.join(args.dir, name + ".dbc")
        if not os.path.exists(path):
            print("    %-22s not present here, skipped" % (name + ".dbc"))
            continue
        if args.command == "apply":
            D.upsert(path, name, rows[name], args.dry_run)
        else:
            bad += D.verify(path, name, rows[name])
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
