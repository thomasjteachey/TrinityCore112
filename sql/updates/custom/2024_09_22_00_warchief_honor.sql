CREATE TABLE IF NOT EXISTS `character_honor_weekly` (
  `guid` INT UNSIGNED NOT NULL,
  `weekly_honor` INT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`guid`),
  CONSTRAINT `fk_character_honor_weekly_characters` FOREIGN KEY (`guid`) REFERENCES `characters` (`guid`) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

CREATE TABLE IF NOT EXISTS `warchief_honor` (
  `id` TINYINT UNSIGNED NOT NULL DEFAULT 1,
  `current_warchief_guid` INT UNSIGNED NOT NULL DEFAULT 0,
  `current_warchief_name` VARCHAR(12) NOT NULL DEFAULT '',
  `last_warchief_guid` INT UNSIGNED NOT NULL DEFAULT 0,
  `last_warchief_name` VARCHAR(12) NOT NULL DEFAULT '',
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

INSERT IGNORE INTO `warchief_honor` (`id`) VALUES (1);
