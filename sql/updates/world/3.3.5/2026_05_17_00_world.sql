-- Attach Stoneclaw Totem Effect script so the pulse removes stealth/prowl from hit targets.
DELETE FROM `spell_script_names` WHERE `ScriptName`='spell_sha_stoneclaw_totem_effect';
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(-5729, 'spell_sha_stoneclaw_totem_effect');
