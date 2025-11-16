-- Grant Moonkin Form the ability to move while snared during Starfire
INSERT INTO `spell_custom_attr` (`entry`, `attributes`) VALUES
(24858, 0x02000000)
ON DUPLICATE KEY UPDATE `attributes` = `attributes` | 0x02000000;
