-- Give every priest their OWN race's abilities, and nobody else's.
--
-- The quest side is fixed in 2026_09_03_02_world_priest_racial_quest_races.sql:
-- all eleven priest racial quests allowed their own race OR'd with the entire
-- opposing faction, so a human could be offered the troll quest in Orgrimmar.
-- A gate does not undo what was already handed out, and it had been running a
-- long time - the contamination is total:
--
--   Hex of Weakness (troll)   held by 21 of 39 priests: Hu, Dw, NE, Tr
--   Wyrm's Shadow  (undead)   held by 20:               Hu, Dw, NE, Un
--   Wisp Form      (nightelf) held by 15:               NE, Un, Tr
--   Fear Ward      (dwarf)    held by 14:               Dw, Un, Tr
--   Chastise       (human)    held by 13:               Hu, Un, Tr
--
-- The correct mapping is read off what each quest actually rewards, cross-checked
-- against the priest racial block in lpluscharacters.createCopyOfChar:
--
--   race            level 10                    level 20
--   Human    (1)    13908 Desperate Prayer      81350 Chastise
--   Dwarf    (3)    81349 Abolish Magic          6346 Fear Ward
--   NightElf (4)    81352 Wisp Form             81351 Elune's Grace
--   Undead   (5)    81357 Wyrm's Shadow          2944 Devouring Curse
--   Troll    (8)     9035 Hex of Weakness       18137 Shadowguard
--
-- Later RANKS are kept for the race that owns the ability - a human with
-- Desperate Prayer rank 5 keeps it. Only the ids priests were actually observed
-- holding are touched; the name families collide with shadow-priest and NPC
-- spells (Devouring Plague 19313+, Shadowguard 28376+, Feedback 6347) and a
-- name-based sweep would have taken those too.
--
-- Abilities this realm REPLACED are removed from everyone, because nothing
-- grants them any more: Starshards, Touch of Weakness, Feedback. None were
-- found on a priest, so those need no statement.
--
-- ⚠ RUN WITH THE PRIESTS OFFLINE. A logged-in character writes its in-memory
-- spell and quest lists over these rows on its next save - the same trap that
-- applied to the shaman two-hander cleanup. 29 of the 39 were online when this
-- was written, 26 of them bots, so this wants the worldserver bounce that the
-- bounty build needs anyway. It is re-runnable: run it again after the restart
-- and it will finish the job.

-- ---------------------------------------------------------------- backups
CREATE TABLE IF NOT EXISTS `character_spell_bak_priest_20260903` AS
SELECT cs.* FROM `character_spell` cs JOIN `characters` c ON c.guid = cs.guid
 WHERE c.class = 5;

CREATE TABLE IF NOT EXISTS `character_qs_rewarded_bak_priest_20260903` AS
SELECT r.* FROM `character_queststatus_rewarded` r JOIN `characters` c ON c.guid = r.guid
 WHERE c.class = 5 AND r.quest IN (5627,5629,5635,5637,5645,5652,5658,5674,5676,5679,5680);

CREATE TABLE IF NOT EXISTS `character_qs_bak_priest_20260903` AS
SELECT q.* FROM `character_queststatus` q JOIN `characters` c ON c.guid = q.guid
 WHERE c.class = 5 AND q.quest IN (5627,5629,5635,5637,5645,5652,5658,5674,5676,5679,5680);

-- ------------------------------------------------- strip foreign racials
-- The universe is spelled out in full in every statement rather than hidden in
-- a variable, so each DELETE is readable on its own and provably bounded.
DELETE cs FROM `character_spell` cs JOIN `characters` c ON c.guid = cs.guid
 WHERE c.class = 5 AND c.race = 1
   AND cs.spell IN (13908,19236,19238,19240,19241,19242,19243,81350,81349,6346,81352,81351,
                    81357,2944,19276,19277,19278,19279,19280,9035,19281,19282,19283,19284,19285,
                    18137,19308,19309,19310,19311,19312)
   AND cs.spell NOT IN (13908,19236,19238,19240,19241,19242,19243,81350);

DELETE cs FROM `character_spell` cs JOIN `characters` c ON c.guid = cs.guid
 WHERE c.class = 5 AND c.race = 3
   AND cs.spell IN (13908,19236,19238,19240,19241,19242,19243,81350,81349,6346,81352,81351,
                    81357,2944,19276,19277,19278,19279,19280,9035,19281,19282,19283,19284,19285,
                    18137,19308,19309,19310,19311,19312)
   AND cs.spell NOT IN (81349,6346);

DELETE cs FROM `character_spell` cs JOIN `characters` c ON c.guid = cs.guid
 WHERE c.class = 5 AND c.race = 4
   AND cs.spell IN (13908,19236,19238,19240,19241,19242,19243,81350,81349,6346,81352,81351,
                    81357,2944,19276,19277,19278,19279,19280,9035,19281,19282,19283,19284,19285,
                    18137,19308,19309,19310,19311,19312)
   AND cs.spell NOT IN (81352,81351);

