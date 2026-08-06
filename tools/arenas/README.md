# Ported arenas

Fourteen arenas taken from the Ascension client data and wired in as
`BATTLEGROUND_CPE` (872) through `BATTLEGROUND_AOA` (885).

Tol'viron (`BATTLEGROUND_TV`, map 980) and Tiger's Peak (`BATTLEGROUND_TTP`,
map 1134) were already here and are untouched. Both were used as controls
throughout, because their coordinates were established independently and any
method that disagreed with them was wrong.

| BG id | Map | Directory | Name |
|---|---|---|---|
| 872 | 982 | coliseumarena | Coliseum of Past Echoes |
| 873 | 983 | nerubianarena | Imperial Arena of Thakraj |
| 874 | 984 | maldraxxuscoliseum | Maldraxxus Coliseum |
| 875 | 985 | nagrandarena2 | Nagrand Arena (Remastered) |
| 876 | 986 | bladesedgearena2b | Blade's Edge Arena (Remastered) |
| 877 | 1007 | karazhanarena | Guardian's Hall |
| 878 | 1008 | ulduararena | Spark of Creator |
| 879 | 1401 | BaradinHoldArena | Baradin Hold Arena |
| 880 | 1402 | obeliskofthestarts | Obelisk of the Stars |
| 881 | 1403 | thetwistingnether | The Twisting Nether |
| 882 | 1504 | BlackrookHoldArena | Black Rook Hold Arena |
| 883 | 1552 | valsharaharena | Ashamane's Fall |
| 884 | 1683 | ulduaroutarena | The Inventor's Library |
| 885 | 1684 | gundrakarena | Amphitheater of Anguish |

Map ids are the ids the source data already used, which is what 980 and 1134
do, so a directory name never has to be reconciled against a map id.

## Shape of the change

**One class, not fourteen.** `BattlegroundCustomArena` serves all of them.
`BattlegroundTV` and `BattlegroundTTP` are each a file pair whose whole content
is four `AddObject` calls; fourteen more would have been fourteen files
differing in twelve numbers, and every coordinate correction would mean a
Jenkins run. Gates and buffs live in `battleground_custom_arena_object` instead,
so a fix is an `UPDATE` plus `.reload battleground_template`.

**Range checks, not id lists.** `IsCustomArena()` and `IsDataDrivenArena()` in
`SharedDefines.h` sit alongside the existing `IsCustomBattleground()`. Every
site that used to name arenas by hand now asks one of those, so adding arena
fifteen is an enum line and SQL — no hunting for switch statements that fail
silently when missed.

## The 8-arena cap

`BattlegroundMgr::GetRandomBG` built its candidate list from
`BattlemasterListEntry::MapID`, which is `int32 MapID[8]` — a fixed-size array
in the client's DBC format. "All Arenas" therefore could not roll a ninth arena
however many were installed. On this realm it was rolling **two**: row 6 listed
only Nagrand and Blade's Edge.

Replaced by `battleground_random_pool`, which has no ceiling and needs no DBC
edit:

```sql
-- take an arena out of rotation
UPDATE battleground_random_pool SET Enabled = 0 WHERE PoolBgTypeId = 6 AND MemberBgTypeId = 879;
-- make one twice as likely
UPDATE battleground_random_pool SET Weight = 2 WHERE PoolBgTypeId = 6 AND MemberBgTypeId = 885;
```

then `.reload battleground_template`. The pool currently holds **22** arenas.

The custom game lobby had a *second* hardcoded list of six; it now reads the
same table, so the two cannot drift.

Both fall back to the DBC array if the table is empty, so a database that has
not run the migration still works.

## WorldStateUI

Two things had to be true, and both are done:

1. **The server must send the world states.** `Player::SendInitWorldStates`
   picks the arena scoreboard by *zone id*, and these arenas report whatever
   zone their source ADTs baked in — Tol'viron's Uldum, and so on. Rather than
   fourteen more `case` labels, a check before the switch asks the battleground
   directly. A missed case here shows no score at all, silently, so the generic
   route is the safer one.
2. **The client must have something to display them with.** Two
   `WorldStateUI.dbc` rows per arena, gated on state `3610`, substituting
   `%3600w` / `%3601w`. Every existing arena has exactly two; these are copied
   from Tiger's Peak's pair. **Client-side — no effect until packed.**

## Where the coordinates came from

Measured from the client terrain, not surveyed in-game.

