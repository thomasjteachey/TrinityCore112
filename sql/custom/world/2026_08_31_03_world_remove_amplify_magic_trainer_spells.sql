-- Amplify Magic is off the trainers on this realm.
--
-- Trainers sell the learn-spells, not the buffs, so the rows to delete are
-- 1267, 8456, 10171 and 10172, which teach Amplify Magic ranks 1-4 (1008,
-- 8455, 10169, 10170).  Deleting by name would also have caught the buffs
-- themselves, which are not trainer rows at all; deleting by learn-spell id
-- is the precise cut.
--
-- Fourteen rows across four trainer sets, sitting behind 35 NPCs:
--   500001  19 NPCs (Anastasia Hartwell and the rest of the Alliance mages)
--   500002   7 NPCs (Baatun and the Horde mages)
--   500023   2 NPCs (Lunaraa)
--   500068   7 NPCs (Brunna Ironaxe) - only ever sold ranks 2 and 3
-- Ranks were level 18/30/42/54 at 18s/80s/1g80s/3g60s.
--
-- Twenty-five characters already know a rank (9 at rank 1, 8 at rank 2, 8 at
-- rank 3).  They keep it.  Unlike the Firestone and Spellstone removal, that
-- is safe to leave alone here: Amplify Magic is not a talent on this realm
-- (checked against talent_lplus, zero rows), so it cannot inflate
-- m_usedTalentCount and trigger the reset-every-login bug.
--
-- Re-runnable, and keeps a backup table so the rows can be put back.

DROP TABLE IF EXISTS zz_amplify_magic_trainer_backup;
CREATE TABLE zz_amplify_magic_trainer_backup AS
    SELECT * FROM trainer_spell WHERE SpellId IN (1267, 8456, 10171, 10172);

DELETE FROM trainer_spell WHERE SpellId IN (1267, 8456, 10171, 10172);

-- To undo:
--   INSERT INTO trainer_spell SELECT * FROM zz_amplify_magic_trainer_backup;
