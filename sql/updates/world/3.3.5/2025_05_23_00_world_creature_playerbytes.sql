CREATE TABLE IF NOT EXISTS `creature_playerbytes` (
  `guid` int unsigned NOT NULL,
  `playerBytes` int unsigned NOT NULL DEFAULT 0,
  `playerBytes2` int unsigned NOT NULL DEFAULT 0,
  PRIMARY KEY (`guid`),
  CONSTRAINT `fk_creature_playerbytes_guid` FOREIGN KEY (`guid`) REFERENCES `creature` (`guid`) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
