-- Align PvPvE spawn points with dedicated player positions
SET @currentDb := DATABASE();

-- Drop legacy CreatureEntry column if it exists
SET @hasCreatureEntry := (
    SELECT COUNT(*)
    FROM INFORMATION_SCHEMA.COLUMNS
    WHERE TABLE_SCHEMA = @currentDb
      AND TABLE_NAME = 'pvpve_dungeon_spawn'
      AND COLUMN_NAME = 'CreatureEntry');
SET @dropCreatureSql := IF(@hasCreatureEntry > 0,
    'ALTER TABLE `pvpve_dungeon_spawn` DROP COLUMN `CreatureEntry`',
    'DO 0');
PREPARE stmt FROM @dropCreatureSql;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

-- Drop legacy RespawnSeconds column if it exists
SET @hasRespawn := (
    SELECT COUNT(*)
    FROM INFORMATION_SCHEMA.COLUMNS
    WHERE TABLE_SCHEMA = @currentDb
      AND TABLE_NAME = 'pvpve_dungeon_spawn'
      AND COLUMN_NAME = 'RespawnSeconds');
SET @dropRespawnSql := IF(@hasRespawn > 0,
    'ALTER TABLE `pvpve_dungeon_spawn` DROP COLUMN `RespawnSeconds`',
    'DO 0');
PREPARE stmt FROM @dropRespawnSql;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

-- Reposition the Stockades PvPvE spawn points into clear areas
UPDATE `pvpve_dungeon_spawn`
SET `MapId` = 34,
    `PositionX` = 52.573,
    `PositionY` = -0.411,
    `PositionZ` = -20.213,
    `Orientation` = 4.69
WHERE `TemplateId` = 1 AND `SpawnIndex` = 0;

UPDATE `pvpve_dungeon_spawn`
SET `MapId` = 34,
    `PositionX` = 90.944,
    `PositionY` = -33.215,
    `PositionZ` = -20.219,
    `Orientation` = 1.57
WHERE `TemplateId` = 1 AND `SpawnIndex` = 1;

UPDATE `pvpve_dungeon_spawn`
SET `MapId` = 34,
    `PositionX` = 90.944,
    `PositionY` = 33.215,
    `PositionZ` = -20.219,
    `Orientation` = 4.71
WHERE `TemplateId` = 1 AND `SpawnIndex` = 2;
