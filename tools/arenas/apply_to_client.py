"""Put the arenas into a local client: art into patch-Z, DBCs into patch-enUS-8.

Two different jobs, because the two archives are in different shape.

patch-Z has a 256-entry hash table. That is a hard ceiling on how many files it
can ever hold -- it is fixed when the archive is created and cannot grow in
place -- and it currently holds 149. So patch-Z is REBUILT: its existing files
are read out, a new archive is created with room to spare, and everything old
plus everything new goes in. Nothing that was in it is lost.

patch-enUS-8 has 16384 slots and 1658 files, so its DBCs are simply replaced in
place. The five are read out of the archive itself rather than copied from the
server, so only the arena rows are added and no other edit is regressed.

Usage:
    python apply_to_client.py --client <dir> --stage <dir> [--dry-run]
"""

import argparse
import os
import shutil
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SCRATCH = (r"C:\Users\broki\AppData\Local\Temp\claude"
           r"\C--Ascension-Launcher-resources-ascension-live"
           r"\b4c02561-8768-40b5-abb1-376e8e1417b6\scratchpad")
sys.path.insert(0, HERE)
sys.path.insert(0, SCRATCH)

from mpq import MPQArchive                      # noqa: E402
from mpqwrite import Storm, MPQ_FILE_COMPRESS, MPQ_FILE_REPLACEEXISTING  # noqa: E402

DBCS = ["Map.dbc", "BattlemasterList.dbc", "PvpDifficulty.dbc",
        "WorldSafeLocs.dbc", "WorldStateUI.dbc"]

INTERNAL = ("(listfile)", "(attributes)", "(signature)", "(user data)")

ART_MAX_FILES = 16384   # 149 existing + ~4600 new, with headroom


def storm():
    s = Storm()
    s._set_path_width(s.detect_path_width())
    return s


def human(n):
    return "%.1f MB" % (n / 1048576.0)


# ---------------------------------------------------------------- patch-Z
def rebuild_art_patch(client, stage, dry_run):
    target = os.path.join(client, "Data", "patch-Z.MPQ")
    print("=" * 72)
    print("patch-Z  <- arena terrain, models and textures")
    print("=" * 72)

    a = MPQArchive(target)
    existing = [f for f in a.list_files() if f.lower() not in INTERNAL]
    print("  existing archive : %d files, hash table %d, %s"
          % (len(existing), a.hash_count, human(os.path.getsize(target))))

    staged = []
    for root, _d, fs in os.walk(stage):
        for f in fs:
            p = os.path.join(root, f)
            staged.append((p, os.path.relpath(p, stage)))
    print("  to add           : %d files, %s"
          % (len(staged), human(sum(os.path.getsize(p) for p, _ in staged))))

    # names staged wins over an identical name already in the archive
    staged_keys = {n.upper().replace("/", "\\") for _p, n in staged}
    keep = [f for f in existing if f.upper().replace("/", "\\") not in staged_keys]
    print("  keeping          : %d existing (%d superseded by staged copies)"
          % (len(keep), len(existing) - len(keep)))
    total = len(keep) + len(staged)
    print("  final            : %d files (hash table will be %d)" % (total, ART_MAX_FILES))
    if total >= ART_MAX_FILES:
        raise SystemExit("!! %d files will not fit a %d-entry hash table" % (total, ART_MAX_FILES))

    if dry_run:
        a.close()
        print("  (dry run, nothing written)")
        return

    tmp = tempfile.mkdtemp(prefix="patchZ", dir=os.path.join(client, "Data"))
    try:
        # 1. read the existing files out
        print("  extracting existing ...", flush=True)
        for i, name in enumerate(keep):
            data = a.read_file(name)
            dest = os.path.join(tmp, name.replace("\\", os.sep).replace("/", os.sep))
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with open(dest, "wb") as fh:
                fh.write(data)
        a.close()

        # 2. build the replacement beside the original
        newpath = target + ".new"
        if os.path.exists(newpath):
            os.remove(newpath)
        s = storm()
        h = s.create(newpath, ART_MAX_FILES)
        t0 = time.time()
        n = 0
        for name in keep:
            src = os.path.join(tmp, name.replace("\\", os.sep).replace("/", os.sep))
            s.add(h, src, name)
            n += 1
        print("  re-added %d existing files" % n, flush=True)
        for p, name in staged:
            s.add(h, p, name)
            n += 1
            if n % 500 == 0:
                print("    %5d/%d  %4.0fs" % (n, total, time.time() - t0), flush=True)
        s.close(h)
        print("  wrote %s (%s) in %.0fs" % (os.path.basename(newpath),
                                            human(os.path.getsize(newpath)), time.time() - t0))

        # 3. verify before swapping
        verify_art(newpath, keep, staged, total)

        bak = target + ".bak-arenas"
        if not os.path.exists(bak):
            shutil.copyfile(target, bak)
            print("  backed up original -> %s" % os.path.basename(bak))
        os.replace(newpath, target)
        print("  swapped in.")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def verify_art(path, keep, staged, expect_total):
    print("  verifying ...", flush=True)
    a = MPQArchive(path)
    listed = [f for f in a.list_files() if f.lower() not in INTERNAL]
    assert len(listed) == expect_total, "listfile has %d, expected %d" % (len(listed), expect_total)

    import random
    checks = 0
    for name in (keep[:3] + random.sample(keep, min(5, len(keep)))):
        a.read_file(name)
        checks += 1
    for p, name in random.sample(staged, min(25, len(staged))):
        with open(p, "rb") as fh:
            want = fh.read()
        got = a.read_file(name)
        assert got == want, "content mismatch for %s" % name
        checks += 1
    a.close()
    print("  OK: %d files listed, %d spot-checked byte-for-byte" % (len(listed), checks))


