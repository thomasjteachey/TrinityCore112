# Blood Beneath the Arena tooling

A customer's level-60 5-man dungeon in the cave under the Gurubashi Arena. The cave was already
built into Stranglethorn's ADT `Azeroth_31_56` (map 0); these scripts lift it into its own
instance, **map 1685**, zone **30609** "Blood Beneath the Arena".

Phases 1+2 (instance shell + content data) were applied to **CenturionDev only** on 2026-10-01.
Phase 3 (encounter scripts) is written in `src/server/scripts/Custom/BloodBeneathArena/`
(compile-checked with MSVC `/Zs` + object/dumpbin, **not built, not committed**); its DB rows
(`gen_bba_phase3_sql.py`) and the boon spells (`bba_spells.py`) are applied to CenturionDev and
patch-enUS-A 1.00154. It does nothing in game until a CenturionDev build.

## Phase 3: how the run plays

1. **Var'jun** (caged): gossip "Open the cage" frees him and offers the quest; "Lead on" walks him
   to Blightblood (dialogue) and on to the cleansing pillar, where he channels.
2. **Bloodbound Gong** (by Blightblood): each ring starts the next trial, Sand → Ice → Forest. A
   trial = wave of 3, wave of 4, then the champion + support drummer. The drummer buffs the
   champion (Enrage 8599, 8 s) every 12 s unless CC'd; at 30% the champion frenzies (+25% damage)
   and the drummer heals (3 s interruptible, 3% of the champion's HP, 20% of her mana each); at
   under 20% mana she channels an un-kickable Evocation, which is the burn window. A wipe
   despawns the trial and re-arms the gong.
3. **Blightblood** hands out one boon per earned step (both waves of a trial, each champion,
   Zalvaxa, the twins: 8 total, auras 92100-92107, party-wide, survive death, stripped on leaving).
4. **Zalvaxa**: after the trials, Var'jun "Call out Zalvaxa". She channels and raises Risen and takes
   no damage until a player lands an interrupt, silence or stun on her; then she teleports to the
   middle. Grave Rot raises a Risen where it expires; at 45% the Risen crawl to her for Blood-Fed
   stacks (+10% damage each); a quarter of Risen burst on death.
5. **Twins** (unlocked by Zalvaxa's death): Thraxia halves magic, Malizzia halves physical; within
   15 yd of each other they heal 1%/s and hit 25% harder. The first to "die" lies down for 30 s;
   kill the other in time or she stands up at 30%.
6. **Vraka'ti** (unlocked by the twins): parleys as friendly. Opening **Vraka'ti's Offering**
   (900302) gives that player 50g, fails the quest for everyone inside and expels the party. Choosing
   to fight: at 25% she unmasks and **Anok'Suten** takes over (Web Wrap cocoons, Shadow Web zones,
   spiderling swarm, Drain Life tether, a kickable self-heal at 50%, enrage at 10%). Anok's death
   completes the quest; Var'jun and Blightblood wait at the start.

Ground effects are an invisible level-61 trigger (`BBASpawnZone`) casting persistent spells.

## Phase 4: named ability spells

`bba_ability_spells.py` appends 38 rows (92110-92151), each a clone of the stock spell phase 3
used, renamed to the design doc's ability and re-tuned (`bp`, duration, cast time, school,
stacks), so cast bars, auras and the combat log say Sand Slash, Grave Rot, Blood Reunion, Web
Cocoon... and the tooltip numbers are what lands. Aura-shaped abilities replaced script hacks:
Ritual Rhythm (8 sec), Blood Frenzy (+25%), Blood-Fed (+10% per stack, 10 stacks), Blood Reunion
(+25% and 1%/s heal, refreshed every second while the twins stand together), The Last Thread.
Applied to CenturionDev `data/dbc/Spell.dbc` and patch-enUS-A 1.00155 (each its own lineage).
Still stock on purpose: trash bolts/heals, Frostbolt, Frost Nova, Entangling Roots, Gouge, Blind
(Thraxia), Vanish, Curse of Agony, Fear, Immolate, Rain of Fire, Drain Life (Zalvaxa/Malizzia) -
their stock names already match the design.

## Testing: `.bba` GM commands (inside the instance)

| command | does |
|---|---|
| `.bba status` | stage, each encounter's state, boons earned/handed out |
| `.bba stage <n>` | jump forward: 1 freed, 3 at the pillar (gong armed), 4 trials done, 6 Zalvaxa done, 7 twins done |
| `.bba trial` | start the next trial as if the gong rang |
| `.bba boons` | hand out every earned boon |

