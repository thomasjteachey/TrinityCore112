#!/usr/bin/env python3
"""Server-side packing of The Eye of Eternity arena into the client patches.

Runs on the download server (needs smpq and /var/www/html/downloads). The rows
are written locally by eoe_dbc.py (it reads the local client for its templates);
this only gets the tables out of the live archives and puts them back in, with
the terrain eoe_terrain.py built.

    eoe_patch.py extract <work>              # live zips -> <work>/<zip>/dbc/<mpq>/*.dbc
    (download, eoe_dbc.py apply, upload to <work>/<zip>/dbc-new/<mpq>/;
     upload the terrain to <work>/terrain/World/Maps/EyeOfEternityArena/)
    eoe_patch.py pack <work>                 # -> <work>/<zip>/staged.zip, verified
    eoe_patch.py publish <work> [zip ...]    # swap staged zips live, bump versions

    patch-enUS-A   Map, BattlemasterList, PvpDifficulty, WorldSafeLocs,
                   WorldStateUI, Light + the map's WDT/WDL and 25 ADTs
    world-terrain  Light (the optional HD World pack ships its own Light.dbc,
                   and Y outranks A, so without this its players get no sky row)

Every archive keeps its own lineage of each table: rows are added to the copy
already inside that archive. publish refuses a zip whose live copy or version
moved since extract (another session publishing), backs up both as
.bak-eoe-<oldversion>, and writes the bumped version last.
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
TERRAIN_DIR = "World/Maps/EyeOfEternityArena"

ARCHIVES = {
    "patch-enUS-A": {"patch-enUS-A.MPQ": ["Map", "BattlemasterList", "PvpDifficulty", "WorldSafeLocs",
                                          "WorldStateUI", "Light"]},
    "world-terrain": {"patch-Y.MPQ": ["Light"]},
}
TERRAIN_INTO = ("patch-enUS-A", "patch-enUS-A.MPQ")


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
    rc, out = run(["smpq", "-l", mpq])
    names = []
    for line in out.splitlines():
        parts = line.strip().split(None, 3)
        if len(parts) == 4 and parts[0].isdigit():
            names.append(parts[3])
    return names


def header(mpq):
    with open(mpq, "rb") as fh:
        head = fh.read(32)
    fmt = struct.unpack_from("<H", head, 0x0C)[0]
    hash_entries, block_entries = struct.unpack_from("<II", head, 0x18)
    return fmt, hash_entries, block_entries


def stored_name(names, rel):
    want = rel.replace("\\", "/").lower()
    for n in names:
        if n.replace("\\", "/").lower() == want:
            return n
    return None


def extract_one(mpq, name, base):
    tmp = os.path.join(base, "x")
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(tmp)
    run(["smpq", "-x", mpq, name], cwd=tmp)
    found = [os.path.join(r, f) for r, _, fs in os.walk(tmp) for f in fs]
    return found[0] if len(found) == 1 and os.path.getsize(found[0]) > 0 else None


def terrain_files(work):
    root = os.path.join(work, "terrain")
    out = []
    for r, _, fs in os.walk(root):
        for f in fs:
            full = os.path.join(r, f)
            out.append(os.path.relpath(full, root).replace(os.sep, "/"))
    return sorted(out)


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
            for table in tables:
                rel = "DBFilesClient/%s.dbc" % table
                listed = stored_name(names, rel)
                found = extract_one(mpq, listed or rel.replace("/", "\\"), base)
                if not found:
                    raise SystemExit("%s: no %s to add rows to" % (mpqname, rel))
                shutil.move(found, os.path.join(dbcdir, table + ".dbc"))
                stored[table] = listed or rel.replace("/", "\\")
            fmt, hashes, blocks = header(mpq)
            entry["mpqs"][mpqname] = {"stored": stored, "count": len(names), "format": fmt,
                                      "hash_entries": hashes, "block_entries": blocks, "md5": md5(mpq)}
            print("%-15s %-18s fmt %d, %d names, hash table %d, block table %d, tables %s" % (
                zipname, mpqname, fmt, len(names), hashes, blocks, ", ".join(tables)))
        manifest[zipname] = entry
    with open(os.path.join(work, "manifest.json"), "w") as fh:
        json.dump(manifest, fh, indent=1)


def pack(work):
    manifest = json.load(open(os.path.join(work, "manifest.json")))
    extra = terrain_files(work)
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
            names = mpq_names(dst)
            newdir = os.path.join(base, "dbc-new", mpqname)
            stage = os.path.join(base, "stage", mpqname)
            shutil.rmtree(stage, ignore_errors=True)
            os.makedirs(os.path.join(stage, "DBFilesClient"))

            # what goes in: (name as stored/added, local file)
            puts = []
            for table, stored in info["stored"].items():
                newfile = os.path.join(newdir, table + ".dbc")
                if not os.path.exists(newfile):
                    raise SystemExit("%s/%s: no new %s.dbc uploaded" % (zipname, mpqname, table))
                shutil.copy2(newfile, os.path.join(stage, "DBFilesClient", table + ".dbc"))
                puts.append((stored, "DBFilesClient/%s.dbc" % table, newfile))
            new_names = 0
            if (zipname, mpqname) == TERRAIN_INTO:
                if not extra:
                    raise SystemExit("no terrain uploaded to %s/terrain" % work)
                for rel in extra:
                    target = os.path.join(stage, rel)
                    os.makedirs(os.path.dirname(target), exist_ok=True)
                    shutil.copy2(os.path.join(work, "terrain", rel), target)
                    listed = stored_name(names, rel)
                    puts.append((listed, rel, target))
                    if not listed:
                        new_names += 1
            if info["hash_entries"] and len(names) + new_names > info["hash_entries"] * 0.9:
                raise SystemExit("%s: hash table %d too small for %d names" % (mpqname, info["hash_entries"], len(names) + new_names))

            for stored, rel, local in puts:
                if stored:
                    rc, out = run(["smpq", "-d", dst, stored])
                    if stored in mpq_names(dst):
                        raise SystemExit("%s: delete of %s did not take: %s" % (mpqname, stored, out))
                rc, out = run(["smpq", "-a", "-f", dst, rel], cwd=stage)
            # Every file comes back out byte-identical to what went in.
            after = mpq_names(dst)
            for stored, rel, local in puts:
                name = stored_name(after, rel) or rel.replace("/", "\\")
                found = extract_one(dst, name, base)
                if not found or md5(found) != md5(local):
                    raise SystemExit("%s: %s round-trip md5 mismatch" % (mpqname, rel))
            fmt, _h, _b = header(dst)
            if fmt != info["format"]:
                raise SystemExit("%s: formatVersion changed %d -> %d" % (mpqname, info["format"], fmt))
            if len(after) != len(names) + new_names:
                raise SystemExit("%s: %d names before, %d after, %d added" % (mpqname, len(names), len(after), new_names))
            print("%-15s %-18s ok: %d files verified (%d new), fmt %d, names %d -> %d" % (
                zipname, mpqname, len(puts), new_names, fmt, len(names), len(after)))
        staged = os.path.join(base, "staged.zip")
        if os.path.exists(staged):
            os.remove(staged)
        with zipfile.ZipFile(staged, "w") as z:
            for mpqname in entry["mpqs"]:
                z.write(os.path.join(outdir, mpqname), mpqname, compress_type=entry["compress"][mpqname])
        with zipfile.ZipFile(staged) as z:
            if z.testzip() is not None:
                raise SystemExit("%s: staged zip fails its CRC test" % zipname)
        entry["staged_md5"] = md5(staged)
        print("%-15s staged.zip %s (%d bytes)" % (zipname, entry["staged_md5"], os.path.getsize(staged)))
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
        shutil.copy2(zpath, "%s.bak-eoe-%s" % (zpath, live_ver))
        shutil.copy2(vpath, "%s.bak-eoe-%s" % (vpath, live_ver))
        tmp = zpath + ".tmp-eoe"
        shutil.copy2(staged, tmp)
        if md5(tmp) != entry["staged_md5"]:
            raise SystemExit("copy of %s corrupted" % zipname)
        os.replace(tmp, zpath)
        with open(vpath + ".tmp-eoe", "w") as fh:
            fh.write(new_ver)
        os.replace(vpath + ".tmp-eoe", vpath)
        print("published %-15s %s -> %s  %s" % (zipname, live_ver, new_ver, md5(zpath)))


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
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
