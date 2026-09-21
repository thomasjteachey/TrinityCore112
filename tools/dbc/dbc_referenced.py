#!/usr/bin/env python3
"""List the strings a .dbc's records actually POINT AT (not just what is in the
string block), filtered by a substring. Proves a rename took, because an
orphaned old string stays in the block and only stops being referenced."""
import struct
import sys

needle = sys.argv[1].lower()
for path in sys.argv[2:]:
    with open(path, 'rb') as f:
        data = f.read()
    record_count, field_count, record_size, string_size = struct.unpack_from('<4I', data, 4)
    header = 20
    block = data[header + record_count * record_size:]
    columns = record_size // 4

    string_columns = []
    for column in range(columns):
        for rec in range(record_count):
            v = struct.unpack_from('<I', data, header + rec * record_size + column * 4)[0]
            if v == 0:
                continue
            if v >= len(block) or block[v - 1] != 0:
                break
        else:
            string_columns.append(column)

    seen = set()
    for rec in range(record_count):
        for column in string_columns:
            v = struct.unpack_from('<I', data, header + rec * record_size + column * 4)[0]
            if not v:
                continue
            end = block.find(b'\0', v)
            s = block[v:end].decode('utf-8', 'replace')
            if needle in s.lower():
                seen.add(s)
    print('%-22s %d records, %d string columns' % (path.split('\\')[-1].split('/')[-1], record_count, len(string_columns)))
    for s in sorted(seen):
        print('    %s' % s)
