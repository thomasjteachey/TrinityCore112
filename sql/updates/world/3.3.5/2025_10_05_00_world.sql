--
-- Add guild and equipment storage for creature mirror image data
--

ALTER TABLE `creature_playerbytes`
    ADD COLUMN `guildId` int unsigned NOT NULL DEFAULT '0' AFTER `playerBytes2`,
    ADD COLUMN `visibleItem0` int unsigned NOT NULL DEFAULT '0' AFTER `guildId`,
    ADD COLUMN `visibleItem1` int unsigned NOT NULL DEFAULT '0' AFTER `visibleItem0`,
    ADD COLUMN `visibleItem2` int unsigned NOT NULL DEFAULT '0' AFTER `visibleItem1`,
    ADD COLUMN `visibleItem3` int unsigned NOT NULL DEFAULT '0' AFTER `visibleItem2`,
    ADD COLUMN `visibleItem4` int unsigned NOT NULL DEFAULT '0' AFTER `visibleItem3`,
    ADD COLUMN `visibleItem5` int unsigned NOT NULL DEFAULT '0' AFTER `visibleItem4`,
    ADD COLUMN `visibleItem6` int unsigned NOT NULL DEFAULT '0' AFTER `visibleItem5`,
    ADD COLUMN `visibleItem7` int unsigned NOT NULL DEFAULT '0' AFTER `visibleItem6`,
    ADD COLUMN `visibleItem8` int unsigned NOT NULL DEFAULT '0' AFTER `visibleItem7`,
    ADD COLUMN `visibleItem9` int unsigned NOT NULL DEFAULT '0' AFTER `visibleItem8`,
    ADD COLUMN `visibleItem10` int unsigned NOT NULL DEFAULT '0' AFTER `visibleItem9`,
    ADD COLUMN `visibleItem11` int unsigned NOT NULL DEFAULT '0' AFTER `visibleItem10`,
    ADD COLUMN `visibleItem12` int unsigned NOT NULL DEFAULT '0' AFTER `visibleItem11`,
    ADD COLUMN `visibleItem13` int unsigned NOT NULL DEFAULT '0' AFTER `visibleItem12`,
    ADD COLUMN `visibleItem14` int unsigned NOT NULL DEFAULT '0' AFTER `visibleItem13`,
    ADD COLUMN `visibleItem15` int unsigned NOT NULL DEFAULT '0' AFTER `visibleItem14`,
    ADD COLUMN `visibleItem16` int unsigned NOT NULL DEFAULT '0' AFTER `visibleItem15`,
    ADD COLUMN `visibleItem17` int unsigned NOT NULL DEFAULT '0' AFTER `visibleItem16`,
    ADD COLUMN `visibleItem18` int unsigned NOT NULL DEFAULT '0' AFTER `visibleItem17`,
    ADD COLUMN `virtualItem0` int unsigned NOT NULL DEFAULT '0' AFTER `visibleItem18`,
    ADD COLUMN `virtualItem1` int unsigned NOT NULL DEFAULT '0' AFTER `virtualItem0`,
    ADD COLUMN `virtualItem2` int unsigned NOT NULL DEFAULT '0' AFTER `virtualItem1`;
