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

Abilities use stock spells with base-point overrides (`CastSpellExtraArgs.AddSpellBP0/1`); ground
effects are an invisible level-61 trigger (`BBASpawnZone`) casting persistent stock spells.

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