# ------------------------------------------------------------- patch-enUS-8
def update_locale_patch(client, dry_run):
    target = os.path.join(client, "Data", "enUS", "patch-enUS-8.MPQ")
    print()
    print("=" * 72)
    print("patch-enUS-8  <- arena DBC rows")
    print("=" * 72)

    a = MPQArchive(target)
    print("  archive: %d files, hash table %d, %s"
          % (len(a.list_files()), a.hash_count, human(os.path.getsize(target))))

    mtime_before = os.path.getmtime(target)

    tmp = tempfile.mkdtemp(prefix="dbc")
    try:
        present = []
        # Every record id each DBC holds before this runs. Other work is live in
        # these same files -- the Violet Hold battleground has rows in four of
        # the five -- and appending must not lose any of it. Checked again after
        # the write rather than assumed.
        before_ids = {}
        for d in DBCS:
            name = "DBFilesClient\\" + d
            if not a.has_file(name):
                print("  !! %s not in the archive - skipped" % d)
                continue
            data = a.read_file(name)
            with open(os.path.join(tmp, d), "wb") as fh:
                fh.write(data)
            before_ids[d] = _record_ids(data)
            present.append(d)
            print("  extracted %-24s %8d bytes, %d records" % (d, len(data), len(before_ids[d])))
        a.close()

        if len(present) != len(DBCS):
            raise SystemExit("!! expected all %d DBCs in the archive" % len(DBCS))

        # append the arena rows to the client's own copies
        print("\n  appending arena rows ...")
        import subprocess
        r = subprocess.run([sys.executable, os.path.join(HERE, "arena_dbc.py"), tmp],
                           capture_output=True, text=True)
        print("   " + r.stdout.strip().splitlines()[-1] if r.stdout.strip() else "")
        if r.returncode:
            print(r.stdout, r.stderr)
            raise SystemExit("arena_dbc.py failed")

        print("\n  verifying the patched DBCs ...")
        r = subprocess.run([sys.executable, os.path.join(HERE, "verify_arena_dbc.py"), tmp],
                           capture_output=True, text=True)
        tail = r.stdout.strip().splitlines()[-1] if r.stdout.strip() else ""
        print("   " + tail)
        if r.returncode:
            print(r.stdout, r.stderr)
            raise SystemExit("verification failed - not writing to the archive")

        for d in present:
            print("  %-24s %8d bytes after" % (d, os.path.getsize(os.path.join(tmp, d))))

        if dry_run:
            print("  (dry run, archive not modified)")
            return

        # Someone else may be editing this archive at the same time. If it moved
        # under us, our extracted copies are stale and writing them back would
        # silently undo their work.
        if os.path.getmtime(target) != mtime_before:
            raise SystemExit("!! %s changed while we were working on it. "
                             "Re-run; nothing has been written." % os.path.basename(target))

        bak = target + ".bak-arenas"
        if not os.path.exists(bak):
            shutil.copyfile(target, bak)
            print("  backed up original -> %s" % os.path.basename(bak))

        s = storm()
        h = s.open(target)
        for d in present:
            s.add(h, os.path.join(tmp, d), "DBFilesClient\\" + d,
                  flags=MPQ_FILE_COMPRESS | MPQ_FILE_REPLACEEXISTING)
        s.close(h)
        print("  wrote %d DBCs into the archive (%s)" % (len(present), human(os.path.getsize(target))))

        # read back out of the archive and re-verify
        a2 = MPQArchive(target)
        out = tempfile.mkdtemp(prefix="dbcback")
        try:
            lost = {}
            for d in present:
                data = a2.read_file("DBFilesClient\\" + d)
                with open(os.path.join(out, d), "wb") as fh:
                    fh.write(data)
                gone = before_ids[d] - _record_ids(data)
                if gone:
                    lost[d] = sorted(gone)
            a2.close()

            if lost:
                for d, ids in lost.items():
                    print("  !! %s LOST %d pre-existing records: %s" % (d, len(ids), ids[:10]))
                raise SystemExit("!! pre-existing rows were lost - restore from "
                                 "patch-enUS-8.MPQ.bak-arenas")
            print("  preserved: every pre-existing record id still present in all %d DBCs"
                  % len(present))
            r = subprocess.run([sys.executable, os.path.join(HERE, "verify_arena_dbc.py"), out],
                               capture_output=True, text=True)
            tail = r.stdout.strip().splitlines()[-1] if r.stdout.strip() else ""
            print("  read back from archive: %s" % tail)
            if r.returncode:
                raise SystemExit("!! DBCs do not verify after being written into the archive")
        finally:
            shutil.rmtree(out, ignore_errors=True)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--client", default=r"C:\Projects\Gamedev\wow\clients\centurion")
    ap.add_argument("--stage", default=os.path.join(SCRATCH, "stage"))
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--only", choices=["art", "dbc"])
    args = ap.parse_args()

    if args.only != "dbc":
        rebuild_art_patch(args.client, args.stage, args.dry_run)
    if args.only != "art":
        update_locale_patch(args.client, args.dry_run)

    print("\ndone.")


if __name__ == "__main__":
    main()
