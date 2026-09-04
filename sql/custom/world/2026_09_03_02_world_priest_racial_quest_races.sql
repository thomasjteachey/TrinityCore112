-- Priest racial quests are offered to their OWN race, and nobody else.
--
-- Every one of the eleven live priest racial quests had its AllowableRaces set
-- to its own race OR'd with the ENTIRE OPPOSING FACTION. Not a stray bit - the
-- same mistake eleven times, exactly:
--
--   classic Alliance block = 1|4|8|64  = 77   (Human, Dwarf, Night Elf, Gnome)
--   classic Horde block    = 2|16|32|128 = 178 (Orc, Undead, Tauren, Troll)
--
--   5635 Desperate Prayer  Human    1 | 178 = 179
--   5637 Abolish Magic     Dwarf    4 | 178 = 182
--   5627 Elune's Grace     NightElf 8 | 178 = 186
--   5658 Wyrm's Shadow     Undead  16 |  77 =  93
--   5652 Hex of Weakness   Troll  128 |  77 = 205
--
-- So a HUMAN priest was eligible for the TROLL quest, whose giver Ur'kyo stands
-- in Orgrimmar. That is the reported bug, and it ran in every direction at once:
-- 21 of the realm's 39 priests hold Hex of Weakness, 20 hold Wyrm's Shadow.
--
-- 5645 carried 1206 = 4 | 178 | 1024, the same error plus Draenei.
--
-- The race each quest belongs to is not a guess: it is read off the ability the
-- quest actually hands out, cross-checked against the priest racial block in
-- lpluscharacters.createCopyOfChar.
--
--   Human     10 Desperate Prayer 13908   20 Chastise      81350
--   Dwarf     10 Abolish Magic    81349   20 Fear Ward      6346
--   NightElf  10 Wisp Form        81352   20 Elune's Grace 81351
--   Undead    10 Wyrm's Shadow    81357   20 Devouring Curse 2944
--   Troll     10 Hex of Weakness   9035   20 Shadowguard   18137
--
-- These eleven are the set refresh_classic_quests_except_priest deliberately
-- protects from the classicmangos refresh, so this edit is not overwritten by
-- the next quest refresh - which is also why the corruption survived so long.
--
-- WORLD table: needs `.reload quest_template` or a restart.
-- Re-runnable.

CREATE TABLE IF NOT EXISTS `quest_template_bak_priestraces_20260903` AS
SELECT `ID`, `LogTitle`, `AllowableRaces` FROM `quest_template`
 WHERE `ID` IN (5627,5629,5635,5637,5645,5652,5658,5674,5676,5679,5680);

UPDATE `quest_template` SET `AllowableRaces` =   1 WHERE `ID` IN (5635, 5676);   -- Human
UPDATE `quest_template` SET `AllowableRaces` =   4 WHERE `ID` IN (5637, 5645);   -- Dwarf
UPDATE `quest_template` SET `AllowableRaces` =   8 WHERE `ID` IN (5627, 5629, 5674); -- Night Elf
UPDATE `quest_template` SET `AllowableRaces` =  16 WHERE `ID` IN (5658, 5679);   -- Undead
UPDATE `quest_template` SET `AllowableRaces` = 128 WHERE `ID` IN (5652, 5680);   -- Troll

-- Verify: every row should read its own single race bit and nothing else.
-- SELECT ID, LogTitle, MinLevel, AllowableRaces FROM quest_template
--  WHERE ID IN (5627,5629,5635,5637,5645,5652,5658,5674,5676,5679,5680)
--  ORDER BY MinLevel, ID;
--
-- To undo:
--   UPDATE quest_template q JOIN quest_template_bak_priestraces_20260903 b ON b.ID = q.ID
--      SET q.AllowableRaces = b.AllowableRaces;
