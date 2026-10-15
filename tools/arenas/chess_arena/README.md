# The Gamesman's Hall arena

Arena type **888** (`BATTLEGROUND_GMH`) on map **1532**, a clone of Karazhan (532) that keeps
532's directory `Karazahn`: the client loads Karazhan's own terrain and castle and needs no new
files beyond the DBC rows. Served by the data-driven `BattlegroundCustomArena`; everything
positional is in `chess_layout.py`. The owner asked for it from the `deathmatch20` teleport (the
chess board), "isolated to the chess board area", and "4 chess looking statues that LOS on the
chessboard". Three layouts on 2026-10-15:

1. both teams teleported from their balconies down onto the board, the stairs shut by portcullises;
2. "instead of teleporting people down ... just barricade the windows and put doors in the 2
   openings to the left and right": bookcases in the windows, stage doors in the side arches;
3. "replace the bookshelfs with something more appropriate (just block it off with the gates we
   use for a lot of other arenas or something, and have them open at arena start)", "the doors look
   bad, they are jutting out", and "the chess map should be considered outside everywhere" - the
   layout described here.

## How it plays

- The chess hall (Karazhan's Gamesman's Hall): a 77 x 77 yd room around the 44 yd board, two
  rows of nine pillars along its sides. Each team waits on the balcony at its end of the hall,
  8.55 yd above the floor.
- **The gates.** Every way off a balcony is shut by the Black Temple gate the other custom arenas
  close with (`BT_GATE`, display 7320): one in each of the balcony's three windows (scale 0.106,
  3.1 yd tall, just behind the slender columns) and one across the flat top landing of each long
  stairway beyond the balcony's two side arches (scale 0.213, 6.2 x 6.2 yd, wall to wall between
  the landing's columns). They are `battleground_custom_arena_object` ObjectType 0: shut through
  the preparation, and when the match begins the bars sink straight into the floor, out of sight.
- **Why the landings and not the arches:** each side arch is a 3.79 yd doorway in a thin
  partition with open space on both flanks, so anything wider than the doorway juts out beside
  it - the second layout's stage doors did - and a square gate that narrow is only 3.8 yd tall.
  The landing just past it is walled on both sides and 7.7 yd high.
- **The way down.** Drop through the open windows into the hall, or go down a long stairway along
  the end wall to a corner landing and down the corner flight to the floor, or drop into the hall
  through the stairway's arches. The way back up is the same: the balconies and stairways are part
  of the arena. Nothing teleports anyone (`battleground_start_exit` has no rows for 888).
- **Outdoors everywhere.** The server already treats every arena map as outdoors
  (`Map::GetFullTerrainStatusForPosition`), but the client makes its own call - it refuses a
  mount or an outdoors-only spell before the server hears of it - from the WMO group under the
  player and that group's WMOAreaTable row. Everything the arena's navmesh reaches stands on one
  group of `Kharazan_Instance.wmo` (WMOID 4449, NameSet 0, group 19658), so its row, **id 36327**,
  has Flags 4 (outdoors) on every layer (`chess_outdoors.py`). The real Karazhan's chess room reads
  outdoors too; nothing on this realm plays there.
- Karazhan's own doors at the far end of each balcony's back room (display 6628) are spawned
  shut, so nothing leads on into the castle. They never open; nobody can click them.
- The leash (`StartMaxDist` 9.5) covers a balcony, its side stairs and the first yard of each
  landing (a stairway gate is 8.8 yd from the start), and stops anyone wandering into the room
  behind.
- Four white marble chess pieces break line of sight on the board, each on a square 1.5
  squares out from the middle on both axes, scale 3.5 (about 7 yd tall): two knights
  (`statueHMpaladin`, 6820) on one side, two bishops (`statueHMpriest`, 1527) on the other, so
  the team swap maps each onto its twin. All face the centre.
- Shadow Sight at the middle of the board's two side edges.
- `battleground_bounds`: the hall floor plus, at each end, the balcony, the stair or room behind
  it as far as Karazhan's door, the long stairways and the corner landings. Anyone outside that
  once the match is on, or below MinZ 216, goes to the middle of the board (ProbeZ 0: the ends
  stack three floors, so there is no one height to look down from for "just inside").

The hall is the mirror image of itself across its long axis (none of 3,760 floor samples
differ) and, but for 14 small props, across the line between the balconies - the mirror that
swaps the teams. The two ends match floor for floor out to the corner landings; behind the
balconies they differ (the SW one climbs a stair to its door at a 60.7, the NE one is a short
room ending at a -52.9).

## Ids

