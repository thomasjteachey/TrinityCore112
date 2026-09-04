-- Take Beast Rider (89799) off everyone and let the gate re-issue it.
--
-- SkillLineAbility row 22020 was corrected on 2026-09-03 (commit 6c062b6932):
-- AcquireMethod 2 -> 1 and MinSkillLineRank 1 -> 200, so the mount is earned at
-- level 40 instead of handed out at character creation. Verified live on both
-- realms' data/dbc/SkillLineAbility.dbc: skill=50 spell=89799 classmask=4
-- acquire=1 minrank=200.
--
-- That gate self-heals - but only on a SKILL UPDATE, and the eight hunters below
-- 40 who still hold it are all OFFLINE. An offline character never gets one, so
-- they have been sitting on a mount the rule already took away from them.
--
-- Deleting it from EVERYONE is safe, and deliberately simpler than picking out
-- who is entitled. Player::_LoadSkills calls LearnSkillRewardedSpells for every
-- loaded skill on LOGIN (Player.cpp:28178-28179), so the very next login decides
-- it correctly from scratch:
--
--     class skill rank >= 200 (level 40+)  ->  granted
--     class skill rank <  200             ->  removed
--
-- So the one level 41 hunter holding it gets it straight back, and nothing has
-- to guess. No character is left worse off than the rule says they should be.
--
-- Holders when this was written, all Barracks+ (Legionnaire+ had none):
--   Fuct 1, Hnoss 1, Yeyo 1, Better 6, Bowjob 7, Porkchop 14, Turhun 26,
--   Arnin 34  -- all below 40, all offline
--   Baku 41   -- entitled, online; re-granted on next login
--
-- Two of them have totaltime 0: never played, already mounted.
--
-- The action-bar row goes too, or the button survives as a dead icon.
--
-- Re-runnable. Run against bpluscharacters (and lpluscharacters, where it is
-- already a no-op).

CREATE TABLE IF NOT EXISTS `character_spell_bak_beastrider_20260903` AS
SELECT * FROM `character_spell` WHERE `spell` = 89799;

CREATE TABLE IF NOT EXISTS `character_action_bak_beastrider_20260903` AS
SELECT * FROM `character_action` WHERE `action` = 89799 AND `type` = 0;

DELETE FROM `character_spell`  WHERE `spell` = 89799;
DELETE FROM `character_action` WHERE `action` = 89799 AND `type` = 0;

-- To undo:
--   INSERT INTO character_spell  SELECT * FROM character_spell_bak_beastrider_20260903;
--   INSERT INTO character_action SELECT * FROM character_action_bak_beastrider_20260903;
