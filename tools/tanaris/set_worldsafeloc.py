"""Rewrite the position of existing WorldSafeLocs.dbc rows, in place.

Positions are fixed-width floats, so unlike a name or a new row this needs no
string-block work and no append -- the values are simply overwritten where they
sit. That matters because the alternative, tanaris_dbc.py --restore followed by
a re-apply, would also roll back unrelated repairs made to those directories
since their backups were taken.

    python set_worldsafeloc.py <dbc-dir> [<dbc-dir> ...]

Edit POSITIONS below to change what it writes.
"""
import os
import shutil
import struct
import sys

EXPECTED_FIELDS = 22
EXPECTED_RECSIZE = 88
FIELD_ID = 0
FIELD_X, FIELD_Y, FIELD_Z = 2, 3, 4

# id -> (x, y, z, label). Start and graveyard share a position per team: the
# spirit healer stands on the spawn point.
POSITIONS = {
    52500: (-9235.11, -3009.99, 17.19, "Tanaris - Alliance Start"),
    52502: (-9235.11, -3009.99, 17.19, "Tanaris - Alliance Graveyard"),
    52501: (-7166.45, -3760.62,  9.40, "Tanaris - Horde Start"),
    52503: (-7166.45, -3760.62,  9.40, "Tanaris - Horde Graveyard"),
}


def patch(path, dry_run=False):
    with open(path, "rb") as f:
        blob = bytearray(f.read())

    magic, rec_count, field_count, rec_size, str_size = struct.unpack_from("<4sIIII", blob, 0)
    if magic != b"WDBC":
        raise ValueError("%s: not a WDBC file" % path)
    if field_count != EXPECTED_FIELDS or rec_size != EXPECTED_RECSIZE:
        raise ValueError("%s: expected %d fields / %d bytes, found %d / %d"
                         % (path, EXPECTED_FIELDS, EXPECTED_RECSIZE, field_count, rec_size))

    changed = 0
    for i in range(rec_count):
        base = 20 + i * rec_size
        rid = struct.unpack_from("<i", blob, base)[0]
        if rid not in POSITIONS:
            continue
        x, y, z, label = POSITIONS[rid]
        old = tuple(round(struct.unpack_from("<f", blob, base + f * 4)[0], 2)
                    for f in (FIELD_X, FIELD_Y, FIELD_Z))
        if old == (round(x, 2), round(y, 2), round(z, 2)):
            print("      %-6d %-30s already correct" % (rid, label))
            continue
        struct.pack_into("<f", blob, base + FIELD_X * 4, x)
        struct.pack_into("<f", blob, base + FIELD_Y * 4, y)
        struct.pack_into("<f", blob, base + FIELD_Z * 4, z)
        print("      %-6d %-30s %s -> (%.2f, %.2f, %.2f)" % (rid, label, old, x, y, z))
        changed += 1

    if not changed or dry_run:
        if dry_run and changed:
            print("      (dry run, %d rows not written)" % changed)
        return changed

    backup = path + ".bak-wsl"
    if not os.path.exists(backup):
        shutil.copyfile(path, backup)
    tmp = path + ".wsl-tmp"
    with open(tmp, "wb") as f:
        f.write(blob)
    os.replace(tmp, path)
    return changed


def main():
    dirs = [a for a in sys.argv[1:] if not a.startswith("--")]
    dry_run = "--dry-run" in sys.argv
    if not dirs:
        raise SystemExit(__doc__)
    total = 0
    for d in dirs:
        print("=== %s ===" % d)
        p = os.path.join(d, "WorldSafeLocs.dbc")
        if not os.path.exists(p):
            print("    WorldSafeLocs.dbc missing, skipped")
            continue
        print("    WorldSafeLocs.dbc")
        total += patch(p, dry_run)
    print("rows changed: %d%s" % (total, " (dry run)" if dry_run else ""))


if __name__ == "__main__":
    main()
