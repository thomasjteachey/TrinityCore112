"""The Gamesman's Hall arena counts as outdoors everywhere: WMOAreaTable Flags |= 4 on Karazhan's
chess hall group.

    python chess_outdoors.py <WMOAreaTable.dbc> [--apply]

The server already treats every arena map as outdoors (Map::GetFullTerrainStatusForPosition), but
the client makes its own call - it refuses a mount or an outdoors-only spell before the server
ever hears of it - from the WMO group under the player and that group's WMOAreaTable row: Flags 4
forces outdoors, Flags 2 indoors. Every spot the arena's navmesh reaches (the hall, its
balconies and back rooms, the long stairways and the corner landings) stands on ONE group of
Kharazan_Instance.wmo: WMOID 4449, NameSet 0, group 19658 (an interior group, MOGP 0x2805), whose
row is id 36327 (area 3457, Flags 0). Rows are global per (WMOID, NameSet, group), so the real
Karazhan's chess room (map 532) reads outdoors too; nothing on this realm plays there.

Only that row's Flags field changes - in place, so the row order the client walks a WMO's block
in is untouched. --apply backs the file up once as <file>.bak-chess-outdoors and verifies that
no other byte moved. Idempotent.
"""
import os
import shutil
import struct
import sys

WMOID, NAMESET, GROUPS = 4449, 0, (19658,)
F_ID, F_WMOID, F_NAMESET, F_GROUP, F_FLAGS, F_AREA = 0, 1, 2, 3, 9, 10
OUTDOORS, INDOORS = 4, 2


def main():
    path = sys.argv[1]
    apply = "--apply" in sys.argv
    d = bytearray(open(path, "rb").read())
    magic, n, nf, rs, ss = struct.unpack_from("<4s4I", d, 0)
    assert magic == b"WDBC" and nf == 28 and rs == 112, (magic, nf, rs)
    hits = []
    for r in range(n):
        base = 20 + r * rs
        rec = struct.unpack_from("<28i", d, base)
        if rec[F_WMOID] == WMOID and rec[F_NAMESET] == NAMESET and rec[F_GROUP] in GROUPS:
            hits.append((r, base, rec))
    if len(hits) != len(GROUPS):
        raise SystemExit("%s: expected one row per group %s, found %d" % (path, GROUPS, len(hits)))
    todo = 0
    for r, base, rec in hits:
        want = (rec[F_FLAGS] | OUTDOORS) & ~INDOORS
        print("%s: row #%d id %d group %d area %d flags %d -> %d%s" % (
            os.path.basename(path), r, rec[F_ID], rec[F_GROUP], rec[F_AREA], rec[F_FLAGS], want,
            "" if want != rec[F_FLAGS] else " (already)"))
        if want != rec[F_FLAGS]:
            todo += 1
            struct.pack_into("<i", d, base + 4 * F_FLAGS, want)
    if not todo or not apply:
        if todo:
            print("dry run - pass --apply to write")
        return
    old = open(path, "rb").read()
    diff = [i for i in range(len(old)) if old[i] != d[i]]
    assert len(old) == len(d) and all(20 + r * rs + 4 * F_FLAGS <= i < 20 + r * rs + 4 * F_FLAGS + 4
                                      for i in diff for r in [((i - 20) // rs)]), "more than the Flags fields would change"
    backup = path + ".bak-chess-outdoors"
    if not os.path.exists(backup):
        shutil.copy2(path, backup)
    open(path + ".tmp", "wb").write(bytes(d))
    os.replace(path + ".tmp", path)
    print("written (%d byte(s) changed), backup %s" % (len(diff), os.path.basename(backup)))


if __name__ == "__main__":
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    main()
