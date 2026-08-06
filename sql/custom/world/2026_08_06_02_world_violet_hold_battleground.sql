-- Violet Hold survival battleground - world DB template.
--
-- Pairs with:
--   sql/custom/dbc/2026_08_06_02_dbc_violet_hold_battleground.sql (DBC mirrors)
--   src/server/game/Battlegrounds/Zones/BattlegroundVHR.{h,cpp}   (BATTLEGROUND_VHR = 105)
--
-- Apply to lplusdevworld while iterating; apply to lplusworld when it ships.
-- lplusdevworld is periodically re-cloned from lplusworld, so this file is the
-- only durable copy of the change - a hand-typed dev edit does not survive.
--
-- No creature or gameobject rows are needed. The eight cell doors and the
-- prison seal are spawned by BattlegroundVHR::SetupBattleground() from the
-- coordinates measured off the stock map-608 spawns, so there is nothing to
-- place on map 1608 by hand. No spirit guides exist at all: death is final for
-- the run.
--
-- No graveyard_zone rows either, and none may ever be added for map 1608 -
-- a reachable graveyard would let a "final" death turn into a corpse run.

-- Replayable.
DELETE FROM battleground_template WHERE ID = 105;
INSERT INTO battleground_template
  (ID, MinPlayersPerTeam, MaxPlayersPerTeam, MinLvl, MaxLvl,
   AllianceStartLoc, AllianceStartO, HordeStartLoc, HordeStartO,
   StartMaxDist, Weight, ScriptName, Comment)
VALUES
  (105,
   1,          -- MinPlayersPerTeam: a lone player may run the gauntlet
   40,         -- MaxPlayersPerTeam: the enemy side must hold the final wave.
               -- Humans are still capped at 10 by BattlemasterList 105's
               -- MaxGroupSize plus the queue handing every group its own
               -- instance and never refilling it.
   60, 69,     -- bracket, must agree with PvpDifficulty 91608
   52520, 0.027,      -- party start: the entrance landing, facing the chamber
   52521, 3.211,      -- enemy nominal start: chamber centre (clones are moved
                      -- into their cells by the wave driver immediately)
   0,          -- StartMaxDist
   1,          -- Weight (unused: VHR is not in the random BG pool)
   '',
   'The Violet Hold Gauntlet');