Going backwards: `.instance unbind` and re-enter.

| piece | what | where it ran |
|---|---|---|
| `build_bba_terrain.py` | Windows, against the client MPQs: private `BloodBeneathArena.wdt` + `_31_56.adt`, every MCNK areaId → 30609 | local |
| `bba_dbc.py` | appends Map 1685, AreaTable 30609 (AreaBit 3827), MapDifficulty 759, Item 204100 to a DBC dir; each file keeps its own lineage | CenturionDev `data/dbc`, and the archive's own copies inside `bba_publish_a.sh` |
| `bba_clone_server.py` | copies grids 55-57 × 30-32 of map 0 to map 1685 (`maps` with the area grid rewritten, `vmaps`, `mmaps`) | CenturionDev `data` |
| `gen_bba_sql.py` | writes `bba_content.sql` / `bba_rollback.sql` (fresh id ranges only, no `USE` - prepend it) | `centuriondevworld` |
| `bba_publish_a.sh` | adds the terrain + DBC rows to `patch-enUS-A` (check / publish) | published 1.00153 |

## Why the area ids are rewritten

A cloned map keeps the source's baked area ids. Here that was 2177 (Battle Ring, `AREA_FLAG_ARENA`)
and 1741 (Gurubashi Arena), which would make the dungeon free-for-all and run the Gurubashi ring
rules. Both the server `.map` area grids and the client ADT's MCNK areaIds carry 30609 instead.
The cave's own WMOs (ruin stairs 431, excavation platforms 887/889, a railing 812) have
WMOAreaTable rows with area 0, so they fall back to 30609. Only the arena WMO on the surface (568)
keeps 1741/30232.

## ⚠ The customer edits spawns on CenturionDev in game

Since 2026-10-01 the customer moves, adds and deletes spawns in map 1685 on dev (`.npc add`,
`.npc move`, waypoints). **Never re-run `gen_bba_sql.py` / `bba_content.sql` on dev**: its first
block deletes guid ranges 922000-922299 and would wipe that work. Fix things with targeted SQL. Rows
added after playtest 1 use guids 923000+ (prowlers) and path 92214801 (catlord patrol); the
customer's own `.npc add` spawns take max(guid)+1. When promoting to live, copy dev's map-1685 rows
across.

## Playtest 1 (customer sheet, 2026-10-01)

| item | fix |
|---|---|
| Catlord too easy | 14000 HP, two Bloodbound Prowlers (922075) in formation per catlord, 922148 patrols the west corridor (path 92214801) |
| Var'jun runs off route | walks a fixed waypoint list laid over the navmesh (`mmpath.py`), short pathfound legs, a stuck leg hops to its waypoint |
| Var'jun at the cauldron | channelling stance (emote 468) + Water Channeling while the ritual / gong trials run |
| medics / hexers melee | casters chase at 20 yd and never melee; Wand Shot (92152, no mana) filler; medics Dispel (65546) CC off allies; mana x4 |
| Ice / Forest medic stuck in rock | wave mobs spawn on the berserker/hexer spots (known good) or 2.5 yd off them, collision-checked |
| bosses CC-able | champions, twins, Vraka'ti, Anok'Suten immune to stun, knockout, root; Zalvaxa becomes so once awake |
| Venomous Breath 160 vs 360 tooltip | not a bug: AutoBalance scales boss damage by party size (2 of 5 players = ~0.44) |
| mini bosses small | champions scale 1.5, supports 1.25 |
| Forcefield | deleted (gameobject 5331187) |

## Ids

| range | used for |
|---|---|
| creature_template 922000-922071 | 34 copies of stock templates (stock rows are never edited) |
| creature guids 922005-922201 | the customer's mock spawns re-created on map 1685, plus the two Tunnel Watchers (922200 on map 0, 922201 inside) |
| gameobject_template 900300-900301 | Bloodbound Gong (no ZG script/event), Skull Pile (generic) |
| gameobject guids 5331000-5331187 | the customer's 61 cave objects on map 1685 |
| quest 60100, item 204100, gossip/npc_text 60100-60105 | quest, Treasure Box, dialogue |

## Promoting to live (Centurion)

Nothing is on live except the client rows (patch-enUS-A ships to everyone, inert without the server
side). To promote: `bba_dbc.py ~/wow/servers/tc-centurion/data/dbc`, `bba_clone_server.py
~/wow/servers/tc-centurion/data --apply`, apply `bba_content.sql` with `USE centurionworld;` - the
mock spawns it copies come from `centuriondevworld`'s map-0 guids, so on live copy the dev rows
across instead of re-running the spawn block - then a live restart.
