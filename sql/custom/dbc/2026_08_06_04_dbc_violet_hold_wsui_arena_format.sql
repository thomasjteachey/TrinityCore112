-- Reformat the Violet Hold top-frame readout to match the custom arenas.
--
-- The first version showed bare numbers behind PvP icons; the arenas'
-- convention (rows 90100+ - "Green Team: %3600w Players Remaining") is plain
-- labelled text, which is what players expect an arena-style scoreboard to
-- say. Also moves the rows onto StateVariable 9400, the battleground's own
-- show flag, instead of "always visible".
--
-- The canonical row values live in 2026_08_06_02_dbc_violet_hold_battleground.sql
-- (already updated); this migration re-applies just the WorldStateUI section
-- for databases where the old-format rows are already present. The binary
-- DBCs need the same change: strip ids 90025-90027 with
-- tools/violet_hold/strip_dbc_rows.py, then re-append with vhr_dbc.py.
--
-- Replayable.
DELETE FROM dbc.worldstateui_lplus WHERE ID IN (90025, 90026, 90027);
INSERT INTO dbc.worldstateui_lplus
  (ID, MapID, AreaID, PhaseShift, Icon,
   String_Lang_enUS, String_Lang_Mask, Tooltip_Lang_enUS, Tooltip_Lang_Mask,
   StateVariable, Type, DynamicIcon, DynamicTooltip_Lang_Mask,
   ExtendedUI, ExtendedUIStateVariable_1, ExtendedUIStateVariable_2, ExtendedUIStateVariable_3)
VALUES
  (90025, 1608, 0, 0, '',
   'Defenders: %9401w Players Remaining', 16712190, '', 16712190,
   9400, 0, '', 16712188,
   '', 0, 0, 0),
  (90026, 1608, 0, 0, '',
   'Dark Reflections: %9402w Remaining', 16712190, '', 16712190,
   9400, 0, '', 16712188,
   '', 0, 0, 0),
  (90027, 1608, 0, 0, '',
   'Wave: %9403w', 16712190, '', 16712190,
   9400, 0, '', 16712188,
   '', 0, 0, 0);
