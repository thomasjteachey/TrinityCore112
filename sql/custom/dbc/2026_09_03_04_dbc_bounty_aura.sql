-- Bounty 90701: the stacking debuff that makes a killer worth killing.
--
-- Fifty stacks, fifteen minutes, refreshed by every kill you take part in. See
-- custom_bounty.h for what the system does with it; this row is only how the
-- player sees it.
--
-- Design notes on the fields that are not obvious:
--
--   Attributes 0xA0000000
--     0x80000000 CANT_CANCEL              - not something you right-click off.
--     0x20000000 UNAFFECTED_BY_INVULNERABILITY
--                                         - and not something you shed with
--       Divine Shield, Ice Block or Mass Dispel either. SpellInfo::CanDispelAura
--       tests this bit FIRST and returns false, which takes the aura out of
--       every school-immunity purge in ApplyAllSpellImmunitiesTo. DispelType and
--       Mechanic are both left at 0 so the dispel-immunity and mechanic-immunity
--       branches never match it either. A bounty you could trinket off would not
--       be a bounty.
--
--   AttributesExC 0x00100000 DEATH_PERSISTENT
--     Load-bearing, not decoration. Unit::Kill calls RemoveAllAurasOnDeath and
--     setDeathState long BEFORE any script hook can read the corpse, so without
--     this the stacks are gone before the gold can be worked out. The code
--     removes it explicitly once the debt is recorded.
--
--   AttributesExD 0x00200000 DONT_REMOVE_IN_ARENA
--     Zoning into a battleground strips every non-passive aura that lacks this.
--     A bounty must not be clearable by queueing.
--
--   AttributesExF 0x00000004 IGNORE_CASTER_AURAS  (inherited from the donor)
--     The debuff has to land whatever the killer is standing in.
--
--   DurationIndex 347 = 900000ms. Fifteen minutes.
--
--   EffectBasePoints_1 0 with EffectDieSides_1 1
--     The 3.3.5 client renders $s1 on an aura as (basePoints + DieSides) times
--     the CURRENT STACK COUNT, so this makes $s1 read as the stack count itself
--     - which, at the default Centurion.Bounty.GoldPercentPerStack of 1.0, is
--     also exactly the percentage of gold at stake. One token, both meanings,
--     and it tracks a retune of neither. DieSides 0 would render a bogus range.
--
--   EffectAura_1 4 = SPELL_AURA_DUMMY. It does nothing on its own; every
--   consequence lives in custom_bounty.cpp and the playerbot manager.
--
-- Cloned from 90232 "Boon of Fortitude", which is already a 50-stack
-- self-applied aura with the right shape. Everything the donor carried that is
-- not wanted is overwritten below rather than left to be discovered later.
--
-- Re-runnable. Applies to BOTH realm mirrors: the id is free in each (lplus
-- topped out at 90633, bplus at 90700 for War Mode).

DELIMITER $$

DROP PROCEDURE IF EXISTS dbc.spAddBountyAura $$
CREATE PROCEDURE dbc.spAddBountyAura(IN tableName VARCHAR(64))
BEGIN
    SET @donor = 90232;
    SET @bounty = 90701;

    SET @sql = CONCAT('DELETE FROM dbc.', tableName, ' WHERE ID = ', @bounty);
    PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

    SET @sql = CONCAT('CREATE TEMPORARY TABLE dbc_bounty_tmp LIKE dbc.', tableName);
    PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

    SET @sql = CONCAT('INSERT INTO dbc_bounty_tmp SELECT * FROM dbc.', tableName, ' WHERE ID = ', @donor);
    PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

    UPDATE dbc_bounty_tmp SET
        ID                        = 90701,
        Attributes                = 2684354560,   -- CANT_CANCEL | UNAFFECTED_BY_INVULNERABILITY
        AttributesEx              = 0,
        AttributesExC             = 1048576,      -- DEATH_PERSISTENT
        AttributesExD             = 2097152,      -- DONT_REMOVE_IN_ARENA
        AttributesExE             = 0,
        AttributesExG             = 0,
        DispelType                = 0,            -- nothing to dispel by type
        Mechanic                  = 0,            -- nothing to trinket off
        DurationIndex             = 347,          -- 15 minutes
        CumulativeAura            = 50,
        Effect_1                  = 6,            -- APPLY_AURA
        EffectAura_1              = 4,            -- DUMMY
        EffectBasePoints_1        = 0,
        EffectDieSides_1          = 1,            -- so $s1 renders as the stack count
        EffectRealPointsPerLevel_1 = 0,
        EffectMiscValue_1         = 0,
        EffectMiscValueB_1        = 0,
        EffectMechanic_1          = 0,
        EffectTriggerSpell_1      = 0,
        ImplicitTargetA_1         = 1,            -- TARGET_UNIT_CASTER
        ImplicitTargetB_1         = 0,
        Effect_2                  = 0,
        EffectAura_2              = 0,
        EffectBasePoints_2        = 0,
        EffectDieSides_2          = 0,
        Effect_3                  = 0,
        EffectAura_3              = 0,
        EffectBasePoints_3        = 0,
        EffectDieSides_3          = 0,
        SpellIconID               = 3139,         -- Spell_Shadow_Skull
        ActiveIconID              = 0,
        SpellLevel                = 0,
        BaseLevel                 = 0,
        MaxLevel                  = 0,
        Name_Lang_enUS            = 'Bounty',
        NameSubtext_Lang_enUS     = '',
        Description_Lang_enUS     = 'There is a price on your head.',
        AuraDescription_Lang_enUS = 'There is a price on your head. The wandering rabble hunt you from further off and in greater numbers, and if you die here you will leave $s1% of your gold behind for whoever finds the body.';

    SET @sql = CONCAT('INSERT INTO dbc.', tableName, ' SELECT * FROM dbc_bounty_tmp');
    PREPARE s FROM @sql; EXECUTE s; DEALLOCATE PREPARE s;

    DROP TEMPORARY TABLE dbc_bounty_tmp;
END $$

DELIMITER ;

CALL dbc.spAddBountyAura('spell_lplus');
CALL dbc.spAddBountyAura('spell_bplus');
DROP PROCEDURE dbc.spAddBountyAura;

-- Verify: both should return one row reading Bounty / 50 / 347 / aura 4.
-- SELECT 'lplus' r, ID, Name_Lang_enUS, CumulativeAura, DurationIndex, EffectAura_1, Attributes
--   FROM dbc.spell_lplus WHERE ID = 90701
-- UNION ALL
-- SELECT 'bplus', ID, Name_Lang_enUS, CumulativeAura, DurationIndex, EffectAura_1, Attributes
--   FROM dbc.spell_bplus WHERE ID = 90701;
