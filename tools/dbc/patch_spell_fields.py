#!/usr/bin/env python3
"""Patch selected uint32 fields in a WotLK Spell.dbc without rebuilding it.

The tuning file maps spell ids to zero-based binary field indexes. Working
from the current binary preserves realm-specific rows that are not present in
the SQL mirror.
"""

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path


HEADER = struct.Struct("<4s4I")
U32 = struct.Struct("<I")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument(
        "--tuning",
        type=Path,
        default=Path(__file__).with_name("violet_hold_boon_tuning.json"),
    )
    parser.add_argument("--id", type=int, action="append", dest="only_ids")
    args = parser.parse_args()

    data = bytearray(args.source.read_bytes())
    magic, record_count, field_count, record_size, string_size = HEADER.unpack_from(data)
    if magic != b"WDBC" or record_size != field_count * U32.size:
        raise SystemExit("not a fixed-width WDBC file")
    records_end = HEADER.size + record_count * record_size
    if records_end + string_size != len(data):
        raise SystemExit("WDBC header sizes do not match file length")

    rows: dict[int, int] = {}
    for row in range(record_count):
        offset = HEADER.size + row * record_size
        spell_id = U32.unpack_from(data, offset)[0]
        if spell_id in rows:
            raise SystemExit(f"duplicate spell id {spell_id}")
        rows[spell_id] = offset

    requested = set(args.only_ids or [])
    changed: list[str] = []
    for edit in json.loads(args.tuning.read_text(encoding="utf-8")):
        spell_id = int(edit["id"])
        if requested and spell_id not in requested:
            continue
        if spell_id not in rows:
            raise SystemExit(f"spell id {spell_id} is missing")
        for field_text, value in edit["fields"].items():
            field = int(field_text)
            if not 0 <= field < field_count:
                raise SystemExit(f"field {field} is outside 0..{field_count - 1}")
            offset = rows[spell_id] + field * U32.size
            old = U32.unpack_from(data, offset)[0]
            U32.pack_into(data, offset, int(value))
            changed.append(f"{spell_id}[{field}]: {old} -> {value}")

    missing = requested - {int(edit["id"]) for edit in json.loads(args.tuning.read_text(encoding="utf-8"))}
    if missing:
        raise SystemExit(f"requested ids absent from tuning file: {sorted(missing)}")
    if not changed:
        raise SystemExit("no fields selected")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(data)
    print("\n".join(changed))


if __name__ == "__main__":
    main()
