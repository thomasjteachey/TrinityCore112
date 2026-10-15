# The Gamesman's Hall arena

Arena type **888** (`BATTLEGROUND_GMH`) on map **1532**, a clone of Karazhan (532) that keeps
532's directory `Karazahn`: the client loads Karazhan's own terrain and castle and needs no new
files beyond the DBC rows. Served by the data-driven `BattlegroundCustomArena`; everything
positional is in `chess_layout.py`. The owner asked for it from the `deathmatch20` teleport (the
chess board), "isolated to the chess board area", teams "teleported down and then unable to run
up the stairs because there are unopenable doors in the way", and "4 chess looking statues that
LOS on the chessboard".

## How it plays

- The chess hall (Karazhan's Gamesman's Hall): a 77 x 77 yd room around the 44 yd board, two
  rows of nine pillars along its sides. Each team waits on the balcony at its end of the hall
  (8.55 yd above the floor). The leash (`StartMaxDist` 9.5) covers the balcony and the arch
  into its stair; a jump down to the hall floor is 10+ yd from the start and is sent back.
- When the match begins, `battleground_start_exit` teleports everyone on a balcony onto the
  middle of their own back rank (19.4 yd from the centre, 38.8 apart), facing the other team,
  with the same teleport visuals as the Eye of Eternity and the Twisting Nether.
- The only stairs up from the hall floor are four short corner flights. A portcullis
  (`KarazahnPortcullis`, display 6702) closes the arch at the top of each, where the flight
  passes through the end wall; it never opens and nobody can click it (addon flags 0x10 |
  0x20). The bottom of a flight was no place for it: the flight's sides are open and only
  0.97 yd high there.
- The two doors at the back of the balconies, where Karazhan's own Gamesman's Hall doors stand
  (display 6628), are spawned shut the same way, so nothing leads on into the castle.
- Four white marble chess pieces break line of sight on the board, each on a square 1.5
  squares out from the middle on both axes, scale 3.5 (about 7 yd tall): two knights
  (`statueHMpaladin`, 6820) on one side, two bishops (`statueHMpriest`, 1527) on the other, so
  the team swap maps each onto its twin. All face the centre.
- Shadow Sight at the middle of the board's two side edges.
- `battleground_bounds`: the hall floor. Anyone off it once the match is on is put back just
  inside; MinZ 216 catches a fall through the floor.

The hall is the mirror image of itself across its long axis (none of 3,760 floor samples
differ) and, but for 14 small props, across the line between the balconies - the mirror that
swaps the teams.

## Ids

| what | ids |
|---|---|
| Map | 1532, Directory `Karazahn`, arena, Karazhan's loading screen (200) and zone (3457), expansion 0 |
| BattlemasterList | 888 (the Eye of Eternity's shape) |
| PvpDifficulty | 93260-93275, 10-89 in 5-level brackets |
| WorldSafeLocs | 52630 (Alliance, south-west balcony), 52631 (Horde, north-east balcony) |
| WorldStateUI | 90130/90131, "N Players Remaining" |
| MapDifficulty | 762 (1532, difficulty 0; server only - without it no static spawn on 1532 loads) |
| gameobject_template | 300408 portcullis, 300409 hall door (type 0, faction 114), 300410 knight, 300411 bishop (type 5) |
| gameobject | 5530560-5530569, fixed (never AUTO_INCREMENT, see `../aoa_pillars/README.md`) |

## What was applied (2026-10-15)

| piece | where |
|---|---|
| the DBC rows (`chess_dbc.py`) | both realms' `data/dbc` (backups `.bak-chess-20261015`), `centurion/dbc`; patch-enUS-A 1.00179 without MapDifficulty |
| map data | map 532's 9 maps, 10 vmaps and 10 mmaps renamed to 1532 (`install_chess.sh`), the hall's tile `15325235.mmtile` carved (`chess_nav.py`) |
| world rows (`gen_chess_sql.py`) | both worlds |
| code | `SharedDefines.h` (BATTLEGROUND_GMH, the custom arena block now ends at 888), `custom_game_lobby.cpp` (its name; the lobby's arena list skips 887, which now sits inside the block) |

The doors' and statues' displays were already in `GameObjectModels.dtree` (stock), so spell
line of sight, Blink and charges see them with no server-side model work.

## The navmesh carve - REDO IT after any regen of map 1532's mmaps

A gameobject is invisible to the navmesh, so without the carve a bot or pet walks through a shut
portcullis or a statue.

1. Copy the realm's `vmaps/1532.vmtree` and `vmaps/1532_35_52.vmtile` (the hall's tile) into
   `<work>/vmaps`, and the four M2s (`KarazahnPortcullis`, `Karazahn_chessroomdoors`,
   `statueHMpaladin`, `statueHMpriest`, from the client) into `<work>`.
2. Nav models into `<work>/vmaps`: `python ../../eoe_arena/pillars/build_nav_vmo.py <name>.m2
   vmaps/<name>_nav.vmo <scale>` with scale 1.0, 1.02, 3.5, 3.5.
3. `python chess_nav.py carve <work>`.
4. On the server, in a dir with those vmaps (every other vmaps file symlinked from the realm),
   `maps/1532*.map` and `dbc/`: `~/wow/servers/tc-centurion/bin/mmaps_generator 1532 --tile 35,52 --allowDebug`
   (delete an existing `mmaps/15325235.mmtile` first - the generator skips it). Run on the
   untouched vmaps it reproduces 532's own tile byte for byte (checked 2026-10-15).
5. `python chess_nav.py check <old.mmtile> <new.mmtile>`: one walk from the middle of the board
   reaches both landings and both Shadow Sights and nothing outside the bounds outline (before:
   738 of 984 polygons - the balconies, the stairs and the castle beyond; after: 0 of 197); the
   statues, portcullises and doors are off the mesh; each corner flight is walkable up to its
   portcullis and its landing beyond is cut off.

## Files

- `chess_layout.py` - every id and coordinate, from the hall frame
- `chess_dbc.py` - the DBC rows (shares `tools/eoe_arena/eoe_dbc.py`'s writer)
- `gen_chess_sql.py` - the world rows, `--rollback` for their removal
- `chess_nav.py` - the navmesh carve and its check
- `chess_patch.py` - server side: the client rows into patch-enUS-A through `eoe_patch.py`
- `install_chess.sh` - server side: DBCs and map data into one realm, refusing anything changed
