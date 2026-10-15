# The Amphitheater of Anguish's columns and walls

Static scenery in the pit of the Amphitheater of Anguish (BG 885, map 1684), same pipeline as
the Eye of Eternity's pillars (`tools/eoe_arena/pillars/`), whose helpers `aoa_pillars.py`
imports:

- two Drakkari columns that break line of sight - `zuldrak_archpost_05`, the column
  `zd_coliseum.wmo` itself is built with (14 of them at scale 1), at scale 1.3: a 6.5 yd square,
  15 yd tall;
- six Drakkari walls that close every way out of the pit - `zuldrak_ruin_15`, a block of the
  troll masonry the coliseum is built of (the yellow frieze), at scale 0.42: 10.9 x 2.6 x 5.8 yd,
  plain box collision, nothing on it to climb.

## The columns

In the team-axis frame (u from the Horde start to the Alliance start, v square to it, origin
midway between the starts): one column on the team axis in front of each team's alcove (u +-13,
v 0), so the starts open out of each other's sight; both face the middle of the open floor
(u 0, v 7.5 - the floor runs v -30 to +48).

The pit is a mirror image of itself across the v axis (its outline to within 1.6 yd), and not
across the team axis or by a half turn - it runs 50 yd toward Shadow Sight 2 and only 35 toward
Shadow Sight 1. A fair layout therefore has to be symmetric across v, which this pair is. A third
column on the long axis (v +22.5) stood for one build; the owner wanted only two.

## The walls

The pit has six ways out, and the bounds outline (`battleground_bounds` 885) teleports anyone
who takes one back in:

- **The stairs.** Each team's alcove has a ramp either side: 7.9 yd wide, cut into the inside
  of the ring wall, climbing 0.8 yd per yard from the pit floor (273.5) to the top of the ring
  (286.5), with the wall on one side and an open edge over the pit on the other. The start pens'
  stair doors (`battleground_custom_arena_object`, "stairs-left/right gate", left as they are)
  only part each ramp's foot from the alcove. The foot is flush with the pit floor on its open
  side, so those doors never kept anyone in the pit off the stairs. A wall stands across each
  ramp where its surface reaches 277.5, about 5 yd up. From there on the open edge is 4+ yd
  above the pit floor, out of reach of a jump (apex about 1.6 yd). Its +X end is 0.15 yd past
  the open edge and 2.85 yd of it is buried in the ring wall; its bottom is 0.1 below the ramp at
  its downhill face.
- **The tunnels.** The pit's short end (v -32) has a corridor under the ring at each corner,
  both running east and down. A wall plugs each along the line of the east block's west face,
  from its corner to the pit's long wall (9.0 and 8.8 yd). Its front face is flush with that
  line and its ends are 0.9-1.0 yd into the walls.

The first try put the Black Temple gate (display 7320) across the ramps; the owner found it
jarring. The positions were measured from the server collision on 2026-10-15: each ramp's
steepest-ascent axis and a cross-section every 0.05 yd, and the corridor mouths from the navmesh
edges that cross the bounds outline. The two teams' walls are each other's images under the
pit's mirror, to within 0.5 yd.

## Guids are fixed - never AUTO_INCREMENT

| rows | guids |
|---|---|
| columns: alliance side, horde side | 5530584, 5530585 |
| walls: Alliance stairs left/right, Horde stairs left/right, tunnels NE/SE | 5530570-5530575 |

A worldserver numbers the spawns it creates itself from MAX(guid)+1 at startup. Outdoor PvP's
objects are among them, and stock `OPvPCapturePoint::DelObject` passes each one to
`GameObject::DeleteFromDB`, which runs a `DELETE FROM gameobject` by guid at shutdown. A row
inserted above the running server's MAX therefore lands on one of those numbers and is deleted at
the next restart. On 2026-10-15 the Hellfire towers' three flags (counter +1, +3, +5) took the
columns 5530584 and 5530586 and the Eye of Eternity's 5530582.

