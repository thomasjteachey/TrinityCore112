-- Violet Hold boon tuning. Keep the two realm mirrors aligned so whichever
-- client patch wins load order advertises the same caps and Tempo value.

-- L+'s mirror predates the later Violet Hold boon rows. The repo master DBC
-- already has them; seed the missing mirror rows from B+ before tuning them.
INSERT INTO `spell_lplus`
SELECT b.* FROM `spell_bplus` b
LEFT JOIN `spell_lplus` l ON l.`ID` = b.`ID`
WHERE b.`ID` IN (90414, 94318) AND l.`ID` IS NULL;

UPDATE `spell_lplus` SET `CumulativeAura` = 95 WHERE `ID` = 90233;
UPDATE `spell_bplus` SET `CumulativeAura` = 95 WHERE `ID` = 90233;

UPDATE `spell_lplus` SET `CumulativeAura` = 40 WHERE `ID` = 90414;
UPDATE `spell_bplus` SET `CumulativeAura` = 40 WHERE `ID` = 90414;

UPDATE `spell_lplus` SET
  `CumulativeAura` = 4,
  `EffectBasePoints_1` = 124,
  `Description_Lang_enUS` = 'Your global cooldown is $s1 ms shorter, to no less than 0.5 sec.',
  `AuraDescription_Lang_enUS` = 'Your global cooldown is $s1 ms shorter, to no less than 0.5 sec.'
WHERE `ID` = 94318;

UPDATE `spell_bplus` SET
  `CumulativeAura` = 4,
  `EffectBasePoints_1` = 124,
  `Description_Lang_enUS` = 'Your global cooldown is $s1 ms shorter, to no less than 0.5 sec.',
  `AuraDescription_Lang_enUS` = 'Your global cooldown is $s1 ms shorter, to no less than 0.5 sec.'
WHERE `ID` = 94318;
