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

## `wmo2m2.py`, `mint_building_display.py`, `wire_workshop_twin.py`

The RTS building pattern: a GameObject for collision plus a creature "twin"
wearing the same model for targeting. Creature displays can only reference M2
models, so WMO buildings need converting first.

`wmo2m2.py` converts a WMO to a static M2 directly -- no Blender. It merges
the group geometry, re-buckets triangles per material into contiguous submesh
ranges, builds the collision mesh from every face including the WMO's
invisible collision-only ones, and copies every fussy convention (bone,
sequence, batch flags, lookup shapes) from a donor doodad the client
demonstrably renders. Dropped by design: doodad decorations, baked interior
lighting, portals.

`mint_building_display.py` registers the converted model in
GameObjectDisplayInfo (11000), CreatureModelData (4000) and
CreatureDisplayInfo (40000), reading the geo box out of the M2 itself. The
CreatureModelData layout was derived EMPIRICALLY -- documentation-from-memory
put an int where the file has a second string; only trust a layout after
checking that every row's value resolves to a string start.

`wire_workshop_twin.py` runs on the game server: stages the client DBCs,
creates and seeds the three `_lplus` mirrors from the minted binaries (so
mirror and binary are born agreeing), probes the heightmap for flat ground,
and spawns the template rows and the test pair.

### Client delivery rules for creature displays (hard-won)

- **patch-F shadows patch-enUS-8** for `CreatureDisplayInfo.dbc` and
  `CreatureModelData.dbc` — it is the HD-creatures pack and carries its own
  copies with 145 extra models. Creature display rows must be minted into
  patch-F's copies and shipped in **patch-Z** (which loads above F), or the
  client renders checkered cubes while the identical rows sit unread in
  enUS-8. `GameObjectDisplayInfo.dbc` is not in patch-F, so GO rows ride
  enUS-8 normally — which is why a GO can render a model while the creature
  twin cubes.
- **v264 M2 animation tracks are nested per-sequence arrays**
  (`M2Array<M2Array<T>>`). A flat v256-style transparency track makes the
  creature renderer read an inner count of 0 and draw the model at weight 0:
  loads fine, fully invisible — while the gameobject renderer, which never
  evaluates the track, draws the same file perfectly.
- `CreatureModelData.ModelName` must spell the model **`.mdx`**.
- Building-sized creatures need `creature_model_info.CombatReach` ≈ footprint
  radius (the workshop uses 45), or melee only connects at the model's center.

## THE RTS BUILDING TEMPLATE (final, proven in-client 2026-08-08)

A building is two co-located spawns sharing one converted model:

1. **GO shell — collision, invisible.** `wmo2m2.py --collision-only` emits the
   model with zero render geometry and the full collision mesh (the stock
   FakeCollision construction). Registered as its own GO display; the GO
   template wears that display. Solid to players, draws nothing.
2. **Creature twin — every pixel, the kill target.** Full conversion at exact
   scale 1.0 (nothing to z-fight against), `npc_rts_building` AI (never
   retaliates, drops combat 5 s after the last hit), CombatReach ≈ footprint
   radius, PACIFIED not used (stripped at load).
3. **Garrison layer.** `VehicleId` 1000 — a byte-clone of Stampy's kit 121
   (flags 0x4008E002; `mint_vehicle.py`) — + npcflag SPELLCLICK +
   `npc_spellclick_spells` row with `user_type 1` so only FRIENDLY units can
   board; the occupant's action bar comes from `creature_template_spell` rows.
   Ownership = faction swap: hostile buildings are targets, friendly ones are
   garrisons, the same template serves both. NEVER set
   VEHICLE_FLAG_FIXED_POSITION (0x200000): it breaks possess-exits on this
   client (locked camera), which is why immobility is built from parts
   instead (next point).
4. **Immobility — EXACT zero, no epsilons (user rule): the rider is a
   PASSENGER, never the mover.** Kit 1000's only seat is the custom seat
   **90000** (`seat_tool.py mint <VehicleSeat.dbc> 1705 90000 800`): seat
   1705 minus CAN_CONTROL (0x800), keeping CAN_CAST (0x20000000, the
   "gunner" vehicle-UI flag) and CAN_ENTER_OR_EXIT. No possess ⇒ the client
   never movers the building ⇒ steering, turning and jumping are impossible
   by construction. Vehicle.cpp sends the bar for CAN_CAST non-control seats
   and PetHandler.cpp routes the ridden-vehicle casts (both custom, this
   fork). VehicleSeat.dbc must reach the client (patch-Z) and both servers;
   mirror `dbc.vehicleseat_lplus` (full 58-field layout).
   Graveyard of possess-seat immobilizers, so nobody retries them:
   - Stun (aura 9454): pins perfectly but the client DEAD-BUTTONS Leave
     Vehicle while its mover is stunned, and a stunned vehicle cannot cast.
   - FacingLimit fields (18/19): per-input clamps that ACCUMULATE — an
     epsilon still lets the rider slowly spin the building.
   - Vehicle.dbc TurnSpeed 0: ignored for non-turret kits.
   - VEHICLE_FLAG_FIXED_POSITION (the stock turret answer, on kits 160/244):
     breaks possess-exits on this client (locked camera), four data points.
   Kept as server-side belts: permanent MOD_ROOT aura **42716** "Self Root
   Forever (No Visual)" in `creature_template_addon` (root never gates exits
   or casting) and `SetSpeed(MOVE_TURN_RATE, 0)` in npc_rts_building.
5. **Door faces +X** (`--rotate`, derived from the WMO's portal bearings), so
   a spawn orientation aims the gate. Dismounts land in front of it via
   `vehicle_seat_addon` (seat 90000: +35 yd along facing, ExitParamValue 1).

Worked example: entries 900001 (shell) + 900116 (twin), displays 11001/11000 +
40000, sql/custom/world/2026_08_08_00_world_tanaris_workshop_twin.sql.
