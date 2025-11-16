-- Shift the walk-while-casting Starfire support from Moonkin Form to the five ranks of Starlight Wrath.
-- Removing the attribute from Moonkin Form ensures the bonus is now talent-gated.
UPDATE `spell_custom_attr`
SET `attributes` = `attributes` & ~0x02000000
WHERE `entry` = 24858
  AND `attributes` & 0x02000000;

DELETE FROM `spell_custom_attr`
WHERE `entry` = 24858
  AND `attributes` = 0;

-- Flag every Starlight Wrath rank with the snare-enabled Starfire casting attribute.
INSERT INTO `spell_custom_attr` (`entry`, `attributes`) VALUES
(16814, 0x02000000),
(16815, 0x02000000),
(16816, 0x02000000),
(16817, 0x02000000),
(16818, 0x02000000)
ON DUPLICATE KEY UPDATE `attributes` = `attributes` | VALUES(`attributes`);
