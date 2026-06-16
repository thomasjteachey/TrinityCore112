DELETE FROM `spell_script_names` WHERE `spell_id` IN (6770, 2070, 11297) AND `ScriptName` IN ('spell_rog_sap_diagnostic', 'spell_rog_sap');
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(6770, 'spell_rog_sap'),
(2070, 'spell_rog_sap'),
(11297, 'spell_rog_sap');
