#!/usr/bin/env python3
"""Apply the custom-game WorldStateUI/PvPDifficulty rows to lplus DBCs."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


STRING_FIELDS_WORLDSTATEUI = {4, *range(5, 21), *range(22, 38), 41, *range(42, 58), 59}


def read_dbc(path: Path) -> tuple[int, list[list[int]], bytes]:
    data = path.read_bytes()
    magic, count, fields, record_size, string_size = struct.unpack_from("<4s4I", data)
    if magic != b"WDBC" or record_size != fields * 4:
        raise ValueError(f"Unsupported DBC layout: {path}")
    records_start = 20
    records_end = records_start + count * record_size
    if records_end + string_size != len(data):
        raise ValueError(f"Invalid DBC size: {path}")
    records = [
        list(struct.unpack_from(f"<{fields}I", data, records_start + i * record_size))
        for i in range(count)
    ]
    return fields, records, data[records_end:]


def read_string(block: bytes, offset: int) -> bytes:
    if offset == 0:
        return b""
    end = block.find(b"\0", offset)
    if end < 0:
        raise ValueError(f"Invalid string offset {offset}")
    return block[offset:end]


def write_dbc(path: Path, fields: int, records: list[list[int]], strings: bytes) -> None:
    body = b"".join(struct.pack(f"<{fields}I", *record) for record in records)
    header = struct.pack("<4s4I", b"WDBC", len(records), fields, fields * 4, len(strings))
    path.write_bytes(header + body + strings)


def update_worldstateui(path: Path) -> None:
    fields, records, old_strings = read_dbc(path)
    if fields != 63:
        raise ValueError(f"Expected 63 WorldStateUI fields, found {fields}")

    updates: dict[tuple[int, int], dict[int, int | bytes]] = {
        (90002, 1189): {2: 0, 5: b"%9000w/%9006w", 39: 0},
        (90003, 1189): {2: 0, 5: b"%9001w/%9006w", 39: 0},
        (90021, 1230): {2: 0, 5: b"%9100w/%9106w", 39: 0},
        (90022, 1230): {2: 0, 5: b"%9101w/%9106w", 39: 0},
        (90004, 980): {39: 3610},
        (90005, 980): {39: 3610},
        (90006, 1134): {39: 3610},
        (90007, 1134): {39: 3610},
        (9300, 1572): {2: 0, 39: 3002},
        (9301, 1572): {2: 0, 39: 3002},
    }

    record_strings: list[dict[int, bytes]] = []
    found: set[tuple[int, int]] = set()
    for record in records:
        values = {index: read_string(old_strings, record[index]) for index in STRING_FIELDS_WORLDSTATEUI}
        key = (record[0], record[1])
        if key in updates:
            found.add(key)
            for index, value in updates[key].items():
                if isinstance(value, bytes):
                    values[index] = value
                else:
                    record[index] = value
        record_strings.append(values)

    missing = set(updates) - found
    if missing:
        raise ValueError(f"WorldStateUI rows missing from {path}: {sorted(missing)}")

    new_strings = bytearray(b"\0")
    offsets: dict[bytes, int] = {b"": 0}
    for record, values in zip(records, record_strings):
        for index in STRING_FIELDS_WORLDSTATEUI:
            value = values[index]
            if value not in offsets:
                offsets[value] = len(new_strings)
                new_strings.extend(value)
                new_strings.append(0)
            record[index] = offsets[value]

    write_dbc(path, fields, records, bytes(new_strings))


def validate_pvpdifficulty(path: Path) -> None:
    fields, records, _ = read_dbc(path)
    if fields != 6:
        raise ValueError(f"Expected 6 PvPDifficulty fields, found {fields}")

    required_maps = {
        489, 529, 726, 761, 1189, 1230, 1615,  # supported battlegrounds
        559, 562, 572, 980, 1134, 1572,        # supported arenas
    }
    present_maps = {record[1] for record in records}
    missing_maps = required_maps - present_maps
    if missing_maps:
        raise ValueError(f"PvPDifficulty maps missing from {path}: {sorted(missing_maps)}")

    obc = [record for record in records if record[0] == 91615]
    if obc != [[91615, 1615, 0, 60, 69, 0]]:
        raise ValueError(f"Unexpected Obsidian Colosseum bracket in {path}: {obc}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("dbc_directory", type=Path)
    args = parser.parse_args()
    update_worldstateui(args.dbc_directory / "WorldStateUI.dbc")
    validate_pvpdifficulty(args.dbc_directory / "PvpDifficulty.dbc")


if __name__ == "__main__":
    main()
