"""Scratch-only: put the EoE arena pillars into map 1616's vmaps so mmaps_generator carves them.

The vmaps written here feed ONE navmesh generation run and are never deployed: at runtime the
pillars are gameobjects (spell LoS through the dynamic tree), and the navmesh only has to know
where they stand. Spawns are encoded the way vmap4assembler encodes a placement:
  pos  = (ZEROPOINT - x, ZEROPOINT - y, z)        internal rep, like the Eye's own spawn
  rot  = (0, worldOrientationDeg - 180, 0)        the Eye: world orientation pi <-> stored 0
The model is registered as Nexus_PillarA_nav (no ".m2" in the name), so the generator does not
reverse its winding: build_go_vmo.py keeps the M2's own winding, which is what an extracted M2
ends up with after the extractor's reversal and the generator's reversal back.
"""
import math, os, re, shutil, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vmtile as V  # noqa: E402
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import layout as L  # noqa: E402

work = sys.argv[1]               # nav1616 dir holding vmaps/
sql = sys.argv[2]                # pillars.sql, the spawn rows actually applied
pillar_vmo = sys.argv[3]         # Nexus_PillarA_GO.m2.vmo (same collision the gameobject uses)

ZEROPOINT = 32.0 * 533.3333
SCALE = 2.5
BBOX = (-1.6639766693115234, -1.1887866258621216, -0.05296637490391731,
        1.663975715637207, 1.80129873752594, 5.183651924133301)
NAME = "Nexus_PillarA_nav"

rows = re.findall(r"INSERT INTO centurionworld\.gameobject \(.*?\) VALUES \((?:\d+, )?300405, 1616, 0, 0, 1, 1, "
                  r"([-\d.]+), ([-\d.]+), ([-\d.]+), ([-\d.]+),", open(sql).read())
assert len(rows) == len(L.PILLAR_BEARINGS), rows
shutil.copy(pillar_vmo, os.path.join(work, "vmaps", NAME + ".vmo"))

tile = os.path.join(work, "vmaps", "1616_29_30.vmtile")
tree = os.path.join(work, "vmaps", "1616.vmtree")
spawns = V.read_vmtile(tile)
tiled, (lo, hi, nodes, objs), rest = V.read_vmtree(tree)
assert nodes == [(3 << 30) | 0, len(objs), 0] and objs == list(range(len(objs))), (nodes, objs)

lo, hi = list(lo), list(hi)
for i, (x, y, z, o) in enumerate((float(a) for a in r) for r in rows):
    yaw_deg = (math.degrees(o) - 180.0) % 360.0
    pos = (ZEROPOINT - x, ZEROPOINT - y, z)
    c, s = math.cos(math.radians(yaw_deg)), math.sin(math.radians(yaw_deg))
    corners = []
    for lx in (BBOX[0], BBOX[3]):
        for ly in (BBOX[1], BBOX[4]):
            for lz in (BBOX[2], BBOX[5]):
                corners.append((pos[0] + SCALE * (lx * c - ly * s), pos[1] + SCALE * (lx * s + ly * c), pos[2] + SCALE * lz))
    blo = tuple(min(p[k] for p in corners) for k in range(3))
    bhi = tuple(max(p[k] for p in corners) for k in range(3))
    ref = len(objs)
    spawns.append(dict(flags=V.MOD_HAS_BOUND, adt=0, id=3616101 + i, pos=pos, rot=(0.0, yaw_deg, 0.0), scale=SCALE,
                       bound=(blo, bhi), name=NAME, ref=ref))
    objs.append(ref)
    lo = [min(lo[k], blo[k]) for k in range(3)]
    hi = [max(hi[k], bhi[k]) for k in range(3)]
    print("pillar %d: world (%.2f, %.2f, %.2f) o=%.1f -> internal (%.2f, %.2f) yaw %.1f, ref %d" %
          (i, x, y, z, math.degrees(o), pos[0], pos[1], yaw_deg, ref))

V.write_vmtile(tile, spawns)
V.write_vmtree(tree, tiled, (tuple(lo), tuple(hi), [(3 << 30) | 0, len(objs), 0], objs), rest)
print("tile now", len(V.read_vmtile(tile)), "spawns; tree objects", V.read_vmtree(tree)[1][3])
