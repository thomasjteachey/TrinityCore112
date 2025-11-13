--
-- Replace DisableLootTag column with auxiliary table
--

ALTER TABLE `creature_template`
  DROP COLUMN `DisableLootTag`;

CREATE TABLE IF NOT EXISTS `creature_template_loot_flags` (
  `Entry` int unsigned NOT NULL,
  `DisableLootTag` tinyint unsigned NOT NULL DEFAULT '0',
  PRIMARY KEY (`Entry`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
