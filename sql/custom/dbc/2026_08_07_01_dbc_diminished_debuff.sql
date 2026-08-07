-- Diminished: one permanent, stacking handicap debuff for Violet Hold.
--
-- ONE spell, up to 100 stacks, and each stack is one percent off the top:
-- 25 stacks = 75% size / health / damage / healing, 50 stacks = half,
-- 75 stacks = quarter. Any value in 1..100 works, so the tiers are a matter of
-- how many stacks you apply rather than which spell you pick.
--
--   90201  Diminished           the debuff - this is the only id you apply
--   90202  Diminished (healing) hidden helper, see below
--
-- Why there is a helper at all: a 3.3.5 spell has exactly three effect slots
-- and this needs four auras. Damage done, maximum health and model scale fill
-- the three; healing done has nowhere to go, and it cannot be folded into the
-- damage effect because Unit::SpellHealingPctDone only ever reads
-- SPELL_AURA_MOD_HEALING_DONE_PERCENT. So 90202 carries healing alone, is
-- hidden from the aura bar, and spell_gen_diminished applies, removes and
-- stack-syncs it. Nothing should ever apply 90202 directly.
--
-- The stored EffectBasePoints of -1 is the per-stack value and is what applies
-- if the script is not loaded - so an unbuilt core degrades to a 1% debuff
-- rather than something wild. spell_gen_diminished overrides the amount with
-- -stackAmount on every recalculation; Aura::SetStackAmount re-runs
-- CalculateAmount for all effects, which is what makes it track the stack count.
--
-- EffectDieSides is 0, NOT the donor's 1. Amounts resolve as
-- basePoints + rand(1..DieSides) and DieSides 1 adds a flat +1, which would
-- turn -1 into 0 and make the whole thing inert.
--
-- Donor 23505 Berserking: three APPLY_AURA effects already, effect 3 is already
-- MOD_SCALE, effect 1 already has EffectMiscValue 127 (all schools) for
-- MOD_DAMAGE_PERCENT_DONE, and DispelType and Mechanic are 0 - which is what
-- makes this undispellable and immune to mechanic-clearing trinkets.
--
-- All four auras take a signed percentage delta:
--   61  MOD_SCALE                    -25 -> scale 1.0 + (-25/100) = 0.75
--   133 MOD_INCREASE_HEALTH_PERCENT  -25 -> UNIT_MOD_HEALTH TOTAL_PCT 75%
--   79  MOD_DAMAGE_PERCENT_DONE      -25 -> 75% damage done
--   136 MOD_HEALING_DONE_PERCENT     -25 -> 75% healing done
--
-- Player object scale is floored at 0.1 in Unit::RecalculateObjectScale, so
-- even 100 stacks leaves a visible (if tiny) character rather than a null model.
--
-- NOT death-persistent: auras drop on death without SPELL_ATTR3_DEATH_PERSISTENT,
-- so in a wave-survival BG this falls off when the player dies. Add 0x00100000
-- to AttributesExC on both rows if it should survive.
--
-- After applying: regenerate Spell.dbc with spell_dbc.py and repack the client
-- patch, or the debuff shows with no name or icon.

DELETE FROM `spell_lplus` WHERE `ID` IN (90201, 90202, 90203, 90204, 90205, 90206);

DROP TEMPORARY TABLE IF EXISTS `tmp_dim`;
CREATE TEMPORARY TABLE `tmp_dim` AS
SELECT * FROM `spell_lplus` WHERE `ID` = 23505;

-- 90201 - the debuff itself
UPDATE `tmp_dim` SET
  `ID`                        = 90201,
  `Name_Lang_enUS`            = 'Diminished',
  `Description_Lang_enUS`     = '',
  `AuraDescription_Lang_enUS` = 'Size, maximum health, damage done and healing done reduced by $w1%.',
  `DurationIndex`             = 21,    -- -1, permanent
  `CumulativeAura`            = 100,   -- max stacks
  `Attributes`                = 256,   -- hide in combat log only
  `SpellIconID`               = 543,   -- Spell_Shadow_CurseOfMannoroth
  `SpellVisualID_1`           = 0,     -- donor's 6943 is Berserking's red flash
  `SpellVisualID_2`           = 0,
  `Effect_1` = 6, `EffectAura_1` = 79,  `EffectBasePoints_1` = -1, `EffectDieSides_1` = 0, `EffectMiscValue_1` = 127, `EffectMechanic_1` = 0, `ImplicitTargetA_1` = 25,
  `Effect_2` = 6, `EffectAura_2` = 133, `EffectBasePoints_2` = -1, `EffectDieSides_2` = 0, `EffectMiscValue_2` = 0,   `EffectMechanic_2` = 0, `ImplicitTargetA_2` = 25,
  `Effect_3` = 6, `EffectAura_3` = 61,  `EffectBasePoints_3` = -1, `EffectDieSides_3` = 0, `EffectMiscValue_3` = 0,   `EffectMechanic_3` = 0, `ImplicitTargetA_3` = 25;
INSERT INTO `spell_lplus` SELECT * FROM `tmp_dim`;

-- 90202 - hidden healing-done helper. 0x80 SPELL_ATTR0_HIDDEN_CLIENTSIDE keeps
-- it out of the aura bar so the player sees one debuff icon, not two.
UPDATE `tmp_dim` SET
  `ID`                        = 90202,
  `Name_Lang_enUS`            = 'Diminished',
  `AuraDescription_Lang_enUS` = 'Healing done reduced.',
  `Attributes`                = 384,   -- 0x100 hide in combat log | 0x80 hidden in UI
  `Effect_1` = 6, `EffectAura_1` = 136, `EffectBasePoints_1` = -1, `EffectDieSides_1` = 0, `EffectMiscValue_1` = 0, `EffectMechanic_1` = 0, `ImplicitTargetA_1` = 25,
  `Effect_2` = 0, `EffectAura_2` = 0, `EffectBasePoints_2` = 0, `EffectDieSides_2` = 0, `EffectMiscValue_2` = 0, `EffectMechanic_2` = 0, `ImplicitTargetA_2` = 0,
  `Effect_3` = 0, `EffectAura_3` = 0, `EffectBasePoints_3` = 0, `EffectDieSides_3` = 0, `EffectMiscValue_3` = 0, `EffectMechanic_3` = 0, `ImplicitTargetA_3` = 0;
INSERT INTO `spell_lplus` SELECT * FROM `tmp_dim`;

DROP TEMPORARY TABLE `tmp_dim`;
