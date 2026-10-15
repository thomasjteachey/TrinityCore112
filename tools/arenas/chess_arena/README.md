# The Gamesman's Hall arena

Arena type **888** (`BATTLEGROUND_GMH`) on map **1532**, a clone of Karazhan (532) that keeps
532's directory `Karazahn`: the client loads Karazhan's own terrain and castle and needs no new
files beyond the DBC rows. Served by the data-driven `BattlegroundCustomArena`; everything
positional is in `chess_layout.py`. The owner asked for it from the `deathmatch20` teleport (the
chess board), "isolated to the chess board area", and "4 chess looking statues that LOS on the
chessboard". The first layout teleported both teams down onto the board and shut the stairs with
portcullises; the second (same day) has them walk down instead: "instead of teleporting people
down in the gamesman hall, just barricade the windows and put doors in the 2 openings to the left
and right".

## How it plays

- The chess hall (Karazhan's Gamesman's Hall): a 77 x 77 yd room around the 44 yd board, two
  rows of nine pillars along its sides. Each team waits on the balcony at its end of the hall,
  8.55 yd above the floor.
- **The windows.** A balcony's front is an arcade of three windows open to the floor. Each holds
  one of Karazhan's tall library bookcases (`KarazahnBookshelfXtralarge`, display **11973**,
  books on both faces), against the windows' inner face: 5.6 yd tall in the middle window, 4.5
  in the side ones, each as wide as its window. With the column plinths and the slender columns
  above them no gap is left wider than 0.46 yd. They are gameobjects with a server collision
  model, so a balcony cannot cast into the hall either.
- **The doors.** A short stair at each side of a balcony goes down through an arch onto the top
  of a long stairway. Each arch holds Karazhan's stage door (`Karazahn_stageDoor`, display 6633,
  scale 1.15 - the doorway is 3.79 x 6.7 yd), shut through the preparation and opened when the
  match begins (`battleground_custom_arena_object` ObjectType 0). Each swings out onto the
  stairway, never into the stair behind it.
- **The way down.** Through a door and down the long stairway along the end wall to a corner
  landing, then down the corner flight to the floor - or drop into the hall through the
  stairway's arches. The way back up is the same; the balconies and stairways are part of the
  arena. Nothing teleports anyone (`battleground_start_exit` has no rows for 888), and the
  corner portcullises of the first layout are gone: they shut the only stairs down.
- Karazhan's own doors at the far end of each balcony's back room (display 6628) are spawned
  shut, so nothing leads on into the castle. They never open; nobody can click them.
- The leash (`StartMaxDist` 9.5) covers a balcony and the stairs down to its doors (a door is
  7.9 yd from the start), and stops anyone wandering into the room behind.
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
room ending at a -52.9). A door cannot be mirrored, so the four are placed by the half turn
instead: the SW +b and NE -b doors hinge on the stairway's back wall, the other two on its hall
side.

## Ids

