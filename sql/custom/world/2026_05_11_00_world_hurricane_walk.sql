-- Flag the custom Hurricane movement aura with the snare-enabled Hurricane channeling attribute.
INSERT INTO `spell_custom_attr` (`entry`, `attributes`) VALUES
(89760, 0x04000000)
ON DUPLICATE KEY UPDATE `attributes` = `attributes` | VALUES(`attributes`);
