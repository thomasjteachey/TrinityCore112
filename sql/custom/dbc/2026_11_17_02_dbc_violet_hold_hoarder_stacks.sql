-- Boon of the Hoarder has two ranks. The runtime counter is the aura's stack
-- amount in personal-broker mode, so the DBC must permit the second stack.

INSERT INTO `spell_lplus`
SELECT b.* FROM `spell_bplus` b
LEFT JOIN `spell_lplus` l ON l.`ID` = b.`ID`
WHERE b.`ID` = 90294 AND l.`ID` IS NULL;

UPDATE `spell_lplus` SET `CumulativeAura` = 2 WHERE `ID` = 90294;
UPDATE `spell_bplus` SET `CumulativeAura` = 2 WHERE `ID` = 90294;
