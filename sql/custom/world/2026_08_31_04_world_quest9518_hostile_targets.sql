-- Quest 9518 "Agents of Destruction" could not be completed: all four of its
-- kill-credit targets were unattackable.
--
--   11680 Horde Scout        (10 needed)
--   11681 Horde Deforester   (5 needed)
--   11684 Warsong Shredder   (2 needed)
--   17304 Overseer Gorthak   (1 needed)
--
-- All four sat on faction template 35, which on this realm carries
-- FriendGroup=7 (Player|Alliance|Horde) and EnemyGroup=0 - friendly to
-- everyone, hostile to nobody, so no player of any race could attack them.
--
-- Template 35 is shared by 14348 creatures here, most of which SHOULD be
-- friendly (vendors, guards, spirit guides, battlemasters), so the template
-- itself is left alone and only these four creatures are moved.
--
-- They move to template 93: parent faction 14 "Monster", FriendGroup=0,
-- EnemyGroup=1.  That is hostile to the Player group as a whole rather than
-- to one side, which is what this realm needs - quest 9518 has
-- AllowableRaces=1279, both factions, so a template that is hostile only to
-- Alliance would have left Horde players just as stuck.
--
-- Note for whoever runs 2026_08_31_00_world_quest_target_factions.sql: that
-- migration would NOT have fixed this quest.  It moves these same four
-- creatures to their stock faction 83, and template 83 on this realm has
-- drifted to FriendGroup=6 / EnemyGroup=0 - friendly to both player factions,
-- exactly as unattackable as 35.  275 of the 841 templates carry that same
-- drift.  Reassigning creatures to stock factions cannot fix a problem that
-- lives in the templates.
--
-- Re-runnable, and keeps a backup table so the rows can be put back.

DROP TABLE IF EXISTS zz_quest9518_faction_backup;
CREATE TABLE zz_quest9518_faction_backup AS
    SELECT entry, name, faction FROM creature_template
    WHERE entry IN (11680, 11681, 11684, 17304);

UPDATE creature_template SET faction = 93
    WHERE entry IN (11680, 11681, 11684, 17304);

-- To undo:
--   UPDATE creature_template ct JOIN zz_quest9518_faction_backup b ON b.entry=ct.entry
--     SET ct.faction = b.faction;
