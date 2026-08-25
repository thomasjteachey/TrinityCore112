-- Recharge (90200): the fourth battleground power rune.
--
-- Full copy of 23451 Speed, the cleanest of the three existing rune buffs to
-- clone: a single APPLY_AURA effect, Attributes 0 (so nothing hides the buff
-- icon), DurationIndex 1 = 10s, and ImplicitTargetA_1 25 which is what lets the
-- trap gameobject apply it to whoever walks over it. Restoration and Berserking
-- were poorer donors - Berserking carries three effects and Attributes 256.
--
-- The aura is deliberately inert: EffectAura_1 becomes 4 (SPELL_AURA_DUMMY) and
-- the actual cooldown wipe happens in spell_gen_recharge on aura apply. The 10s
-- duration exists purely so the enemy team can see who picked the rune up.
--
-- EffectBasePoints stays at the donor's semantics: values resolve as
-- basePoints + rand(1..DieSides), so with DieSides 1 a stored 0 resolves to 1.
-- Nothing reads it for a dummy aura, but leaving 99 behind would surface as
-- "$s1" garbage if the aura description were ever rewritten to use it.
--
-- SpellVisualID_1 is left on the donor's 6922. That is Speed's pickup visual -
-- correct family and scale, guaranteed present, but it reads as a speed effect.
-- Swap it for 6942 (Restoration, green) or 6943 (Berserking, red) with a
-- one-column UPDATE if a different flavour is wanted.
--
-- After applying: regenerate the binary Spell.dbc from this table with
-- tools\recolor\itemforge\spell_dbc.py (run --verify first, it must report 0
-- mismatches), then repack the client patch. Table-only edits are wiped by a
-- DB refresh.

DELETE FROM `spell_lplus` WHERE `ID` = 90200;

DROP TEMPORARY TABLE IF EXISTS `tmp_recharge`;
CREATE TEMPORARY TABLE `tmp_recharge` AS
SELECT * FROM `spell_lplus` WHERE `ID` = 23451;

UPDATE `tmp_recharge` SET
  `ID` = 90200,
  `Name_Lang_enUS` = 'Recharge',
  `Description_Lang_enUS` = '',
  `AuraDescription_Lang_enUS` = 'All of your cooldowns have been reset.',
  `EffectAura_1` = 4,
  `EffectBasePoints_1` = 0,
  `SpellIconID` = 58;

INSERT INTO `spell_lplus` SELECT * FROM `tmp_recharge`;
DROP TEMPORARY TABLE `tmp_recharge`;
