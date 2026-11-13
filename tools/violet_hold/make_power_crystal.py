"""Violet Hold rune crystals: Creature_PowerCrystal (the bobbing quest crystal with its
orbiting shards and glow) in the four Silvermyst crystal colours.

  python make_power_crystal.py <Creature_PowerCrystal.m2> <out.m2> <BLUE|ORANGE|YELLOW|PINK>

The stock model hard-codes its crystal texture (texture 2,
SILVERMYSTCRYSTAL_001_BLUE.BLP, type 0 - a display cannot swap it) and tints its glow
and flare teal through colour track 1. The copy points texture 2 at the chosen
Silvermyst colour (the new name is appended at the end of the file and the texture
record repointed) and sets every key of colour 1 to a matching glow. Nothing else
changes: same mesh, bones, global sequences and animations, same skin (shipped
beside the model as <out>00.skin).
"""
import struct
import sys

sys.path.insert(0, r"C:\Projects\Gamedev\wow\tools\mpqpy")
import m2tracks as M

TEXTURE_DIR = "WORLD\\EXPANSION01\\DOODADS\\SILVERMYST\\CRYSTALS\\SILVERMYSTCRYSTAL_001_%s.BLP"
GLOW = {
    "BLUE": (0.0, 0.8156863451004028, 0.6509804129600525),   # stock
    "ORANGE": (1.0, 0.45, 0.10),
    "YELLOW": (1.0, 0.85, 0.30),
    "PINK": (1.0, 0.35, 0.70),
}
CRYSTAL_TEXTURE = 2
GLOW_COLOR = 1


def build(src, colour):
    b = bytearray(src)
    M.check_header(b)
    nt, ot = M.header_array(b, "textures")
    tex = ot + CRYSTAL_TEXTURE * 16
    ttype, tflags, ln, lo = struct.unpack_from("<IIII", b, tex)
    old = bytes(b[lo:lo + ln]).rstrip(b"\0").decode()
    assert ttype == 0 and old.upper().endswith("SILVERMYSTCRYSTAL_001_BLUE.BLP"), old

    while len(b) % 16:
        b.append(0)
    name = (TEXTURE_DIR % colour).encode() + b"\0"
    ofs = len(b)
    b.extend(name)
    struct.pack_into("<II", b, tex + 8, len(name), ofs)

    glow = GLOW[colour]
    done = 0
    for t in M.tracks(b):
        if t["kind"] == "colors" and t["index"] == GLOW_COLOR and t["label"] == "color":
            for s in range(t["nv"]):
                cnt, vo = M.entry(b, t["ov"], s)
                for k in range(cnt):
                    struct.pack_into("<3f", b, vo + k * 12, *glow)
                    done += 1
    assert done, "glow colour track not found"
    return bytes(b)


def verify(out, colour):
    M.check_header(out)
    nt, ot = M.header_array(out, "textures")
    _, _, ln, lo = struct.unpack_from("<IIII", out, ot + CRYSTAL_TEXTURE * 16)
    assert out[lo:lo + ln].rstrip(b"\0").decode() == TEXTURE_DIR % colour
    for t in M.tracks(out):
        if t["kind"] == "colors" and t["index"] == GLOW_COLOR and t["label"] == "color":
            _, vals = M.keys(out, t, 0)
            assert all(abs(a - b) < 1e-6 for v in vals for a, b in zip(struct.unpack("<3f", v), GLOW[colour]))


if __name__ == "__main__":
    src = open(sys.argv[1], "rb").read()
    colour = sys.argv[3].upper()
    out = build(src, colour)
    verify(out, colour)
    open(sys.argv[2], "wb").write(out)
    print("ok: %s %s (%d -> %d bytes)" % (sys.argv[2], colour, len(src), len(out)))
