"""Repoint a WorldMapArea row's AreaName at a different art directory.

AreaName is what the client uses to find world map art:
Interface\\WorldMap\\<AreaName>\\<AreaName>1..12.blp. Row 9532 (map 1620) was
borrowing "Tanaris", so the battleground drew Blizzard's Tanaris image and
depended on WorldMapOverlay rows for the zone detail -- which the client
refuses to match to map 1620 (GetNumMapOverlays() returns 0 there).

Pointing it at "TanarisBG" instead, alongside the pre-baked art from
build_worldmap_art.py, makes the whole map visible with no overlays involved.

THIS AND THE ART MUST SHIP TOGETHER. On its own this change makes the map
blank, because the client will look for art that is not in the patch yet.

    python set_worldmaparea_name.py <dbc-dir> [...] --id 9532 --name TanarisBG
"""
import os
import shutil
import struct
import sys

EXPECTED_FIELDS = 11
EXPECTED_RECSIZE = 44
FIELD_ID = 0
FIELD_AREANAME = 3


def read_cstr(block, off):
    if off == 0:
        return ""
    return block[off:block.index(b"\x00", off)].decode("utf-8", "replace")


def set_name(path, row_id, new_name, dry_run=False):
    with open(path, "rb") as f:
        blob = f.read()

    magic, rec_count, field_count, rec_size, str_size = struct.unpack_from("<4sIIII", blob, 0)
    if magic != b"WDBC":
        raise ValueError("%s: not a WDBC file" % path)
    if field_count != EXPECTED_FIELDS or rec_size != EXPECTED_RECSIZE:
        raise ValueError("%s: expected %d fields / %d bytes, found %d / %d"
                         % (path, EXPECTED_FIELDS, EXPECTED_RECSIZE, field_count, rec_size))

    recs = bytearray(blob[20:20 + rec_count * rec_size])
    block = bytearray(blob[20 + rec_count * rec_size:])
    if len(block) != str_size:
        raise ValueError("%s: string block is %d bytes, header says %d"
                         % (path, len(block), str_size))

    target = None
    for i in range(rec_count):
        if struct.unpack_from("<i", recs, i * rec_size + FIELD_ID * 4)[0] == row_id:
            target = i
            break
    if target is None:
        print("      row %d not present, skipped" % row_id)
        return False

    off_pos = target * rec_size + FIELD_AREANAME * 4
    cur = read_cstr(block, struct.unpack_from("<I", recs, off_pos)[0])
    if cur == new_name:
        print("      AreaName already %r, left alone" % new_name)
        return False

    # Append the new string and repoint; existing offsets stay valid.
    struct.pack_into("<I", recs, off_pos, len(block))
    block += new_name.encode("utf-8") + b"\x00"
    print("      AreaName %r -> %r" % (cur, new_name))

    if dry_run:
        print("      (dry run, nothing written)")
        return True

    backup = path + ".bak-wmaname"
    if not os.path.exists(backup):
        shutil.copyfile(path, backup)

    out = bytearray(struct.pack("<4sIIII", b"WDBC", rec_count, field_count, rec_size, len(block)))
    out += recs
    out += block
    tmp = path + ".wma-tmp"
    with open(tmp, "wb") as f:
        f.write(out)
    os.replace(tmp, path)
    return True


def main():
    args = sys.argv[1:]
    dry_run = "--dry-run" in args
    row_id, new_name, dirs = 9532, "TanarisBG", []
    i = 0
    while i < len(args):
        if args[i] == "--id":
            i += 1
            row_id = int(args[i])
        elif args[i] == "--name":
            i += 1
            new_name = args[i]
        elif not args[i].startswith("--"):
            dirs.append(args[i])
        i += 1

    if not dirs:
        raise SystemExit(__doc__)

    changed = 0
    for d in dirs:
        print("=== %s ===" % d)
        p = os.path.join(d, "WorldMapArea.dbc")
        if not os.path.exists(p):
            print("    WorldMapArea.dbc missing, skipped")
            continue
        print("    WorldMapArea.dbc")
        if set_name(p, row_id, new_name, dry_run):
            changed += 1
    print("files changed: %d%s" % (changed, " (dry run)" if dry_run else ""))


if __name__ == "__main__":
    main()
