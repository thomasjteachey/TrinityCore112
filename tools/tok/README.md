# Temple of Kotmogu

Battleground type **106** (`BATTLEGROUND_TOK`, queue type 20) on map **998**
(`ValleyOfPower`), ported from the Ascension client data. Code:
`src/server/game/Battlegrounds/Zones/BattlegroundTOK.{h,cpp}`.

## Rules (retail Mists of Pandaria)

- 10 a side, brackets 10-19 ... 60-69, first team to **1600** wins.
- Four Orbs of Power on pedestals in the courtyard corners. Click one to carry
  it; one orb per player.
- Every 5 s each carrier scores **2** outside the courtyard, **4** on the raised
  courtyard floor, **8** down in the sunken centre pool (retail's 3/4/5 was too
  flat to notice). A team holding several orbs gets a bonus on its whole tick:
  **+25%** with 2 orbs, **+50%** with 3, **double** with all 4. A killing blow
  on an enemy scores **10**. A carrier gets a centre-screen message saying what
  the orb is worth whenever they move into a different ring.
  Tables: `TOK_RING_POINTS` / `TOK_TEAM_ORB_BONUS_PCT` in `BattlegroundTOK.cpp`.
- Holding an orb is a stacking debuff (Ascension's numbers, per stack: +30%
  damage taken, -7% healing taken, +10% damage done; one stack every 10 s, max
  10). Tune it in `Spell.dbc` 92300-92303, not in code.
- An orb goes straight back to its pedestal when its carrier dies, leaves,
  mounts, turns immune (Divine Shield, Ice Block), gets into a vehicle or
  otherwise loses the holder aura. Carriers cannot stealth.
- Custom games: the lobby's resource limit and resource-rate rules apply.
- Each team starts on a walled platform at the top of a ramp inside an
  MG_NORTHGATEB gatehouse. A gate (mogu_northgate_door_03, entry 300228) closes
  the arch at the foot of the ramp until the match opens. Its spot is the
  gatehouse's own: the WMO's doodad set places the door's open-state collision
  at local (166.46, -26.20, -32.58), carried through each gatehouse placement.
- No loading screen: Map 998's LoadingScreenID is 0 (owner's call), so the
  client shows its default. LoadingScreens row 334 is still in the archives,
  unused.

Scoring rings are rectangles from the temple's WMO placements, with a height
test for the sunken centre (floor z 5.56 against 10.56 for the courtyard).

## Ids

| what | ids |
|---|---|
| Map / areas | 998 / 6051 (zone), 6060, 6061, 6136 - the ids baked into the ADTs. AreaBits 3828-3831 (Ascension's 4490+ overflow the client's 4096-bit field). |
| BattlemasterList | 106 (must equal the type id, which must stay < 256) |
| PvpDifficulty | 90998, 93239-93243 |
| WorldSafeLocs | 52700/52701 starts, 52702-52705 graveyards (Ascension 5978/5979, 6034-6037) |
| WorldStateUI | 90028/90029, states 9500-9505 |
| WorldMapArea | 9534 (art `Interface\WorldMap\TempleofKotmogu\`, terrain-style) |
| LoadingScreens | 334 (row present, not referenced - Map 998 uses none) |
| Spell | 92300-92303 stack debuff, 92304-92307 hidden holder (Ascension 8445xx) |
| SpellIcon / SpellVisual / Kit / KitModelAttach / EffectName | Ascension's own: 6182-6185 / 82278-82281 / 80003-80006 / 80003-80013 / 80024-80028 |
| GameObjectDisplayInfo | 11533 orb, 11960-11963 coloured light columns, 11964 start gate |
| gameobject_template | 300220-300223 orbs (lockless flag stands), 300224-300227 light columns, 300228 start gate (door, faction 114, flags 32) |
| trinity_string | 20120, 20121 |

## Tools

- `tok_dbc.py show | apply <dir> | verify <dir>` - every DBC row, built from
  Ascension's MPQs. Appends only, backs up as `.bak-tok`, refuses an id that is
  already taken by different data.
- `stage_client_files.py <have.json> <out>` - walks the ADT/WMO/M2 reference
  graph and stages every client file the target client lacks (480 files,
  ~70 MB), plus the md5translate block the minimap needs.

## Server terrain

Generated with `tools/mapdata/regen_map_data.ps1 -Maps 998` against a scratch
client (`C:\Projects\Gamedev\wow\data\client-scratch-tok`): hardlinks of the
real client plus a private `Data\patch-Z.MPQ` (the staged files) and
`Data\enUS\patch-enUS-Z.MPQ` (Map/AreaTable with row 998). The second one is
needed because `vmap4extractor` loads the locale patches last, so
patch-enUS-A's Map.dbc otherwise wins and "map 998 is not in Map.dbc".
25 `.map`, 15 vmap tree/tiles + 101 models, 15 mmap files; no file it wrote
existed on the servers with different content.
