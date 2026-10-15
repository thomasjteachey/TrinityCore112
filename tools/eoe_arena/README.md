# The Eye of Eternity arena

Arena type **886** (`BATTLEGROUND_EOE`) on map **1616**, a clone of the Eye of
Eternity (616, directory `NexusRaid`) in its own directory, `EyeOfEternityArena`.
Served by the data-driven `BattlegroundCustomArena`; everything positional is in
`layout.py`.

## How it plays

- Each team starts on its own floating pad - Coldarra's
  `nexus_floating_platform_01_small.wmo` - off the ends of two of the Eye's
  diagonal arms, 115 yd from the centre and 14 yd above the floor. The leash
  (`StartMaxDist` 24) holds them there until the gates open; jumping off brings
  you back.
- When the gates open, `battleground_start_exit` teleports everyone on a pad,
  pets included, onto the Eye: each team on its own side, 33 yd out, 66 yd apart,
  facing the middle. The teleport is drawn with `Teleport Visual Only` (64446,
  `Teleport.mdx` at double size, the Gurubashi ring's teleport) on everyone moved
  and on the ground where they vanish and where they land (Battleground.cpp,
  shared with The Twisting Nether).
- The Eye is enclosed: an invisible collision wall (18 sides, 54-56 yd out)
  rises 32 yd above the floor under a sloped invisible lid, so nobody falls off.
  `battleground_bounds` (MinZ 240, a 50 yd circle) is the safety net, and it is
  also what brings anyone left on a pad after the gates onto the Eye.
- Shadow Sight on the axis square to the teams'. No gates: the void is the gate.
- Four Nexus pillars on a square round the centre break line of sight
  (`pillars/`): gameobjects for spell LoS, carved into the navmesh for bots. Any
  regen of 1616's mmaps must redo the carve.
- The sky is the Eye's own: Light 3616 is a copy of 616's map-wide light 1773 for
  map 1616, `NexusRaid_StarrySky`. A map with no Light row gets the default
  daytime sky.
- A ghost stays in the arena (`BattlegroundCustomArena::GetClosestGraveyard`
  returns null). The terrain keeps area 4500, and graveyard_zone ties 4500 to the
  Coldarra graveyard on Northrend.

## Why the floor needed a new map directory

On 616 the floor is not static geometry. The platform WMO is placed with MODF
flag 0x1 (destroyable) because gameobject 193070 is the floor Malygos breaks in
phase three, and `vmap4extractor` skips destroyable placements. The server has no
vmap tiles and no navmesh for 616 at all - its collision is the gameobject's.
`eoe_terrain.py` copies the 25 tiles under the new directory with that flag
cleared, so the client, the vmaps and the mmaps all see an ordinary floor, and
adds the two pads. Each new placement is referenced from the MCRF list of every
terrain chunk its extents box covers (Blizzard's rule for the platform: chunks
5..12 x 5..13 of tile 29_30); the client never draws or collides with a
placement no chunk references. The rewrite is verified against the source
(every other byte, every sub-chunk offset) before it is written.

## Ids

| what | ids |
|---|---|
| Map | 1616 `EyeOfEternityArena`, arena, loading screen 236, expansion 0 |
| BattlemasterList | 886 (The Twisting Nether's shape) |
| PvpDifficulty | 93244-93259, 10-89 in 5-level brackets |
| WorldSafeLocs | 52628 (Alliance pad), 52629 (Horde pad) |
| WorldStateUI | 90128/90129, "N Players Remaining" |
| Light | 3616 (copy of 1773) |
| MapDifficulty | 760 (1616, difficulty 0; server only - without it no static spawn on 1616 loads) |
| ADT placements | uniqueId 3616001/3616002 for the pads |
| Pillars | gameobject_template 300405, four spawns on a square (`pillars/README.md`) |

## Tools

- `layout.py` - every coordinate and id. `python layout.py` prints them.
- `eoe_terrain.py <out>` - the client terrain from the local client's MPQs,
  plus the minimap block for `md5translate.trs` and a manifest.
- `eoe_dbc.py show | apply <dir> | verify <dir>` - the rows, templated from the
  client's own 616/881/52618/90118/1773 so every layer gets the same bytes.
  Appends only; refuses an id that holds other data; backs up as `.bak-eoe`.
- `gen_eoe_sql.py <db> [--rollback]` - the world rows (template, pool, start
  exits, bounds, buffs). Idempotent. Not committed as SQL: applied directly.
- `eoe_patch.py extract|pack|publish <work>` - server side, `patch-enUS-A`
  (the six tables + the terrain) and `world-terrain` (Light: the HD World pack
  ships its own Light.dbc and outranks A).
- `install_server.sh <realm> check|apply` - server side, a realm's data/dbc
  and map 1616's maps/vmaps/mmaps from `~/eoe-arena-20261015`.

## Server terrain

`.map` and vmaps from `tools/mapdata/regen_map_data.ps1 -Maps 1616 -SkipMmaps`
against a scratch client (`C:\Projects\Gamedev\wow\data\client-scratch-eoe`:
hardlinks of the real client, a private `Data\patch-Z.MPQ` with the terrain and
`Data\enUS\patch-enUS-Z.MPQ` with Map.dbc + row 1616, both built with
`mpqtool new`). The 25 `.map` files come out byte-identical to 616's, and both
`.vmo` models to the ones every realm already had.

The navmesh was built on the server with the realm's own `mmaps_generator`
(Jenkins, so it has the four-digit map id fix 4948d1a929 the local Aug-15 build
lacks; it needs `--allowDebug`). One tile, 16163029: a 30-poly floor island
inside the wall, one island per pad, three pylon tops, nothing on the lid.

## State (2026-10-15)

| layer | done |
|---|---|
| centurionworld / centuriondevworld | rows applied |
| tc-centurion / tc-centuriondev data/dbc | rows appended (`.bak-eoe-20261015`) |
| maps / vmaps / mmaps 1616 | installed on both realms |
| patch-enUS-A | 1.00174 (DBC rows + terrain) |
| world-terrain | 1.00008 (Light) |
| minimap | not shipped: md5translate.trs lives in the 1.2 GB art base (patch-Y); the block is in the staging dir for the next art publish |

Everything is inert until a worldserver built with `BATTLEGROUND_EOE` starts: an
older binary skips template 886 (`CreateBattleground` default) and logs the two
buff rows as "not a data-driven arena".
