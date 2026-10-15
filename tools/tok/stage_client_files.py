"""Stage every client file Temple of Kotmogu needs, out of the Ascension data.

    python stage_client_files.py <have-index.json> <out-dir> [--report]

<have-index.json> lists what the target client already has: a JSON object
whose keys are archive paths (upper case, backslashes), as written by a scan of
the client's MPQs. Only files missing from it are staged, except the map files,
which are always staged. Optional HD packs must already be left out of it, since
a player without them is the one who needs the file.

What gets walked (the ADTs only *reference* their art by path):

    ADT  MTEX -> textures (+ the _s / _h companions the client loads unnamed)
         MMDX -> M2 doodads   -> skins, embedded textures, .anim, particle models
         MWMO -> WMOs         -> groups, MOTX textures, MODN doodads, MOSB skybox
    plus the art the battleground itself uses: the orb and pedestal models, the
    four light columns, the orb icons, the loading screen, the world map and the
    minimap.

Writes <out-dir>/<archive path> for each file, <out-dir>/_manifest.json, and
<out-dir>/_md5translate_ValleyOfPower.txt - the block to append to the client's
textures\\Minimap\\md5translate.trs, which a stock client needs before it will
draw a single minimap tile.
"""

import argparse
import json
import os
import struct
import sys
from collections import defaultdict

ASC_TOOLS = r"C:\Ascension\ExtractedMaps\_tools"
ASC_DATA = r"C:\Ascension\Launcher\resources\ascension-live\Data"
MAP_DIR = "ValleyOfPower"

sys.path.insert(0, ASC_TOOLS)
from mpq import MPQArchive  # noqa: E402

EXTRA_MODELS = [
    "World\\Expansion04\\Doodads\\Murderball\\murderball_pedestal_ball.m2",  # the orb GO
    "World\\Expansion04\\Doodads\\Murderball\\murderball_ball_fx.m2",        # orb over a carrier
    "Spells\\auracyanverytall_v2.m2",
    "Spells\\aurapurpleverytall_v2.m2",
    "Spells\\auragreenverytall_v2.m2",
    "Spells\\auraorangeverytall_v2.m2",
]
EXTRA_FILES = [
    "Interface\\Glues\\LoadingScreens\\LoadScreenKotmoguScenario.blp",
    "Interface\\Glues\\LoadingScreens\\LoadScreenKotmoguScenarioWide.blp",
    "Interface\\Icons\\achievement_battleground_templeofkotmogu_02_cyan.blp",
    "Interface\\Icons\\achievement_battleground_templeofkotmogu_02_green.blp",
    "Interface\\Icons\\achievement_battleground_templeofkotmogu_02_orange.blp",
    "Interface\\Icons\\achievement_battleground_templeofkotmogu_02_purple.blp",
] + ["Interface\\WorldMap\\TempleofKotmogu\\TempleofKotmogu%d.blp" % i for i in range(1, 13)]


def norm(path):
    return path.upper().replace("/", "\\")


class AscIndex:
    """Archive path -> (archive, stored name). Later archive (by name) wins, the
    order the arena port used and verified in game."""

    def __init__(self):
        self.by = {}
        self.handles = {}
        for fn in sorted(os.listdir(ASC_DATA)):
            if not fn.lower().endswith(".mpq"):
                continue
            archive = MPQArchive(os.path.join(ASC_DATA, fn))
            for name in archive.list_files():
                self.by[norm(name)] = (fn, name)
            archive.close()

    def get(self, path):
        return self.by.get(norm(path))

    def read(self, path):
        hit = self.get(path)
        if not hit:
            return None
        if hit[0] not in self.handles:
            self.handles[hit[0]] = MPQArchive(os.path.join(ASC_DATA, hit[0]))
        return self.handles[hit[0]].read_file(hit[1])


def chunks(data, start=0, end=None):
    end = len(data) if end is None else end
    pos = start
    while pos + 8 <= end:
        magic = data[pos:pos + 4][::-1].decode("ascii", "replace")
        size = struct.unpack_from("<I", data, pos + 4)[0]
        if pos + 8 + size > end:
            break
        yield magic, pos + 8, size
        pos += 8 + size


