"""Generate the dbc.worldmapoverlay_lplus mirror from the binary WorldMapOverlay.dbc.

WorldMapOverlay had no _lplus mirror at all, so there was no SQL side to edit -
only the binary. This creates one and seeds it from the live customized binary,
so the table joins the rest of the dbc.*_lplus set and later changes can be
written as replayable SQL like every other DBC.

Field layout (17 x int32, field 8 is a string offset) checked against the
binary header and three known Tanaris rows.
"""
import struct

SRC = r"C:/Projects/Gamedev/wow/data/dbc/lplus/WorldMapOverlay.dbc"
OUT_SEED = r"C:/Projects/Gamedev/wow/servers/tc-lplus/sql/custom/dbc/2026_08_06_01_dbc_worldmapoverlay_mirror.sql"

COLS = ["ID", "MapAreaID", "AreaID_1", "AreaID_2", "AreaID_3", "AreaID_4",
        "MapPointX", "MapPointY", "TextureName", "TextureWidth", "TextureHeight",
        "OffsetX", "OffsetY", "HitRectTop", "HitRectLeft", "HitRectBottom",
        "HitRectRight"]
STR_FIELD = 8

blob = open(SRC, "rb").read()
magic, rc, fc, rs, sb = struct.unpack_from("<4sIIII", blob, 0)
assert magic == b"WDBC" and fc == 17 and rs == 68, "unexpected WorldMapOverlay layout"
recs = blob[20:20 + rc * rs]
block = blob[20 + rc * rs:]


def cstr(off):
    if off == 0:
        return ""
    return block[off:block.index(b"\x00", off)].decode("utf-8", "replace")


def esc(t):
    return t.replace("\\", "\\\\").replace("'", "\\'")


rows = []
for i in range(rc):
    o = i * rs
    v = [struct.unpack_from("<i", recs, o + k * 4)[0] for k in range(fc)]
    v[STR_FIELD] = cstr(struct.unpack_from("<I", recs, o + STR_FIELD * 4)[0])
    rows.append(v)


def literal(v):
    return "'%s'" % esc(v) if isinstance(v, str) else str(v)


with open(OUT_SEED, "w", newline="\n") as f:
    f.write("""-- Create and seed the dbc.worldmapoverlay_lplus mirror.
--
-- WorldMapOverlay had NO _lplus mirror, unlike every other DBC this server
-- customizes - mirror coverage is not universal. That left the binary as the
-- only place the table could be edited, which breaks the rule that a DBC change
-- lands in both the SQL mirror and the binary.
--
-- WorldMapOverlay is what draws the explored-area artwork on the world map.
-- Each row attaches a texture to one or more AreaIDs, keyed by MapAreaID - and
-- MapAreaID is a WorldMapArea ROW ID, not a map id. A cloned map therefore needs
-- its own set of overlay rows pointing at its own WorldMapArea row, or its map
-- renders as blank parchment however well explored the zone is.
--
-- Seeded from the live customized binary
-- (wow/data/dbc/lplus/WorldMapOverlay.dbc, %d rows), so the mirror starts in
-- agreement with it. DDL matches the other dbc.*_lplus tables.
--
-- Replayable: drops and rebuilds the whole table.

DROP TABLE IF EXISTS dbc.worldmapoverlay_lplus;
CREATE TABLE dbc.worldmapoverlay_lplus (
  `ID` int NOT NULL DEFAULT '0',
  `MapAreaID` int NOT NULL DEFAULT '0',
  `AreaID_1` int NOT NULL DEFAULT '0',
  `AreaID_2` int NOT NULL DEFAULT '0',
  `AreaID_3` int NOT NULL DEFAULT '0',
  `AreaID_4` int NOT NULL DEFAULT '0',
  `MapPointX` int NOT NULL DEFAULT '0',
  `MapPointY` int NOT NULL DEFAULT '0',
  `TextureName` text COLLATE utf8mb3_unicode_ci,
  `TextureWidth` int NOT NULL DEFAULT '0',
  `TextureHeight` int NOT NULL DEFAULT '0',
  `OffsetX` int NOT NULL DEFAULT '0',
  `OffsetY` int NOT NULL DEFAULT '0',
  `HitRectTop` int NOT NULL DEFAULT '0',
  `HitRectLeft` int NOT NULL DEFAULT '0',
  `HitRectBottom` int NOT NULL DEFAULT '0',
  `HitRectRight` int NOT NULL DEFAULT '0',
  PRIMARY KEY (`ID`)
) ENGINE=MyISAM DEFAULT CHARSET=utf8mb3 COLLATE=utf8mb3_unicode_ci;

INSERT INTO dbc.worldmapoverlay_lplus
  (%s)
VALUES
""" % (rc, ", ".join(COLS)))
    lines = ["  (%s)" % ", ".join(literal(v) for v in r) for r in rows]
    f.write(",\n".join(lines))
    f.write(";\n")

print("wrote %s  (%d rows)" % (OUT_SEED, len(rows)))

# The Tanaris clones, emitted for reuse by the battleground script.
tan = [r for r in rows if r[1] == 161]
print("Tanaris overlays to clone: %d" % len(tan))
base = 1700
assert not ({base + i for i in range(len(tan))} & {r[0] for r in rows}), "id block not free"
clones = []
for n, r in enumerate(sorted(tan, key=lambda x: x[0])):
    c = list(r)
    c[0] = base + n
    c[1] = 9532
    clones.append(c)
with open(r"C:/Users/broki/AppData/Local/Temp/claude/C--Projects-Gamedev-wow-servers-tc-lplus/d8730d13-6a54-4c96-a72d-1e15b9470ffa/scratchpad/clones.sql", "w", newline="\n") as f:
    f.write(",\n".join("  (%s)" % ", ".join(literal(v) for v in c) for c in clones) + ";\n")
print("clone ids %d..%d" % (clones[0][0], clones[-1][0]))
