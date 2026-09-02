-- Correction: it was DETECT Magic that had to come off the trainers, not
-- AMPLIFY Magic. This reverts 2026_08_31_03 and removes the right spell.
--
-- Part 1 restores the fourteen Amplify Magic rows that 2026_08_31_03 deleted
-- (learn-spells 1267/8456/10171/10172, ranks 1-4, across trainer sets 500001,
-- 500002, 500023 and 500068). If that migration is ever replayed it will strip
-- them again, so treat THIS file as the later word on Amplify Magic.
--
-- Part 2 removes Detect Magic. Trainers sell the learn-spell, so the row to
-- delete is 2858 (which teaches the buff, 2855) - deleting 2855 would do
-- nothing, since it is not a trainer row at all. Three rows, one per trainer
-- set, behind 28 NPCs: 500001 (19, Anastasia Hartwell and the Alliance mages),
-- 500002 (7, Baatun and the Horde mages) and 500023 (2, Lunaraa). Level 16 for
-- 15 silver.
--
-- Twenty-five characters already know Detect Magic (2855) and keep it. That is
-- safe here for the same reason it was for Amplify Magic: it is not a talent on
-- this realm, so it cannot inflate m_usedTalentCount and cause the
-- reset-every-login bug that a stray talent spell in character_spell does.
--
-- Re-runnable, and keeps a backup table so the rows can be put back.

-- Part 1: Amplify Magic goes back on the trainers.
INSERT INTO trainer_spell SELECT * FROM zz_amplify_magic_trainer_backup
    ON DUPLICATE KEY UPDATE trainer_spell.SpellId = trainer_spell.SpellId;

-- Part 2: Detect Magic comes off them.
DROP TABLE IF EXISTS zz_detect_magic_trainer_backup;
CREATE TABLE zz_detect_magic_trainer_backup AS
    SELECT * FROM trainer_spell WHERE SpellId = 2858;

DELETE FROM trainer_spell WHERE SpellId = 2858;

-- To undo part 2:
--   INSERT INTO trainer_spell SELECT * FROM zz_detect_magic_trainer_backup;
