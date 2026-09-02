-- Violet Hold gets real level brackets instead of one 1-80 catch-all.
--
-- Map 1608 had a single PvpDifficulty row, 91608, spanning levels 1 to 80, so
-- every queue landed in the same bracket and a level 12 could be matched
-- against a level 60. The brackets below mirror Warsong Gulch (map 489) exactly,
-- which is the shape the rest of the game uses:
--
--   RangeIndex 0  10-19      RangeIndex 3  40-49
--   RangeIndex 1  20-29      RangeIndex 4  50-59
--   RangeIndex 2  30-39      RangeIndex 5  60-69
--
-- Stopping at 60-69 rather than continuing to 80 is deliberate: this realm caps
-- at 60, so 60-69 is the level-60 bracket and everything above it would be dead
-- rows. Levels 1-9 are deliberately left with no bracket at all - they cannot
-- queue, which is the intended floor.
--
-- 91608 is reused as the first bracket rather than deleted, so the ID the
-- battleground template comments already reference stays meaningful. The five
-- new IDs sit above the table's current maximum (93223), which matters:
-- PvpDifficulty.dbc IS ID-sorted, so appending them keeps it sorted and no rows
-- have to move.
--
-- battleground_template 105 is widened to match. Those two must agree - the
-- template gates who may queue at all, the PvpDifficulty rows decide which
-- bracket they land in - and a template narrower than the brackets silently
-- refuses players the brackets would have accepted.
--
-- Re-runnable.

DELETE FROM dbc.pvpdifficulty_lplus WHERE MapID = 1608;

INSERT INTO dbc.pvpdifficulty_lplus (ID, MapID, RangeIndex, MinLevel, MaxLevel, Difficulty) VALUES
  (91608, 1608, 0, 10, 19, 0),
  (93224, 1608, 1, 20, 29, 0),
  (93225, 1608, 2, 30, 39, 0),
  (93226, 1608, 3, 40, 49, 0),
  (93227, 1608, 4, 50, 59, 0),
  (93228, 1608, 5, 60, 69, 0);

-- To undo:
--   DELETE FROM dbc.pvpdifficulty_lplus WHERE MapID = 1608;
--   INSERT INTO dbc.pvpdifficulty_lplus (ID, MapID, RangeIndex, MinLevel, MaxLevel, Difficulty)
--     VALUES (91608, 1608, 0, 1, 80, 0);
