-- Every quest-required loot drop is MULTI_DROP, so a party questing together
-- each gets their own copy.
--
-- Why this flag: Player::StoreLootItem says it outright -
--
--     //if only one person is supposed to loot the item, then set it to looted
--     if (!item->freeforall)
--         item->is_looted = true;
--
-- freeforall comes from ITEM_FLAG_MULTI_DROP (0x800), and that shared
-- is_looted is what Loot::FillQuestLoot tests before offering a quest item to
-- the next player.  Without the flag the first looter does not merely hide the
-- drop from everyone else - it erases it, before their loot window is even
-- built.  Reported live: two players on "A Putrid Task", both mid-collection,
-- one looted a Putrid Claw (2855) and the other could not.
--
-- Blizzard set MULTI_DROP on some quest items and not others - 776 of these
-- 2168 already had it, which is why the bug looks intermittent.
--
-- Eligibility is untouched: AllowedForPlayer still requires the player to hold
-- the quest and still need the item, so this hands nothing to anyone who does
-- not need it.
--
-- Re-runnable.  Requires a worldserver restart: item templates are cached at
-- startup, so the flag does not take effect on a running realm.

DROP TABLE IF EXISTS zz_questloot_items;
CREATE TABLE zz_questloot_items (Item INT UNSIGNED PRIMARY KEY);

INSERT IGNORE INTO zz_questloot_items (Item) SELECT DISTINCT Item FROM creature_loot_template   WHERE QuestRequired = 1;
INSERT IGNORE INTO zz_questloot_items (Item) SELECT DISTINCT Item FROM gameobject_loot_template WHERE QuestRequired = 1;
INSERT IGNORE INTO zz_questloot_items (Item) SELECT DISTINCT Item FROM item_loot_template       WHERE QuestRequired = 1;
INSERT IGNORE INTO zz_questloot_items (Item) SELECT DISTINCT Item FROM fishing_loot_template    WHERE QuestRequired = 1;
INSERT IGNORE INTO zz_questloot_items (Item) SELECT DISTINCT Item FROM skinning_loot_template   WHERE QuestRequired = 1;
INSERT IGNORE INTO zz_questloot_items (Item) SELECT DISTINCT Item FROM mail_loot_template       WHERE QuestRequired = 1;
INSERT IGNORE INTO zz_questloot_items (Item) SELECT DISTINCT Item FROM reference_loot_template  WHERE QuestRequired = 1;

-- Keep what we overwrote, so the change can be undone item by item.
DROP TABLE IF EXISTS zz_questloot_flags_backup;
CREATE TABLE zz_questloot_flags_backup AS
    SELECT it.entry, it.name, it.flags AS old_flags, NOW() AS backed_up
    FROM item_template it JOIN zz_questloot_items z ON z.Item = it.entry
    WHERE it.flags & 2048 = 0;

UPDATE item_template it JOIN zz_questloot_items z ON z.Item = it.entry
SET it.flags = it.flags | 2048
WHERE it.flags & 2048 = 0;

-- To undo:
--   UPDATE item_template it JOIN zz_questloot_flags_backup b ON b.entry = it.entry
--   SET it.flags = b.old_flags;
