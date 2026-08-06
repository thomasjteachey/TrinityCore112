-- Tanaris deathmatch battleground - world DB template.
--
-- Pairs with:
--   sql/custom/dbc/2026_08_06_00_dbc_tanaris_battleground.sql   (DBC mirrors)
--   src/server/game/Battlegrounds/Zones/BattlegroundTRT.{h,cpp} (BATTLEGROUND_TRT = 104)
--
-- Apply to lplusdevworld while iterating; apply to lplusworld when it ships.
-- lplusdevworld is periodically re-cloned from lplusworld, so this file is the
-- only durable copy of the change - a hand-typed dev edit does not survive.
--
-- No creature or gameobject rows are needed. The gates, the buff nodes and both
-- spirit guides are spawned by the battleground itself in SetupBattleground(),
-- the way the other custom battlegrounds here do it, so there is nothing to
-- place on map 1620 by hand.
--
-- No graveyard_zone rows either: BattlegroundTRT::GetClosestGraveyard overrides
-- the lookup, which is why Scarlet Chapel and the Obsidian Colosseum have none.

-- Replayable.
DELETE FROM battleground_template WHERE ID = 104;
INSERT INTO battleground_template
  (ID, MinPlayersPerTeam, MaxPlayersPerTeam, MinLvl, MaxLvl,
   AllianceStartLoc, AllianceStartO, HordeStartLoc, HordeStartO,
   StartMaxDist, Weight, ScriptName, Comment)
VALUES
  (104,
   2,          -- MinPlayersPerTeam: same 2v2 floor the other custom BGs use,
   10,         -- MaxPlayersPerTeam: matches BattlemasterList 104 MaxGroupSize
   1, 80,      -- bracket, must agree with PvpDifficulty 91620 (widened
               -- 2026-08-06: the server caps at 60, so a 60 floor locked
               -- out every leveling character)
   52500, 0,          -- Alliance start, facing east down the arena toward Horde
   52501, 3.14159,    -- Horde start, facing west toward Alliance
   40,         -- StartMaxDist
   1,          -- Weight (unused: TRT is not in the random BG pool)
   '',
   'Tanaris Deathmatch');
