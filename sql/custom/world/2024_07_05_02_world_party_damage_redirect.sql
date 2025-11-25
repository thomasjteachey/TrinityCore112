-- Bind the custom Paladin party damage redirect aura to its script implementation.
DELETE FROM `spell_script_names` WHERE `spell_id` = 83256 AND `ScriptName` = 'spell_pal_party_damage_redirect';
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(83256, 'spell_pal_party_damage_redirect');
