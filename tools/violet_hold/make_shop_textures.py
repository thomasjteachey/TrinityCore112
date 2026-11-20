"""Write the white-with-alpha TGA shapes the Violet Hold shop frame tints at runtime.

usage: python make_shop_textures.py <addon Textures folder>

Every file is 32-bit uncompressed TGA, power-of-two sized, bottom-left origin
(descriptor 0x08), RGB white so SetVertexColor / SetGradient colour it.
Edges are 4x4 supersampled.
"""
import math
import os
import struct
import sys

SS = 4


def write_tga(path, w, h, alpha_at):
    header = struct.pack('<BBBHHBHHHHBB', 0, 0, 2, 0, 0, 0, 0, 0, w, h, 32, 0x08)
    rows = []
    for y in range(h - 1, -1, -1):          # bottom row first
        row = bytearray()
        for x in range(w):
            a = alpha_at(x, y)
            row += bytes((255, 255, 255, max(0, min(255, int(round(a * 255))))))
        rows.append(bytes(row))
    with open(path, 'wb') as f:
        f.write(header)
        for r in rows:
            f.write(r)


def coverage(inside):
    """Pixel coverage of a shape given as inside(px, py) on continuous coordinates."""
    def at(x, y):
        hits = 0
        for i in range(SS):
            for j in range(SS):
                if inside(x + (i + 0.5) / SS, y + (j + 0.5) / SS):
                    hits += 1
        return hits / (SS * SS)
    return at


def main(out):
    os.makedirs(out, exist_ok=True)

    # Filled circle, 64x64.
    write_tga(os.path.join(out, 'circle.tga'), 64, 64,
              coverage(lambda px, py: math.hypot(px - 32, py - 32) <= 31.0))

    # Thin ring, 64x64: the rim round a round icon.
    write_tga(os.path.join(out, 'ring.tga'), 64, 64,
              coverage(lambda px, py: 28.5 <= math.hypot(px - 32, py - 32) <= 31.0))

    # Filled diamond, 32x32: the frame's corner jewels.
    write_tga(os.path.join(out, 'diamond.tga'), 32, 32,
              coverage(lambda px, py: abs(px - 16) + abs(py - 16) <= 15.5))

    # Price-tag end, 16x32: points left; mirrored with SetTexCoord for the right end.
    write_tga(os.path.join(out, 'tagcap.tga'), 16, 32,
              coverage(lambda px, py: px >= abs(py - 16) * (16.0 / 16.0)))

    # Padlock, 32x32: Boon of Safekeeping's lock on a shop card.
    def padlock(px, py):
        body = 7.0 <= px <= 25.0 and 15.0 <= py <= 29.0
        d = math.hypot(px - 16.0, py - 14.0)
        shackle = py <= 14.0 and 5.0 <= d <= 8.5
        legs = 14.0 < py <= 16.0 and (8.0 <= px <= 11.0 or 21.0 <= px <= 24.0)
        return body or shackle or legs
    write_tga(os.path.join(out, 'lock.tga'), 32, 32, coverage(padlock))

    # Soft glow, 128x128: a flat core (hidden behind whatever it haloes),
    # then a falloff over the outer 55% so the light shows past that thing's edge.
    def glow(x, y):
        d = math.hypot(x + 0.5 - 64, y + 0.5 - 64) / 64.0
        return min(1.0, max(0.0, (1.0 - d) / 0.55)) ** 1.2
    write_tga(os.path.join(out, 'glow.tga'), 128, 128, glow)


if __name__ == '__main__':
    main(sys.argv[1])
