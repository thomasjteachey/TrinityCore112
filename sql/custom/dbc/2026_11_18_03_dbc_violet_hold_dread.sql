-- Dread now makes Psychic Scream undispellable and removes it from the
-- diminishing-return system in addition to preventing damage breaks. The
-- mechanics live in VioletHoldClassBoons and the core aura/spell hooks; these
-- strings keep both realm DBC mirrors aligned with the master Spell.dbc.

UPDATE `spell_lplus` SET
  `Description_Lang_enUS`='Damage no longer breaks the fear of your Psychic Scream. Your Psychic Scream is undispellable and ignores diminishing returns.',
  `AuraDescription_Lang_enUS`='Damage no longer breaks the fear of your Psychic Scream. Your Psychic Scream is undispellable and ignores diminishing returns.'
WHERE `ID`=94352;

UPDATE `spell_bplus` SET
  `Description_Lang_enUS`='Damage no longer breaks the fear of your Psychic Scream. Your Psychic Scream is undispellable and ignores diminishing returns.',
  `AuraDescription_Lang_enUS`='Damage no longer breaks the fear of your Psychic Scream. Your Psychic Scream is undispellable and ignores diminishing returns.'
WHERE `ID`=94352;
