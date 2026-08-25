"""Pack arena minimap tiles and their translate entries into patch-Y.

Three sources, one destination:

  * Blade's Edge (986) -- 25 real tiles that already exist in the Ascension
    data. They were missed originally because Ascension uses two flat naming
    conventions and the first packer only matched the one with a separating
    underscore (`coliseumarena_35_14.blp` vs `BladesEdgeArena2b18_24.blp`).
  * Guardian's Hall (1007) and Spark of Creator (1008) -- no tiles exist,
    because both maps are a single WMO with no terrain for a minimap generator
    to draw. Rendered from the WMO mesh by make_minimap.py.
  * md5translate.trs -- a stock client resolves a minimap through this table, so
    tiles alone do nothing. The client's existing table is read out of the
    archive, appended to, and written back.

patch-Y is large, shared, and has other work's backups beside it, so this backs
it up, refuses to write if it changed underneath, and verifies afterwards that
every file it did not put there is still present.
"""

import argparse
import os
import re
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SCRATCH = (r"C:\Users\broki\AppData\Local\Temp\claude"
           r"\C--Ascension-Launcher-resources-ascension-live"
           r"\b4c02561-8768-40b5-abb1-376e8e1417b6\scratchpad")
sys.path.insert(0, HERE)
sys.path.insert(0, SCRATCH)

from mpq import MPQArchive                                             # noqa: E402
from mpqwrite import Storm, MPQ_FILE_COMPRESS, MPQ_FILE_REPLACEEXISTING  # noqa: E402
from deps import Index                                                 # noqa: E402
from blp import inspect as blp_inspect                                 # noqa: E402

PATCH_Y = r"C:\Projects\Gamedev\wow\clients\centurion\Data\patch-Y.MPQ"
TRS = "textures\\Minimap\\md5translate.trs"
RENDERED = os.path.join(HERE, "_minimap")

PREFIX = "TEXTURES\\MINIMAP\\"
TAIL = re.compile(r"^_?(\d{1,2})_(\d{1,2})\.BLP$")

# directory -> where its tiles come from
COPY_FROM_SOURCE = ["bladesedgearena2b"]
FROM_RENDER = ["karazhanarena", "ulduararena"]


