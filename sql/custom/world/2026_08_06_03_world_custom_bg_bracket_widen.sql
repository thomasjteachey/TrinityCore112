-- Widen the Tanaris and Violet Hold level gates to 1-80.
--
-- battleground_template MinLvl/MaxLvl feed Battleground::SetLevelRange, and
-- the join handler refuses a player above GetMaxLevel() outright. Both rows
-- shipped as 60-69; on this 60-cap server that locked out every leveling
-- character (the matching PvpDifficulty bracket is widened by
-- 2026_08_06_03_dbc_custom_bg_bracket_widen.sql).
--
-- Row 104 (Tanaris) only exists in lplusdevworld today; the UPDATE is a no-op
-- where the row is absent, so this applies cleanly to both world DBs.
--
-- Replayable.
UPDATE battleground_template SET MinLvl = 1, MaxLvl = 80 WHERE ID IN (104, 105);

-- The custom battlegrounds now use six level bands (10-19 through 60-69).
-- These template bounds are the outer access gate; PvpDifficulty supplies the
-- actual bracket selected by the queue.
UPDATE battleground_template SET MinLvl = 10, MaxLvl = 69 WHERE ID IN (100, 101, 105);