DELETE cs FROM `character_spell` cs JOIN `characters` c ON c.guid = cs.guid
 WHERE c.class = 5 AND c.race = 5
   AND cs.spell IN (13908,19236,19238,19240,19241,19242,19243,81350,81349,6346,81352,81351,
                    81357,2944,19276,19277,19278,19279,19280,9035,19281,19282,19283,19284,19285,
                    18137,19308,19309,19310,19311,19312)
   AND cs.spell NOT IN (81357,2944,19276,19277,19278,19279,19280);

DELETE cs FROM `character_spell` cs JOIN `characters` c ON c.guid = cs.guid
 WHERE c.class = 5 AND c.race = 8
   AND cs.spell IN (13908,19236,19238,19240,19241,19242,19243,81350,81349,6346,81352,81351,
                    81357,2944,19276,19277,19278,19279,19280,9035,19281,19282,19283,19284,19285,
                    18137,19308,19309,19310,19311,19312)
   AND cs.spell NOT IN (9035,19281,19282,19283,19284,19285,18137,19308,19309,19310,19311,19312);

-- ------------------------------------------------- grant what they are owed
-- Base rank only. Higher ranks come from the trainer, as they always did.
INSERT IGNORE INTO `character_spell` (`guid`,`spell`,`active`,`disabled`)
SELECT c.guid, 13908, 1, 0 FROM `characters` c WHERE c.class=5 AND c.race=1 AND c.level>=10;
INSERT IGNORE INTO `character_spell` (`guid`,`spell`,`active`,`disabled`)
SELECT c.guid, 81350, 1, 0 FROM `characters` c WHERE c.class=5 AND c.race=1 AND c.level>=20;

INSERT IGNORE INTO `character_spell` (`guid`,`spell`,`active`,`disabled`)
SELECT c.guid, 81349, 1, 0 FROM `characters` c WHERE c.class=5 AND c.race=3 AND c.level>=10;
INSERT IGNORE INTO `character_spell` (`guid`,`spell`,`active`,`disabled`)
SELECT c.guid,  6346, 1, 0 FROM `characters` c WHERE c.class=5 AND c.race=3 AND c.level>=20;

INSERT IGNORE INTO `character_spell` (`guid`,`spell`,`active`,`disabled`)
SELECT c.guid, 81352, 1, 0 FROM `characters` c WHERE c.class=5 AND c.race=4 AND c.level>=10;
INSERT IGNORE INTO `character_spell` (`guid`,`spell`,`active`,`disabled`)
SELECT c.guid, 81351, 1, 0 FROM `characters` c WHERE c.class=5 AND c.race=4 AND c.level>=20;

INSERT IGNORE INTO `character_spell` (`guid`,`spell`,`active`,`disabled`)
SELECT c.guid, 81357, 1, 0 FROM `characters` c WHERE c.class=5 AND c.race=5 AND c.level>=10;
INSERT IGNORE INTO `character_spell` (`guid`,`spell`,`active`,`disabled`)
SELECT c.guid,  2944, 1, 0 FROM `characters` c WHERE c.class=5 AND c.race=5 AND c.level>=20;

INSERT IGNORE INTO `character_spell` (`guid`,`spell`,`active`,`disabled`)
SELECT c.guid,  9035, 1, 0 FROM `characters` c WHERE c.class=5 AND c.race=8 AND c.level>=10;
INSERT IGNORE INTO `character_spell` (`guid`,`spell`,`active`,`disabled`)
SELECT c.guid, 18137, 1, 0 FROM `characters` c WHERE c.class=5 AND c.race=8 AND c.level>=20;

-- ------------------------------------------------- forget the wrong quests
-- So the right one can be taken. The quest that belongs to the character's own
-- race is deliberately LEFT completed - they earned that one.
DELETE r FROM `character_queststatus_rewarded` r JOIN `characters` c ON c.guid = r.guid
 WHERE c.class = 5
   AND r.quest IN (5627,5629,5635,5637,5645,5652,5658,5674,5676,5679,5680)
   AND NOT ( (c.race=1 AND r.quest IN (5635,5676))
          OR (c.race=3 AND r.quest IN (5637,5645))
          OR (c.race=4 AND r.quest IN (5627,5629,5674))
          OR (c.race=5 AND r.quest IN (5658,5679))
          OR (c.race=8 AND r.quest IN (5652,5680)) );

DELETE q FROM `character_queststatus` q JOIN `characters` c ON c.guid = q.guid
 WHERE c.class = 5
   AND q.quest IN (5627,5629,5635,5637,5645,5652,5658,5674,5676,5679,5680)
   AND NOT ( (c.race=1 AND q.quest IN (5635,5676))
          OR (c.race=3 AND q.quest IN (5637,5645))
          OR (c.race=4 AND q.quest IN (5627,5629,5674))
          OR (c.race=5 AND q.quest IN (5658,5679))
          OR (c.race=8 AND q.quest IN (5652,5680)) );

-- To undo:
--   DELETE cs FROM character_spell cs JOIN characters c ON c.guid=cs.guid WHERE c.class=5;
--   INSERT INTO character_spell SELECT * FROM character_spell_bak_priest_20260903;
--   INSERT IGNORE INTO character_queststatus_rewarded SELECT * FROM character_qs_rewarded_bak_priest_20260903;
--   INSERT IGNORE INTO character_queststatus          SELECT * FROM character_qs_bak_priest_20260903;
