-- Scorpid Healing (81291) stops rolling a die on top of a calculated heal.
--
-- Mongoose Bite now heals for a multiple of the Strength its consumed Scorpid
-- Sting is draining, and passes that figure in as the spell's base point. But
-- SpellEffectInfo::CalcValue adds rand(1..EffectDieSides) on top of an
-- overridden base point, so with DieSides left at its stock 83 the result would
-- land anywhere up to 83 over the intended number - the scaling would be real
-- but the figure would never be the one the tooltip or the config implies.
--
-- DieSides 0 is the case that returns the base point untouched (1 would add a
-- flat +1). EffectBasePoints_1 is left at 262: it is what the heal falls back
-- to if no Strength drain is found on the sting, which keeps a differently
-- shaped sting healing for something sane.
--
-- Applied to both realms. The binary Spell.dbc was patched to match on each -
-- field 74, verified against Scorpid Sting ranks 1 and 4 first, since the
-- WDBXEditor definition is two fields short of the shipped file and its
-- ordinals cannot be trusted here. The server reads the binary; the mirrors
-- alone change nothing.
--
-- Re-runnable.

UPDATE dbc.spell_bplus SET EffectDieSides_1 = 0 WHERE ID = 81291;
UPDATE dbc.spell_lplus SET EffectDieSides_1 = 0 WHERE ID = 81291;

-- To undo (and re-patch both binaries):
--   UPDATE dbc.spell_bplus SET EffectDieSides_1 = 83 WHERE ID = 81291;
--   UPDATE dbc.spell_lplus SET EffectDieSides_1 = 83 WHERE ID = 81291;
