CREATE TABLE IF NOT EXISTS `creature_playerbytes` (
  `guid` int unsigned NOT NULL,

  `race` tinyint unsigned NOT NULL DEFAULT 0,
  `class` tinyint unsigned NOT NULL DEFAULT 0,
  `gender` tinyint unsigned NOT NULL DEFAULT 0,
  `playerBytes` int unsigned NOT NULL DEFAULT 0,
  `playerBytes2` int unsigned NOT NULL DEFAULT 0,
  PRIMARY KEY (`guid`),
  CONSTRAINT `fk_creature_playerbytes_guid` FOREIGN KEY (`guid`) REFERENCES `creature` (`guid`) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

ALTER TABLE `creature_playerbytes`
  ADD COLUMN IF NOT EXISTS `race` tinyint unsigned NOT NULL DEFAULT 0 AFTER `guid`,
  ADD COLUMN IF NOT EXISTS `class` tinyint unsigned NOT NULL DEFAULT 0 AFTER `race`,
  ADD COLUMN IF NOT EXISTS `gender` tinyint unsigned NOT NULL DEFAULT 0 AFTER `class`;

