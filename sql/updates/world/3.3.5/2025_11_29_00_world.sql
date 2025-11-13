--
-- Add DisableLootTag toggle for creature_template
--

ALTER TABLE `creature_template`
  ADD COLUMN `DisableLootTag` tinyint unsigned NOT NULL DEFAULT '0' AFTER `RegenHealth`;
