#!/usr/bin/env python3
"""Merge the Temple of Kotmogu client art into the art base (patch-Y) on the
download server, and publish it.

    tok_art.py stage <work> <files.tgz>   # build <work>/staged.zip, verified
    tok_art.py publish <work>             # swap it live, bump patch-Y.version

<files.tgz> is the stage_client_files.py output (map, models, textures, icons,
loading screen, world map, minimap tiles) plus _md5translate_ValleyOfPower.txt,
the block appended to textures\\Minimap\\md5translate.trs.

The served zip is extracted fresh, so this always builds on whatever is live
(promote_patch_z.py and the forge's patch cache both start from that zip too).
A staged file whose name patch-Y already holds is skipped when identical and
stops the run when it differs - this only ever adds art. Every added file is
read back out of the rebuilt archive and compared by md5; the file count must
grow by exactly the number added. publish refuses if the live zip or version
moved since stage, backs both up as .bak-tok-<oldversion> and writes the
version last.
"""
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tarfile
import zipfile

LIVE = "/var/www/html/downloads/patches"
ZIP = os.path.join(LIVE, "patch-Y.zip")
VER = os.path.join(LIVE, "patch-Y.version")
INNER = "patch-Y.MPQ"
TRS_BLOCK = "_md5translate_ValleyOfPower.txt"


