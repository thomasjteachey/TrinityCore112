"""Append a single attachment point to a wmo2m2.py-built M2.

Why: vehicle seats place their rider at the M2 attachment referenced by
VehicleSeat.AttachmentID, plus the seat's AttachmentOffset. The client
resolves the ID through the model's attachment lookup table - and when the
model has NO attachments (wmo2m2.py writes none), the lookup fails and the
client silently discards the offset too, rendering the rider at the model
origin no matter what the seat says. The server never reads the M2, so the
server-side transport position was always right - the desync is purely
visual.

The cure is one attachment (id 0) ON bone 0 AT the model origin, so the
composed position is attachment(0,0,0) + AttachmentOffset and all tuning
stays in VehicleSeat.dbc.

Usage:  python add_m2_attachment.py <model.m2>

Idempotent: refuses to run if the model already has attachments.
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
    with open(path, "rb") as f:
        data = bytearray(f.read())

    magic, version = struct.unpack_from("<4sI", data, 0)
    assert magic == b"MD20" and version == 264, (magic, version)

    n_attach, _ = struct.unpack_from("<2I", data, OFS_ATTACH)
    if n_attach:
        print(f"{path}: already has {n_attach} attachment(s), nothing to do")
        return

    # pad to 16 so the appended block sits on the same alignment the
    # builder's Blocks() helper uses
    while len(data) % 16:
        data += b"\x00"

    # M2Attachment (40 B): id u32, bone u16, unk u16, pos 3f,
    # animateAttached M2Track<u8>: interp u16, gseq i16, timestamps (0,0),
    # values (0,0) - an empty track reads as "always attached".
    ofs_attach = len(data)
    data += struct.pack("<IHH3f", 0, 0, 0, 0.0, 0.0, 0.0)
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
