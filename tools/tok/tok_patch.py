#!/usr/bin/env python3
"""Server-side packing of the Temple of Kotmogu client DBC rows.

Runs on the download server (it needs smpq and /var/www/html/downloads).
The rows themselves are written locally by tok_dbc.py, because that reads the
Ascension MPQs; this script only gets the tables out of the live archives and
puts them back in.

    tok_patch.py extract <work>              # live zips -> <work>/<zip>/dbc/*.dbc
    (download, tok_dbc.py apply, upload to <work>/<zip>/dbc-new/)
    tok_patch.py pack <work>                 # -> <work>/<zip>/staged.zip, verified
    tok_patch.py publish <work> [zip ...]    # swap staged zips live, bump versions

Every archive keeps its own lineage of each table: rows are added to the copy
already inside that archive, never copied across from another one. Tables an
archive does not carry yet (LoadingScreens, SpellIcon and the spell visual
tables in patch-enUS-A) are seeded from the stock client copy placed in
<work>/<zip>/seed/ before applying.

publish refuses a zip whose live copy or version moved since extract, backs up
both as .bak-tok-<oldversion>, and writes the bumped version last.
"""
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import zipfile

LIVE = "/var/www/html/downloads/patches"

# zip -> {inner MPQ: [tables]}
ARCHIVES = {
    "patch-enUS-A": {"patch-enUS-A.MPQ": [
        "Map", "AreaTable", "BattlemasterList", "WorldSafeLocs", "WorldStateUI",
        "WorldMapArea", "Spell", "GameObjectDisplayInfo",
        "LoadingScreens", "SpellIcon", "SpellVisual", "SpellVisualKit",
        "SpellVisualKitModelAttach", "SpellVisualEffectName"]},
    "patch-dungeon-maps": {"patch-M.MPQ": ["WorldMapArea"]},
    "hd-spells": {
        "patch-G.MPQ": ["SpellVisual", "SpellVisualKit", "SpellVisualKitModelAttach", "SpellVisualEffectName"],
        "patch-H.MPQ": ["SpellVisual", "SpellVisualKit", "SpellVisualEffectName"]},
    "hd-misc": {"patch-L.MPQ": ["LoadingScreens"]},
}


