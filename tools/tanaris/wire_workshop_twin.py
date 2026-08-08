"""Server-side wiring for the converted Goblin Workshop twin pattern.

Runs ON the game server. Four jobs, in an order chosen so nothing can drift:

1. Copy the three display DBCs (already minted) from the dev server tree into
   the client staging directory.
2. Create and seed dbc.gameobjectdisplayinfo_lplus / creaturemodeldata_lplus /
   creaturedisplayinfo_lplus mirrors FROM those minted binaries -- seeding
   after the mint means the mirrors are born already containing the new rows,
   so mirror and binary cannot disagree.
3. Find flat ground near the existing EPL guinea-pig workshop by reading the
   server's own 000*.map heightmaps.
4. Insert the world rows: gameobject_template 900001 (GO shell, display 11000),
   creature_template 900116 (the twin, display 40000, scale 1.02),
   creature_model_info for display 40000, and a spawned pair on that ground.
"""
import os
import shutil
import struct
import subprocess
import sys

sys.path.insert(0, "/home/brokilodeluxe/tanaris_bg")
from mint_building_display import SPECS   # field layouts, single source of truth

DEV_DBC = "/home/brokilodeluxe/wow/servers/tc-lplus-dev/data/dbc"
STAGE = "/home/brokilodeluxe/tanaris_bg/client_dbc/DBFilesClient"
MYSQL = ["mysql", "--defaults-extra-file=/home/brokilodeluxe/itemforge/my.cnf"]

COLS = {
    "GameObjectDisplayInfo.dbc": ("gameobjectdisplayinfo_lplus",
        ["ID", "ModelName"] + ["Sound_%d" % i for i in range(1, 11)]
        + ["GeoBoxMinX", "GeoBoxMinY", "GeoBoxMinZ", "GeoBoxMaxX", "GeoBoxMaxY", "GeoBoxMaxZ",
           "ObjectEffectPackageID"]),
    # Column order matches the EMPIRICALLY derived layout in
    # mint_building_display.SPECS -- note ModelNameAlt at index 3.
    "CreatureModelData.dbc": ("creaturemodeldata_lplus",
        ["ID", "Flags", "ModelName", "ModelNameAlt", "ModelScale", "SizeClass",
         "BloodID", "FootprintTextureLength", "FootprintTextureWidth",
         "FootprintParticleScale", "FoleyMaterialID", "FootprintTexture",
         "FootstepShakeSize", "DeathThudShakeSize", "CollisionWidth",
         "CollisionHeight", "MouthHeight", "GeoBoxMinX", "GeoBoxMinY",
         "GeoBoxMinZ", "GeoBoxMaxX", "GeoBoxMaxY", "GeoBoxMaxZ",
         "WorldEffectScale", "AttachedEffectScale", "MissileCollisionRadius",
         "MissileCollisionPush", "MissileCollisionRaise"]),
    "CreatureDisplayInfo.dbc": ("creaturedisplayinfo_lplus",
        ["ID", "ModelID", "SoundID", "ExtendedDisplayInfoID", "CreatureModelScale",
         "CreatureModelAlpha", "TextureVariation_1", "TextureVariation_2",
         "TextureVariation_3", "PortraitTextureName", "BloodLevel", "BloodID",
         "NPCSoundID", "ParticleColorID", "CreatureGeosetData",
         "ObjectEffectPackageID"]),
}


