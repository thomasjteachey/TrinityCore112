-- Bind Surprise Bear combo point handling to Maul and Swipe spells.
DELETE FROM `spell_script_names` WHERE `ScriptName` = 'spell_dru_surprise_bear_combo';
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(-6807, 'spell_dru_surprise_bear_combo'), -- Maul (all ranks)
(-779,  'spell_dru_surprise_bear_combo'), -- Swipe (Bear, all ranks)
(62078, 'spell_dru_surprise_bear_combo'); -- Swipe (Cat)
