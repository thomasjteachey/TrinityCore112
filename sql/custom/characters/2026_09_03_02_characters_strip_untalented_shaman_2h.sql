-- Take 2H axes and maces off shamans who never had the talent.
--
-- The weapon master handed these out with no class gate at all: spells 15985
-- and 15987 had NO SkillLineAbility rows, and IsSpellFitByClassAndRace returns
-- true for a spell with none. The gate is in place now, but a gate does not
-- undo what was already bought.
--
-- The talent is 16269 "Two-Handed Axes and Maces", which teaches skills 197 and
-- 199 itself. Holding it is the test for whether a shaman is entitled to these,
-- so a shaman WITH it keeps everything and is not touched.
--
-- Both the SKILL and the weapon-skill SPELLS go. Removing only the spells would
-- be undone on the next skill update: SkillLineAbility row 3213 grants spell 197
-- whenever skill 172 is present, and its ClassMask legitimately includes shaman
-- because the talent route has to work. The skill row is the thing that has to
-- leave.
--
-- Counted 2026-09-03 on bpluscharacters: two characters affected, Claude (12)
-- and Rotar (10), both with skill 172 and no talent, both offline at the time.
-- Korgul, Riverwind and Tempestra hold 16269 and are deliberately untouched.
--
-- RUN WITH THOSE CHARACTERS OFFLINE. A logged-in character writes its in-memory
-- skill and spell lists over these rows on its next save.
--
-- Re-runnable.

CREATE TABLE IF NOT EXISTS `character_skills_bak_shaman2h_20260903` AS
SELECT cs.* FROM `character_skills` cs
  JOIN `characters` c ON c.guid = cs.guid
 WHERE c.class = 7 AND cs.skill IN (160, 172)
   AND NOT EXISTS (SELECT 1 FROM `character_spell` t
                    WHERE t.guid = c.guid AND t.spell = 16269);

CREATE TABLE IF NOT EXISTS `character_spell_bak_shaman2h_20260903` AS
SELECT sp.* FROM `character_spell` sp
  JOIN `characters` c ON c.guid = sp.guid
 WHERE c.class = 7 AND sp.spell IN (197, 199, 15985, 15987)
   AND NOT EXISTS (SELECT 1 FROM `character_spell` t
                    WHERE t.guid = c.guid AND t.spell = 16269);

-- The talent holders are resolved through a DERIVED table. Referencing
-- character_spell directly in the subquery of a DELETE against character_spell
-- is MySQL error 1093 ("can't specify target table for update in FROM clause");
-- wrapping it materialises the set first and is allowed.
DELETE sp FROM `character_spell` sp
  JOIN `characters` c ON c.guid = sp.guid
 WHERE c.class = 7 AND sp.spell IN (197, 199, 15985, 15987)
   AND sp.guid NOT IN (SELECT guid FROM
        (SELECT DISTINCT guid FROM `character_spell` WHERE spell = 16269) AS talented);

DELETE cs FROM `character_skills` cs
  JOIN `characters` c ON c.guid = cs.guid
 WHERE c.class = 7 AND cs.skill IN (160, 172)
   AND cs.guid NOT IN (SELECT guid FROM
        (SELECT DISTINCT guid FROM `character_spell` WHERE spell = 16269) AS talented);

-- To undo:
--   INSERT INTO character_skills SELECT * FROM character_skills_bak_shaman2h_20260903;
--   INSERT INTO character_spell  SELECT * FROM character_spell_bak_shaman2h_20260903;