def sql(text):
    r = subprocess.run(MYSQL, input=text, capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit("mysql failed: %s" % r.stderr[:400])
    return r.stdout


def esc(s):
    return s.replace("\\", "\\\\").replace("'", "\\'")


# ---- 1. staging copies -----------------------------------------------------
for name in SPECS:
    shutil.copyfile(os.path.join(DEV_DBC, name), os.path.join(STAGE, name))
print("staged 3 client DBCs")

# ---- 2. mirrors ------------------------------------------------------------
for name, spec in SPECS.items():
    table, cols = COLS[name]
    blob = open(os.path.join(DEV_DBC, name), "rb").read()
    magic, rc, fc, rs, sb = struct.unpack_from("<4sIIII", blob, 0)
    assert magic == b"WDBC" and fc == spec["count"]
    recs = blob[20:20 + rc * rs]
    block = blob[20 + rc * rs:]

    def cstr(off):
        return "" if off == 0 else block[off:block.index(b"\x00", off)].decode("utf-8", "replace")

    ddl = ["DROP TABLE IF EXISTS dbc.%s;" % table,
           "CREATE TABLE dbc.%s (" % table]
    parts = []
    for col, kind in zip(cols, spec["fields"]):
        if kind == "i":
            parts.append("  `%s` int NOT NULL DEFAULT '0'" % col)
        elif kind == "f":
            parts.append("  `%s` float NOT NULL DEFAULT '0'" % col)
        else:
            parts.append("  `%s` text COLLATE utf8mb3_unicode_ci" % col)
    parts.append("  PRIMARY KEY (`ID`)")
    ddl.append(",\n".join(parts))
    ddl.append(") ENGINE=MyISAM DEFAULT CHARSET=utf8mb3 COLLATE=utf8mb3_unicode_ci;")

    values = []
    for i in range(rc):
        o = i * rs
        vals = []
        for k, kind in enumerate(spec["fields"]):
            raw = recs[o + k * 4:o + k * 4 + 4]
            if kind == "i":
                vals.append(str(struct.unpack("<i", raw)[0]))
            elif kind == "f":
                vals.append(repr(round(struct.unpack("<f", raw)[0], 6)))
            else:
                vals.append("'%s'" % esc(cstr(struct.unpack("<I", raw)[0])))
        values.append("(%s)" % ",".join(vals))

    stmt = "\n".join(ddl) + "\nINSERT INTO dbc.%s (%s) VALUES\n%s;\n" % (
        table, ",".join("`%s`" % c for c in cols), ",\n".join(values))
    sql(stmt)
    n = sql("SELECT COUNT(*) FROM dbc.%s;" % table).split()[-1]
    print("mirror dbc.%s seeded: %s rows (binary has %d)" % (table, n, rc))

# ---- 3. ground probe -------------------------------------------------------
MAPS = "/home/brokilodeluxe/wow/servers/tc-lplus-dev/data/maps"
SIZE = 533.3333
_cache = {}


def height(x, y):
    gx, gy = int(32 - x / SIZE), int(32 - y / SIZE)
    key = (gx, gy)
    if key not in _cache:
        p = os.path.join(MAPS, "000%02u%02u.map" % (gx, gy))
        if not os.path.exists(p):
            _cache[key] = None
        else:
            b = open(p, "rb").read()
            hOfs = struct.unpack_from("<4sI4sIIII", b, 0)[5]
            fourcc, flags, gh, gmax = struct.unpack_from("<IIff", b, hOfs)
            if flags & 0x1:
                _cache[key] = ("flat", gh)
            elif flags & 0x2:
                raw = struct.unpack_from("<%dH" % (129 * 129), b, hOfs + 16)
                step = (gmax - gh) / 65535.0
                _cache[key] = ("grid", [gh + r * step for r in raw])
            elif flags & 0x4:
                raw = struct.unpack_from("<%dB" % (129 * 129), b, hOfs + 16)
                step = (gmax - gh) / 255.0
                _cache[key] = ("grid", [gh + r * step for r in raw])
            else:
                _cache[key] = ("grid", list(struct.unpack_from("<%df" % (129 * 129), b, hOfs + 16)))
    tile = _cache[key]
    if tile is None:
        return None
    if tile[0] == "flat":
        return tile[1]
    xs = 128 * (32 - x / SIZE)
    ys = 128 * (32 - y / SIZE)
    return tile[1][(int(xs) & 127) * 129 + (int(ys) & 127)]


BASE = (3658.98, -3608.99, 44.737)
best = None
for dx, dy in ((80, 0), (-80, 0), (0, 80), (0, -80), (60, 60), (60, -60), (-60, 60), (-60, -60)):
    x, y = BASE[0] + dx, BASE[1] + dy
    # sample the building footprint corners too, not just the centre
    hs = [height(x + ox, y + oy) for ox in (-25, 0, 25) for oy in (-22, 0, 22)]
    if any(h is None for h in hs):
        continue
    spread = max(hs) - min(hs)
    score = spread + abs(sum(hs) / len(hs) - BASE[2]) * 0.1
    if best is None or score < best[0]:
        best = (score, x, y, sum(hs) / len(hs), spread)
_s, X, Y, Z, spread = best
print("spawn spot: (%.1f, %.1f) ground z=%.2f footprint spread %.2f yd" % (X, Y, Z, spread))

# ---- 4. world rows ---------------------------------------------------------
O = 0.822483
R2, R3 = 0.399748, 0.916625
world = """
USE lplusdevworld;
DELETE FROM gameobject_template WHERE entry=900001;
INSERT INTO gameobject_template (entry, type, displayId, name, size, Data0, Data1)
VALUES (900001, 5, 11000, 'Goblin Workshop (BG shell)', 1, 0, 0);

DELETE FROM creature_model_info WHERE DisplayID=40000;
INSERT INTO creature_model_info (DisplayID, BoundingRadius, CombatReach, Gender, DisplayID_Other_Gender)
VALUES (40000, 12, 15, 2, 0);

DELETE FROM creature_template WHERE entry=900116;
INSERT INTO creature_template
  (entry, modelid1, name, subname, minlevel, maxlevel, faction, npcflag,
   unit_class, unit_flags, flags_extra, MovementType, RegenHealth,
   mechanic_immune_mask, HealthModifier, ArmorModifier, scale, speed_walk, speed_run,
   AIName, ScriptName)
VALUES
  (900116, 40000, 'Goblin Workshop', 'Converted WMO twin', 80, 80, 14, 0,
   1, 131072, 1073742080, 0, 0, 551238167, 30, 1, 1.02, 1, 1.14286, '', '');

DELETE FROM gameobject WHERE guid=2136202;
INSERT INTO gameobject
  (guid, id, map, zoneId, areaId, spawnMask, phaseMask,
   position_x, position_y, position_z, orientation,
   rotation0, rotation1, rotation2, rotation3, spawntimesecs, animprogress, state)
VALUES
  (2136202, 900001, 0, 139, 139, 1, 1, {X}, {Y}, {Z}, {O}, 0, 0, {R2}, {R3}, 120, 100, 1);

DELETE FROM creature WHERE guid=1022787;
INSERT INTO creature
  (guid, id, map, zoneId, areaId, spawnMask, phaseMask, modelid, equipment_id,
   position_x, position_y, position_z, orientation,
   spawntimesecs, wander_distance, currentwaypoint, curhealth, curmana,
   MovementType, npcflag, unit_flags, dynamicflags)
VALUES
  (1022787, 900116, 0, 139, 139, 1, 1, 0, 0, {X}, {Y}, {Z}, {O},
   60, 0, 0, 500000, 0, 0, 0, 131072, 0);
""".format(X=round(X, 2), Y=round(Y, 2), Z=round(Z + 0.5, 2), O=O, R2=R2, R3=R3)
sql(world)
print(sql("SELECT entry,displayId,name FROM lplusdevworld.gameobject_template WHERE entry=900001;"))
print(sql("SELECT entry,modelid1,scale,name FROM lplusdevworld.creature_template WHERE entry=900116;"))
print(sql("SELECT guid,id,position_x,position_y,position_z FROM lplusdevworld.gameobject WHERE guid=2136202;"))
print(sql("SELECT guid,id,position_x,position_y,position_z FROM lplusdevworld.creature WHERE guid=1022787;"))
print("DONE")
