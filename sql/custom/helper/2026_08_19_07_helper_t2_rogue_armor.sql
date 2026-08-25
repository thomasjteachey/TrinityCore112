-- T2 rogue-armour group (2026-08-19): repoint the trained originals at their
-- replacement wrappers. Statements are in exactly the form used inside
-- helper.spMakeLPlusSkillLineAbility and
-- helper.spMakeLPlusPlayerCreateInfoSpellCustom, so they can be folded into
-- those procedure bodies verbatim. Both statements are emitted for every
-- wrapper even where only one currently matches a row.
--
-- Wrapper rows: sql/custom/dbc/2026_08_19_07_dbc_t2_rogue_armor.sql.

-- Crippling Poison rank 1: 3408 -> 90510

-- Crippling Poison rank 2: 11202 -> 90511

-- Sprint rank 1: 2983 -> 90514
update dbc.skilllineability_lplus set spell = 90514 where spell = 2983;
update lplusworld.playercreateinfo_spell_custom set spell = 90514 where spell = 2983;

-- Sprint rank 2: 8696 -> 90515
update dbc.skilllineability_lplus set spell = 90515 where spell = 8696;
update lplusworld.playercreateinfo_spell_custom set spell = 90515 where spell = 8696;

-- Sprint rank 3: 11305 -> 90516
update dbc.skilllineability_lplus set spell = 90516 where spell = 11305;
update lplusworld.playercreateinfo_spell_custom set spell = 90516 where spell = 11305;

-- Cold Blood 14177: NO repoint. It is a TALENT (Talent.dbc 142), not a trained
-- spell; its only SkillLineAbility row (7799, skill 253) is spellbook
-- placement. The wrapper is 14177 itself, rewritten in place in
-- sql/custom/dbc/2026_08_19_07_dbc_t2_rogue_armor.sql, so the talent, the
-- spellbook row, trainers, character_spell/character_talent/character_action
-- and the client's SkillLineAbility all stay exactly as they are. A
-- `... set spell = 90519 where spell = 14177` statement must NOT be folded
-- into the procedures (90519 does not exist).
--
-- Note for the mirror: dbc.skilllineability_lplus (10256 rows, == the binary)
-- has NO rows for Crippling Poison 3408/11202 - the two statements above for
-- them match zero rows there and only matter for the client-side
-- SkillLineAbility lineage / playercreateinfo. Sprint's rows are 7638/7639/
-- 7640 (SupercededBySpell 8696 / 11305 / 0): with `spell` repointed to the
-- wrappers, SupercededBySpell still names the ORIGINAL next rank. The server
-- only reads that column as a "client knows about supersede" flag, but the
-- client hides a rank from the spellbook only if it knows the spell named
-- there - so the chain is repointed as well (same table, same form):
update dbc.skilllineability_lplus set SupercededBySpell = 90515 where SupercededBySpell = 8696;
update dbc.skilllineability_lplus set SupercededBySpell = 90516 where SupercededBySpell = 11305;

-- NOTE (2026-08-19 review): 3408/11202 (Crippling Poison coat) and 14177 (Cold Blood)
-- are deliberately NOT repointed - the coat keeps its id and swaps the enchant by
-- script; Cold Blood is a talent rewritten in place. Only the Sprint ranks wrap.
