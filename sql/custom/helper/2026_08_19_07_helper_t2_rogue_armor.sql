-- T2 rogue-armour group (2026-08-19): repoint the trained originals at their
-- replacement wrappers. Statements are in exactly the form used inside
-- helper.spMakeLPlusSkillLineAbility and
-- helper.spMakeLPlusPlayerCreateInfoSpellCustom, so they can be folded into
-- those procedure bodies verbatim. Both statements are emitted for every
-- wrapper even where only one currently matches a row.
--
-- Wrapper rows: sql/custom/dbc/2026_08_19_07_dbc_t2_rogue_armor.sql.

-- Crippling Poison rank 1: 3408 -> 90510
update dbc.skilllineability_lplus set spell = 90510 where spell = 3408;
update lplusworld.playercreateinfo_spell_custom set spell = 90510 where spell = 3408;

-- Crippling Poison rank 2: 11202 -> 90511
update dbc.skilllineability_lplus set spell = 90511 where spell = 11202;
update lplusworld.playercreateinfo_spell_custom set spell = 90511 where spell = 11202;

-- Sprint rank 1: 2983 -> 90514
update dbc.skilllineability_lplus set spell = 90514 where spell = 2983;
update lplusworld.playercreateinfo_spell_custom set spell = 90514 where spell = 2983;

-- Sprint rank 2: 8696 -> 90515
update dbc.skilllineability_lplus set spell = 90515 where spell = 8696;
update lplusworld.playercreateinfo_spell_custom set spell = 90515 where spell = 8696;

-- Sprint rank 3: 11305 -> 90516
update dbc.skilllineability_lplus set spell = 90516 where spell = 11305;
update lplusworld.playercreateinfo_spell_custom set spell = 90516 where spell = 11305;

-- Cold Blood: 14177 -> 90519
update dbc.skilllineability_lplus set spell = 90519 where spell = 14177;
update lplusworld.playercreateinfo_spell_custom set spell = 90519 where spell = 14177;
