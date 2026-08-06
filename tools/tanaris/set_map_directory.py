"""Repoint a Map.dbc row's Directory field at a different terrain folder.

Used to move battleground map 1620 off the shared "Kalimdor" terrain and onto
its own "TanarisBG" copy, so editing the battleground cannot change the live
Tanaris zone.

This edits one existing row rather than appending a new one, so it cannot go
through tanaris_dbc.py's append path. It also deliberately does NOT go through
that tool's --restore: the servers' WorldStateUI.dbc was re-synced after its
backup was taken, and a blanket restore would quietly undo that.

The edit is the standard WDBC string move: append the new string to the end of
the string block and repoint the record's offset at it. Existing offsets stay
valid, so nothing else in the file has to change. The old string is left in
place as a few orphaned bytes, which is harmless.

    python set_map_directory.py <dbc-dir> [<dbc-dir> ...] --id 1620 --dir TanarisBG
"""
import os
import shutil
import struct
import sys

# Map.dbc: field 0 is ID, field 1 is Directory (a string offset).
FIELD_ID = 0
FIELD_DIRECTORY = 1
EXPECTED_FIELDS = 66
EXPECTED_RECSIZE = 264


def read_cstr(block, off):
    if off == 0:
        return ""
    return block[off:block.index(b"\x00", off)].decode("utf-8", "replace")


def set_directory(path, map_id, new_dir, dry_run=False):
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
        if struct.unpack_from("<i", recs, i * rec_size + FIELD_ID * 4)[0] == map_id:
            target = i
            break
    if target is None:
        print("      map id %d not present, skipped" % map_id)
        return False

    off_pos = target * rec_size + FIELD_DIRECTORY * 4
    cur_off = struct.unpack_from("<I", recs, off_pos)[0]
    cur = read_cstr(block, cur_off)
    if cur == new_dir:
        print("      Directory already %r, left alone" % new_dir)
        return False

    new_off = len(block)
    block += new_dir.encode("utf-8") + b"\x00"
    struct.pack_into("<I", recs, off_pos, new_off)

    print("      Directory %r -> %r" % (cur, new_dir))
    if dry_run:
        print("      (dry run, nothing written)")
        return True

    backup = path + ".bak-mapdir"
    if not os.path.exists(backup):
        shutil.copyfile(path, backup)

    out = bytearray(struct.pack("<4sIIII", b"WDBC", rec_count, field_count, rec_size, len(block)))
    out += recs
    out += block
    tmp = path + ".mapdir-tmp"
    with open(tmp, "wb") as f:
        f.write(out)
    os.replace(tmp, path)
    return True


def main():
    args = sys.argv[1:]
    dry_run = "--dry-run" in args
    map_id = 1620
    new_dir = "TanarisBG"
    dirs = []
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--id":
            i += 1
            map_id = int(args[i])
        elif a == "--dir":
            i += 1
            new_dir = args[i]
        elif not a.startswith("--"):
            dirs.append(a)
        i += 1

    if not dirs:
        raise SystemExit(__doc__)

    changed = 0
    for d in dirs:
        print("=== %s ===" % d)
        p = os.path.join(d, "Map.dbc")
        if not os.path.exists(p):
            print("    Map.dbc missing, skipped")
            continue
        print("    Map.dbc")
        if set_directory(p, map_id, new_dir, dry_run):
            changed += 1
    print("files changed: %d%s" % (changed, " (dry run)" if dry_run else ""))


if __name__ == "__main__":
    main()
