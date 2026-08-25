-- Diminished: one permanent, stacking handicap debuff for Violet Hold.
--
--   90201  Diminished   the debuff - this is the only id anything should apply
--   90202  Diminished   hidden model-scale companion, see below
--
-- Every stack is one percent less damage done, one percent less healing done,
-- and one percent MORE damage taken. Up to 100 stacks, so the wave tiers are a
-- matter of how many stacks are applied rather than which spell is picked.
--
-- BASE POINTS ARE PER STACK, NOT TOTALS. AuraEffect::CalculateAmount
-- (SpellAuraEffects.cpp) multiplies the amount by the stack count AFTER the
-- script's calc-amount handlers run:
--
--     GetBase()->CallScriptEffectCalcAmountHandlers(this, amount, ...);
--     amount *= GetBase()->GetStackAmount();
--
-- An earlier version had the script return the total (-stacks), which the
-- engine then multiplied again - so a 25-stack clone took 625% extra damage
-- instead of 25% and shrank past the 0.1 scale floor. Stock Fire Vulnerability
-- is the model to copy: 3% per stack in the DBC, engine does the rest. Nothing
-- computes amounts in script any more.
--
-- Why there is a companion spell: a 3.3.5 spell has exactly three effect slots
-- and this needs four auras. Damage done, damage taken and healing done fill
-- the parent, so scale rides on 90202 at the same one-percent-per-stack rate,
-- with the companion mirroring the parent's stack count exactly.
-- Unit::RecalculateObjectScale floors player scale at 0.1, so even 100 stacks
-- leaves a clickable speck. spell_gen_diminished applies, stack-syncs and
-- removes it; nothing should ever apply 90202 directly.
--
-- Effect 2 was MOD_INCREASE_HEALTH_PERCENT (133) and is now damage taken. The
-- health version was invisible in play: the clone is set to full health after
-- it is built and that aura preserves health PERCENT, so a weakened clone still
-- showed a completely full bar.
--
-- MiscValue must stay 127 on effects 1 and 2 - auras 79 and 87 are read through
-- GetTotalAuraMultiplierByMiscMask against the damage school, so a zero mask
-- would silently apply to nothing.
--
-- EffectDieSides is 0, NOT the donor's 1. Amounts resolve as
-- basePoints + rand(1..DieSides) and DieSides 1 adds a flat +1, which would
-- turn the per-stack -1 into 0 and make the whole thing inert.
--
-- Donor 23505 Berserking: three APPLY_AURA effects already, and DispelType and
-- Mechanic are both 0 - which is what makes this undispellable and immune to
-- mechanic-clearing trinkets.
--
-- AttributesExB 0x04000000 SPELL_ATTR2_UNAFFECTED_BY_AURA_SCHOOL_IMMUNE is what
-- stops Divine Shield and Ice Block eating it. Those carry
-- SPELL_ATTR1_DISPEL_AURAS_ON_IMMUNITY, so on application they sweep every
-- non-positive aura whose school the immunity covers - see
-- SpellInfo::ApplyAllSpellImmunitiesTo. SpellInfo::CanDispelAura returns false
-- unconditionally for auras holding this bit, which is why it is used here in
-- preference to SPELL_ATTR0_UNAFFECTED_BY_INVULNERABILITY (0x20000000): that
-- one is overridden whenever the dispelling spell carries it too, so a
-- Mass Dispel style effect would still strip the handicap. Neither Divine
-- Shield (642) nor Ice Block (45438) holds 0x20000000 today, but the ATTR2
-- route does not depend on that staying true. It also lets the debuff land on
-- a target already immune to new aura applications, which is what a clone
-- spawning under a shield needs.
--
-- Player object scale is floored at 0.1 in Unit::RecalculateObjectScale.
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
  `AuraDescription_Lang_enUS` = 'Damage done and healing done reduced, and damage taken increased.',
  `DurationIndex`             = 21,    -- -1, permanent
  `CumulativeAura`            = 100,   -- max stacks
  `Attributes`                = 256,        -- hide in combat log only
  `AttributesExB`             = 67108864,   -- 0x04000000, see note below
  `SpellIconID`               = 543,   -- Spell_Shadow_CurseOfMannoroth
  `SpellVisualID_1`           = 0,     -- donor's 6943 is Berserking's red flash
  `SpellVisualID_2`           = 0,
  `Effect_1` = 6, `EffectAura_1` = 79,  `EffectBasePoints_1` = -1, `EffectDieSides_1` = 0, `EffectMiscValue_1` = 127, `EffectMechanic_1` = 0, `ImplicitTargetA_1` = 25,
  `Effect_2` = 6, `EffectAura_2` = 87,  `EffectBasePoints_2` =  1, `EffectDieSides_2` = 0, `EffectMiscValue_2` = 127, `EffectMechanic_2` = 0, `ImplicitTargetA_2` = 25,
  `Effect_3` = 6, `EffectAura_3` = 136, `EffectBasePoints_3` = -1, `EffectDieSides_3` = 0, `EffectMiscValue_3` = 0,   `EffectMechanic_3` = 0, `ImplicitTargetA_3` = 25;
INSERT INTO `spell_lplus` SELECT * FROM `tmp_dim`;

-- 90202 - hidden model-scale helper, mirroring the parent's stack count. 0x80 SPELL_ATTR0_HIDDEN_CLIENTSIDE keeps
-- it out of the aura bar so the player sees one debuff icon, not two.
UPDATE `tmp_dim` SET
  `ID`                        = 90202,
  `Name_Lang_enUS`            = 'Diminished',
  `AuraDescription_Lang_enUS` = 'Size reduced.',
  `Attributes`                = 384,        -- 0x100 hide in combat log | 0x80 hidden in UI
  `AttributesExB`             = 67108864,   -- 0x04000000, same immunity opt-out as the parent
  `Effect_1` = 6, `EffectAura_1` = 61, `EffectBasePoints_1` = -1, `EffectDieSides_1` = 0, `EffectMiscValue_1` = 0, `EffectMechanic_1` = 0, `ImplicitTargetA_1` = 25,
  `Effect_2` = 0, `EffectAura_2` = 0, `EffectBasePoints_2` = 0, `EffectDieSides_2` = 0, `EffectMiscValue_2` = 0, `EffectMechanic_2` = 0, `ImplicitTargetA_2` = 0,
  `Effect_3` = 0, `EffectAura_3` = 0, `EffectBasePoints_3` = 0, `EffectDieSides_3` = 0, `EffectMiscValue_3` = 0, `EffectMechanic_3` = 0, `ImplicitTargetA_3` = 0;
INSERT INTO `spell_lplus` SELECT * FROM `tmp_dim`;

DROP TEMPORARY TABLE `tmp_dim`;
