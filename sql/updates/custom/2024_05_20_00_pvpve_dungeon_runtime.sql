ALTER TABLE `pvpve_dungeon_template`
    ADD COLUMN IF NOT EXISTS `MaxRuntimeSecs` INT UNSIGNED NOT NULL DEFAULT 0 AFTER `MaxPlayersPerTeam`;

UPDATE `pvpve_dungeon_template`
SET `MaxRuntimeSecs` = 900
WHERE `Id` = 1 AND `MaxRuntimeSecs` = 0;
