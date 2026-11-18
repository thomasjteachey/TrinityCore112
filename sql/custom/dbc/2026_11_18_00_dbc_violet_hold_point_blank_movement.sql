-- Point Blank also lets Aimed Shot and Multi-Shot be cast while moving.
-- Seed L+'s older mirror from B+ so both realm DBCs advertise the same rule.

INSERT INTO `spell_lplus`
SELECT b.* FROM `spell_bplus` b
LEFT JOIN `spell_lplus` l ON l.`ID` = b.`ID`
WHERE b.`ID` = 94346 AND l.`ID` IS NULL;

UPDATE `spell_lplus` SET
  `Description_Lang_enUS` = 'No dead zone: Auto Shot and your shots work in melee range; Auto Shot keeps firing while you move, and you can cast Aimed Shot and Multi-Shot while moving.',
  `AuraDescription_Lang_enUS` = 'No dead zone: Auto Shot and your shots work in melee range; Auto Shot keeps firing while you move, and you can cast Aimed Shot and Multi-Shot while moving.'
WHERE `ID` = 94346;

UPDATE `spell_bplus` SET
  `Description_Lang_enUS` = 'No dead zone: Auto Shot and your shots work in melee range; Auto Shot keeps firing while you move, and you can cast Aimed Shot and Multi-Shot while moving.',
  `AuraDescription_Lang_enUS` = 'No dead zone: Auto Shot and your shots work in melee range; Auto Shot keeps firing while you move, and you can cast Aimed Shot and Multi-Shot while moving.'
WHERE `ID` = 94346;