`adt_probe.py` reads WMO and M2 placements out of the ADTs and converts them to
world space. `wmo_floor.py` then extracts the arena's WMO from the MPQ and reads
its floor height off the mesh — necessary because the placement gives the WMO's
*origin*, and the gap to the floor is not constant (Tiger's Peak: under a yard;
Tol'viron: 22 yards).

Both have `--selftest`, checked against the two live arenas:

- Tol'viron's measured centre lands **1.2 yards** from the midpoint of its real
  start locations; floor within **1.7**.
- Tiger's Peak floor within **2.9**; its `_PLATFORM` WMOs match the shipped
  shadowsight positions to **0.5**.

Run them before trusting any change to this pipeline:

```bash
python adt_probe.py --selftest
python wmo_floor.py --selftest
```

Confidence is recorded per arena at the bottom of the generated world SQL. Two
are weak — **Baradin Hold** and **The Inventor's Library** have no arena-shaped
WMO to lock onto, and their centres are the most plausible structure rather than
a measurement. Expect to move those.

Everything positional lives in SQL — starts in `WorldSafeLocs`, gates in
`battleground_custom_arena_object` — so corrections never need a rebuild.

## Files

```
adt_probe.py         ADT/WDT reader; WMO + M2 placements in world coords
find_arena_wmo.py    picks the arena structure out of each map's WMO list
resolve_hard.py      collision panes / size bands / gate clusters for the awkward maps
wmo_floor.py         floor height from the WMO mesh
gen_arena_sql.py     THE ARENA TABLE. Generates both SQL scripts.
arena_dbc.py         writes the same rows into the binary DBCs
verify_arena_dbc.py  reads them back and checks them
```

`gen_arena_sql.py` is the single source of truth — `arena_dbc.py` imports the
arena table from it, so the SQL mirrors and the binaries cannot disagree about a
coordinate. Edit it, re-run both.

## What has been applied

| | dev | prod |
|---|---|---|
| `lplusdevworld` (templates, pool, gates) | **applied** | not applied |
| `dbc.*_lplus` mirrors | **applied** (shared) | shared — visible on next prod restart |
| `tc-lplus-dev/data/dbc/*.dbc` binaries | **applied** | not applied |
| `itemforge/dbc` client-patch staging | not applied | — |
| terrain (maps/vmaps/mmaps) | **absent** | absent |

The `dbc.*_lplus` mirrors are shared between realms, as the Tanaris handoff
notes. Every id used is new and unused, and backups were taken:

```sql
-- rollback
DELETE FROM dbc.map_lplus              WHERE ID IN (982,983,984,985,986,1007,1008,1401,1402,1403,1504,1552,1683,1684);
DELETE FROM dbc.battlemasterlist_lplus WHERE ID BETWEEN 872 AND 885;
DELETE FROM dbc.pvpdifficulty_lplus    WHERE ID BETWEEN 93000 AND 93223;
DELETE FROM dbc.worldsafelocs_lplus    WHERE ID BETWEEN 52600 AND 52627;
DELETE FROM dbc.worldstateui_lplus     WHERE ID BETWEEN 90100 AND 90127;
-- full snapshots: dbc.<name>_lplus_bak_arenas
-- binaries:       <file>.dbc.bak-arenas, or  python arena_dbc.py --restore <dir>
```

## Still to do

**1. Terrain.** Nothing will load until the 14 maps exist on both sides. None of
it is present yet — `maps/`, `vmaps/` and `mmaps/` have zero files for all 14
ids. The source is the Ascension client data (extracted to
`C:\Ascension\ExtractedMaps\World\Maps\<Directory>\`). Server side, the standard
extractors have to run against a client install containing these maps, landing
in `<server>/data/{maps,vmaps,mmaps}`. mmaps are the long pole and playerbots
need them to path.

Stage and `mv` into place rather than overwriting while the server runs (§1.7 of
the handoff); a restart is needed regardless because tiles are cached.

**2. Client patch.** The map folders and the `WorldStateUI.dbc` rows have to be
packed. Packing MPQs is the user's job, and `publish_patch.py` is outward-facing
— ask first.

**3. Build.** No local compiler; Jenkins builds. Before concluding something did
not work, confirm the symbol is in the built source (§1.6).

**4. Shadowsight buffs.** Deliberately not seeded — the user is filling these in.
An arena with no buff rows simply has no buffs and is otherwise fully playable:

```sql
INSERT INTO battleground_custom_arena_object
  (BgTypeId, GoEntry, X, Y, Z, Orientation, Rotation0, Rotation1, Rotation2, Rotation3, ObjectType, Comment)
VALUES (872, 184663, <x>, <y>, <z>, <o>, 0, 0, 0, 1, 1, 'shadowsight');
```

then `.reload battleground_template`. 184663/184664 are the two Shadow Sight
nodes.

## Verification checklist

- [ ] Jenkins build is green and `BattlegroundCustomArena` is in the built source
- [ ] `.go xyz <x> <y> <z> <mapid>` lands on arena floor, no hole — all 14
- [ ] `.gps` at each start; correct `WorldSafeLocs` 52600-52627 where it is off
- [ ] Queue pops for each arena (if not, suspect **PvpDifficulty** first — it is
      the silent one, and each map needs all 16 brackets)
- [ ] Both teams spawn on their own side and the gates open on the countdown
- [ ] Scoreboard shows "N Players Remaining" for both teams
- [ ] All Arenas rolls arenas beyond the old eight over enough pops
- [ ] Lobby lists all 14 and each starts
- [ ] Bots can path (needs mmaps)
