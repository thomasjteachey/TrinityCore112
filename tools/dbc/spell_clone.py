#!/usr/bin/env python3
"""Dump a Spell.dbc row, or append clones of one with fields overridden.

    spell_clone.py dump  <Spell.dbc> <id>
    spell_clone.py clone <Spell.dbc> <donor> <spec.json> [--write]

spec.json is a list of {"id": N, "name": "...", "description": "...",
"tooltip": "...", "fields": {"<index>": value, ...}}. Indices are 0-based
Spell.dbc FIELD indices (234 per record), NOT mirror column ordinals: see
reference_spell_lplus_pipeline for why those differ (+0 / +1 / +2).

Clones are appended in ascending id order; the new ids must all be above the
file's current highest id and not already present, or nothing is written.
Strings go on the end of the string block. Without --write it only prints
what it would do. With --write the original is kept as <file>.bak-<stamp>.

Fixed 3.3.5 field indices used here (DBCStructure.h SpellEntry):
    136 SpellName[enUS]   153 Rank[enUS]   170 Description[enUS]   187 ToolTip[enUS]
    each string group is 16 locale slots + 1 flags dword; the flags dword of the
    enUS-only strings is copied from the donor.
"""
import json
import shutil
import struct
import sys
import time

HEADER = 20
NAME, RANK, DESC, TIP = 136, 153, 170, 187


def load(path):
    blob = open(path, "rb").read()
    magic, records, fields, recsize, strsize = struct.unpack_from("<4sIIII", blob, 0)
    if magic != b"WDBC" or fields != 234 or recsize != 936:
        sys.exit("unexpected header %r %d %d %d" % (magic, records, fields, recsize))
    rows = blob[HEADER:HEADER + records * recsize]
    strings = blob[HEADER + records * recsize:]
    assert len(strings) == strsize
    return records, rows, strings


def row_of(rows, records, sid):
    for r in range(records):
        if struct.unpack_from("<I", rows, r * 936)[0] == sid:
            return list(struct.unpack_from("<234I", rows, r * 936))
    return None


def string_at(strings, off):
    end = strings.index(b"\0", off)
    return strings[off:end].decode("utf-8", "replace")


def dump(path, sid):
    records, rows, strings = load(path)
    row = row_of(rows, records, sid)
    if not row:
        sys.exit("%d not found" % sid)
    for i, v in enumerate(row):
        if v:
            extra = ""
            if i in (NAME, RANK, DESC, TIP):
                extra = "  %r" % string_at(strings, v)
            print("%3d  %10d  0x%08X%s" % (i, v, v, extra))


def clone(path, donor, spec_path, write):
    records, rows, strings = load(path)
    base = row_of(rows, records, donor)
    if not base:
        sys.exit("donor %d not found" % donor)
    top = max(struct.unpack_from("<I", rows, r * 936)[0] for r in range(records))
    spec = sorted(json.load(open(spec_path)), key=lambda s: s["id"])

    strings = bytearray(strings)

    def add_string(text):
        if not text:
            return 0
        off = len(strings)
        strings.extend(text.encode("utf-8") + b"\0")
        return off

    new_rows = bytearray()
    for s in spec:
        if s["id"] <= top:
            sys.exit("id %d is not above the current highest id %d" % (s["id"], top))
        row = list(base)
        row[0] = s["id"]
        for group, key in ((NAME, "name"), (RANK, "rank"), (DESC, "description"), (TIP, "tooltip")):
            for loc in range(16):
                row[group + loc] = 0
            row[group] = add_string(s.get(key, ""))
        for k, v in s.get("fields", {}).items():
            row[int(k)] = v & 0xFFFFFFFF
        new_rows += struct.pack("<234I", *row)
        changed = [i for i in range(234) if row[i] != base[i]]
        print("%d %r: %d fields differ from donor %d: %s" % (s["id"], s.get("name"), len(changed), donor,
              ", ".join("%d=%d" % (i, row[i]) for i in changed if i not in (0, NAME, RANK, DESC, TIP))))
        top = s["id"]

    if not write:
        print("dry run - pass --write to append %d row(s)" % len(spec))
        return

    stamp = time.strftime("%Y%m%d-%H%M%S")
    shutil.copy2(path, "%s.bak-%s" % (path, stamp))
    out = struct.pack("<4sIIII", b"WDBC", records + len(spec), 234, 936, len(strings))
    open(path, "wb").write(out + rows + bytes(new_rows) + bytes(strings))
    print("wrote %s (%d -> %d records), backup .bak-%s" % (path, records, records + len(spec), stamp))


if __name__ == "__main__":
    if len(sys.argv) >= 4 and sys.argv[1] == "dump":
        dump(sys.argv[2], int(sys.argv[3]))
    elif len(sys.argv) >= 5 and sys.argv[1] == "clone":
        clone(sys.argv[2], int(sys.argv[3]), sys.argv[4], "--write" in sys.argv)
    else:
        sys.exit(__doc__)
