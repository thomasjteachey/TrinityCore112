# The Eye of Eternity arena's line-of-sight pillars

Three of the Nexus's own pillars (`Nexus_PillarA`, the same `JLO_NEX_*` textures as the
Eye's floor) at scale 2.5 on map 1616, in an equilateral triangle at r 20 round the centre
(`PILLAR_BEARINGS` in `../layout.py`): one corner on the square axis at 315, the other two
at 75 and 195, each 30 degrees off its own team's line to the centre. The triangle is its
own mirror image across the 135/315 axis, the mirror that swaps the teams, and every pillar
faces the centre, so neither side's view differs. (The first layout, 2026-10-15, was a line
through the centre on the square axis; the owner asked for a triangle.)

The spawns only load because map 1616 has a `MapDifficulty` row (760, difficulty 0, the
row stock Dalaran Sewers has; `../eoe_dbc.py`, both realms' `data/dbc`, server only).
ObjectMgr allows a map only the spawn modes MapDifficulty lists, so before that row every
pillar was skipped at startup as "wrong spawn mask ... unsupported difficulty modes".

## Where each piece lives (all applied 2026-10-15)

| piece | where |
|---|---|
| display | GameObjectDisplayInfo **11970** -> `World\Expansion02\Doodads\Nexus\Nexus_PillarA.mdx`, in both realms' `data/dbc`, in `patch-enUS-A` (1.00175) and in `centurion/dbc` |
| spell LoS (server) | `vmaps/Nexus_PillarA_GO.m2.vmo` + a `GameObjectModels.dtree` entry (isWmo 0), both realms, from `tools/tanaris/build_go_vmo.py --doodad` |
| the pillars | `gameobject_template` 300405 "Nexus Pillar" (type 5, size 2.5, addon flags 48) and three `gameobject` rows on map 1616, both worlds, from `gen_pillars_sql.py` |
| bot pathing | map 1616's navmesh tile `mmaps/16163029.mmtile`, regenerated with the pillars carved out (both realms) |

The pillars are gameobjects because that is what blocks a spell: static M2 doodads are
ignored by spell line of sight (`ModelIgnoreFlags::M2`), the dynamic tree is not. A
gameobject is invisible to the navmesh, though, so the navmesh is carved separately.

## The navmesh carve - REDO IT after any regen of map 1616's mmaps

A regen from the terrain alone drops the pillars out of the navmesh without a word, and
bots go back to walking through them. To redo it (Windows, repo build tools):

1. Copy the realm's `maps/1616*.map`, `vmaps/1616.vmtree`, `vmaps/1616_*.vmtile`, the
   two platform models (`Nexus_Raid_Floating_Platform.wmo.vmo`,
   `Nexus_Floating_Platform_01_Small.wmo.vmo`) and `dbc/LiquidType.dbc` into a scratch
   work dir. These vmaps are a scratch copy and are never deployed.
2. `python build_nav_vmo.py Nexus_PillarA.m2 Nexus_PillarA_nav.vmo 2.5` (the M2 comes out
   of lichking.MPQ). This is the pillar's collision plus horizontal plugs every 1.2 yd.
   Without them Recast leaves a walkable floor island inside each hollow pillar.
3. `python add_nav_pillars.py <work> <pillars.sql> Nexus_PillarA_nav.vmo` adds the three
   spawns to `1616_29_30.vmtile` and the tree, at the positions the SQL applied.
4. In `<work>`: `Build\bin\RelWithDebInfo\mmaps_generator.exe 1616`. The triangle carve
   was built with the realm's own generator instead (`tc-centurion/bin/mmaps_generator
   1616 --allowDebug` in a server dir holding these vmaps, `maps/` and `dbc/`), which has
   the four-digit map id fix the local Aug-15 build lacks.
5. `python navcheck.py <old.mmtile> <new.mmtile>`: the pillar middles go off the mesh,
   while the landings, the buffs and the r 12 ring stay on it, and the old line's centre
   and 135 spots are floor again. The floor must stay one island (two polys on top of a
   pillar's capital, 10 yd up and unconnected, are expected).

Before trusting a run, regenerate WITHOUT step 3 and compare with the uncarved tile: only
the BV-tree section may differ (qsort tie order between builds). `dtile.py a b` prints the
differing bytes per section. On 2026-10-15 everything else was byte-identical, and the
realm's generator reproduced the uncarved tile byte for byte, BV tree included.

## Files

- `gen_pillars_sql.py`: the template and spawn SQL for both worlds, from `../layout.py`
- `add_godi_row.py`: appends one GameObjectDisplayInfo row, refuses an existing id, and
  checks that every old record and string survives byte for byte
- `build_nav_vmo.py`, `add_nav_pillars.py`, `vmtile.py`, `dtile.py`, `navcheck.py`: the carve