| what | ids |
|---|---|
| Map | 1532, Directory `Karazahn`, arena, Karazhan's loading screen (200) and zone (3457), expansion 0 |
| BattlemasterList | 888 (the Eye of Eternity's shape) |
| PvpDifficulty | 93260-93275, 10-89 in 5-level brackets |
| WorldSafeLocs | 52630 (Alliance, south-west balcony), 52631 (Horde, north-east balcony) |
| WorldStateUI | 90130/90131, "N Players Remaining" |
| MapDifficulty | 762 (1532, difficulty 0; server only - without it no static spawn on 1532 loads) |
| GameObjectDisplayInfo | 11973 `KarazahnBookshelfXtralarge.mdx` (no stock row named it) |
| gameobject_template | 300409 hall door (type 0, faction 114), 300410 knight, 300411 bishop, 300412 bookcase 0.42 and 300414 bookcase 0.335 (type 5), 300413 balcony door (type 0, faction 114, a battleground object). 300408 was the first layout's portcullis, deleted |
| gameobject | 5530564-65 hall doors, 5530566-69 statues, 5530560-63 + 5530576-77 bookcases - fixed (never AUTO_INCREMENT, see `../aoa_pillars/README.md`) |

## What was applied (2026-10-15)

| piece | where |
|---|---|
| the DBC rows (`chess_dbc.py`) | both realms' `data/dbc` (backups `.bak-chess-20261015`; GameObjectDisplayInfo `.bak-gates-20261015`), `centurion/dbc`; patch-enUS-A 1.00179 (first layout, without MapDifficulty) and 1.00185 (GameObjectDisplayInfo 11973) |
| the bookcase's server collision | `vmaps/KarazahnBookshelfXtralarge_GO.m2.vmo` + its `GameObjectModels.dtree` entry, both realms (`install_gates.sh`, `tools/tanaris/build_go_vmo.py --doodad`) |
| map data | map 532's 9 maps, 10 vmaps and 10 mmaps renamed to 1532 (`install_chess.sh`), the hall's tile `15325235.mmtile` carved (`chess_nav.py`; second carve `7d0d9da530cd`, first `1adb8e13283d` kept as `.bak-gates-20261015`) |
| world rows (`gen_chess_sql.py`) | both worlds - CenturionDev at once, Centurion at its restart (a `.reload battleground_template` before the restart would name the door template the running server has not loaded) |
| code | `SharedDefines.h` (BATTLEGROUND_GMH, the custom arena block now ends at 888), `custom_game_lobby.cpp` (its name; the lobby's arena list skips 887, which now sits inside the block) |

The doors' and statues' displays were already in `GameObjectModels.dtree` (stock), so spell
line of sight, Blink and charges see them with no server-side model work; the bookcase's display
is new and got its own model.

## The navmesh carve - REDO IT after any regen of map 1532's mmaps

A gameobject is invisible to the navmesh, so without the carve a bot or pet walks through a
statue, a bookcase or a shut hall door. The balcony doors are deliberately NOT carved: they open
at the start and the way down runs through them.

1. Copy the realm's `vmaps/1532.vmtree` and `vmaps/1532_35_52.vmtile` (the hall's tile) into
   `<work>/vmaps`, and the four M2s (`Karazahn_chessroomdoors`, `statueHMpaladin`,
   `statueHMpriest`, `KarazahnBookshelfXtralarge`, from the client) into `<work>`.
2. Nav models into `<work>/vmaps`: `python ../../eoe_arena/pillars/build_nav_vmo.py <name>.m2
   vmaps/<name>_nav.vmo <scale>` with scale 1.02, 3.5, 3.5 and 0.335 (the two bookcase sizes
   share the one built at the smaller scale).
3. `python chess_nav.py carve <work>`.
4. On the server, in a dir with those vmaps (every other vmaps file symlinked from the realm),
   `maps/1532*.map` and `dbc/`: `~/wow/servers/tc-centurion/bin/mmaps_generator 1532 --tile 35,52 --allowDebug`
   (delete an existing `mmaps/15325235.mmtile` first - the generator skips it). Run on the
   untouched vmaps it reproduces 532's own tile byte for byte (checked 2026-10-15).
5. `python chess_nav.py check <old.mmtile> <new.mmtile>`: one walk from the middle of the board
   reaches both balcony starts - through the corner flights, the corner landings, the long
   stairways and the side doorways, each of which it names - and both Shadow Sights, and
   nothing outside the bounds outline (before: 668 of 984 polygons, the castle beyond; after:
   0 of 255); the rooms past Karazhan's doors are cut off; the statues, the hall doors and the
   bookcases are off the mesh.

## Files

- `chess_layout.py` - every id and coordinate, from the hall frame
- `chess_dbc.py` - the DBC rows (shares `tools/eoe_arena/eoe_dbc.py`'s writer)
- `gen_chess_sql.py` - the world rows, `--rollback` for their removal
- `chess_nav.py` - the navmesh carve and its check
- `chess_patch.py` - server side: the client rows into patch-enUS-A through `eoe_patch.py`
  (`--only GameObjectDisplayInfo` for the second layout's one new row)
- `install_chess.sh` - server side: DBCs and map data into one realm, refusing anything changed
- `install_gates.sh` - server side, second layout: the bookcase's display row, its collision
  model and the new hall tile, md5-guarded the same way
