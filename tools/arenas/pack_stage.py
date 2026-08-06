"""Extract everything the arenas need into a staging tree, ready to pack.

Two sets of files:
  * the terrain itself -- World/Maps/<Directory>/*.wdt|.adt|.wdl
  * the closure deps.py worked out -- the WMOs, models, textures and .skin files
    those ADTs reference, minus anything the client already has

Names are taken from the source archive's listfile rather than from the
uppercased lookup key, so what lands in the patch reads the way the rest of the
client does. MPQ lookups hash the uppercased name, so this is cosmetic -- but a
patch listing is something people read.

Usage:
    python pack_stage.py --plan pack_plan.json --out <dir> [--zip <file>]
"""

import argparse
import json
import os
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
SCRATCH = (r"C:\Users\broki\AppData\Local\Temp\claude"
           r"\C--Ascension-Launcher-resources-ascension-live"
           r"\b4c02561-8768-40b5-abb1-376e8e1417b6\scratchpad")
sys.path.insert(0, HERE)
sys.path.insert(0, SCRATCH)

from deps import Index          # noqa: E402
from gen_arena_sql import ARENAS  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--plan", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--zip")
    args = ap.parse_args()

    with open(args.plan, encoding="utf-8") as fh:
        plan = json.load(fh)

    print("indexing client archives ...", flush=True)
    idx = Index()

    # the terrain: every file under each arena's map directory
    terrain = []
    for bg, mid, directory, name, cx, cy, cz, conf, ev in ARENAS:
        prefix = ("WORLD\\MAPS\\" + directory + "\\").upper()
        for key in idx.by_path:
            if key.startswith(prefix):
                terrain.append(key)
    print("terrain files: %d" % len(terrain))

    todo = list(dict.fromkeys(terrain + plan["files"]))
    print("total to stage: %d" % len(todo))

    os.makedirs(args.out, exist_ok=True)
    written, failed, total = 0, [], 0
    for i, key in enumerate(todo):
        hit = idx.get(key)
        if not hit:
            failed.append(key)
            continue
        _arch, realname, _size = hit
        data = idx.read(key)
        if data is None:
            failed.append(key)
            continue
        dest = os.path.join(args.out, realname.replace("/", os.sep).replace("\\", os.sep))
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        with open(dest, "wb") as fh:
            fh.write(data)
        written += 1
        total += len(data)
        if written % 500 == 0:
            print("  %5d/%d  %7.1f MB" % (written, len(todo), total / 1048576.0), flush=True)

    idx.close()
    print("\nstaged %d files, %.1f MB -> %s" % (written, total / 1048576.0, args.out))
    if failed:
        print("FAILED %d:" % len(failed))
        for f in failed[:10]:
            print("   %s" % f)

    if args.zip:
        print("zipping ...", flush=True)
        n = 0
        with zipfile.ZipFile(args.zip, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
            for root, _d, fs in os.walk(args.out):
                for f in fs:
                    p = os.path.join(root, f)
                    z.write(p, os.path.relpath(p, args.out))
                    n += 1
                    if n % 1000 == 0:
                        print("  %d ..." % n, flush=True)
        print("wrote %s (%.1f MB)" % (args.zip, os.path.getsize(args.zip) / 1048576.0))


if __name__ == "__main__":
    main()
