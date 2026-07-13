-- Obsidian Colosseum: rebuild the two top-frame rows from the Warsong Gulch
-- score/flag rows.  Each row always shows its team's score; StateVariable 2
-- adds DynamicIcon immediately after the score while that team carries the
-- shared flag.

DROP TEMPORARY TABLE IF EXISTS `obc_worldstateui`;
CREATE TEMPORARY TABLE `obc_worldstateui` LIKE `worldstateui_lplus`;

INSERT INTO `obc_worldstateui`
SELECT *
FROM `worldstateui_lplus`
WHERE (`ID` = 2 AND `MapID` = 489)
   OR (`ID` = 3 AND `MapID` = 489);

UPDATE `obc_worldstateui`
SET
    `MapID` = 1615,
    `AreaID` = 0,
    `String_Lang_enUS` = CASE `ID`
        WHEN 2 THEN '%9200w/%9206w'
        ELSE '%9201w/%9206w'
    END,
    `String_Lang_enGB` = '',
    `String_Lang_koKR` = '',
    `String_Lang_frFR` = '',
    `String_Lang_deDE` = '',
    `String_Lang_enCN` = '',
    `String_Lang_zhCN` = '',
    `String_Lang_enTW` = '',
    `String_Lang_zhTW` = '',
    `String_Lang_esES` = '',
    `String_Lang_esMX` = '',
    `String_Lang_ruRU` = '',
    `String_Lang_ptPT` = '',
    `String_Lang_ptBR` = '',
    `String_Lang_itIT` = '',
    `String_Lang_Unk` = '',
    `String_Lang_Mask` = 0,
    `Tooltip_Lang_enUS` = CASE `ID`
        WHEN 2 THEN 'Alliance score'
        ELSE 'Horde score'
    END,
    `Tooltip_Lang_enGB` = '',
    `Tooltip_Lang_koKR` = '',
    `Tooltip_Lang_frFR` = '',
    `Tooltip_Lang_deDE` = '',
    `Tooltip_Lang_enCN` = '',
    `Tooltip_Lang_zhCN` = '',
    `Tooltip_Lang_enTW` = '',
    `Tooltip_Lang_zhTW` = '',
    `Tooltip_Lang_esES` = '',
    `Tooltip_Lang_esMX` = '',
    `Tooltip_Lang_ruRU` = '',
    `Tooltip_Lang_ptPT` = '',
    `Tooltip_Lang_ptBR` = '',
    `Tooltip_Lang_itIT` = '',
    `Tooltip_Lang_Unk` = '',
    `Tooltip_Lang_Mask` = 0,
    `StateVariable` = CASE `ID` WHEN 2 THEN 9203 ELSE 9204 END,
    `DynamicIcon` = CASE `ID`
        WHEN 2 THEN 'Interface\\WorldStateFrame\\AllianceFlag'
        ELSE 'Interface\\WorldStateFrame\\HordeFlag'
    END,
    `DynamicTooltip_Lang_enUS` = CASE `ID`
        WHEN 2 THEN 'Alliance has the flag'
        ELSE 'Horde has the flag'
    END,
    `DynamicTooltip_Lang_enGB` = '',
    `DynamicTooltip_Lang_koKR` = '',
    `DynamicTooltip_Lang_frFR` = '',
    `DynamicTooltip_Lang_deDE` = '',
    `DynamicTooltip_Lang_enCN` = '',
    `DynamicTooltip_Lang_zhCN` = '',
    `DynamicTooltip_Lang_enTW` = '',
    `DynamicTooltip_Lang_zhTW` = '',
    `DynamicTooltip_Lang_esES` = '',
    `DynamicTooltip_Lang_esMX` = '',
    `DynamicTooltip_Lang_ruRU` = '',
    `DynamicTooltip_Lang_ptPT` = '',
    `DynamicTooltip_Lang_ptBR` = '',
    `DynamicTooltip_Lang_itIT` = '',
    `DynamicTooltip_Lang_Unk` = '',
    `DynamicTooltip_Lang_Mask` = 0,
    `ID` = CASE `ID` WHEN 2 THEN 9400 ELSE 9401 END;

DELETE FROM `worldstateui_lplus`
WHERE `ID` IN (9400, 9401);

INSERT INTO `worldstateui_lplus`
SELECT * FROM `obc_worldstateui`;

DROP TEMPORARY TABLE `obc_worldstateui`;
