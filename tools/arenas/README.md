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

### Every arena is back (2026-10-15)

Nine arenas had been switched off: Dalaran Sewers (10) and the Ring of Valor (11)
in `disables`, Guardian's Hall (877) and Spark of Creator (878) only in the pool,
and Baradin Hold Arena (879), Obelisk of the Stars (880), The Twisting Nether
(881), The Inventor's Library (884) and Amphitheater of Anguish (885) in both.
All nine are in the All Arenas rotation again. What each needed:

| BG | Map | What was wrong | Fix |
|---|---|---|---|
| 10 | 617 | **Players and pets dropped under the floor.** The art patch carried the *3.0* Dalaran Sewers WMO (common-2.MPQ) with only its indoor flags edited, shadowing the final 3.3.5 one in patch.MPQ. The 3.0 centre platform has two holes (at 1305,778 and 1279,806) straight down to a basin at z -4..-17; the live server's collision had been extracted from the same file. Blizzard's own rescue trigger (5326) puts you back at z 8.11 - inside the 3.0 platform (top 9.34), so you fell again. Its exterior flag edit also left it with no minimap. | patch-Y 1.00027 drops the override, so clients load the 3.3.5 WMO (solid floor at 7.11, interior groups so Blizzard's WMO minimap draws; outdoors still comes from WMOAreaTable flag 4). Server vmaps/mmaps for 617 rebuilt from it - byte-identical to the old Barracks+ extraction CenturionDev still had - with off-mesh links from both start pipes down to the ring, so bots can leave the pipes. Starts (WSL 1362/1363) moved onto the pipe axis and 1.5 yd **behind** each pipe's gate (GO 192642/192643), facing the arena (`battleground_template` 10: AllianceStartO pi, HordeStartO 0). Blizzard's points sat inside the tube wall and 1-3 yd on the arena side of the gates, so the gates held nobody and either team could drop in before the start. They are no further out than that because the bot fence (`BattlegroundFence.cpp`) refuses to exist once a start is more than 1.4x its measured radius from the centre: 66.6 yd against a 67.8 yd limit. Area triggers 5326/5328-5331 now land on the south ring (1292, 760, 7), floor in both geometries. |
| 11 | 618 | The elevators are type 11 transports the server never moves: pets and bots stay in the shafts below the floor when the gates open. | `battleground_bounds`: after 20.5 s anyone below z 27 is lifted to 28.28 where they stand (what AzerothCore does by hand). |
| 877 | 1007 | Held back for its minimap (untextured mesh render). | Textured top-down render from the client's own WMO + doodad textures (`topdown/make_tiles.py`), patch-Y 1.00027. |
| 878 | 1008 | Same, and the old render was rotated 180 degrees (local WMO X/Y added to world X/Y instead of subtracted). | Same. |
| 879 | 1401 | No start rooms; the two gates were "derived" placeholders standing in the courtyard. | Placeholder gates removed, `StartMaxDist` 12 holds both teams (and their pets) at their start until the gates would open; shadowsight in the NE/SW corners. Enclosed by its walls. |
| 880 | 1402 | Same; the starts are just inside two wall gaps closed by invisible collision panes. | Same; shadowsight in the north/south gaps. Enclosed. |
| 881 | 1403 | The shipped starts (Ascension's WSL 2037/2038) are two balcony alcoves on the Kil'jaeden ship that nothing connects to - the navmesh from each reaches its own alcove only. | Starts moved onto the Legion arena platform's north/south edges (WSL 52618/52619), leash + bounds outline: falling off the platform (or being knocked off) puts you back on its nearest edge. |
| 884 | 1683 | Open terrace: the north stairs and south hillside lead off it, the navmesh reaches the whole map. | Leash, shadowsight, bounds outline traced from the top collision surface (excludes the buildings); MinZ 945. |
| 885 | 1684 | Open pit: tunnels at pit level under the 286-high ring lead out of the arena. | Leash, shadowsight, bounds outline = the pit and the lower stairs; MinZ 266. |

**`battleground_bounds`** (code: `Battleground::_CheckBounds`, commit 9df5f27056) is
generic: per battleground type a MinZ, an optional outline in world X/Y, and where to put
someone back (the floor under the nearest inside point, the same X/Y at a fixed height, or
a safe point). Checked twice a second for every combatant and every pet, guardian and
totem they control, once the gates open plus GraceMs. Edit rows and `.reload
battleground_template`.

How the data was chosen - navmesh reachability from the starts, the outlines, the buff
spots - is in `topdown/README.md`; the shapes are measured, so a rerun reproduces them.

### Later the same day, at the owner's request

- **Real start gates for 879, 880 and 885** (`MEASURED` in `gen_arena_sql.py`): rows of
  the Black Temple gate cut to fit (templates 300402-300404), wall to wall, closing a pen
  around each start - under the two bridges of each Baradin Hold corner, across the
  recess between two Obelisk wall towers, at the foot of both staircases and across the
  front of each Amphitheater landing. Each pen was checked closed on the navmesh with
  the real segment extents. The leash stays, widened to the pen (38 / 24 / 13 yd): it
  only ever catches a pet or bot the server walks through a shut gate.
  The Inventor's Library has none yet - there is no doorway or small room near its
  starts in the client or server geometry, and an open-floor pen there is ~70 yd of gate.
- **Bots drop out of Dalaran Sewers' pipes**: `battleground_drop_exit` rows (data-driven
  `BattlegroundDropExit`, the mechanism Eye of the Storm's start platforms use): a bot
  in a pipe walks to the mouth and is set down on the ring below, its pet with it.
- **Every arena is open sky**: the server treats any arena map as outdoors
  (`Map::GetFullTerrainStatusForPosition`), and the client's own WMOAreaTable was given
  Flags 4 on every indoor WMO group people stand on or in (patch-enUS-A 1.00170):
  Nefarian's Arena (5 BWL floor groups), the Imperial Arena of Thakraj (3 Azjol-Nerub
  roof groups) and two new rows for Obelisk's Uldum titan micro (NameSet 1). Dalaran
  Sewers and the Ring of Valor already had them. The survey (navmesh points within
  90 yd of each arena's centre, topmost WMO surface under them, plus interior-group
  boxes they stand inside) is scratchpad `stood_groups.py`; scenery groups that nobody
  reaches - the Duskwood inn on Ruins of Lordaeron's map, say - are left alone, because
  WMOAreaTable rows are global and those WMOs stand in the open world too.
- **A skirmish left to bots closes** the moment its last person leaves (gates shut or
  open), so queueing again rolls a new match instead of re-seating you in the old one.
- **The Twisting Nether starts on the ship's balconies again** (Ascension's spawns,
  WSL 52618/52619), and `battleground_start_exit` - a team's exit when the gates open,
  data per battleground and team - puts everyone, pets included, onto their own edge of
  the platform. The bots-only fence refuses this arena with balcony starts (they reach
  67.8 yd of a 54.0 yd limit), so nothing pulls a bot back up; the platform's bounds
  outline still holds everyone once they are down.
