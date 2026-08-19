-- ===========================================================================
-- T2 set bonuses - shaman/warlock group (NN = 04): skill-line and create-spell
-- repoints for the 13 Shadow Bolt wrappers (Felflame Bolt 90318).
--
-- To be folded into helper.spMakeLPlusSkillLineAbility and
-- helper.spMakeLPlusPlayerCreateInfoSpellCustom, in exactly the form those
-- procedures already use for Purge (370 -> 81324):
--   update dbc.skilllineability_lplus set spell = W where spell = ORIG;
--   update lplusworld.playercreateinfo_spell_custom set spell = W where spell = ORIG;
--
-- ORIG -> W (wrapper), one per rank; the originals stay in Spell.dbc for the
-- creatures that cast them, they just stop being the thing warlocks learn.
-- ===========================================================================

-- Rank 1   686 -> 90420
update dbc.skilllineability_lplus set spell = 90420 where spell = 686;
update lplusworld.playercreateinfo_spell_custom set spell = 90420 where spell = 686;
-- Rank 2   695 -> 90421
update dbc.skilllineability_lplus set spell = 90421 where spell = 695;
update lplusworld.playercreateinfo_spell_custom set spell = 90421 where spell = 695;
-- Rank 3   705 -> 90422
update dbc.skilllineability_lplus set spell = 90422 where spell = 705;
update lplusworld.playercreateinfo_spell_custom set spell = 90422 where spell = 705;
-- Rank 4   1088 -> 90423
update dbc.skilllineability_lplus set spell = 90423 where spell = 1088;
update lplusworld.playercreateinfo_spell_custom set spell = 90423 where spell = 1088;
-- Rank 5   1106 -> 90424
update dbc.skilllineability_lplus set spell = 90424 where spell = 1106;
update lplusworld.playercreateinfo_spell_custom set spell = 90424 where spell = 1106;
-- Rank 6   7641 -> 90425
update dbc.skilllineability_lplus set spell = 90425 where spell = 7641;
update lplusworld.playercreateinfo_spell_custom set spell = 90425 where spell = 7641;
-- Rank 7   11659 -> 90426
update dbc.skilllineability_lplus set spell = 90426 where spell = 11659;
update lplusworld.playercreateinfo_spell_custom set spell = 90426 where spell = 11659;
-- Rank 8   11660 -> 90427
update dbc.skilllineability_lplus set spell = 90427 where spell = 11660;
update lplusworld.playercreateinfo_spell_custom set spell = 90427 where spell = 11660;
-- Rank 9   11661 -> 90428
update dbc.skilllineability_lplus set spell = 90428 where spell = 11661;
update lplusworld.playercreateinfo_spell_custom set spell = 90428 where spell = 11661;
-- Rank 10  25307 -> 90429
update dbc.skilllineability_lplus set spell = 90429 where spell = 25307;
update lplusworld.playercreateinfo_spell_custom set spell = 90429 where spell = 25307;
-- Rank 11  27209 -> 90430
update dbc.skilllineability_lplus set spell = 90430 where spell = 27209;
update lplusworld.playercreateinfo_spell_custom set spell = 90430 where spell = 27209;
-- Rank 12  47808 -> 90431
update dbc.skilllineability_lplus set spell = 90431 where spell = 47808;
update lplusworld.playercreateinfo_spell_custom set spell = 90431 where spell = 47808;
-- Rank 13  47809 -> 90432
update dbc.skilllineability_lplus set spell = 90432 where spell = 47809;
update lplusworld.playercreateinfo_spell_custom set spell = 90432 where spell = 47809;

-- ---------------------------------------------------------------------------
-- Note: the SkillLineAbility rows for all 13 ranks already carry
-- SupercededBySpell = 0 in this fork's SkillLineAbility.dbc (verified in the
-- binary), so rank supersession is entirely `spell_ranks` (chained in
-- 2026_08_19_04_world_t2_shaman_warlock.sql) - nothing else to repoint.
-- Row 6408 (rank 1) keeps its AcquireMethod 2 (learned at creation).
-- ---------------------------------------------------------------------------
