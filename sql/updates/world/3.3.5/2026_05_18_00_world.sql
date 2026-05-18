DELETE FROM `spell_script_names` WHERE `spell_id` IN (1953, 89780) AND `ScriptName` IN ('spell_mage_blink', 'spell_mage_time_travel_blink');
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(1953, 'spell_mage_blink'),
(89780, 'spell_mage_time_travel_blink');
