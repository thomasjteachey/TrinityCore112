-- Consecration wrappers (2026-08-20): repoint the trained originals at their
-- replacement wrappers. Statements are in exactly the form used inside
-- helper.spMakeLPlusSkillLineAbility and
-- helper.spMakeLPlusPlayerCreateInfoSpellCustom, so they can be folded into
-- those procedure bodies verbatim.
--
-- THIS IS NOT OPTIONAL. Both procedures REBUILD their tables from the
-- legionnaire base every time they run, so a repoint applied only to
-- dbc.skilllineability_lplus is silently undone the next time anyone runs
-- spMakeLPlusSkillLineAbility - and the paladin gets his six original
-- Consecration buttons back alongside the six wrappers.
--
-- Wrapper rows: sql/custom/dbc/2026_08_20_10_dbc_t2_consecration_wrappers.sql.
-- All SIX ranks: SkillLineAbility has SupercededBySpell = 0 on every one, so a
-- trained paladin keeps them all as separate spellbook buttons.

-- Consecration rank 1: 26573 -> 90526
update dbc.skilllineability_lplus set spell = 90526 where spell = 26573;
update lplusworld.playercreateinfo_spell_custom set spell = 90526 where spell = 26573;

-- Consecration rank 2: 20116 -> 90527
update dbc.skilllineability_lplus set spell = 90527 where spell = 20116;
update lplusworld.playercreateinfo_spell_custom set spell = 90527 where spell = 20116;

-- Consecration rank 3: 20922 -> 90528
update dbc.skilllineability_lplus set spell = 90528 where spell = 20922;
update lplusworld.playercreateinfo_spell_custom set spell = 90528 where spell = 20922;

-- Consecration rank 4: 20923 -> 90529
update dbc.skilllineability_lplus set spell = 90529 where spell = 20923;
update lplusworld.playercreateinfo_spell_custom set spell = 90529 where spell = 20923;

-- Consecration rank 5: 20924 -> 90530
update dbc.skilllineability_lplus set spell = 90530 where spell = 20924;
update lplusworld.playercreateinfo_spell_custom set spell = 90530 where spell = 20924;

-- Consecration rank 6: 27173 -> 90531
update dbc.skilllineability_lplus set spell = 90531 where spell = 27173;
update lplusworld.playercreateinfo_spell_custom set spell = 90531 where spell = 27173;
