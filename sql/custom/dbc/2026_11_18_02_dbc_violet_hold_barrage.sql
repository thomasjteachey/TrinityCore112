-- Barrage is now a hunter-centered full-range sweep. Target selection and
-- crowd-control protection live in VioletHoldClassBoons/Spell.cpp; these
-- strings keep both client/server DBC lineages honest about the mechanic.

UPDATE `spell_lplus` SET
  `Description_Lang_enUS`='Multi-Shot hits every non-crowd-controlled target within range of you, regardless of where you aim.',
  `AuraDescription_Lang_enUS`='Multi-Shot hits every non-crowd-controlled target within range of you, regardless of where you aim.'
WHERE `ID`=94348;

UPDATE `spell_bplus` SET
  `Description_Lang_enUS`='Multi-Shot hits every non-crowd-controlled target within range of you, regardless of where you aim.',
  `AuraDescription_Lang_enUS`='Multi-Shot hits every non-crowd-controlled target within range of you, regardless of where you aim.'
WHERE `ID`=94348;