| what | ids |
|---|---|
| Map | 1532, Directory `Karazahn`, arena, Karazhan's loading screen (200) and zone (3457), expansion 0 |
| BattlemasterList | 888 (the Eye of Eternity's shape) |
| PvpDifficulty | 93260-93275, 10-89 in 5-level brackets |
| WorldSafeLocs | 52630 (Alliance, south-west balcony), 52631 (Horde, north-east balcony) |
| WorldStateUI | 90130/90131, "N Players Remaining" |
| MapDifficulty | 762 (1532, difficulty 0; server only - without it no static spawn on 1532 loads) |
| WMOAreaTable | row 36327 (Karazhan's chess hall group) Flags 0 -> 4 |
| GameObjectDisplayInfo | 11973 `KarazahnBookshelfXtralarge.mdx` - the second layout's bookcase, unused, kept because it is published |
| gameobject_template | 300409 hall door (type 0, faction 114), 300410 knight, 300411 bishop (type 5), 300412 window gate (BT_GATE 0.106) and 300413 stairway gate (BT_GATE 0.213), both type 0 battleground objects. Gone: 300408 (first layout's portcullis), 300414 (second layout's bookcase) |
| gameobject | 5530564-65 hall doors, 5530566-69 statues - fixed (never AUTO_INCREMENT, see `../aoa_pillars/README.md`) |

## What was applied (2026-10-15)

| piece | where |
|---|---|
| the DBC rows (`chess_dbc.py`) | both realms' `data/dbc` (backups `.bak-chess-20261015`; GameObjectDisplayInfo `.bak-gates-20261015`), `centurion/dbc`; patch-enUS-A 1.00179 (first layout, without MapDifficulty) and 1.00185 (GameObjectDisplayInfo 11973) |
| WMOAreaTable 36327 outdoors (`chess_outdoors.py`) | both realms' `data/dbc` (backup `.bak-chess-outdoors`), `centurion/dbc`, patch-enUS-A 1.00186 |
| the bookcase's server collision | `vmaps/KarazahnBookshelfXtralarge_GO.m2.vmo` + its `GameObjectModels.dtree` entry, both realms (`install_gates.sh`) |
| map data | map 532's 9 maps, 10 vmaps and 10 mmaps renamed to 1532 (`install_chess.sh`), the hall's tile `15325235.mmtile` carved (`chess_nav.py`): third carve `3d4a4fd6b97a`, second `7d0d9da530cd`, first `1adb8e13283d` |
| world rows (`gen_chess_sql.py`) | both worlds - CenturionDev at once, Centurion at its restart (a `.reload battleground_template` before the restart would name gate templates the running server has not loaded) |
| code | `SharedDefines.h` (BATTLEGROUND_GMH, the custom arena block now ends at 888), `custom_game_lobby.cpp` (its name; the lobby's arena list skips 887, which now sits inside the block) |

## The navmesh carve - REDO IT after any regen of map 1532's mmaps

A gameobject is invisible to the navmesh, so without the carve a bot or pet walks through a
statue or a shut hall door. The start gates are deliberately NOT carved: they sink at the start
and the way down runs through them.

1. Copy the realm's `vmaps/1532.vmtree` and `vmaps/1532_35_52.vmtile` (the hall's tile) into
   `<work>/vmaps`, and the three M2s (`Karazahn_chessroomdoors`, `statueHMpaladin`,
   `statueHMpriest`, from the client) into `<work>`.
2. Nav models into `<work>/vmaps`: `python ../../eoe_arena/pillars/build_nav_vmo.py <name>.m2
   vmaps/<name>_nav.vmo <scale>` with scale 1.02, 3.5 and 3.5.
3. `python chess_nav.py carve <work>`.
4. On the server, in a dir with those vmaps (every other vmaps file symlinked from the realm),
   `maps/1532*.map` and `dbc/`: `~/wow/servers/tc-centurion/bin/mmaps_generator 1532 --tile 35,52 --allowDebug`
   (delete an existing `mmaps/15325235.mmtile` first - the generator skips it). Run on the
   untouched vmaps it reproduces 532's own tile byte for byte (checked 2026-10-15).
5. `python chess_nav.py check <old.mmtile> <new.mmtile>`: one walk from the middle of the board
   reaches both balcony starts - through the corner flights, the corner landings, the long
   stairways, the side doorways and the gate spots, each of which it names - and both Shadow
   Sights and the window bays, and nothing outside the bounds outline (before: 668 of 984
   polygons, the castle beyond; after: 0 of 260); the rooms past Karazhan's doors are cut off;
   the statues and the hall doors are off the mesh.

## Files

- `chess_layout.py` - every id and coordinate, from the hall frame
- `chess_dbc.py` - the DBC rows (shares `tools/eoe_arena/eoe_dbc.py`'s writer)
- `chess_outdoors.py` - the WMOAreaTable outdoors flag, one field of one row, in place
- `gen_chess_sql.py` - the world rows, `--rollback` for their removal
- `chess_nav.py` - the navmesh carve and its check
- `chess_patch.py` - server side: client rows into patch-enUS-A through `eoe_patch.py`
  (`--only <Table>` ships one table)
- `install_chess.sh` - server side: DBCs and map data into one realm, refusing anything changed
- `install_gates.sh` - server side, later layouts: the bookcase's display row, its collision
  model and the current hall tile, md5-guarded the same way
