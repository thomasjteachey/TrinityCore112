# Looking at an arena from above

The tools that brought the last nine arenas back (2026-10-15). They answer three
questions without logging in: what is really there, where can a bot walk, and does
the minimap show it.

All of it is plain Python + numpy + Pillow. Two inputs:

* **the client**, read through its own archive priority (`cl.py`, `WOW_CLIENT_DATA`
  overrides the path) - every file is the copy the game would load;
* **a realm's server data** for the maps in question, copied into `srv/`
  (`ARENA_SRV_DATA` overrides it):

  ```bash
  cd ~/wow/servers/tc-centurion/data && tar czf - mmaps/1403* maps/1403* vmaps/1403* > live.tgz
  ```

  plus, in `srv/vmo/`, every `.vmo`/`.m2` model the map's vmtiles name
  (`vmapread.map_spawns()` lists them).

## The renders

| tool | what it draws |
|---|---|
| `mmcomp.py [bg...]` | the client's own minimap tiles around each arena, team starts marked |
| `render.py` | top-down geometry from the CLIENT files (terrain, WMOs, M2 collision), `mode='collide'` or `'render'` (visible faces) |
| `heightmap.py` | the SERVER's top collision surface (vmaps + `.map` terrain) as a colour relief with labelled heights - walls, pits and platforms are obvious |
| `slice.py` | the same, plus horizontal cuts through the server collision at player height (what actually blocks walking - invisible collision panes included) and the navmesh reachable from the starts |
| `texrender.py` + `m2render.py` | a textured orthographic bird's-eye render of WMOs and their doodad sets, from the client's textures - effectively a screenshot from above |
| `make_tiles.py` | `texrender` at 4x supersampling, cut into the 512x512 DXT1 minimap tiles the client looks up as `textures\Minimap\<dir>_<col>_<row>.blp` |

**Placement transform.** World = MID - (R * local + ipos) for X/Y, with
`ipos = (pos.z, pos.x, pos.y)` from MODF/MDDF and `R = Rz(rot.y) Ry(rot.x) Rx(rot.z)` -
TrinityCore's own vmap convention (`geo.placement_to_world`). The 2026-08 minimap
tool added local X/Y to world X/Y instead, which rotates everything 180 degrees about
the WMO origin; Spark of Creator's minimap shipped that way. Validated against
Tol'viron (rot 0) and Blade's Edge remake (rot -50) whose Blizzard/Ascension tiles
line up with the render.

## The navmesh

`navmesh.py` reads `.mmtile` files (TC stores Recast coordinates as world
`(Y, Z, X)`), links polygons inside a tile, across tile borders and through off-mesh
connections, and BFSes from a start. Use the **detail mesh** height range of a
polygon, not its vertices: a curved floor (the Dalaran pipes) has polygon vertices
3 yards above the surface you stand on.

`fence.py` replays `BattlegroundFence::BuildFence` (the bots-only containment) for a pair
of team starts against the server collision: whether the fence will exist and how many
yards a start may still move outward before the fence refuses the arena. Run it before
moving any arena start in WorldSafeLocs.

`outline.py` turns "the floor reachable from both starts within a height band" into a
simplified polygon for `battleground_bounds.Boundary`. Where the navmesh leaks under
the walls (Amphitheater of Anguish has pit-level tunnels under its ring) take the
region from the top collision surface instead - `heightmap.run()` gives the raster -
which is what the 884 and 885 outlines are.

## What the 2026-10-15 pass found, and how

* **Dalaran Sewers** - `probe.py`'s list of collision surfaces under a point showed
  the 3.0 WMO's holes (ceiling, then nothing until the basin) and the 3.3.5 floor at
  7.11 in the same spots. The server data was rebuilt with the stock extractors
  against a scratch client: hard links to every MPQ plus a `patch-Z.MPQ` holding
  patch.MPQ's three WMO files (`smpq -c -M 1`), then `vmap4extractor -m 617`,
  `vmap4assembler Buildings vmaps -m 617`, and the fixed Linux
  `mmaps_generator 617 --offMeshInput` with the two pipe drops
  (`617 30,29 (x y z) (x y z) 2.0`; the tile pair is tileX,tileY = Y-index,X-index).
* **Dalaran Sewers' gates** - the gate models (`Dalaran_SewerDoor_01.m2`, scale 1.62) are
  1.5 yd slabs that plug each pipe at X 1349-1351.5 (east) and 1232-1234.4 (west).
  Blizzard's spawn points stand on the ARENA side of them, so the starts were moved
  1.5 yd behind each gate. That is as far as they can go: `fence.py` puts the
  bot fence's limit at 67.8 yd from the centre, and the new starts are at 66.6.
* **The Twisting Nether** - `slice.py` over the whole map showed the start alcoves as
  islands and the Legion platform 40 yards east with its own navmesh.
* **Buff spots** - points on the perpendicular bisector of the two starts (so equally
  far from both), kept only if on reachable navmesh, inside the outline, and at least a
  few yards from any wall cut.
