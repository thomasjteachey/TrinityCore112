"""Dump or patch Vehicle.dbc facing-limit fields (18=FacingLimitRight,
19=FacingLimitLeft; 0 means unlimited). 3.3.5 layout, 40 fields.

  python vehicle_facing.py dump <Vehicle.dbc> <id> [<id>...]
  python vehicle_facing.py set  <Vehicle.dbc> <id> <right> <left>
"""
import struct
import sys

def load(path):
    with open(path, "rb") as f:
        data = bytearray(f.read())
    magic, records, fields, rsize, ssize = struct.unpack_from("<4siiii", data, 0)
    assert magic == b"WDBC", path
    return data, records, fields, rsize

def row_off(data, records, rsize, want_id):
    for i in range(records):
        off = 20 + i * rsize
        if struct.unpack_from("<i", data, off)[0] == want_id:
            return off
    raise SystemExit(f"id {want_id} not found")

def main():
    mode, path = sys.argv[1], sys.argv[2]
    data, records, fields, rsize = load(path)
    print(f"{path}: {records} records, {fields} fields")
    if mode == "dump":
        for want in (int(a) for a in sys.argv[3:]):
            off = row_off(data, records, rsize, want)
            flags, turnspeed = struct.unpack_from("<if", data, off + 4)
            right, left = struct.unpack_from("<ff", data, off + 18 * 4)
            print(f"  id {want}: flags={flags:#010x} turnSpeed={turnspeed} "
                  f"facingRight={right} facingLeft={left}")
    elif mode == "set":
        want, right, left = int(sys.argv[3]), float(sys.argv[4]), float(sys.argv[5])
        off = row_off(data, records, rsize, want)
        struct.pack_into("<ff", data, off + 18 * 4, right, left)
        with open(path, "wb") as f:
            f.write(data)
        print(f"  id {want}: facing limits set to {right}/{left}")
    elif mode == "setturn":
        want, turn = int(sys.argv[3]), float(sys.argv[4])
        off = row_off(data, records, rsize, want)
        struct.pack_into("<f", data, off + 2 * 4, turn)
        with open(path, "wb") as f:
            f.write(data)
        print(f"  id {want}: turnSpeed set to {turn}")

main()
