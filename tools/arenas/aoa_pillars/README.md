# The Amphitheater of Anguish's line-of-sight pillars

Three Drakkari columns in the pit of the Amphitheater of Anguish (BG 885, map 1684):
`zuldrak_archpost_05`, the column `zd_coliseum.wmo` itself is built with (14 of them at scale
1), at scale 1.3 - a 6.5 yd square, 15 yd tall. Same pipeline as the Eye of Eternity's
(`tools/eoe_arena/pillars/`), whose helpers `aoa_pillars.py` imports.

## Layout

In the team-axis frame (u from the Horde start to the Alliance start, v square to it, origin
midway between the starts): an equilateral triangle of side 26 yd, one column on the team axis
in front of each team's alcove (u +-13, v 0) and the third on the long axis at v +22.5, between
the centre and Shadow Sight 2. Every column faces the triangle's centroid.

Why that shape: the pit is a mirror image of itself across the v axis (its outline to within
1.6 yd), and not across the team axis or by a half turn - it runs 50 yd toward Shadow Sight 2
and only 35 toward Shadow Sight 1. A fair layout therefore has to be symmetric across v, which
this triangle is, and its centroid (v 7.5) is the middle of the open floor (v -30 to +48). The
two columns on the team axis also mean the starts open out of each other's sight.

## What was applied (2026-10-15)

| piece | where |
|---|---|
| GameObjectDisplayInfo 11971 -> `ZulDrak_ArchPost_05.mdx` | both realms' `data/dbc`, `patch-enUS-A` 1.00176, `centurion/dbc` |
| MapDifficulty 761 (1684, difficulty 0) | both realms' `data/dbc`, `centurion/dbc` (server only: without it ObjectMgr skips every static spawn on 1684 as "wrong spawn mask") |
| `vmaps/ZulDrak_ArchPost_05_GO.m2.vmo` + `GameObjectModels.dtree` entry | both realms (`tools/tanaris/build_go_vmo.py --doodad`) - spell line of sight |
| gameobject_template 300406 "Drakkari Pillar" + three spawns | both worlds (`aoa_pillars.py sql`) |
| map 1684's navmesh tile `16842137.mmtile`, carved | both realms; CenturionDev also took live's other eight 1684 tiles (its own came from an older generator run; every other 1684 input was identical) |

All of it is read at startup: the columns appear at each realm's next restart.

## The navmesh carve - REDO IT after any regen of map 1684's mmaps

1. Scratch work dir: copies of the realm's `vmaps/1684.vmtree` and `vmaps/1684_37_21.vmtile`
   (that tile holds the whole pit); everything else is the realm's own (symlinks are fine).
2. `python ../../eoe_arena/pillars/build_nav_vmo.py zuldrak_archpost_05.m2 <work>/vmaps/ZulDrak_ArchPost_05_nav.vmo 1.3`
   (the M2 is in lichking.MPQ): the column's collision box plus horizontal plugs.
3. `python aoa_pillars.py carve <work>` adds the three columns to the scratch tile and grows the
   tree's object list (mmaps_generator collects every loaded instance and never walks the tree).
4. In `<work>` on the server: `~/wow/servers/tc-centurion/bin/mmaps_generator 1684 --tile 37,21 --allowDebug`.
   Run it once on the untouched vmaps first: on 2026-10-15 that reproduced the live tile byte for byte.
5. `python aoa_pillars.py check <old.mmtile> <new.mmtile>`: column middles and 2 yd rings off
   the mesh; 6.5 yd rings, both starts, the centre and both Shadow Sights on it (56/56). Both
   starts, the centre and the buffs must stay one connected area.

## Files

- `aoa_pillars.py` - layout, SQL, the two DBC rows, the carve and the check
- `aoa_patch.py` - server side: the display row into patch-enUS-A through `eoe_patch.py`
