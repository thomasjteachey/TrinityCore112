# Tanaris battleground tooling

Supporting scripts for the Tanaris deathmatch battleground
(`BATTLEGROUND_TRT` = 104, map 1620). Map 1620 is a clone of Kalimdor (map 1)
reused as an instanced battleground.

A DBC change has to land in **both** the `dbc.*_lplus` SQL mirrors and every
binary `.dbc`. They are separate sources of truth and drift silently. The SQL
side is `sql/custom/dbc/2026_08_06_00_dbc_tanaris_battleground.sql`; the binary
side is `tanaris_dbc.py` below.

## `clone_map_data.sh`

Copies Kalimdor's server-side terrain to map 1620 — `maps/`, `vmaps/` and
`mmaps/` alike. None of those formats embed the map id (they key off the
filename), so a byte copy under the new name is a complete clone and **no
extractor or mmap generation is needed**. Run it on the game server; it does
both the dev and prod data trees and skips anything already present.

## `tanaris_dbc.py`

Appends the map/area/battlemaster/bracket/graveyard/worldmap rows to the six
binary DBCs. Point it at any number of `dbc` directories:

```bash
python3 tanaris_dbc.py /home/brokilodeluxe/wow/servers/tc-lplus-dev/data/dbc
```

- `--dry-run` validates headers and reports what would change, writing nothing.
- `--restore` puts back the `.bak-tanaris` copy first, which is how to re-apply
  after changing a value — the tool appends rows, it does not rewrite them.

It refuses to touch a file whose field count or record size does not match the
expected layout, so a DBC from a different client build fails loudly instead of
being corrupted.

Directories that need it: both server `data/dbc` trees, the local workspace at
`wow/data/dbc/lplus`, and a staging copy for whatever gets packed into the
client patch.

`WorldMapArea.dbc` is the odd one out: it is **client-side only**. The server
never reads it for battleground logic, so the player arrow stays missing from
the world map until that row reaches a packed client patch, however correct the
database looks.

## `tanaris_tiles.py` and `build_tanaris_terrain.py`

Together these build the private terrain copy the battleground renders, so that
editing it cannot change the live Tanaris zone.

`tanaris_tiles.py` runs on the game server and works out which tiles Tanaris
actually occupies by reading the area map inside each `001*.map` — much tighter
than a bounding box, which would sweep in ocean and neighbouring zones. It grows
that set by a ring of tiles for the horizon and writes `tiles.txt`. Note the
axis swap it handles: a server tile is `%03u<gx><gy>.map` but the client's ADT
for it is `<Map>_<gy>_<gx>.adt`.

`build_tanaris_terrain.py` runs on Windows against the client MPQs and produces
`wow/data/patch-staging/TanarisBG/`, ready to pack:

```bash
python build_tanaris_terrain.py
```

It resolves every tile through the whole MPQ chain and takes the copy from the
**highest-priority** archive that holds it — some Tanaris tiles and the WDT are
already overridden in patch-Y, so reading from `common.MPQ` would ship older
terrain than the client actually uses. It also rewrites the WDT's MAIN grid so
only the shipped tiles are flagged, and appends a section to the minimap's
`md5translate.trs` pointing at the same artwork Kalimdor already uses.

ADTs reference textures and models by absolute path and never name their own map
directory, so renaming the file is a complete rename.

## `set_map_directory.py`

Repoints an existing `Map.dbc` row's `Directory` at a different terrain folder —
how map 1620 was moved from `Kalimdor` onto `TanarisBG`. Separate from
`tanaris_dbc.py` because that tool only appends rows, and because its
`--restore` would undo unrelated repairs made to those files since their backups
were taken.

## `verify_dbc.py`

Decodes the appended rows back out and prints them field by field, using an
independent parse of the same layout. A row that packed wrong shows up here as
garbage rather than as a client crash later.

## `probe_height.py`

Reads real terrain heights out of the server's `.map` tiles, mirroring
`GridMap::getHeightFromFloat`. Every Z in the battleground — starts,
graveyards, gates, buff nodes — was measured with this rather than guessed, and
the arena bounds were drawn from it to keep a mesa on the Alliance side out of
the playable area. Re-run it before moving anything.
