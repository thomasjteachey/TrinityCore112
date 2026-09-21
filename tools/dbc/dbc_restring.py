#!/usr/bin/env python3
"""Rename a string inside a .dbc, without knowing the file's layout.

    dbc_restring.py [--write] <file.dbc> [<file.dbc> ...] -- <old> <new> [<old> <new> ...]

A DBC is a header, fixed-size records, then one string block that the records
point into by byte offset. Rewriting a string in place is only safe while the
length is identical, and "Coliseum" -> "Colosseum" is a byte longer, so instead:

  - the new string is APPENDED to the end of the string block, which moves
    nothing that already exists,
  - every field in a STRING column whose value is exactly the offset of the old
    string is repointed at the new one. A column counts as a string column only
    when every one of its values across every record is either 0 or lands on the
    first byte of a string in the block - otherwise an ordinary number that
    happens to equal the offset would be "repointed" into nonsense,
  - the header's string block size is updated.

Nothing else moves, so every other offset in the file stays true and no schema
is needed. The old string is left where it is; it simply stops being referenced.
"""
import struct
import sys


def restring(path, pairs, write):
    with open(path, 'rb') as f:
        data = bytearray(f.read())

    if bytes(data[:4]) != b'WDBC':
        return '%s: not a DBC' % path
    record_count, field_count, record_size, string_size = struct.unpack_from('<4I', data, 4)
    header = 20
    records_end = header + record_count * record_size
    block_start = records_end
    if block_start + string_size != len(data):
        return '%s: string block does not reach the end of the file' % path

    block = bytearray(data[block_start:])

    # Which columns hold string offsets: every value 0, or the first byte of a
    # string in the block. Checked against the block as it arrived.
    columns = record_size // 4
    string_columns = []
    for column in range(columns):
        for rec in range(record_count):
            value = struct.unpack_from('<I', data, header + rec * record_size + column * 4)[0]
            if value == 0:
                continue
            if value >= len(block) or (value and block[value - 1] != 0):
                break
        else:
            string_columns.append(column)

    changes = []
    for old, new in pairs:
        old_b, new_b = old.encode('utf-8'), new.encode('utf-8')
        # Offsets of the old string: every occurrence that starts a string
        # (preceded by a NUL) and ends with one.
        offsets = []
        pos = 0
        while True:
            pos = block.find(old_b + b'\0', pos)
            if pos < 0:
                break
            if pos == 0 or block[pos - 1] == 0:
                offsets.append(pos)
            pos += 1
        if not offsets:
            continue

        new_offset = len(block)
        block += new_b + b'\0'

        repointed = 0
        for offset in offsets:
            for rec in range(record_count):
                base = header + rec * record_size
                for column in string_columns:
                    at = base + column * 4
                    if struct.unpack_from('<I', data, at)[0] == offset:
                        struct.pack_into('<I', data, at, new_offset)
                        repointed += 1
        if not repointed:
            return '%s: "%s" is in the string block but nothing points at it' % (path, old)
        changes.append('%s -> %s (%d field%s)' % (old, new, repointed, '' if repointed == 1 else 's'))

    if not changes:
        return '%s: nothing to change' % path

    out = bytearray(data[:records_end]) + block
    struct.pack_into('<I', out, 16, len(block))

    if write:
        with open(path, 'wb') as f:
            f.write(out)
    return '%s: %s%s' % (path.split('/')[-1], '; '.join(changes), '' if write else '  (dry run)')


def main():
    args = sys.argv[1:]
    write = '--write' in args
    args = [a for a in args if a != '--write']
    if '--' not in args:
        sys.exit(__doc__)
    split = args.index('--')
    files, rest = args[:split], args[split + 1:]
    if not files or len(rest) % 2:
        sys.exit(__doc__)
    pairs = list(zip(rest[0::2], rest[1::2]))
    for path in files:
        print(restring(path, pairs, write))


if __name__ == '__main__':
    main()
