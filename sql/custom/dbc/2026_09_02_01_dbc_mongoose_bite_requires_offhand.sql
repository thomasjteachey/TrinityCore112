-- Mongoose Bite (81285) could be cast bare-handed, or with nothing in the off
-- hand, despite its tooltip saying "Requires a weapon in both hands".
--
-- The spell's ATTRIBUTES were already correct. AttributesExC is 0x01000400,
-- which is SPELL_ATTR3_MAIN_HAND (bit 10, 0x400) and SPELL_ATTR3_REQ_OFFHAND
-- (bit 24, 0x01000000) - both set, exactly as intended.
--
-- The problem is the gate that reads them, Spell::CheckItems:
--
--     if (... && m_spellInfo->EquippedItemClass >= 0 && !IsWeaponRequirementWaived(...))
--     {
--         if (HasAttribute(SPELL_ATTR3_MAIN_HAND))  ...
--         if (HasAttribute(SPELL_ATTR3_REQ_OFFHAND)) ...
--     }
--
-- 81285 carried EquippedItemClass = -1, so the whole block was skipped and both
-- weapon requirements were never evaluated. The requirement had been put on the
-- wrong half of the pair: the damage effect 81286 already has
-- EquippedItemClass = 2 with subclass mask 173555, which is why THAT half was
-- gated properly.
--
-- Setting the class on 81285 makes the existing attributes take effect. The
-- subclass mask is copied from 81286 rather than invented, so the two halves of
-- the same ability cannot disagree about which weapons qualify.
--
-- Applied to both realms - Barracks Plus and Legionnaire Plus - and the binary
-- Spell.dbc must be patched to match on each, plus the client patch. The
-- mirrors alone change nothing: the server reads the binary.
--
-- Re-runnable.

UPDATE dbc.spell_bplus s
    JOIN dbc.spell_bplus ref ON ref.ID = 81286
    SET s.EquippedItemClass = 2,
        s.EquippedItemSubclass = ref.EquippedItemSubclass
    WHERE s.ID = 81285;

UPDATE dbc.spell_lplus s
    JOIN dbc.spell_lplus ref ON ref.ID = 81286
    SET s.EquippedItemClass = 2,
        s.EquippedItemSubclass = ref.EquippedItemSubclass
    WHERE s.ID = 81285;

-- To undo:
--   UPDATE dbc.spell_bplus SET EquippedItemClass = -1, EquippedItemSubclass = 0 WHERE ID = 81285;
--   UPDATE dbc.spell_lplus SET EquippedItemClass = -1, EquippedItemSubclass = 0 WHERE ID = 81285;
