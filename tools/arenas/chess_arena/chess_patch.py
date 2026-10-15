#!/usr/bin/env python3
"""Server side: The Gamesman's Hall's client rows into patch-enUS-A.

    chess_patch.py extract <work> [--only T1,T2]   # live zip -> <work>/patch-enUS-A/dbc/patch-enUS-A.MPQ/*.dbc
    (download them, chess_dbc.py apply <dir>, upload to <work>/patch-enUS-A/dbc-new/patch-enUS-A.MPQ/)
    chess_patch.py pack <work> [--only ...]
    chess_patch.py publish <work> [--only ...]

Pass an ABSOLUTE <work>: smpq runs from a subdirectory of it. The packing is
tools/eoe_arena/eoe_patch.py (md5 round trip of what goes in, formatVersion and name count
unchanged, refuses to publish over a zip that moved since extract) with five tables and no
terrain: the map shares Karazhan's directory, so the client already has every file it loads.
MapDifficulty is server only. --only ships a subset: the second layout's one new client row
was GameObjectDisplayInfo 11973 (`--only GameObjectDisplayInfo`, backups tagged "gates").
Pass the same --only to all three steps.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)                                       # beside it on the server
sys.path.insert(0, os.path.join(HERE, "..", "..", "eoe_arena"))
import eoe_patch as P  # noqa: E402

P.ARCHIVES = {"patch-enUS-A": {"patch-enUS-A.MPQ": ["Map", "BattlemasterList", "PvpDifficulty",
                                                    "WorldSafeLocs", "WorldStateUI", "GameObjectDisplayInfo"]}}
P.TERRAIN_INTO = None
P.BACKUP_TAG = "chess"

if __name__ == "__main__":
    args = sys.argv[1:]
    if "--only" in args:
        i = args.index("--only")
        tables = args[i + 1].split(",")
        del args[i:i + 2]
        P.ARCHIVES = {"patch-enUS-A": {"patch-enUS-A.MPQ": tables}}
        P.BACKUP_TAG = "gates" if tables == ["GameObjectDisplayInfo"] else "chess"
    if len(args) < 2:
        raise SystemExit(__doc__)
    sys.argv = [sys.argv[0]] + args
    cmd, work = sys.argv[1], sys.argv[2]
    if cmd == "extract":
        P.extract(work)
    elif cmd == "pack":
        P.pack(work)
    elif cmd == "publish":
        P.publish(work, set(sys.argv[3:]))
    else:
        raise SystemExit(__doc__)
