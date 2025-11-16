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
  `MapId` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `PositionX` FLOAT NOT NULL DEFAULT 0,
  `PositionY` FLOAT NOT NULL DEFAULT 0,
  `PositionZ` FLOAT NOT NULL DEFAULT 0,
  `Orientation` FLOAT NOT NULL DEFAULT 0,
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

INSERT INTO `pvpve_dungeon_spawn` (`TemplateId`, `SpawnIndex`, `MapId`, `PositionX`, `PositionY`, `PositionZ`, `Orientation`)
VALUES
  -- Center staging area in the entry hall
  (1, 0, 34, 52.573, -0.411, -20.213, 4.69),
  -- South cell block (left wing)
  (1, 1, 34, 90.944, -33.215, -20.219, 1.57),
  -- North cell block (right wing)
  (1, 2, 34, 90.944, 33.215, -20.219, 4.71)
ON DUPLICATE KEY UPDATE
  `MapId` = VALUES(`MapId`),
  `PositionX` = VALUES(`PositionX`),
  `PositionY` = VALUES(`PositionY`),
  `PositionZ` = VALUES(`PositionZ`),
  `Orientation` = VALUES(`Orientation`);
