-- Attach Viper Sting spell script so its periodic mana leech breaks crowd control effects that break on damage.
DELETE FROM `spell_script_names` WHERE `ScriptName`='spell_hun_viper_sting';
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(-3034, 'spell_hun_viper_sting');
