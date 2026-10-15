#!/usr/bin/env python3
"""Server side: The Gamesman's Hall's client rows into patch-enUS-A.

    chess_patch.py extract <work>     # live zip -> <work>/patch-enUS-A/dbc/patch-enUS-A.MPQ/*.dbc
    (download them, chess_dbc.py apply <dir>, upload to <work>/patch-enUS-A/dbc-new/patch-enUS-A.MPQ/)
    chess_patch.py pack <work>
    chess_patch.py publish <work>

Pass an ABSOLUTE <work>: smpq runs from a subdirectory of it. The packing is
tools/eoe_arena/eoe_patch.py (md5 round trip of what goes in, formatVersion and name count
unchanged, refuses to publish over a zip that moved since extract) with five tables and no
terrain: the map shares Karazhan's directory, so the client already has every file it loads.
MapDifficulty is server only.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)                                       # beside it on the server
sys.path.insert(0, os.path.join(HERE, "..", "..", "eoe_arena"))
import eoe_patch as P  # noqa: E402

P.ARCHIVES = {"patch-enUS-A": {"patch-enUS-A.MPQ": ["Map", "BattlemasterList", "PvpDifficulty",
                                                    "WorldSafeLocs", "WorldStateUI"]}}
P.TERRAIN_INTO = None
P.BACKUP_TAG = "chess"

if __name__ == "__main__":
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    cmd, work = sys.argv[1], sys.argv[2]
    if cmd == "extract":
        P.extract(work)
    elif cmd == "pack":
        P.pack(work)
    elif cmd == "publish":
        P.publish(work, set(sys.argv[3:]))
    else:
        raise SystemExit(__doc__)
