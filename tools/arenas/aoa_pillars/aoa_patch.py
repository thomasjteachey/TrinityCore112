#!/usr/bin/env python3
"""Server side: GameObjectDisplayInfo 11971 (the Amphitheater's columns) into patch-enUS-A.

    aoa_patch.py extract <work>     # live zip -> <work>/patch-enUS-A/dbc/patch-enUS-A.MPQ/GameObjectDisplayInfo.dbc
    (download it, aoa_pillars.py dbc <dir>, upload to <work>/patch-enUS-A/dbc-new/patch-enUS-A.MPQ/)
    aoa_patch.py pack <work>
    aoa_patch.py publish <work>

The packing is tools/eoe_arena/eoe_patch.py (md5 round trip of what goes in, formatVersion and
name count unchanged, refuses to publish over a zip that moved since extract) with one table and
no terrain. GameObjectDisplayInfo's winning client copy is patch-enUS-A's: no art or HD pack
carries one.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)                                       # beside it on the server
sys.path.insert(0, os.path.join(HERE, "..", "..", "eoe_arena"))
import eoe_patch as P  # noqa: E402

P.ARCHIVES = {"patch-enUS-A": {"patch-enUS-A.MPQ": ["GameObjectDisplayInfo"]}}
P.TERRAIN_INTO = None
P.BACKUP_TAG = "aoapillars"

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
