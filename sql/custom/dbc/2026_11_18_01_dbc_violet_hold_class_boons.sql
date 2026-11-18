-- Nine approved Violet Hold class-artifact record auras. Their mechanics are
-- in VioletHoldClassBoons; these rows provide the permanent boon aura, name,
-- description, and a real stock icon to both client/server DBC lineages.
-- Replayable; keep both realm mirrors byte-semantically aligned.

DELETE FROM `spell_lplus` WHERE `ID` BETWEEN 94406 AND 94414;
DELETE FROM `spell_bplus` WHERE `ID` BETWEEN 94406 AND 94414;

DROP TEMPORARY TABLE IF EXISTS `tmp_vhr_class_boon_lplus`;
CREATE TEMPORARY TABLE `tmp_vhr_class_boon_lplus` AS
SELECT * FROM `spell_bplus` WHERE `ID` = 94316;

UPDATE `tmp_vhr_class_boon_lplus` SET `ID`=94406, `Name_Lang_enUS`='Boon of the Unending Volley', `SpellIconID`=126,
  `Description_Lang_enUS`='Volley never ends. You can act after placing it, and recasting Volley moves it to the new target area.',
  `AuraDescription_Lang_enUS`='Volley is permanent and movable.';
INSERT INTO `spell_lplus` SELECT * FROM `tmp_vhr_class_boon_lplus`;
UPDATE `tmp_vhr_class_boon_lplus` SET `ID`=94407, `Name_Lang_enUS`='Boon of the War Banner', `SpellIconID`=456,
  `Description_Lang_enUS`='Battle Shout plants a permanent banner. Enemies within 30 yards deal 10% more damage and take 10% less damage. Recasting Battle Shout moves the banner.',
  `AuraDescription_Lang_enUS`='Battle Shout plants a movable permanent danger zone.';
INSERT INTO `spell_lplus` SELECT * FROM `tmp_vhr_class_boon_lplus`;
UPDATE `tmp_vhr_class_boon_lplus` SET `ID`=94408, `Name_Lang_enUS`='Boon of the Aspect Savant', `SpellIconID`=1181,
  `Description_Lang_enUS`='All of your Aspects can be active at once. Aspect of the Cheetah and Aspect of the Pack no longer daze their targets.',
  `AuraDescription_Lang_enUS`='All Aspects coexist; Cheetah and Pack cannot daze.';
INSERT INTO `spell_lplus` SELECT * FROM `tmp_vhr_class_boon_lplus`;
UPDATE `tmp_vhr_class_boon_lplus` SET `ID`=94409, `Name_Lang_enUS`='Boon of the Mirror Image', `SpellIconID`=331,
  `Description_Lang_enUS`='Once every 5 seconds, canceling a cast summons a copy of you that finishes the cast.',
  `AuraDescription_Lang_enUS`='Canceled casts are finished by a Mirror Image, once every 5 sec.';
INSERT INTO `spell_lplus` SELECT * FROM `tmp_vhr_class_boon_lplus`;
UPDATE `tmp_vhr_class_boon_lplus` SET `ID`=94410, `Name_Lang_enUS`='Boon of the Mana Conduit', `SpellIconID`=47,
  `Description_Lang_enUS`='Each tick of Evocation restores 5% of every ally''s maximum primary resource within 40 yards, including rage and energy.',
  `AuraDescription_Lang_enUS`='Evocation restores nearby allies'' primary resources.';
INSERT INTO `spell_lplus` SELECT * FROM `tmp_vhr_class_boon_lplus`;
UPDATE `tmp_vhr_class_boon_lplus` SET `ID`=94411, `Name_Lang_enUS`='Boon of the Curseweaver', `SpellIconID`=544,
  `Description_Lang_enUS`='All Curses can coexist. Casting one Curse applies the highest rank you know of every Curse.',
  `AuraDescription_Lang_enUS`='Your Curses coexist and are cast together.';
INSERT INTO `spell_lplus` SELECT * FROM `tmp_vhr_class_boon_lplus`;
UPDATE `tmp_vhr_class_boon_lplus` SET `ID`=94412, `Name_Lang_enUS`='Boon of Patient Growth', `SpellIconID`=64,
  `Description_Lang_enUS`='Your healing-over-time effects pause their ticks and duration while their target is at full health, resuming when the target is injured.',
  `AuraDescription_Lang_enUS`='Healing-over-time pauses on targets at full health.';
INSERT INTO `spell_lplus` SELECT * FROM `tmp_vhr_class_boon_lplus`;
UPDATE `tmp_vhr_class_boon_lplus` SET `ID`=94413, `Name_Lang_enUS`='Boon of Cleanse', `SpellIconID`=321,
  `Description_Lang_enUS`='Every 6 seconds, automatically remove one harmful Magic, Disease, or Poison effect from an ally. This has no range or line-of-sight requirement and triggers no global cooldown.',
  `AuraDescription_Lang_enUS`='Automatically Cleanse one ally every 6 sec.';
INSERT INTO `spell_lplus` SELECT * FROM `tmp_vhr_class_boon_lplus`;
UPDATE `tmp_vhr_class_boon_lplus` SET `ID`=94414, `Name_Lang_enUS`='Boon of the Lightning Rod', `SpellIconID`=62,
  `Description_Lang_enUS`='Every active totem draws a Lightning Bolt from you every 10 seconds. Every enemy in the path between you and that totem is struck.',
  `AuraDescription_Lang_enUS`='Every totem draws a piercing Lightning Bolt every 10 sec.';
INSERT INTO `spell_lplus` SELECT * FROM `tmp_vhr_class_boon_lplus`;
DROP TEMPORARY TABLE `tmp_vhr_class_boon_lplus`;

-- Copy the now-complete L+ rows into B+ so every field, not only the edited
-- strings, remains identical between the two mirror lineages.
INSERT INTO `spell_bplus`
SELECT * FROM `spell_lplus` WHERE `ID` BETWEEN 94406 AND 94414;

-- Pack Leader is now a native 30-yard party area aura: -15% damage taken,
-- all schools. The aura engine handles entering/leaving the radius directly.
UPDATE `spell_lplus` SET `Effect_1`=35, `EffectBasePoints_1`=-16,
  `EffectRadiusIndex_1`=10, `EffectAura_1`=87, `EffectMiscValue_1`=127
WHERE `ID`=94327;
UPDATE `spell_bplus` SET `Effect_1`=35, `EffectBasePoints_1`=-16,
  `EffectRadiusIndex_1`=10, `EffectAura_1`=87, `EffectMiscValue_1`=127
WHERE `ID`=94327;

-- Overflow now transfers all overhealing.
UPDATE `spell_lplus` SET
  `Description_Lang_enUS`='One hundred percent of your overhealing jumps to the lowest-health ally within 40 yards.',
  `AuraDescription_Lang_enUS`='100% of overhealing jumps to the lowest-health nearby ally.'
WHERE `ID`=94402;
UPDATE `spell_bplus` SET
  `Description_Lang_enUS`='One hundred percent of your overhealing jumps to the lowest-health ally within 40 yards.',
  `AuraDescription_Lang_enUS`='100% of overhealing jumps to the lowest-health nearby ally.'
WHERE `ID`=94402;