def md5(path):
    h = hashlib.md5()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def run(args, cwd=None):
    r = subprocess.run(args, cwd=cwd, capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def mpq_names(mpq):
    """Stored names as smpq -l prints them (StormLib finds them listfile or not)."""
    rc, out = run(["smpq", "-l", mpq])
    names = []
    for line in out.splitlines():
        # "<size> <date> <time> <name>"; the name may itself hold spaces
        parts = line.strip().split(None, 3)
        if len(parts) == 4 and parts[0].isdigit():
            names.append(parts[3])
    return names


def format_version(mpq):
    with open(mpq, "rb") as fh:
        head = fh.read(16)
    return struct.unpack_from("<H", head, 0x0C)[0]


def stored_name(names, table):
    want = ("dbfilesclient/%s.dbc" % table).lower()
    for n in names:
        if n.replace("\\", "/").lower() == want:
            return n
    return None


def extract(work):
    os.makedirs(work, exist_ok=True)
    manifest = {}
    for zipname, inner in ARCHIVES.items():
        zpath = os.path.join(LIVE, zipname + ".zip")
        vpath = os.path.join(LIVE, zipname + ".version")
        base = os.path.join(work, zipname)
        shutil.rmtree(base, ignore_errors=True)
        os.makedirs(os.path.join(base, "unzipped"))
        with zipfile.ZipFile(zpath) as z:
            infos = {i.filename: i.compress_type for i in z.infolist()}
            z.extractall(os.path.join(base, "unzipped"))
        entry = {"zip_md5": md5(zpath), "version": open(vpath).read(), "compress": infos, "mpqs": {}}
        for mpqname, tables in inner.items():
            mpq = os.path.join(base, "unzipped", mpqname)
            names = mpq_names(mpq)
            dbcdir = os.path.join(base, "dbc", mpqname)
            os.makedirs(dbcdir)
            stored = {}
            slash = "/" if any("/" in n for n in names) else "\\"
            for table in tables:
                # A listfile-less archive may hold a table smpq -l cannot name:
                # try the exact spellings before calling it new.
                listed = stored_name(names, table)
                candidates = [listed] if listed else \
                    ["DBFilesClient\\%s.dbc" % table, "DBFilesClient/%s.dbc" % table]
                stored[table] = None
                for name in candidates:
                    tmp = os.path.join(base, "x")
                    shutil.rmtree(tmp, ignore_errors=True)
                    os.makedirs(tmp)
                    rc, out = run(["smpq", "-x", mpq, name], cwd=tmp)
                    found = [os.path.join(r, f) for r, _, fs in os.walk(tmp) for f in fs]
                    if len(found) == 1 and os.path.getsize(found[0]) > 20:
                        shutil.move(found[0], os.path.join(dbcdir, table + ".dbc"))
                        stored[table] = name
                        break
                if listed and not stored[table]:
                    raise SystemExit("%s: could not extract listed %s" % (mpqname, listed))
            entry["mpqs"][mpqname] = {"stored": stored, "slash": slash, "count": len(names),
                                      "format": format_version(mpq), "md5": md5(mpq)}
            print("%-20s %-18s fmt %d, %d names, tables: %s" % (
                zipname, mpqname, format_version(mpq), len(names),
                ", ".join("%s%s" % (t, "" if stored[t] else " (NEW)") for t in tables)))
        manifest[zipname] = entry
    with open(os.path.join(work, "manifest.json"), "w") as fh:
        json.dump(manifest, fh, indent=1)


def pack(work):
    manifest = json.load(open(os.path.join(work, "manifest.json")))
    for zipname, entry in manifest.items():
        base = os.path.join(work, zipname)
        outdir = os.path.join(base, "out")
        shutil.rmtree(outdir, ignore_errors=True)
        os.makedirs(outdir)
        for mpqname, info in entry["mpqs"].items():
            src = os.path.join(base, "unzipped", mpqname)
            if md5(src) != info["md5"]:
                raise SystemExit("%s changed since extract" % src)
            dst = os.path.join(outdir, mpqname)
            shutil.copy2(src, dst)
            newdir = os.path.join(base, "dbc-new", mpqname)
            stage = os.path.join(base, "stage", mpqname)
            shutil.rmtree(stage, ignore_errors=True)
            os.makedirs(os.path.join(stage, "DBFilesClient"))
            added = 0
            for table, stored in info["stored"].items():
                newfile = os.path.join(newdir, table + ".dbc")
                if not os.path.exists(newfile):
                    raise SystemExit("%s/%s: no new %s.dbc uploaded" % (zipname, mpqname, table))
                if stored:
                    rc, out = run(["smpq", "-d", dst, stored])
                    if stored in mpq_names(dst):
                        raise SystemExit("%s: delete of %s did not take: %s" % (mpqname, stored, out))
                else:
                    added += 1
                rel = "DBFilesClient/" + table + ".dbc"
                shutil.copy2(newfile, os.path.join(stage, "DBFilesClient", table + ".dbc"))
                rc, out = run(["smpq", "-a", "-f", dst, rel], cwd=stage)
            # Verify: every table comes back out byte-identical to what was uploaded.
            names = mpq_names(dst)
            for table in info["stored"]:
                # Archives whose listfile does not name their tables (patch-G)
                # are read back by exact name; the md5 round-trip is the proof.
                name = stored_name(names, table) or "DBFilesClient\\%s.dbc" % table
                tmp = os.path.join(base, "verify")
                shutil.rmtree(tmp, ignore_errors=True)
                os.makedirs(tmp)
                run(["smpq", "-x", dst, name], cwd=tmp)
                found = [os.path.join(r, f) for r, _, fs in os.walk(tmp) for f in fs]
                if len(found) != 1 or md5(found[0]) != md5(os.path.join(newdir, table + ".dbc")):
                    raise SystemExit("%s: %s round-trip md5 mismatch" % (mpqname, table))
            if format_version(dst) != info["format"]:
                raise SystemExit("%s: formatVersion changed %d -> %d" % (mpqname, info["format"], format_version(dst)))
            if info["count"] and len(names) < info["count"] + added:
                raise SystemExit("%s: lost names (%d before, %d after, %d added)" % (mpqname, info["count"], len(names), added))
            print("%-20s %-18s ok: %d tables verified, %d new, fmt %d, names %d -> %d" % (
                zipname, mpqname, len(info["stored"]), added, format_version(dst), info["count"], len(names)))
        staged = os.path.join(base, "staged.zip")
        if os.path.exists(staged):
            os.remove(staged)
        with zipfile.ZipFile(staged, "w") as z:
            for mpqname in entry["mpqs"]:
                z.write(os.path.join(outdir, mpqname), mpqname, compress_type=entry["compress"][mpqname])
        entry["staged_md5"] = md5(staged)
        print("%-20s staged.zip %s (%d bytes)" % (zipname, entry["staged_md5"], os.path.getsize(staged)))
    with open(os.path.join(work, "manifest.json"), "w") as fh:
        json.dump(manifest, fh, indent=1)


def publish(work, only):
    manifest = json.load(open(os.path.join(work, "manifest.json")))
    for zipname, entry in manifest.items():
        if only and zipname not in only:
            continue
        zpath = os.path.join(LIVE, zipname + ".zip")
        vpath = os.path.join(LIVE, zipname + ".version")
        staged = os.path.join(work, zipname, "staged.zip")
        live_ver = open(vpath).read()
        if md5(zpath) != entry["zip_md5"] or live_ver != entry["version"]:
            raise SystemExit("REFUSING %s: live archive moved since extract" % zipname)
        if md5(staged) != entry.get("staged_md5"):
            raise SystemExit("REFUSING %s: staged zip changed since pack" % zipname)
        major, minor = live_ver.strip().split(".")
        new_ver = "%s.%05d" % (major, int(minor) + 1)
        shutil.copy2(zpath, "%s.bak-tok-%s" % (zpath, live_ver))
        shutil.copy2(vpath, "%s.bak-tok-%s" % (vpath, live_ver))
        tmp = zpath + ".tmp-tok"
        shutil.copy2(staged, tmp)
        if md5(tmp) != entry["staged_md5"]:
            raise SystemExit("copy of %s corrupted" % zipname)
        os.replace(tmp, zpath)
        with open(vpath + ".tmp-tok", "w") as fh:
            fh.write(new_ver)
        os.replace(vpath + ".tmp-tok", vpath)
        print("published %-20s %s -> %s  %s" % (zipname, live_ver, new_ver, md5(zpath)))


def main():
    cmd, work = sys.argv[1], sys.argv[2]
    if cmd == "extract":
        extract(work)
    elif cmd == "pack":
        pack(work)
    elif cmd == "publish":
        publish(work, set(sys.argv[3:]))
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