def strings(blob):
    return [s.decode("utf-8", "replace") for s in blob.split(b"\0") if s]


def m2_refs(blob):
    """Textures, external animations and particle models an M2 (v264) names."""
    out = {"tex": [], "anims": [], "models": [], "views": 1}
    if not blob or blob[:4] != b"MD20":
        return out
    version = struct.unpack_from("<I", blob, 4)[0]
    if version != 264:
        raise SystemExit("M2 version %d is not loadable by a 3.3.5 client" % version)
    out["views"] = max(1, struct.unpack_from("<I", blob, 0x44)[0])
    count, offset = struct.unpack_from("<II", blob, 0x50)
    for i in range(min(count, 512)):
        kind, _flags, length, name_ofs = struct.unpack_from("<IIII", blob, offset + i * 16)
        if kind == 0 and length > 1:
            out["tex"].append(blob[name_ofs:name_ofs + length - 1].decode("utf-8", "replace"))
    count, offset = struct.unpack_from("<II", blob, 0x1C)
    for i in range(min(count, 2000)):
        base = offset + i * 64
        anim_id, variation = struct.unpack_from("<HH", blob, base)
        if not struct.unpack_from("<I", blob, base + 12)[0] & 0x20:
            out["anims"].append((anim_id, variation))
    count, offset = struct.unpack_from("<II", blob, 0x128)
    for i in range(min(count, 256)):
        base = offset + i * 476
        for field in (0x18, 0x20):
            length, name_ofs = struct.unpack_from("<II", blob, base + field)
            if 1 < length < 260:
                out["models"].append(blob[name_ofs:name_ofs + length].split(b"\0")[0].decode("utf-8", "replace"))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("have")
    ap.add_argument("out")
    ap.add_argument("--report", action="store_true", help="list, do not write")
    args = ap.parse_args()

    have = set(json.load(open(args.have)))
    idx = AscIndex()

    staged = {}              # key -> (archive, stored name, why)
    missing = []
    m2_queue, wmo_queue = [], []

    def want(path, why, always=False):
        key = norm(path)
        if key in staged or (key in have and not always):
            return
        hit = idx.get(key)
        if not hit:
            missing.append((key, why))
            return
        staged[key] = (hit[0], hit[1], why)

    map_prefix = "WORLD\\MAPS\\%s\\" % MAP_DIR.upper()
    map_files = sorted(k for k in idx.by if k.startswith(map_prefix))
    for key in map_files:
        want(key, "map", always=True)
        data = idx.read(key)
        if key.endswith(".WDT"):
            for magic, off, size in chunks(data):
                if magic == "MWMO":
                    wmo_queue += [(w, key) for w in strings(data[off:off + size])]
        if not key.endswith(".ADT"):
            continue
        for magic, off, size in chunks(data):
            if magic == "MTEX":
                for tex in strings(data[off:off + size]):
                    want(tex, key)
                    stem = tex[:-4]
                    for companion in (stem + "_s.blp", stem + "_h.blp"):
                        if idx.get(companion):
                            want(companion, key)
            elif magic == "MMDX":
                m2_queue += [(m, key) for m in strings(data[off:off + size])]
            elif magic == "MWMO":
                wmo_queue += [(w, key) for w in strings(data[off:off + size])]

    m2_queue += [(m, "battleground") for m in EXTRA_MODELS]
    for path in EXTRA_FILES:
        want(path, "battleground")

    seen = set()
    while wmo_queue:
        wmo, why = wmo_queue.pop()
        if norm(wmo) in seen:
            continue
        seen.add(norm(wmo))
        want(wmo, why)
        blob = idx.read(wmo)
        if not blob:
            continue
        groups = 0
        for magic, off, size in chunks(blob):
            if magic == "MOTX":
                for tex in strings(blob[off:off + size]):
                    if idx.get(tex):
                        want(tex, wmo)
            elif magic in ("MODN", "MOSB"):
                m2_queue += [(m, wmo) for m in strings(blob[off:off + size])]
            elif magic == "MOHD":
                groups = struct.unpack_from("<I", blob, off + 4)[0]
        stem = wmo.rsplit(".", 1)[0]
        for group in range(groups):
            want("%s_%03d.wmo" % (stem, group), wmo)

    while m2_queue:
        model, why = m2_queue.pop()
        if model.lower().endswith((".mdx", ".mdl")):
            model = model[:-4] + ".m2"
        if norm(model) in seen:
            continue
        seen.add(norm(model))
        want(model, why)
        if norm(model) in have:
            continue
        refs = m2_refs(idx.read(model))
        stem = model.rsplit(".", 1)[0]
        for view in range(refs["views"]):
            want("%s%02d.skin" % (stem, view), model)
        for tex in refs["tex"]:
            want(tex, model)
        for anim_id, variation in refs["anims"]:
            if idx.get("%s%04d-%02d.anim" % (stem, anim_id, variation)):
                want("%s%04d-%02d.anim" % (stem, anim_id, variation), model)
        m2_queue += [(m, model) for m in refs["models"]]

    # Minimap tiles: Ascension keeps them as Textures\Minimap\<Dir>\mapX_Y.blp.
    # They ship flat, the way the ported arenas' do, under an md5translate entry.
    minimap = []
    prefix = "TEXTURES\\MINIMAP\\%s\\" % MAP_DIR.upper()
    for key in sorted(k for k in idx.by if k.startswith(prefix)):
        base = os.path.basename(key)                    # MAP27_28.BLP
        col, row = base[3:-4].split("_")
        flat = "%s_%d_%d.blp" % (MAP_DIR.lower(), int(col), int(row))
        staged["TEXTURES\\MINIMAP\\" + flat.upper()] = (idx.get(key)[0], idx.get(key)[1], "minimap", flat)
        minimap.append((int(col), int(row), flat))

    by_ext = defaultdict(lambda: [0, 0])
    total = 0
    sizes = {}
    for key, entry in staged.items():
        archive = MPQArchive(os.path.join(ASC_DATA, entry[0]))
        info = archive.file_info(entry[1])
        archive.close()
        size = info["size"] if info else 0
        sizes[key] = size
        total += size
        ext = os.path.splitext(key)[1].lower()
        by_ext[ext][0] += 1
        by_ext[ext][1] += size

    print("%d files, %.1f MB" % (len(staged), total / 1048576.0))
    for ext, (n, size) in sorted(by_ext.items(), key=lambda kv: -kv[1][1]):
        print("    %-6s %5d  %8.1f MB" % (ext, n, size / 1048576.0))
    real_missing = sorted(set(k for k, why in missing))
    print("referenced but in no archive: %d (WMO _LOD textures a 3.3.5 client never loads, among them)" % len(real_missing))
    for key in real_missing:
        if "_LOD" not in key:
            print("    !! %s" % key)

    if args.report:
        return

    os.makedirs(args.out, exist_ok=True)
    manifest = {}
    for key, entry in sorted(staged.items()):
        archive_name, stored = entry[0], entry[1]
        target = entry[3] if len(entry) > 3 else None
        rel = ("Textures\\Minimap\\" + target) if target else stored
        data = idx.read(stored) if not target else idx.handles.setdefault(
            archive_name, MPQArchive(os.path.join(ASC_DATA, archive_name))).read_file(stored)
        path = os.path.join(args.out, *rel.replace("/", "\\").split("\\"))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as fh:
            fh.write(data)
        manifest[rel] = {"from": archive_name, "bytes": len(data)}

    lines = ["dir: %s" % MAP_DIR]
    for col, row, flat in sorted(minimap):
        lines.append("%s\\map%d_%d.blp\t%s" % (MAP_DIR, col, row, flat))
    with open(os.path.join(args.out, "_md5translate_%s.txt" % MAP_DIR), "w", newline="\r\n") as fh:
        fh.write("\n".join(lines) + "\n")
    with open(os.path.join(args.out, "_manifest.json"), "w") as fh:
        json.dump(manifest, fh, indent=1, sort_keys=True)
    print("wrote %d files under %s" % (len(manifest), args.out))


if __name__ == "__main__":
    main()
