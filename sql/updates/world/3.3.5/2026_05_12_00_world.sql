DELETE FROM `spell_script_names` WHERE `spell_id` IN (12165, 12830, 12831, 12832, 12833) AND `ScriptName`='spell_polearm_staff_outer_aura';
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(12165, 'spell_polearm_staff_outer_aura'),
(12830, 'spell_polearm_staff_outer_aura'),
(12831, 'spell_polearm_staff_outer_aura'),
(12832, 'spell_polearm_staff_outer_aura'),
(12833, 'spell_polearm_staff_outer_aura');

DELETE FROM `spell_dbc` WHERE `Id` IN (89769, 89770, 89771, 89772, 89773);
INSERT INTO `spell_dbc` (`Id`, `Attributes`, `AttributesEx3`, `DurationIndex`, `Effect1`, `EffectImplicitTargetA1`, `EffectApplyAuraName1`, `EffectBasePoints1`, `SpellName`) VALUES
(89769, 64, 1048576, 21, 6, 1, 4, 0, 'Polearm/Staff Inner Aura 1'),
(89770, 64, 1048576, 21, 6, 1, 4, 0, 'Polearm/Staff Inner Aura 2'),
(89771, 64, 1048576, 21, 6, 1, 4, 0, 'Polearm/Staff Inner Aura 3'),
(89772, 64, 1048576, 21, 6, 1, 4, 1, 'Polearm/Staff Inner Aura - Melee Range 1'),
(89773, 64, 1048576, 21, 6, 1, 4, 2, 'Polearm/Staff Inner Aura - Melee Range 2');
