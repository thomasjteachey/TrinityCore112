"""Work out every client file the ported arenas actually need.

Shipping World/Maps alone gives a client that loads the terrain and then draws
nothing standing on it: the ADTs only *reference* their models and textures by
path. So the reference graph has to be walked and closed over.

    ADT   -- MTEX -> .blp textures
          -- MMDX -> .m2 models      -> embedded .blp names, plus .skin files
          -- MWMO -> .wmo buildings  -> MOTX .blp, MODN .m2, and _NNN.wmo groups

Anything already present in the target client is dropped, so only the genuinely
missing files get packed.

Usage:
    python deps.py --report                 # what is referenced, and how big
    python deps.py --have have.txt --plan plan.json
        --have  a listing of what the client already has, one path per line
        --plan  written out for pack_patch.py to consume
"""

import argparse
import json
import os
import struct
import sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
SCRATCH = (r"C:\Users\broki\AppData\Local\Temp\claude"
           r"\C--Ascension-Launcher-resources-ascension-live"
           r"\b4c02561-8768-40b5-abb1-376e8e1417b6\scratchpad")
sys.path.insert(0, HERE)
sys.path.insert(0, SCRATCH)

from mpq import MPQArchive          # noqa: E402
from adt_probe import iter_chunks, find_chunk, split_strings  # noqa: E402
from gen_arena_sql import ARENAS    # noqa: E402

DATA = r"C:\Ascension\Launcher\resources\ascension-live\Data"
MAPS = r"C:\Ascension\ExtractedMaps\World\Maps"

BLIZZ = {"common.mpq", "common-2.mpq", "expansion.mpq", "lichking.mpq",
         "patch.mpq", "patch-2.mpq", "patch-3.mpq"}


# ------------------------------------------------------------------ MPQ index
class Index:
    """path (upper, backslashes) -> (archive, real name, size)."""

    def __init__(self):
        self.by_path = {}
        self._open = {}
        seen = set()
        names = sorted(os.listdir(DATA))
        for fn in names:
            if not fn.lower().endswith(".mpq"):
                continue
            if fn.lower() in seen:
                continue
            seen.add(fn.lower())
            a = MPQArchive(os.path.join(DATA, fn))
            for f in a.list_files():
                info = a.file_info(f)
                # later archives win, matching client load order
                self.by_path[f.upper().replace("/", "\\")] = (fn, f, info["size"] if info else 0)
            a.close()

    def get(self, path):
        return self.by_path.get(path.upper().replace("/", "\\"))

    def read(self, path):
        hit = self.get(path)
        if not hit:
            return None
        arch, real, _ = hit
        if arch not in self._open:
            self._open[arch] = MPQArchive(os.path.join(DATA, arch))
        try:
            return self._open[arch].read_file(real)
        except Exception:
            return None

    def close(self):
        for a in self._open.values():
            a.close()


# ----------------------------------------------------------------- extractors
def adt_refs(path):
    with open(path, "rb") as fh:
        data = fh.read()
    out = {"blp": [], "m2": [], "wmo": []}
    for magic, key in (("MTEX", "blp"), ("MMDX", "m2"), ("MWMO", "wmo")):
        off, size = find_chunk(data, magic)
        if off is not None:
            out[key] = split_strings(data[off:off + size])
    return out


def wmo_refs(blob):
    """Textures and doodads from a WMO root, plus its group count."""
    tex, doodads, ngroups = [], [], 0
    off, size = find_chunk(blob, "MOTX")
    if off is not None:
        tex = split_strings(blob[off:off + size])
    off, size = find_chunk(blob, "MODN")
    if off is not None:
        doodads = split_strings(blob[off:off + size])
    off, size = find_chunk(blob, "MOHD")
    if off is not None and size >= 8:
        ngroups = struct.unpack_from("<I", blob, off + 4)[0]
    return tex, doodads, ngroups


def m2_refs(blob):
    """Texture filenames embedded in an M2 (type 0 == literal path)."""
    out = []
    if len(blob) < 0x150 or blob[:4] not in (b"MD20", b"MD21"):
        return out
    try:
        n_tex, ofs_tex = struct.unpack_from("<II", blob, 0x50)
        for i in range(min(n_tex, 512)):
            base = ofs_tex + i * 16
            if base + 16 > len(blob):
                break
            ttype, tflags, length, ofs = struct.unpack_from("<IIII", blob, base)
            if ttype == 0 and length > 1 and ofs + length <= len(blob):
                name = blob[ofs:ofs + length - 1].decode("utf-8", "replace")
                if name:
                    out.append(name)
    except Exception:
        pass
    return out


