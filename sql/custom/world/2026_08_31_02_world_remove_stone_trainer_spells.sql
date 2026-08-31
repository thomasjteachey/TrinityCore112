-- Create Firestone (607) and Create Spellstone (6485) are talents on this
-- realm, so no trainer should still be selling them.
--
-- Four rows: both spells on trainer 500006 - the shared warlock trainer set
-- behind twenty-four NPCs including Demisette Cloyce, Maximillian Crowe and
-- Grol'dar - and on trainer 500019, Kartosh.  Both were level 36 for 9000
-- copper.  Only the first rank of each was ever trainer-taught here.
--
-- Checked before running: no character had learned either from a trainer, so
-- nothing needs unlearning.  That check matters on this realm - a talent spell
-- sitting in character_spell without a spent talent point is what drove the
-- reset-every-login bug, because m_usedTalentCount sums character_spell
-- unfiltered.  If this migration is ever re-run against a database where
-- somebody DID train one, unlearn it as well or that character will reset its
-- talents on every login.
--
-- Re-runnable, and keeps a backup table so the rows can be put back.

DROP TABLE IF EXISTS zz_stone_trainer_backup;
CREATE TABLE zz_stone_trainer_backup AS
    SELECT * FROM trainer_spell WHERE SpellId IN (607, 6485);

DELETE FROM trainer_spell WHERE SpellId IN (607, 6485);

-- Should any character have trained one before this ran:
--   SELECT guid, spell FROM characters.character_spell WHERE spell IN (607, 6485);
--
-- To undo:
--   INSERT INTO trainer_spell SELECT * FROM zz_stone_trainer_backup;
