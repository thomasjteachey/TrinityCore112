-- Custom-game WorldStateUI compatibility.
-- The server sends the configured score limits through the *_MAX world states;
-- these rows must reference those states instead of fixed or unrelated values.
-- BattlemasterList maps Scarlet Chapel to 1189 and Blackrock Throne to 1230.

UPDATE `dbc`.`worldstateui_lplus`
SET
    `AreaID` = 0,
    `String_Lang_enUS` = '%9000w/%9006w',
    `StateVariable` = 0
WHERE `ID` = 90002 AND `MapID` = 1189;

UPDATE `dbc`.`worldstateui_lplus`
SET
    `AreaID` = 0,
    `String_Lang_enUS` = '%9001w/%9006w',
    `StateVariable` = 0
WHERE `ID` = 90003 AND `MapID` = 1189;

UPDATE `dbc`.`worldstateui_lplus`
SET
    `AreaID` = 0,
    `String_Lang_enUS` = '%9100w/%9106w',
    `StateVariable` = 0
WHERE `ID` = 90021 AND `MapID` = 1230;

UPDATE `dbc`.`worldstateui_lplus`
SET
    `AreaID` = 0,
    `String_Lang_enUS` = '%9101w/%9106w',
    `StateVariable` = 0
WHERE `ID` = 90022 AND `MapID` = 1230;

-- Tol'Viron and Tiger's Peak expose arena rows with world state 3610.
UPDATE `dbc`.`worldstateui_lplus`
SET `StateVariable` = 3610
WHERE `ID` IN (90004, 90005) AND `MapID` = 980;

UPDATE `dbc`.`worldstateui_lplus`
SET `StateVariable` = 3610
WHERE `ID` IN (90006, 90007) AND `MapID` = 1134;

-- Nefarian's Arena uses the Ruins-style arena visibility world state.
UPDATE `dbc`.`worldstateui_lplus`
SET
    `AreaID` = 0,
    `StateVariable` = 3002
WHERE `ID` IN (9300, 9301) AND `MapID` = 1572;

-- Explicit brackets for custom maps that are not part of the stock client
-- PvPDifficulty data. The stock battleground and arena maps keep their full
-- existing bracket sets.
INSERT INTO `dbc`.`pvpdifficulty_lplus` (`ID`, `MapID`, `RangeIndex`, `MinLevel`, `MaxLevel`, `Difficulty`) VALUES
(91189, 1189, 0, 60, 69, 0), -- Scarlet Chapel
(91230, 1230, 0, 60, 69, 0), -- Blackrock Throne
(91191, 1572, 0, 60, 69, 0), -- Nefarian's Arena
(91615, 1615, 0, 60, 69, 0)  -- Obsidian Colosseum
ON DUPLICATE KEY UPDATE
    `MapID` = VALUES(`MapID`),
    `RangeIndex` = VALUES(`RangeIndex`),
    `MinLevel` = VALUES(`MinLevel`),
    `MaxLevel` = VALUES(`MaxLevel`),
    `Difficulty` = VALUES(`Difficulty`);
