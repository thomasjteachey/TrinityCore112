-- Surprise! (89160) scales with level instead of hitting flat.
--
-- The gnome pocket explosive did 132-218 at every level, because
-- EffectRealPointsPerLevel was zero and SpellEffectInfo::CalcValue only applies
-- level scaling when it is not:
--
--     damage = BasePoints
--            + (clamp(level, BaseLevel, MaxLevel) - max(BaseLevel, SpellLevel))
--              * RealPointsPerLevel
--            + roll(1..DieSides)
--
-- So a level 5 gnome threw exactly the same grenade as a level 60 one. With
-- BaseLevel 1, MaxLevel 60, BasePoints 12 and 2.76 per level it now runs:
--
--     level  1   ->  12
--     level 30   ->  92
--     level 60   -> 174   (and stops there)
--
-- 174 is close to the old 132-218 average, so the top end is essentially
-- unchanged and only the early game moves - which is the complaint.
--
-- EffectDieSides_1 goes to 0. A flat 1-87 roll on top of a 12 point base would
-- have swamped the scaling at low level, which is the exact thing being fixed,
-- and linear is what was asked for. Put a small die back if it wants variance.
--
-- SpellLevel is deliberately left at 0. A non-zero value would invite
-- Unit::CalculateLevelPenalty, which returns early only while SpellLevel is 0
-- or >= MaxLevel; max(BaseLevel, SpellLevel) still yields the intended origin
-- of 1.
--
-- Applied to both realms. The binary Spell.dbc was patched to match on each and
-- in both client patches - the client computes the tooltip's $s1 from its own
-- copy, so a server-only change would show the old flat number. Field indices
-- were verified against four spells with distinct known values first, because
-- the WDBXEditor definition is two fields short of the shipped file.
--
-- Re-runnable.

UPDATE dbc.spell_bplus
   SET MaxLevel = 60,
       BaseLevel = 1,
       EffectBasePoints_1 = 12,
       EffectDieSides_1 = 0,
       EffectRealPointsPerLevel_1 = 2.76
 WHERE ID = 89160;

UPDATE dbc.spell_lplus
   SET MaxLevel = 60,
       BaseLevel = 1,
       EffectBasePoints_1 = 12,
       EffectDieSides_1 = 0,
       EffectRealPointsPerLevel_1 = 2.76
 WHERE ID = 89160;

-- To undo (and re-patch both binaries and both client patches):
--   UPDATE dbc.spell_<realm> SET MaxLevel = 0, BaseLevel = 0,
--          EffectBasePoints_1 = 131, EffectDieSides_1 = 87,
--          EffectRealPointsPerLevel_1 = 0 WHERE ID = 89160;
