-- Custom PvPvE dungeon tables
CREATE TABLE IF NOT EXISTS `pvpve_dungeon_template` (
  `Id` INT UNSIGNED NOT NULL,
  `MapId` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `Name` VARCHAR(100) NOT NULL DEFAULT '',
  `Enabled` TINYINT(1) NOT NULL DEFAULT 1,
  `MinLevel` TINYINT UNSIGNED NOT NULL DEFAULT 1,
  `MaxLevel` TINYINT UNSIGNED NOT NULL DEFAULT 60,
  `MaxTeams` TINYINT UNSIGNED NOT NULL DEFAULT 1,
  `MinPlayersPerTeam` TINYINT UNSIGNED NOT NULL DEFAULT 1,
  `MaxPlayersPerTeam` TINYINT UNSIGNED NOT NULL DEFAULT 5,
  `MaxRuntimeSecs` INT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`Id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

CREATE TABLE IF NOT EXISTS `pvpve_dungeon_spawn` (
  `TemplateId` INT UNSIGNED NOT NULL,
  `SpawnIndex` TINYINT UNSIGNED NOT NULL,
  `CreatureEntry` INT UNSIGNED NOT NULL,
  `MapId` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `PositionX` FLOAT NOT NULL DEFAULT 0,
  `PositionY` FLOAT NOT NULL DEFAULT 0,
  `PositionZ` FLOAT NOT NULL DEFAULT 0,
  `Orientation` FLOAT NOT NULL DEFAULT 0,
  `RespawnSeconds` INT UNSIGNED NOT NULL DEFAULT 30,
  PRIMARY KEY (`TemplateId`, `SpawnIndex`),
  CONSTRAINT `FK_pvpve_dungeon_spawn_template` FOREIGN KEY (`TemplateId`) REFERENCES `pvpve_dungeon_template` (`Id`) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

INSERT INTO `pvpve_dungeon_template` (`Id`, `MapId`, `Name`, `Enabled`, `MinLevel`, `MaxLevel`, `MaxTeams`, `MinPlayersPerTeam`, `MaxPlayersPerTeam`, `MaxRuntimeSecs`)
VALUES
  (1, 34, 'Stockades PvPvE Brawl', 1, 20, 30, 3, 3, 5, 900)
ON DUPLICATE KEY UPDATE
  `MapId` = VALUES(`MapId`),
  `Name` = VALUES(`Name`),
  `Enabled` = VALUES(`Enabled`),
  `MinLevel` = VALUES(`MinLevel`),
  `MaxLevel` = VALUES(`MaxLevel`),
  `MaxTeams` = VALUES(`MaxTeams`),
  `MinPlayersPerTeam` = VALUES(`MinPlayersPerTeam`),
  `MaxPlayersPerTeam` = VALUES(`MaxPlayersPerTeam`),
  `MaxRuntimeSecs` = VALUES(`MaxRuntimeSecs`);

INSERT INTO `pvpve_dungeon_spawn` (`TemplateId`, `SpawnIndex`, `CreatureEntry`, `MapId`, `PositionX`, `PositionY`, `PositionZ`, `Orientation`, `RespawnSeconds`)
VALUES
  (1, 0, 1706, 34, 48.825, -7.284, -20.216, 5.78, 30),
  (1, 1, 1707, 34, 73.594, -10.873, -20.219, 4.60, 30),
  (1, 2, 1708, 34, 96.325, -12.443, -20.219, 3.10, 30)
ON DUPLICATE KEY UPDATE
  `CreatureEntry` = VALUES(`CreatureEntry`),
  `MapId` = VALUES(`MapId`),
  `PositionX` = VALUES(`PositionX`),
  `PositionY` = VALUES(`PositionY`),
  `PositionZ` = VALUES(`PositionZ`),
  `Orientation` = VALUES(`Orientation`),
  `RespawnSeconds` = VALUES(`RespawnSeconds`);