- **Dalaran Sewers and the Ring of Valor are in the custom-game arena list** (the
  ported arenas were already offered as a block; these two were left out while
  disabled).

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

## Zone names

The client does **not** take the zone name from Map.dbc. It reads the `areaid`
baked into each MCNK terrain chunk and looks that up in AreaTable.dbc. Ported
terrain keeps the ids it was authored with, and none of them existed here — so
an arena showed whatever zone the player came from (map 982 announced itself as
The Violet Hold).

Because the ids were *absent* rather than *taken*, rows could be added under the
same ids and no terrain had to be rewritten. `area_ids.py` reads them out of the
ADTs; 22 rows cover the 14 arenas, on AreaBits 3800–3821.

Three arenas — 1402, 1683, 1684 — have most of their chunks on area id 0, which
is "no area" and cannot be given a row. The ids that do exist cover the built-up
part, which is where the arena is. If a stale name shows up standing out in the
open there, the fix is rewriting the MCNK `areaid` fields.

## Where the coordinates came from

**Team starts now come from Ascension's own `WorldSafeLocs.dbc`** — the exact
teleport targets the source server used, so they are on the floor and inside the
arena by construction. `ascension_starts.py` reads them and emits a paste-ready
`MEASURED` block. That superseded the geometric derivation for all 14.

It checks out: Ascension's map-982 starts land **1.1 yards** from the values
surveyed in-game with `.gps`, with orientations agreeing to 0.02 rad. Map 982
keeps the in-game survey since it was measured in this client; the other 13 use
Ascension's.

Gates and shadowsight buffs are not in any DBC. Map 982's were surveyed in-game.
The rest are derived from the team starts — 85% of the way from centre to each
start, 0.8 yards below it, which is what 982's surveyed gates actually measure.
Expect to walk them.

The older geometric pipeline is still here and still useful for anything the
DBCs do not cover. `adt_probe.py` reads WMO and M2 placements out of the ADTs
and converts them to world space; `wmo_floor.py` extracts the arena's WMO from
the MPQ and reads its floor height off the mesh — necessary because the
placement gives the WMO's *origin*, and the gap to the floor is not constant
(Tiger's Peak: under a yard; Tol'viron: 22 yards).

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
| server terrain (maps/vmaps/mmaps) | **absent** | absent |

### Local client — `C:\Projects\Gamedev\wow\clients\centurion`

Applied by `apply_to_client.py`. Nothing was published to the download server.

| | |
|---|---|
| `Data/patch-Z.MPQ` | **rebuilt**: 147 original files + 4,576 arena files = 4,723, 61.7 MB → 491 MB |
| `Data/enUS/patch-enUS-8.MPQ` | **5 DBCs replaced** with arena rows appended |
| backups | `patch-Z.MPQ.bak-arenas`, `patch-enUS-8.MPQ.bak-arenas` |

patch-Z had to be *rebuilt* rather than appended to: its hash table was 256
entries, a ceiling fixed when an archive is created, and it already held 149
files. The replacement has 32768.

**patch-enUS-8 is shared with the Violet Hold battleground.** That work has rows
in four of the five DBCs and its own `patch-enUS-8.MPQ.bak-vhr`. Nothing here
collides with it:

| DBC | Violet Hold | arenas |
|---|---|---|
| BattlemasterList | 105 | 872–885 |
| WorldStateUI | 90025–90027 | 90100–90127 |
| WorldSafeLocs | 52520–52521 | 52600–52627 |
| PvpDifficulty | 91608 | 93000–93223 |
| AreaTable | 30608 | *none* |

`apply_to_client.py` reads the five DBCs out of the archive itself rather than
copying them from the server, appends only, and then checks that every record id
present beforehand is still there. It also refuses to write if the archive
changed underneath it — which it did once, mid-run, when the other agent
rebuilt it.

**Both agents are writing to this file**, so a rebuild on either side can drop
the other's rows. Re-run `apply_to_client.py --only dbc` after any Violet Hold
rebuild; it is idempotent and will re-append only what is missing.

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

## Still to do (August 2026 - all done since; kept for the record)

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
