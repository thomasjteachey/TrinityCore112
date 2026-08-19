-- ===========================================================================
-- T2 set bonuses - druid / hunter group: skill-line / create-info repoints for
-- the Serpent Sting replacement wrappers (Cinderbite 90334).
--
-- One wrapper per Serpent Sting rank, 90460-90471 (see
-- sql/custom/dbc/2026_08_19_05_dbc_t2_druid_hunter.sql). Exactly the form the
-- stored procedures helper.spMakeLPlusSkillLineAbility and
-- helper.spMakeLPlusPlayerCreateInfoSpellCustom use for the 370 -> 81324 Purge
-- repoint; both statements are emitted for every rank even where only the
-- skill line carries it (playercreateinfo_spell_custom normally lists rank 1
-- only - the extra UPDATEs then match zero rows). Fold these into the
-- procedures.
--
-- Chain (stock rank -> wrapper):
--   1978->90460 13549->90461 13550->90462 13551->90463 13552->90464 13553->90465
--   13554->90466 13555->90467 25295->90468 27016->90469 49000->90470 49001->90471
-- ===========================================================================

-- Serpent Sting Rank 1
update dbc.skilllineability_lplus set spell = 90460 where spell = 1978;
update lplusworld.playercreateinfo_spell_custom set spell = 90460 where spell = 1978;

-- Serpent Sting Rank 2
update dbc.skilllineability_lplus set spell = 90461 where spell = 13549;
update lplusworld.playercreateinfo_spell_custom set spell = 90461 where spell = 13549;

-- Serpent Sting Rank 3
update dbc.skilllineability_lplus set spell = 90462 where spell = 13550;
update lplusworld.playercreateinfo_spell_custom set spell = 90462 where spell = 13550;

-- Serpent Sting Rank 4
update dbc.skilllineability_lplus set spell = 90463 where spell = 13551;
update lplusworld.playercreateinfo_spell_custom set spell = 90463 where spell = 13551;

-- Serpent Sting Rank 5
update dbc.skilllineability_lplus set spell = 90464 where spell = 13552;
update lplusworld.playercreateinfo_spell_custom set spell = 90464 where spell = 13552;

-- Serpent Sting Rank 6
update dbc.skilllineability_lplus set spell = 90465 where spell = 13553;
update lplusworld.playercreateinfo_spell_custom set spell = 90465 where spell = 13553;

-- Serpent Sting Rank 7
update dbc.skilllineability_lplus set spell = 90466 where spell = 13554;
update lplusworld.playercreateinfo_spell_custom set spell = 90466 where spell = 13554;

-- Serpent Sting Rank 8
update dbc.skilllineability_lplus set spell = 90467 where spell = 13555;
update lplusworld.playercreateinfo_spell_custom set spell = 90467 where spell = 13555;

-- Serpent Sting Rank 9
update dbc.skilllineability_lplus set spell = 90468 where spell = 25295;
update lplusworld.playercreateinfo_spell_custom set spell = 90468 where spell = 25295;

-- Serpent Sting Rank 10
update dbc.skilllineability_lplus set spell = 90469 where spell = 27016;
update lplusworld.playercreateinfo_spell_custom set spell = 90469 where spell = 27016;

-- Serpent Sting Rank 11
update dbc.skilllineability_lplus set spell = 90470 where spell = 49000;
update lplusworld.playercreateinfo_spell_custom set spell = 90470 where spell = 49000;

-- Serpent Sting Rank 12
update dbc.skilllineability_lplus set spell = 90471 where spell = 49001;
update lplusworld.playercreateinfo_spell_custom set spell = 90471 where spell = 49001;
