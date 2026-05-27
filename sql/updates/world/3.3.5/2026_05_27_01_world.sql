-- Battleground worldstate UI icon visibility: use pickup world states for CTF flag icons
-- WSG/TP state vars (2338/2339, 6110/6111) are always 1 when flags are at base, causing icons to always show.
-- Use *_FLAG_UNK_* vars (1545/1546), which are 1 only while a flag is carried, 0 at base, -1 on ground.

UPDATE `worldstateui_lplus`
SET `StateVariable` = 1546
WHERE `ID` = 2 AND `MapID` = 489;

UPDATE `worldstateui_lplus`
SET `StateVariable` = 1545
WHERE `ID` = 3 AND `MapID` = 489;

UPDATE `worldstateui_lplus`
SET `StateVariable` = 1546
WHERE `ID` = 313 AND `MapID` = 726;

UPDATE `worldstateui_lplus`
SET `StateVariable` = 1545
WHERE `ID` = 314 AND `MapID` = 726;
