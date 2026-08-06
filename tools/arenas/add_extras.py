"""Append the pieces the first patch-Z pass missed: specular maps and minimaps.

Two gaps, both invisible until the client actually renders the map.

1. `_s.blp` specular companions. An ADT names its ground textures in MTEX but
   never their specular halves; the client loads those by convention. Without
   them the terrain draws bright green.

2. Minimaps. Ascension stores them flat as
   `textures\\minimap\\<dir>_<col>_<row>.blp` and its own client resolves that
   directly. A stock client goes through `textures\\Minimap\\md5translate.trs`,
   which maps a logical `<dir>\\map<col>_<row>.blp` onto a real filename -- so
   the tiles are useless here until translate entries exist for them. The
   client's existing table is read out of patch-Z, appended to, and written back.

patch-Z was rebuilt with a 32768-entry hash table and holds ~4.7k files, so this
appends rather than rebuilding.

Usage: python add_extras.py [--dry-run]
"""

import argparse
import os
import re
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SCRATCH = (r"C:\Users\broki\AppData\Local\Temp\claude"
           r"\C--Ascension-Launcher-resources-ascension-live"
           r"\b4c02561-8768-40b5-abb1-376e8e1417b6\scratchpad")
sys.path.insert(0, HERE)
sys.path.insert(0, SCRATCH)

from mpq import MPQArchive                       # noqa: E402
from mpqwrite import Storm, MPQ_FILE_COMPRESS, MPQ_FILE_REPLACEEXISTING  # noqa: E402
from deps import Index                           # noqa: E402
from gen_arena_sql import ARENAS                 # noqa: E402

PATCH_Z = r"C:\Projects\Gamedev\wow\clients\centurion\Data\patch-Z.MPQ"
TRS_NAME = "textures\\Minimap\\md5translate.trs"

TILE_RE = re.compile(r"^(?P<dir>.+)_(?P<col>\d{1,2})_(?P<row>\d{1,2})\.blp$", re.I)


def storm():
    s = Storm()
    s._set_path_width(s.detect_path_width())
    return s


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    print("indexing source archives ...", flush=True)
    idx = Index()

    have = set()
    a = MPQArchive(PATCH_Z)
    client_listing = a.list_files()
    print("patch-Z currently: %d files, hash table %d" % (len(client_listing), a.hash_count))

    # ---------------------------------------------------- 1. missing textures
    missing_path = os.path.join(HERE, "missing_textures.txt")
    tex = []
    if os.path.exists(missing_path):
        with open(missing_path, encoding="utf-8") as fh:
            tex = [l.strip() for l in fh if l.strip()]
    print("\nspecular / texture files listed as missing: %d" % len(tex))

    # ---------------------------------------------------- 2. minimap tiles
    tiles = []          # (archive key, archived name, mapdir, col, row)
    for bg, mid, directory, name, cx, cy, cz, conf, ev in ARENAS:
        pref = ("TEXTURES\\MINIMAP\\" + directory + "_").upper()
        for key in idx.by_path:
            if not key.startswith(pref):
                continue
            real = idx.by_path[key][1]
            m = TILE_RE.match(os.path.basename(real))
            if not m:
                continue
            tiles.append((key, real, directory, int(m.group("col")), int(m.group("row"))))
    print("minimap tiles found: %d across %d arenas"
          % (len(tiles), len({t[2] for t in tiles})))

    # ---------------------------------------------------- 3. merged trs
    trs_existing = a.read_file(TRS_NAME).decode("utf-8", "replace")
    a.close()
    lines = trs_existing.replace("\r\n", "\n").split("\n")
    while lines and not lines[-1].strip():
        lines.pop()
    existing_dirs = {l.split(":", 1)[1].strip().lower()
                     for l in lines if l.lower().startswith("dir:")}
    print("existing translate table: %d lines, %d dir blocks"
          % (len(lines), len(existing_dirs)))

    added_blocks = 0
    by_dir = {}
    for key, real, directory, col, row in tiles:
        by_dir.setdefault(directory, []).append((col, row, os.path.basename(real)))
    for directory in sorted(by_dir):
        if directory.lower() in existing_dirs:
            print("  %-22s already in the table, left alone" % directory)
            continue
        lines.append("dir: %s" % directory)
        for col, row, fname in sorted(by_dir[directory]):
            lines.append("%s\\map%d_%d.blp\t%s" % (directory, col, row, fname))
        added_blocks += 1
    print("translate blocks added: %d" % added_blocks)

    if args.dry_run:
        print("\n(dry run) would add %d textures + %d tiles + rewritten trs"
              % (len(tex), len(tiles)))
        idx.close()
        return

    # ---------------------------------------------------- stage and pack
    tmp = tempfile.mkdtemp(prefix="extras")
    try:
        staged = []

        for t in tex:
            key = t.upper().replace("/", "\\")
            hit = idx.get(key)
            if not hit:
                print("  !! not in source: %s" % t)
                continue
            data = idx.read(key)
            if data is None:
                print("  !! unreadable: %s" % t)
                continue
            name = hit[1]
            dest = os.path.join(tmp, name.replace("/", os.sep).replace("\\", os.sep))
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with open(dest, "wb") as fh:
                fh.write(data)
            staged.append((dest, name))

        for key, real, directory, col, row in tiles:
            data = idx.read(key)
            if data is None:
                print("  !! unreadable tile: %s" % real)
                continue
            # normalise onto the path the translate table resolves against
            name = "textures\\Minimap\\" + os.path.basename(real)
            dest = os.path.join(tmp, name.replace("\\", os.sep))
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with open(dest, "wb") as fh:
                fh.write(data)
            staged.append((dest, name))

        trs_path = os.path.join(tmp, "md5translate.trs")
        with open(trs_path, "w", encoding="utf-8", newline="\r\n") as fh:
            fh.write("\n".join(lines) + "\n")
        staged.append((trs_path, TRS_NAME))
        print("\nstaged %d files (%.1f MB)"
              % (len(staged), sum(os.path.getsize(p) for p, _ in staged) / 1048576.0))

        idx.close()

        bak = PATCH_Z + ".bak-extras"
        if not os.path.exists(bak):
            import shutil
            shutil.copyfile(PATCH_Z, bak)
            print("backed up -> %s" % os.path.basename(bak))

        s = storm()
        h = s.open(PATCH_Z)
        for p, name in staged:
            s.add(h, p, name, flags=MPQ_FILE_COMPRESS | MPQ_FILE_REPLACEEXISTING)
        s.close(h)
        print("appended to patch-Z (%.1f MB)" % (os.path.getsize(PATCH_Z) / 1048576.0))

        # ------------------------------------------------ verify
        a2 = MPQArchive(PATCH_Z)
        listed = a2.list_files()
        print("\nverifying ...")
        print("  %d files in archive" % len(listed))
        bad = 0
        import random
        for p, name in random.sample(staged, min(20, len(staged))):
            with open(p, "rb") as fh:
                want = fh.read()
            if a2.read_file(name) != want:
                print("  !! mismatch %s" % name)
                bad += 1
        got_trs = a2.read_file(TRS_NAME).decode("utf-8", "replace")
        for directory in sorted(by_dir):
            tag = "dir: %s" % directory
            if tag not in got_trs:
                print("  !! translate block missing for %s" % directory)
                bad += 1
        a2.close()
        print("  %s" % ("OK - %d spot-checked, all translate blocks present"
                        % min(20, len(staged)) if not bad else "%d PROBLEMS" % bad))
    finally:
        import shutil
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
