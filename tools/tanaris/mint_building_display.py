"""Register the converted building model in the display DBCs.

Adds three rows so the WMO-turned-M2 building (wmo2m2.py) exists on both sides
of the twin pattern:

    GameObjectDisplayInfo  11000  -- the GO shell (collision + visuals)
    CreatureModelData       4000  -- the model file, creature-side
    CreatureDisplayInfo    40000  -- the display the twin creature wears

The geo boxes are read out of the M2's own vertex-box header rather than
hand-typed, so they always match the actual geometry.

All three are CLIENT DBCs: nothing renders until they are packed. Same
four-location pipeline as tanaris_dbc.py, same idempotent append behaviour.

    python mint_building_display.py <dbc-dir> [<dbc-dir> ...] [--dry-run]
"""
import os
import shutil
import struct
import sys

M2_PATH = r"C:\Projects\Gamedev\wow\data\patch-staging\TanarisBG-models\World\TanarisBG\WgWorkshopBG.m2"
MODEL_REF = r"World\TanarisBG\WgWorkshopBG.m2"

GO_DISPLAY_ID = 11000
MODEL_DATA_ID = 4000
CRE_DISPLAY_ID = 40000

SPECS = {
    # ID, ModelName, Sound_1..10, GeoBoxMin xyz, GeoBoxMax xyz, ObjectEffectPackageID
    "GameObjectDisplayInfo.dbc": {"fields": "is" + "i" * 10 + "ffffff" + "i", "count": 19},
    # Layout derived EMPIRICALLY from the stock file, not from documentation:
    # a field only counts as a string ref if every row's value resolves to the
    # START of a null-terminated string. That test found a second string at
    # index 3 (an alternate model path, used by roughly half the rows) and
    # showed the apparent strings later in the record are mid-string
    # coincidences. Documentation-from-memory got this wrong once already.
    # ID, Flags, ModelName, ModelNameAlt, ModelScale, SizeClass, BloodID,
    # FootprintLength/Width/ParticleScale, FoleyMaterialID, FootprintTexture,
    # FootstepShakeSize, DeathThudShakeSize, CollisionWidth/Height,
    # MouthHeight, GeoBox, WorldEffectScale, AttachedEffectScale, Missile*3
    "CreatureModelData.dbc": {"fields": "ii" + "ss" + "f" + "ii" + "fff" + "i" + "s" + "ii" + "fff" + "ffffff" + "ff" + "fff", "count": 28},
    # ID, ModelID, SoundID, ExtendedDisplayInfoID, CreatureModelScale,
    # CreatureModelAlpha, TextureVariation_1..3, PortraitTextureName,
    # BloodLevel, BloodID, NPCSoundID, ParticleColorID, CreatureGeosetData,
    # ObjectEffectPackageID
    "CreatureDisplayInfo.dbc": {"fields": "iiiif" + "i" + "ssss" + "iiiiii", "count": 16},
}


def m2_vertex_box(path):
    b = open(path, "rb").read()
    assert b[:4] == b"MD20"
    fl = struct.unpack_from("<14f", b, 8 + 38 * 4)
    return fl[0:3], fl[3:6]


def build_rows():
    mn, mx = m2_vertex_box(M2_PATH)
    box = list(mn) + list(mx)
    return {
        "GameObjectDisplayInfo.dbc": [
            [GO_DISPLAY_ID, MODEL_REF] + [0] * 10 + box + [0],
        ],
        "CreatureModelData.dbc": [
            [MODEL_DATA_ID,
             0,                     # Flags
             MODEL_REF,
             "",                    # ModelNameAlt: empty, like 625 stock rows
             1.0,                   # ModelScale
             1,                     # SizeClass
             -1,                    # BloodID: the common stock value
             0.0, 0.0, 0.0,         # footprint dims
             0,                     # FoleyMaterialID
             "",                    # FootprintTexture
             0, 0,                  # shakes
             5.0, 20.0,             # CollisionWidth/Height
             0.0,                   # MouthHeight
             ] + box + [
             1.0, 1.0,              # world/attached effect scale
             0.0, 0.0, 0.0],        # missile collision
        ],
        "CreatureDisplayInfo.dbc": [
            [CRE_DISPLAY_ID,
             MODEL_DATA_ID,
             0,                     # SoundID
             0,                     # ExtendedDisplayInfoID
             1.0,                   # scale (the 1.02 twin offset lives in creature_template)
             255,                   # alpha
             "", "", "",            # texture variations: M2 paths are hardcoded
             "",                    # portrait
             0, 0, 0, 0, 0, 0],
        ],
    }


def patch(path, spec, rows, dry_run):
    blob = open(path, "rb").read()
    magic, rc, fc, rs, sb = struct.unpack_from("<4sIIII", blob, 0)
    if magic != b"WDBC" or fc != spec["count"] or rs != spec["count"] * 4:
        raise ValueError("%s: layout mismatch (fields=%d recsize=%d)" % (path, fc, rs))
    if len(spec["fields"]) != fc:
        raise ValueError("%s: spec is %d chars for %d fields" % (path, len(spec["fields"]), fc))

    recs = bytearray(blob[20:20 + rc * rs])
    block = bytearray(blob[20 + rc * rs:])
    existing = {struct.unpack_from("<i", recs, i * rs)[0] for i in range(rc)}

    added = 0
    for row in rows:
        if row[0] in existing:
            print("      id %-6d already present" % row[0])
            continue
        packed = bytearray()
        for kind, value in zip(spec["fields"], row):
            if kind == "i":
                packed += struct.pack("<i", int(value))
            elif kind == "f":
                packed += struct.pack("<f", float(value))
            else:
                if value == "":
                    packed += struct.pack("<I", 0)
                else:
                    packed += struct.pack("<I", len(block))
                    block += str(value).encode("utf-8") + b"\x00"
        recs += packed
        rc += 1
        added += 1
        print("      id %-6d appended" % row[0])

    if not added or dry_run:
        if dry_run and added:
            print("      (dry run, not written)")
        return added

    backup = path + ".bak-building"
    if not os.path.exists(backup):
        shutil.copyfile(path, backup)
    out = struct.pack("<4sIIII", b"WDBC", rc, fc, rs, len(block)) + bytes(recs) + bytes(block)
    tmp = path + ".bld-tmp"
    with open(tmp, "wb") as f:
        f.write(out)
    os.replace(tmp, path)
    return added


def main():
    dirs = [a for a in sys.argv[1:] if not a.startswith("--")]
    dry_run = "--dry-run" in sys.argv
    if not dirs:
        raise SystemExit(__doc__)
    rows = build_rows()
    total = 0
    for d in dirs:
        print("=== %s ===" % d)
        for name, spec in SPECS.items():
            p = os.path.join(d, name)
            if not os.path.exists(p):
                print("    %-26s MISSING, skipped" % name)
                continue
            print("    %s" % name)
            total += patch(p, spec, rows[name], dry_run)
    print("rows added: %d%s" % (total, " (dry run)" if dry_run else ""))


if __name__ == "__main__":
    main()