# ---------------------------------------------------------------------- walk
def walk():
    idx = Index()
    print("indexed %d client files" % len(idx.by_path), flush=True)

    wanted = {}            # normalised path -> size
    missing = []           # referenced but not in any archive
    per_arena = defaultdict(lambda: {"files": 0, "bytes": 0})

    def add(path, arena):
        if not path:
            return
        key = path.upper().replace("/", "\\")
        if key in wanted:
            return
        hit = idx.get(key)
        if not hit:
            missing.append(path)
            return
        wanted[key] = hit[2]
        per_arena[arena]["files"] += 1
        per_arena[arena]["bytes"] += hit[2]
        return hit

    seen_wmo, seen_m2 = set(), set()

    for bg, mid, directory, name, cx, cy, cz, conf, ev in ARENAS:
        d = os.path.join(MAPS, directory)
        if not os.path.isdir(d):
            print("!! missing map dir %s" % d)
            continue

        wmo_q, m2_q = [], []
        for fn in sorted(os.listdir(d)):
            if not fn.lower().endswith(".adt"):
                continue
            refs = adt_refs(os.path.join(d, fn))
            for t in refs["blp"]:
                add(t, directory)
                # Every ground texture has a `_s.blp` specular companion that
                # the client loads with it and that MTEX never names. Leave them
                # out and the terrain renders bright green -- which is exactly
                # what happened to maps 985, 986, 1504 and 1552 on the first
                # pass. add() ignores anything that does not exist, so naming a
                # companion that was never authored is harmless.
                if t.lower().endswith(".blp"):
                    add(t[:-4] + "_s.blp", directory)
            for m in refs["m2"]:
                m2_q.append(m)
            for w in refs["wmo"]:
                wmo_q.append(w)

        # WMOs: root, its groups, its textures, its doodads
        for w in wmo_q:
            key = w.upper().replace("/", "\\")
            if key in seen_wmo:
                continue
            seen_wmo.add(key)
            hit = add(w, directory)
            blob = idx.read(w)
            if not blob:
                continue
            tex, doodads, ngroups = wmo_refs(blob)
            for t in tex:
                add(t, directory)
            for dd in doodads:
                m2_q.append(dd)
            stem = w.rsplit(".", 1)[0]
            for g in range(ngroups):
                add("%s_%03d.wmo" % (stem, g), directory)

        # M2s: the model, its .skin files, its textures
        for m in m2_q:
            key = m.upper().replace("/", "\\")
            if key in seen_m2:
                continue
            seen_m2.add(key)
            # ADTs and WMOs name doodads .mdx/.mdl; on disk they are .m2
            cand = m
            if cand.lower().endswith((".mdx", ".mdl")):
                cand = cand[:-4] + ".m2"
            add(cand, directory)
            stem = cand.rsplit(".", 1)[0]
            for s in range(4):
                if idx.get("%s%02d.skin" % (stem, s)):
                    add("%s%02d.skin" % (stem, s), directory)
            blob = idx.read(cand)
            if blob:
                for t in m2_refs(blob):
                    add(t, directory)

    idx.close()
    return wanted, missing, per_arena


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--report", action="store_true")
    ap.add_argument("--have")
    ap.add_argument("--plan")
    args = ap.parse_args()

    wanted, missing, per_arena = walk()

    have = set()
    if args.have:
        with open(args.have, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                line = line.strip().replace("/", "\\")
                if line:
                    have.add(line.upper())

    need = {p: s for p, s in wanted.items() if p not in have}

    by_ext = defaultdict(lambda: [0, 0])
    for p, s in need.items():
        e = os.path.splitext(p)[1].lower()
        by_ext[e][0] += 1
        by_ext[e][1] += s

    print("\n=== referenced by the 14 arenas ===")
    print("  %d files, %.1f MB" % (len(wanted), sum(wanted.values()) / 1048576.0))
    if have:
        print("  client already has %d of them" % (len(wanted) - len(need)))
    print("\n=== to pack ===")
    print("  %d files, %.1f MB" % (len(need), sum(need.values()) / 1048576.0))
    for e, (n, b) in sorted(by_ext.items(), key=lambda kv: -kv[1][1]):
        print("    %-6s %6d  %9.1f MB" % (e or "(none)", n, b / 1048576.0))

    if missing:
        uniq = sorted(set(m.upper() for m in missing))
        print("\n=== referenced but not in any archive: %d ===" % len(uniq))
        for m in uniq[:15]:
            print("    %s" % m)
        if len(uniq) > 15:
            print("    ... and %d more" % (len(uniq) - 15))

    if args.plan:
        with open(args.plan, "w", encoding="utf-8") as fh:
            json.dump({"files": sorted(need), "bytes": sum(need.values())}, fh, indent=1)
        print("\nwrote %s" % args.plan)


if __name__ == "__main__":
    main()
