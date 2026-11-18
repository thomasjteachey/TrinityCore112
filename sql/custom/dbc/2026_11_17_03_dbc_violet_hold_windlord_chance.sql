-- Boon of the Windlord gives both Windfury sources a fixed 50% proc chance.
-- Seed L+'s older mirror from B+ so both realm DBCs advertise the same value.

INSERT INTO `spell_lplus`
SELECT b.* FROM `spell_bplus` b
LEFT JOIN `spell_lplus` l ON l.`ID` = b.`ID`
WHERE b.`ID` = 94401 AND l.`ID` IS NULL;

UPDATE `spell_lplus` SET
  `Description_Lang_enUS` = 'Windfury Weapon and Windfury Totem have a 50% chance to proc.',
  `AuraDescription_Lang_enUS` = 'Windfury Weapon and Windfury Totem have a 50% chance to proc.'
WHERE `ID` = 94401;

UPDATE `spell_bplus` SET
  `Description_Lang_enUS` = 'Windfury Weapon and Windfury Totem have a 50% chance to proc.',
  `AuraDescription_Lang_enUS` = 'Windfury Weapon and Windfury Totem have a 50% chance to proc.'
WHERE `ID` = 94401;
