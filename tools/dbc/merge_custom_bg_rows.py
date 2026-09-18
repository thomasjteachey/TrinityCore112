"""Copy the custom battleground rows into another archive's DBC tables.

WHY. `patch-dungeon-maps` installs as **patch-M.MPQ** - a NON-locale archive -
and it carries its own DBFilesClient/WorldMapArea.dbc, DungeonMap.dbc and
DungeonMapChunk.dbc. Archives rank by their LAST letter, and a non-locale "M"
outranks a locale "A", so patch-M's copies beat patch-enUS-A's. Its
WorldMapArea has 161 rows and **not one row for any custom battleground map** -
1189 Scarlet Chapel, 1230 Blackrock Throne, 1615 Obsidian Colosseum, 1620
Tanaris and 1608 Violet Hold all lost their world map the day it shipped
(2026-09-14). That is why Scarlet Chapel's map worked on L+ and not on
Centurion, and why three rounds of edits to patch-enUS-A changed nothing: the
client never read them.

patch-M was almost certainly built from a clean table, so this MERGES rather
than replaces - it keeps every row patch-M has (that is the whole point of the
dungeon-maps patch) and copies in only the rows whose MapID is one of ours.

Strings are copied by APPENDING to the target's string block and repointing the
field, never by editing in place: every record indexes that block by byte
offset, so rewriting one in place corrupts every row after it.

Idempotent: a row already present is overwritten with the source's values, so
re-running after a partial failure is safe. The original is copied to
<name>.bak-bgmerge once and never overwritten by a later run.

    python merge_custom_bg_rows.py <source-dbc-dir> <target-dbc-dir> [--dry-run]

Source is a directory holding the GOOD tables (patch-enUS-A's, as fixed by
fix_bg_world_maps.py); target is the one to merge into (patch-M's).
"""
import os
import shutil
import struct
import sys

# Every map id whose rows belong to us. Anything keyed on one of these is
# copied; everything else in the target is left exactly as it is.
CUSTOM_MAPS = {1189, 1230, 1615, 1620, 1608}

# field spec per file, and which column holds the MapID
SPECS = {
    "WorldMapArea.dbc": {
        "fields": "iii" + "s" + "ffff" + "iii",
        "count": 11,
        "map_field": 1,
    },
    "DungeonMap.dbc": {
        # ID, MapID, FloorIndex, MinY, MaxY, MinX, MaxX, ParentWorldMapID
        "fields": "iii" + "ffff" + "i",
        "count": 8,
        "map_field": 1,
    },
    "DungeonMapChunk.dbc": {
        # ID, MapID, WmoGroupID, DungeonMapID, MinZ
        "fields": "iiii" + "f",
        "count": 5,
        "map_field": 1,
    },
}


def read_dbc(path, spec):
    with open(path, "rb") as f:
        blob = f.read()
    name = os.path.basename(path)
    magic, rec_count, field_count, rec_size, str_size = struct.unpack_from("<4sIIII", blob, 0)
    if magic != b"WDBC":
        raise ValueError("%s: not a WDBC file" % name)
    if field_count != spec["count"] or rec_size != spec["count"] * 4:
        raise ValueError("%s: expected %d fields / %d bytes, found %d / %d"
                         % (name, spec["count"], spec["count"] * 4, field_count, rec_size))
    rec_start = 20
    recs = bytearray(blob[rec_start:rec_start + rec_count * rec_size])
    strs = bytearray(blob[rec_start + rec_count * rec_size:])
    if len(strs) != str_size:
        raise ValueError("%s: string block is %d bytes, header says %d" % (name, len(strs), str_size))
    return rec_count, rec_size, recs, strs


def read_string(block, off):
    if off == 0 or off >= len(block):
        return ""
    end = block.find(b"\x00", off)
    return block[off:end].decode("utf-8", "replace")


def merge(src_path, dst_path, spec, dry_run=False):
    name = os.path.basename(dst_path)
    s_count, s_size, s_recs, s_strs = read_dbc(src_path, spec)
    d_count, d_size, d_recs, d_strs = read_dbc(dst_path, spec)
    fields = spec["fields"]
    mf = spec["map_field"]

    # Decode the source rows we care about into python values, so the string
    # fields come across as text rather than offsets into the WRONG block.
    wanted = []
    for i in range(s_count):
        off = i * s_size
        map_id = struct.unpack_from("<i", s_recs, off + mf * 4)[0]
        if map_id not in CUSTOM_MAPS:
            continue
        row = []
        for fi, kind in enumerate(fields):
            raw = struct.unpack_from("<I", s_recs, off + fi * 4)[0]
            if kind == "s":
                row.append(read_string(s_strs, raw))
            elif kind == "f":
                row.append(struct.unpack_from("<f", s_recs, off + fi * 4)[0])
            else:
                row.append(struct.unpack_from("<i", s_recs, off + fi * 4)[0])
        wanted.append(row)

    index_of = {}
    for i in range(d_count):
        index_of[struct.unpack_from("<i", d_recs, i * d_size)[0]] = i

    def pack(row):
        out = bytearray()
        for kind, value in zip(fields, row):
            if kind == "s":
                if value == "":
                    out += struct.pack("<I", 0)
                else:
                    out += struct.pack("<I", len(d_strs))
                    d_strs.extend(value.encode("utf-8") + b"\x00")
            elif kind == "f":
                out += struct.pack("<f", float(value))
            else:
                out += struct.pack("<i", int(value))
        return out

    added = replaced = 0
    for row in wanted:
        blob = pack(row)
        if row[0] in index_of:
            i = index_of[row[0]]
            if d_recs[i * d_size:(i + 1) * d_size] == blob:
                print("      id %-6s map %-5s already identical" % (row[0], row[mf]))
                continue
            d_recs[i * d_size:(i + 1) * d_size] = blob
            print("      id %-6s map %-5s replaced" % (row[0], row[mf]))
            replaced += 1
        else:
            d_recs += blob
            index_of[row[0]] = d_count
            d_count += 1
            print("      id %-6s map %-5s added" % (row[0], row[mf]))
            added += 1

    print("    %s: %d source rows for our maps -> %d added, %d replaced"
          % (name, len(wanted), added, replaced))
    if not (added or replaced):
        return 0
    if dry_run:
        print("    (dry run, nothing written)")
        return added + replaced

    out = bytearray(struct.pack("<4sIIII", b"WDBC", d_count, spec["count"],
                                spec["count"] * 4, len(d_strs)))
    out += d_recs
    out += d_strs

    backup = dst_path + ".bak-bgmerge"
    if not os.path.exists(backup):
        shutil.copyfile(dst_path, backup)
    tmp = dst_path + ".bgmerge-tmp"
    with open(tmp, "wb") as f:
        f.write(out)
    os.replace(tmp, dst_path)
    return added + replaced


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    dry_run = "--dry-run" in sys.argv
    if len(args) != 2:
        raise SystemExit("usage: merge_custom_bg_rows.py [--dry-run] <source-dbc-dir> <target-dbc-dir>")
    src, dst = args
    total = 0
    for fname, spec in SPECS.items():
        sp, dp = os.path.join(src, fname), os.path.join(dst, fname)
        if not os.path.exists(sp):
            print("  %-22s source missing, skipped" % fname); continue
        if not os.path.exists(dp):
            print("  %-22s target missing, skipped" % fname); continue
        print("  %s" % fname)
        total += merge(sp, dp, spec, dry_run)
    print("total rows added/replaced: %d%s" % (total, " (dry run)" if dry_run else ""))


if __name__ == "__main__":
    main()