def source_tiles(idx, directory):
    """[(archive key, real name, col, row)] under either flat convention."""
    up = directory.upper()
    out = []
    for key in idx.by_path:
        if not key.startswith(PREFIX):
            continue
        base = key[len(PREFIX):]
        if "\\" in base or not base.startswith(up):
            continue
        m = TAIL.match(base[len(up):])
        if m:
            out.append((key, idx.by_path[key][1], int(m.group(1)), int(m.group(2))))
    return sorted(out, key=lambda t: (t[2], t[3]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    a = MPQArchive(PATCH_Y)
    listing = a.list_files()
    before = {f.upper().replace("/", "\\") for f in listing}
    print("patch-Y: %d files, hash table %d, %.1f MB"
          % (len(listing), a.hash_count, os.path.getsize(PATCH_Y) / 1048576.0))
    mtime_before = os.path.getmtime(PATCH_Y)

    trs_name = next((f for f in listing if f.lower().endswith("md5translate.trs")), None)
    if not trs_name:
        raise SystemExit("!! patch-Y has no md5translate.trs -- refusing to guess where it lives")
    trs_text = a.read_file(trs_name).decode("utf-8", "replace")
    a.close()
    lines = trs_text.replace("\r\n", "\n").split("\n")
    while lines and not lines[-1].strip():
        lines.pop()
    have_dirs = {l.split(":", 1)[1].strip().lower() for l in lines if l.lower().startswith("dir:")}
    print("translate table: %s, %d lines, %d dir blocks\n" % (trs_name, len(lines), len(have_dirs)))

    idx = Index()
    staged = []          # (local path, archived name)
    tmp = tempfile.mkdtemp(prefix="minimap")
    trs_added = []

    try:
        # ---- Blade's Edge: copy the real tiles out of the source
        for d in COPY_FROM_SOURCE:
            tiles = source_tiles(idx, d)
            print("%s: %d tile(s) in source" % (d, len(tiles)))
            entries = []
            for key, real, col, row in tiles:
                blob = idx.read(key)
                if blob is None:
                    print("   !! unreadable %s" % real)
                    continue
                info = blp_inspect(blob)
                if info.get("error"):
                    print("   !! %s: %s" % (real, info["error"]))
                    continue
                fname = os.path.basename(real)
                p = os.path.join(tmp, fname)
                with open(p, "wb") as fh:
                    fh.write(blob)
                # MPQ lookups hash the uppercased name, so the case here is
                # cosmetic -- match the archive's existing spelling anyway.
                staged.append((p, "Textures\\Minimap\\" + fname))
                entries.append((col, row, fname))
            if d.lower() not in have_dirs and entries:
                trs_added.append((d, sorted(entries)))

        # ---- Karazhan / Ulduar: the tiles rendered from their WMOs
        for d in FROM_RENDER:
            entries = []
            for fn in sorted(os.listdir(RENDERED)):
                if not fn.lower().endswith(".blp") or not fn.startswith(d + "_"):
                    continue
                m = TAIL.match(fn[len(d):].upper())
                if not m:
                    continue
                col, row = int(m.group(1)), int(m.group(2))
                p = os.path.join(RENDERED, fn)
                with open(p, "rb") as fh:
                    info = blp_inspect(fh.read())
                if info.get("error"):
                    print("   !! %s: %s" % (fn, info["error"]))
                    continue
                staged.append((p, "Textures\\Minimap\\" + fn))
                entries.append((col, row, fn))
            print("%s: %d rendered tile(s)" % (d, len(entries)))
            if d.lower() not in have_dirs and entries:
                trs_added.append((d, sorted(entries)))

        idx.close()

        for d, entries in trs_added:
            lines.append("dir: %s" % d)
            for col, row, fname in entries:
                lines.append("%s\\map%d_%d.blp\t%s" % (d, col, row, fname))
        print("\nstaging %d tile(s), adding %d translate block(s): %s"
              % (len(staged), len(trs_added), ", ".join(d for d, _ in trs_added) or "none"))

        if args.dry_run:
            print("(dry run, nothing written)")
            return

        trs_path = os.path.join(tmp, "md5translate.trs")
        with open(trs_path, "w", encoding="utf-8", newline="\r\n") as fh:
            fh.write("\n".join(lines) + "\n")
        staged.append((trs_path, trs_name))

        if os.path.getmtime(PATCH_Y) != mtime_before:
            raise SystemExit("!! patch-Y changed while we were working. Re-run; nothing written.")

        bak = PATCH_Y + ".bak-minimaps"
        if not os.path.exists(bak):
            shutil.copyfile(PATCH_Y, bak)
            print("backed up -> %s" % os.path.basename(bak))

        s = Storm()
        s._set_path_width(s.detect_path_width())
        h = s.open(PATCH_Y)
        for p, name in staged:
            s.add(h, p, name, flags=MPQ_FILE_COMPRESS | MPQ_FILE_REPLACEEXISTING)
        s.close(h)
        print("wrote %d file(s); patch-Y now %.1f MB"
              % (len(staged), os.path.getsize(PATCH_Y) / 1048576.0))

        # ---- verify
        a2 = MPQArchive(PATCH_Y)
        after = {f.upper().replace("/", "\\") for f in a2.list_files()}
        lost = before - after
        if lost:
            print("!! %d pre-existing file(s) disappeared:" % len(lost))
            for f in sorted(lost)[:10]:
                print("     %s" % f)
            raise SystemExit("!! restore from patch-Y.MPQ.bak-minimaps")
        print("preserved: all %d pre-existing files still present" % len(before))

        bad = 0
        for p, name in staged:
            if name.lower().endswith(".trs"):
                continue
            with open(p, "rb") as fh:
                want = fh.read()
            if a2.read_file(name) != want:
                print("   !! mismatch %s" % name)
                bad += 1
        got_trs = a2.read_file(trs_name).decode("utf-8", "replace")
        for d, _e in trs_added:
            if ("dir: %s" % d) not in got_trs:
                print("   !! translate block missing for %s" % d)
                bad += 1
        a2.close()
        print("verified %d tile(s) byte-for-byte and %d translate block(s): %s"
              % (len(staged) - 1, len(trs_added), "OK" if not bad else "%d PROBLEM(S)" % bad))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