A new row is safe when its guid was at or below MAX(guid) when the running server started (live
started 13:47 at 5530585). It is also safe when the guid is so far above MAX that the server
cannot count up to it before its next restart: CenturionDev started at 10:16 near 5331214, about
200,000 short. After a restart that loads the row, MAX includes it and it is safe for good.
`aoa_pillars.py sql` deletes by entry and re-inserts these exact guids.

## What was applied (2026-10-15)

| piece | where |
|---|---|
| GameObjectDisplayInfo 11971 -> `ZulDrak_ArchPost_05.mdx`, 11972 -> `ZulDrak_Ruin_15.mdx` | both realms' `data/dbc`, `patch-enUS-A` (11971 in 1.00176, 11972 in 1.00178), `centurion/dbc` |
| MapDifficulty 761 (1684, difficulty 0) | both realms' `data/dbc`, `centurion/dbc` (server only: without it ObjectMgr skips every static spawn on 1684 as "wrong spawn mask") |
| `vmaps/ZulDrak_ArchPost_05_GO.m2.vmo`, `vmaps/ZulDrak_Ruin_15_GO.m2.vmo` + their `GameObjectModels.dtree` entries | both realms (`tools/tanaris/build_go_vmo.py --doodad`) - spell line of sight, Blink, charges |
| gameobject_template 300406 "Drakkari Pillar" + two spawns, 300407 "Drakkari Wall" + six spawns | both worlds (`aoa_pillars.py sql`) |
| map 1684's navmesh tile `16842137.mmtile`, columns and walls carved | both realms (backups `.bak-aoawalls-20261015` = BT gates, `.bak-stairgates-20261015` = columns only, live `.bak-aoapillars-20261015` = uncarved); CenturionDev also took live's other eight 1684 tiles (its own came from an older generator run; every other 1684 input was identical) |

All of it is read at startup: the spawns appear at each realm's next restart.

## The navmesh carve - REDO IT after any regen of map 1684's mmaps

1. Scratch work dir: copies of the realm's `vmaps/1684.vmtree` and `vmaps/1684_37_21.vmtile`
   (that tile holds the whole pit); everything else is the realm's own (symlinks are fine).
2. Nav models, both from lichking.MPQ, into `<work>/vmaps`:
   `python ../../eoe_arena/pillars/build_nav_vmo.py zuldrak_archpost_05.m2 ZulDrak_ArchPost_05_nav.vmo 1.3`
   and `... zuldrak_ruin_15.m2 ZulDrak_Ruin_15_nav.vmo 0.42` - each model's collision plus
   horizontal plugs every 1.2 yd, or Recast leaves a walkable floor inside.
3. `python aoa_pillars.py carve <work>` adds the two columns and the six walls to the scratch
   tile and grows the tree's object list (mmaps_generator collects every loaded instance and
   never walks the tree). It refuses a tile that already holds them - start again from the
   realm's copies.
4. In `<work>` on the server: `~/wow/servers/tc-centurion/bin/mmaps_generator 1684 --tile 37,21 --allowDebug`.
   It skips a tile whose `.mmtile` already exists in `<work>/mmaps` - delete it first. Run it
   once on the untouched vmaps first: on 2026-10-15 that reproduced the live tile byte for byte.
5. `python aoa_pillars.py check <old.mmtile> <new.mmtile>`:
   - column middles and 2 yd rings off the mesh; 6.5 yd rings, the removed column's spot, both
     starts, the centre and both Shadow Sights on it;
   - each wall's middle off the mesh;
   - one walk from the Alliance start reaches the Horde start, the centre and both Shadow
     Sights, and NOTHING outside the bounds outline (before: 3990 of 4039 polygons; after: 0
     of 58);
   - on each ramp, 2.5 yd below the wall is reachable and 2.5, 4 and 7 yd above it are not; in
     front of each tunnel plug is reachable and 4 yd behind it is not.

   Run it on the uncarved tile against itself to see every check flagged.

## Files

- `aoa_pillars.py` - layout, SQL, the DBC rows, the carve and the check
- `aoa_patch.py` - server side: the display rows into patch-enUS-A through `eoe_patch.py` (pass
  it an ABSOLUTE work dir: smpq runs from a subdirectory of it)
