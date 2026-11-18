-- Boon of Resilience: 45% final cap. Its base grant is 6%; the live 1.5x
-- stacking-boon multiplier makes each offer 9%, reaching this cap exactly.

INSERT INTO `spell_lplus`
SELECT b.* FROM `spell_bplus` b
LEFT JOIN `spell_lplus` l ON l.`ID` = b.`ID`
WHERE b.`ID` = 90412 AND l.`ID` IS NULL;

UPDATE `spell_lplus` SET `CumulativeAura` = 45 WHERE `ID` = 90412;
UPDATE `spell_bplus` SET `CumulativeAura` = 45 WHERE `ID` = 90412;