def md5(path):
    h = hashlib.md5()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def run(args, cwd=None):
    r = subprocess.run(args, cwd=cwd, capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def names_of(mpq):
    rc, out = run(["smpq", "-l", mpq])
    names = []
    for line in out.splitlines():
        parts = line.strip().split(None, 3)
        if len(parts) == 4 and parts[0].isdigit():
            names.append(parts[3])
    return names


def key(name):
    return name.replace("\\", "/").lower()


def extract_one(mpq, name, into):
    shutil.rmtree(into, ignore_errors=True)
    os.makedirs(into)
    run(["smpq", "-x", mpq, name], cwd=into)
    found = [os.path.join(r, f) for r, _, fs in os.walk(into) for f in fs]
    return found[0] if len(found) == 1 else None


def stage(work, tgz):
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    files = os.path.join(work, "files")
    with tarfile.open(tgz) as t:
        t.extractall(files)

    live_md5, live_ver = md5(ZIP), open(VER).read()
    with zipfile.ZipFile(ZIP) as z:
        info = z.getinfo(INNER)
        z.extract(INNER, work)
    mpq = os.path.join(work, INNER)
    fmt_before = struct.unpack_from("<H", open(mpq, "rb").read(16), 0x0C)[0]
    before = names_of(mpq)
    have = {key(n): n for n in before}
    print("patch-Y %s: %d files, format %d" % (live_ver, len(before), fmt_before))

    # The minimap translate table: append our block once.
    trs_name = next((n for n in before if key(n).endswith("textures/minimap/md5translate.trs")), None)
    if not trs_name:
        raise SystemExit("patch-Y has no md5translate.trs - refusing to guess where it lives")
    trs_file = extract_one(mpq, trs_name, os.path.join(work, "trs"))
    raw = open(trs_file, "rb").read().decode("utf-8")
    newline = "\r\n" if "\r\n" in raw else "\n"
    block = open(os.path.join(files, TRS_BLOCK), encoding="utf-8").read().replace("\r\n", "\n").strip("\n")
    os.remove(os.path.join(files, TRS_BLOCK))
    if "dir: valleyofpower" in raw.lower():
        print("md5translate.trs already has ValleyOfPower - left alone")
        trs_rel = None
    else:
        text = raw.rstrip("\r\n") + newline + block.replace("\n", newline) + newline
        trs_rel = trs_name.replace("\\", "/")
        os.makedirs(os.path.dirname(os.path.join(files, trs_rel)), exist_ok=True)
        with open(os.path.join(files, trs_rel), "wb") as fh:
            fh.write(text.encode("utf-8"))
        print("md5translate.trs: +%d lines" % len(block.split("\n")))

    # What to add: everything staged, minus names patch-Y already holds identically.
    to_add, same, clash = [], 0, []
    for root, _, fs in os.walk(files):
        for f in fs:
            rel = os.path.relpath(os.path.join(root, f), files).replace(os.sep, "/")
            if rel.startswith("_"):
                continue
            existing = have.get(key(rel))
            if existing and rel != trs_rel:
                got = extract_one(mpq, existing, os.path.join(work, "cmp"))
                if got and md5(got) == md5(os.path.join(files, rel)):
                    same += 1
                    continue
                clash.append(existing)
                continue
            to_add.append(rel)
    if clash:
        raise SystemExit("patch-Y already holds different versions of %d staged files, e.g. %s" % (len(clash), clash[:5]))
    new_count = sum(1 for r in to_add if key(r) not in have)
    print("adding %d files (%d new names), %d already present and identical" % (len(to_add), new_count, same))

    if trs_rel:
        rc, out = run(["smpq", "-d", mpq, trs_name])
    for i in range(0, len(to_add), 100):
        rc, out = run(["smpq", "-a", "-f", mpq] + to_add[i:i + 100], cwd=files)
        if rc != 0:
            raise SystemExit("smpq -a failed: %s" % out[-2000:])

    after = names_of(mpq)
    if len(after) != len(before) + new_count:
        raise SystemExit("file count %d -> %d, expected +%d" % (len(before), len(after), new_count))
    fmt_after = struct.unpack_from("<H", open(mpq, "rb").read(16), 0x0C)[0]
    if fmt_after != fmt_before:
        raise SystemExit("formatVersion changed %d -> %d" % (fmt_before, fmt_after))
    bad = 0
    for rel in to_add:
        got = extract_one(mpq, rel, os.path.join(work, "verify"))
        if not got or md5(got) != md5(os.path.join(files, rel)):
            bad += 1
            print("   !! %s did not round-trip" % rel)
    if bad:
        raise SystemExit("%d files failed the round-trip" % bad)
    print("verified: %d files read back byte-identical, %d -> %d names" % (len(to_add), len(before), len(after)))

    staged = os.path.join(work, "staged.zip")
    with zipfile.ZipFile(staged, "w") as z:
        z.write(mpq, INNER, compress_type=info.compress_type)
    manifest = {"live_md5": live_md5, "live_version": live_ver, "staged_md5": md5(staged),
                "added": to_add}
    with open(os.path.join(work, "manifest.json"), "w") as fh:
        json.dump(manifest, fh, indent=1)
    print("staged.zip %s (%d bytes)" % (manifest["staged_md5"], os.path.getsize(staged)))


def publish(work):
    manifest = json.load(open(os.path.join(work, "manifest.json")))
    staged = os.path.join(work, "staged.zip")
    live_ver = open(VER).read()
    if md5(ZIP) != manifest["live_md5"] or live_ver != manifest["live_version"]:
        raise SystemExit("REFUSING: patch-Y moved since stage - re-run stage")
    if md5(staged) != manifest["staged_md5"]:
        raise SystemExit("REFUSING: staged zip changed")
    major, minor = live_ver.strip().split(".")
    new_ver = "%s.%05d" % (major, int(minor) + 1)
    shutil.copy2(ZIP, "%s.bak-tok-%s" % (ZIP, live_ver))
    shutil.copy2(VER, "%s.bak-tok-%s" % (VER, live_ver))
    tmp = ZIP + ".tmp-tok"
    shutil.copy2(staged, tmp)
    if md5(tmp) != manifest["staged_md5"]:
        raise SystemExit("copy corrupted")
    os.replace(tmp, ZIP)
    with open(VER + ".tmp-tok", "w") as fh:
        fh.write(new_ver)
    os.replace(VER + ".tmp-tok", VER)
    print("published patch-Y %s -> %s  %s" % (live_ver, new_ver, md5(ZIP)))


if __name__ == "__main__":
    if sys.argv[1] == "stage":
        stage(sys.argv[2], sys.argv[3])
    elif sys.argv[1] == "publish":
        publish(sys.argv[2])
    else:
        raise SystemExit(__doc__)
