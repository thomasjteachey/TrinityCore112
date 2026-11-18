-- Violet Hold boon expansion: four permanent record auras.
--
-- 94402 Overflow       - healer-only; core turns 25% of overhealing into a
--                        second heal, logged under this spell.
-- 94403 Second Opinion - run-wide record; grants three broker-hand rerolls.
-- 94404 Crucible       - run-wide record; deathless clears call a common-only
--                        broker for the following wave.
-- 94405 Primal Echo    - druid artifact; shifting summons the outgoing form.
--
-- All four use Communion (94316), the established visible, permanent,
-- death-persistent, uncancellable one-stack record-aura shape. Their effects
-- live in code. Client-visible names and icons require Spell.dbc to be rebuilt
-- from this mirror and published through the normal Violet Hold patch path.
-- Keep both realm mirrors in lockstep: patch-enUS-A outranks patch-enUS-8 in a
-- shared client, so an L+-only row would still resolve to the question mark.
-- Replayable.

DELETE FROM `spell_lplus` WHERE `ID` BETWEEN 94402 AND 94405;
DELETE FROM `spell_bplus` WHERE `ID` BETWEEN 94402 AND 94405;

DROP TEMPORARY TABLE IF EXISTS `tmp_vhr_new_boon_lplus`;
CREATE TEMPORARY TABLE `tmp_vhr_new_boon_lplus` AS
-- L+'s mirror predates the existing Violet Hold record block; use the
-- byte-compatible B+ Communion row instead of silently creating zero rows.
SELECT * FROM `spell_bplus` WHERE `ID` = 94316;

UPDATE `tmp_vhr_new_boon_lplus` SET
  `ID` = 94402,
  `Name_Lang_enUS` = 'Boon of Overflow',
  `SpellIconID` = 135,
  `Description_Lang_enUS` = 'Twenty-five percent of your overhealing jumps to the lowest-health ally within 40 yards.',
  `AuraDescription_Lang_enUS` = '25% of overhealing jumps to the lowest-health nearby ally.';
INSERT INTO `spell_lplus` SELECT * FROM `tmp_vhr_new_boon_lplus`;

UPDATE `tmp_vhr_new_boon_lplus` SET
  `ID` = 94403,
  `Name_Lang_enUS` = 'Boon of Second Opinion',
  `SpellIconID` = 1644,
  `Description_Lang_enUS` = 'Three times this run, your party may replace every offer in a boon broker''s hand.',
  `AuraDescription_Lang_enUS` = 'Your party has three broker-hand rerolls.';
INSERT INTO `spell_lplus` SELECT * FROM `tmp_vhr_new_boon_lplus`;

UPDATE `tmp_vhr_new_boon_lplus` SET
  `ID` = 94404,
  `Name_Lang_enUS` = 'Boon of the Crucible',
  `SpellIconID` = 156,
  `Description_Lang_enUS` = 'Clearing a wave without a party member dying calls an extra broker for the next wave. That broker offers only common boons.',
  `AuraDescription_Lang_enUS` = 'Deathless waves call an extra common-boon broker.';
INSERT INTO `spell_lplus` SELECT * FROM `tmp_vhr_new_boon_lplus`;

UPDATE `tmp_vhr_new_boon_lplus` SET
  `ID` = 94405,
  `Name_Lang_enUS` = 'Boon of the Primal Echo',
  `SpellIconID` = 285,
  `Description_Lang_enUS` = 'Shapeshifting leaves the form you departed beside you for 12 seconds, fighting or healing in your place.',
  `AuraDescription_Lang_enUS` = 'Shapeshifting leaves a Primal Echo for 12 sec.';
INSERT INTO `spell_lplus` SELECT * FROM `tmp_vhr_new_boon_lplus`;

DROP TEMPORARY TABLE IF EXISTS `tmp_vhr_new_boon_lplus`;

DROP TEMPORARY TABLE IF EXISTS `tmp_vhr_new_boon_bplus`;
CREATE TEMPORARY TABLE `tmp_vhr_new_boon_bplus` AS
SELECT * FROM `spell_bplus` WHERE `ID` = 94316;

UPDATE `tmp_vhr_new_boon_bplus` SET
  `ID` = 94402,
  `Name_Lang_enUS` = 'Boon of Overflow',
  `SpellIconID` = 135,
  `Description_Lang_enUS` = 'Twenty-five percent of your overhealing jumps to the lowest-health ally within 40 yards.',
  `AuraDescription_Lang_enUS` = '25% of overhealing jumps to the lowest-health nearby ally.';
INSERT INTO `spell_bplus` SELECT * FROM `tmp_vhr_new_boon_bplus`;

UPDATE `tmp_vhr_new_boon_bplus` SET
  `ID` = 94403,
  `Name_Lang_enUS` = 'Boon of Second Opinion',
  `SpellIconID` = 1644,
  `Description_Lang_enUS` = 'Three times this run, your party may replace every offer in a boon broker''s hand.',
  `AuraDescription_Lang_enUS` = 'Your party has three broker-hand rerolls.';
INSERT INTO `spell_bplus` SELECT * FROM `tmp_vhr_new_boon_bplus`;

UPDATE `tmp_vhr_new_boon_bplus` SET
  `ID` = 94404,
  `Name_Lang_enUS` = 'Boon of the Crucible',
  `SpellIconID` = 156,
  `Description_Lang_enUS` = 'Clearing a wave without a party member dying calls an extra broker for the next wave. That broker offers only common boons.',
  `AuraDescription_Lang_enUS` = 'Deathless waves call an extra common-boon broker.';
INSERT INTO `spell_bplus` SELECT * FROM `tmp_vhr_new_boon_bplus`;

UPDATE `tmp_vhr_new_boon_bplus` SET
  `ID` = 94405,
  `Name_Lang_enUS` = 'Boon of the Primal Echo',
  `SpellIconID` = 285,
  `Description_Lang_enUS` = 'Shapeshifting leaves the form you departed beside you for 12 seconds, fighting or healing in your place.',
  `AuraDescription_Lang_enUS` = 'Shapeshifting leaves a Primal Echo for 12 sec.';
INSERT INTO `spell_bplus` SELECT * FROM `tmp_vhr_new_boon_bplus`;

DROP TEMPORARY TABLE IF EXISTS `tmp_vhr_new_boon_bplus`;
