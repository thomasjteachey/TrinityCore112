-- Custom-game WorldStateUI compatibility.
-- The server sends the configured score limits through the *_MAX world states;
-- these rows must reference those states instead of fixed or unrelated values.

UPDATE `worldstateui_lplus`
SET
    `Icon` = 'Interface\\TargetingFrame\\UI-PVP-Alliance',
    `String_Lang_enUS` = '%9000w/%9006w',
    `Tooltip_Lang_enUS` = 'Alliance score'
WHERE `ID` = 298 AND `MapID` = 801;

UPDATE `worldstateui_lplus`
SET
    `Icon` = 'Interface\\TargetingFrame\\UI-PVP-Horde',
    `String_Lang_enUS` = '%9001w/%9006w',
    `Tooltip_Lang_enUS` = 'Horde score'
WHERE `ID` = 299 AND `MapID` = 801;

UPDATE `worldstateui_lplus`
SET
    `AreaID` = 0,
    `String_Lang_enUS` = '%9100w/%9106w'
WHERE `ID` = 90002 AND `MapID` = 1189;

UPDATE `worldstateui_lplus`
SET
    `AreaID` = 0,
    `String_Lang_enUS` = '%9101w/%9106w'
WHERE `ID` = 90003 AND `MapID` = 1189;

-- Tol'Viron and Tiger's Peak expose arena rows with world state 3610.
UPDATE `worldstateui_lplus`
SET `StateVariable` = 3610
WHERE `ID` IN (90004, 90005) AND `MapID` = 980;

UPDATE `worldstateui_lplus`
SET `StateVariable` = 3610
WHERE `ID` IN (90006, 90007) AND `MapID` = 1134;

-- Nefarian's Arena uses the Ruins-style arena visibility world state.
UPDATE `worldstateui_lplus`
SET `StateVariable` = 3002
WHERE `ID` IN (9300, 9301) AND `MapID` = 1572;
