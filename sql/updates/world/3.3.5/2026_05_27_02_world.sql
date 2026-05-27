-- Revert CTF top-frame rows to use flag-state worldstates so score rows remain visible.
-- Note: StateVariable governs row visibility; using 1545/1546 hides the entire row at base (state 0).

UPDATE `worldstateui_lplus`
SET `StateVariable` = 2339
WHERE `ID` = 2 AND `MapID` = 489;

UPDATE `worldstateui_lplus`
SET `StateVariable` = 2338
WHERE `ID` = 3 AND `MapID` = 489;

UPDATE `worldstateui_lplus`
SET `StateVariable` = 6111
WHERE `ID` = 313 AND `MapID` = 726;

UPDATE `worldstateui_lplus`
SET `StateVariable` = 6110
WHERE `ID` = 314 AND `MapID` = 726;
