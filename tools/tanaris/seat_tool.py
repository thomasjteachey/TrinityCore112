"""VehicleSeat.dbc inspection/minting + Vehicle.dbc seat pointer.

  python seat_tool.py dump <VehicleSeat.dbc> <id> [<id>...]
  python seat_tool.py maxid <VehicleSeat.dbc>
  python seat_tool.py mint <VehicleSeat.dbc> <srcId> <newId> <andNotFlags-hex>
  python seat_tool.py vehseat <Vehicle.dbc> <vehicleId> [<newSeat0>]
"""
import struct
import sys

def load(path):
    with open(path, "rb") as f:
        data = bytearray(f.read())
    magic, records, fields, rsize, ssize = struct.unpack_from("<4siiii", data, 0)
    assert magic == b"WDBC", path
    return data, records, fields, rsize, ssize

def row_off(data, records, rsize, want_id):
    for i in range(records):
        off = 20 + i * rsize
        if struct.unpack_from("<i", data, off)[0] == want_id:
            return off
    raise SystemExit(f"id {want_id} not found")

def main():
    mode, path = sys.argv[1], sys.argv[2]
    data, records, fields, rsize, ssize = load(path)
    print(f"{path}: {records} records, {fields} fields, {rsize} B/rec")
    if mode == "dump":
        for want in (int(a) for a in sys.argv[3:]):
            off = row_off(data, records, rsize, want)
            flags = struct.unpack_from("<I", data, off + 4)[0]
            flagsB = struct.unpack_from("<I", data, off + (fields - 1) * 4)[0]
            print(f"  seat {want}: flags={flags:#010x} lastField(flagsB?)={flagsB:#010x}")
    elif mode == "maxid":
        ids = [struct.unpack_from("<i", data, 20 + i * rsize)[0] for i in range(records)]
        print(f"  max id = {max(ids)}, sorted ascending = {ids == sorted(ids)}")
    elif mode == "mint":
        src, new, andnot = int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5], 16)
        if any(struct.unpack_from("<i", data, 20 + i * rsize)[0] == new for i in range(records)):
            print(f"  seat {new} already present, refusing")
            return
        off = row_off(data, records, rsize, src)
        row = bytearray(data[off:off + rsize])
        struct.pack_into("<i", row, 0, new)
        flags = struct.unpack_from("<I", row, 4)[0]
        struct.pack_into("<I", row, 4, flags & ~andnot)
        rec_end = 20 + records * rsize
        data[rec_end:rec_end] = row
        struct.pack_into("<i", data, 4, records + 1)
        with open(path, "wb") as f:
            f.write(data)
        print(f"  seat {new} minted from {src}: flags {flags:#010x} -> {flags & ~andnot:#010x}")
    elif mode == "vehseat":
        want = int(sys.argv[3])
        off = row_off(data, records, rsize, want)
        seats = struct.unpack_from("<8i", data, off + 6 * 4)
        print(f"  vehicle {want}: seats={list(seats)}")
        if len(sys.argv) > 4:
            for slot, arg in enumerate(sys.argv[4:]):
                struct.pack_into("<i", data, off + (6 + slot) * 4, int(arg))
            with open(path, "wb") as f:
                f.write(data)
            print(f"  vehicle {want}: seats -> {sys.argv[4:]}")

main()
