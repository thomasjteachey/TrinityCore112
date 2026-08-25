"""Add (or move) the vehicle-seat attachment point on a wmo2m2.py-built M2.

WHERE THE RIDER SITS IS THE M2's CALL, NOT THE DBC's. Proven twice on this
client: the seat position comes from the M2 attachment named by
VehicleSeat.AttachmentID, and VehicleSeat.AttachmentOffset is only a
FALLBACK used when that attachment cannot be resolved. Stock data agrees -
Stampy's seat 1705 is AttachmentID 21 with offset (0,0,0), yet its rider
sits on the mammoth's back, not inside it.

Consequences for converted WMOs, which ship with zero attachments:
  * no attachment at all -> lookup fails -> rider renders at the model
    origin (the DBC offset does get consulted, but an unresolvable
    AttachmentID like -1 fails earlier and lands at origin anyway);
  * an attachment at the WRONG spot -> the client happily obeys it and
    ignores the DBC offset entirely. An attachment at the origin puts the
    rider on the building's floor no matter what the seat says.
So the position belongs HERE, and VehicleSeat.AttachmentOffset should be
left at (0,0,0) exactly as the stock seats have it.

The server never reads the M2: Vehicle.cpp derives the passenger's
transport offset from AttachmentOffset alone, so with a zeroed offset the
server treats the rider as standing at the vehicle's origin. That is
stock behaviour for every vehicle in the game, not a bug.

Usage:
  python add_m2_attachment.py <model.m2> [<x> <y> <z>]

Model-space coordinates, same frame the collision bounds are in (door on
+X for wmo2m2 output). Defaults to the origin. Re-running MOVES an
attachment this tool wrote, so seat tuning never needs a rebuild.

v264 header: the 8 trailing M2Array pairs start at offset 240
(attachments, attachmentLookup, events, lights, cameras, cameraLookup,
ribbons, particles); total header size 304 (global flags without 0x8).
"""
import struct
import sys

OFS_ATTACH = 240
OFS_ATTACH_LOOKUP = 248

def main():
    path = sys.argv[1]
    pos = tuple(float(v) for v in sys.argv[2:5]) if len(sys.argv) >= 5 else (0.0, 0.0, 0.0)
    with open(path, "rb") as f:
        data = bytearray(f.read())

    magic, version = struct.unpack_from("<4sI", data, 0)
    assert magic == b"MD20" and version == 264, (magic, version)

    n_attach, ofs_existing = struct.unpack_from("<2I", data, OFS_ATTACH)
    if n_attach:
        # Written by an earlier run: just move it. (Any model with real
        # authored attachments is not ours to rewrite - refuse those.)
        if n_attach != 1:
            print(f"{path}: has {n_attach} attachments, refusing to guess which is the seat")
            return
        struct.pack_into("<3f", data, ofs_existing + 8, *pos)
        with open(path, "wb") as f:
            f.write(data)
        print(f"{path}: attachment 0 moved to {pos}")
        return

    # pad to 16 so the appended block sits on the same alignment the
    # builder's Blocks() helper uses
    while len(data) % 16:
        data += b"\x00"

    # M2Attachment (40 B): id u32, bone u16, unk u16, pos 3f,
    # animateAttached M2Track<u8>: interp u16, gseq i16, timestamps (0,0),
    # values (0,0) - an empty track reads as "always attached".
    ofs_attach = len(data)
    data += struct.pack("<IHH3f", 0, 0, 0, *pos)
    data += struct.pack("<Hh4I", 0, -1, 0, 0, 0, 0)

    # lookup[attachment id] -> attachment index; seat AttachmentID 0 -> 0
    ofs_lookup = len(data)
    data += struct.pack("<h", 0)

    struct.pack_into("<2I", data, OFS_ATTACH, 1, ofs_attach)
    struct.pack_into("<2I", data, OFS_ATTACH_LOOKUP, 1, ofs_lookup)

    with open(path, "wb") as f:
        f.write(data)
    print(f"{path}: attachment 0 @ origin added (record @{ofs_attach}, lookup @{ofs_lookup})")

main()
