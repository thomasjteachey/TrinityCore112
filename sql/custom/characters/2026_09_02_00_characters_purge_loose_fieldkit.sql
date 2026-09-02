-- Remove every field-kit duplicate that is not being worn.
--
-- RUN THIS WITH THE WORLDSERVER STOPPED. It deletes rows for characters the
-- running server may hold in memory, and a character save would write them
-- straight back - or worse, write back a reference to an item_instance that no
-- longer exists.
--
-- Most of the work is already done in code: DestroyLooseFieldKit sweeps a
-- character's bags every time the kit is issued, which is at login and on
-- resurrection, so the population cleans itself as characters cycle. This file
-- is for finishing the job in one pass at a restart, and for the two cases the
-- code sweep cannot reach - mail attachments, and characters that never log in
-- again.
--
-- Worn pieces are deliberately left alone. They are doing their job, they are
-- destroyed automatically when something replaces them, and stripping a
-- character mid-session would leave it bare until its next login or death.
-- To take those too, drop the `AND NOT (ci.bag = 0 AND ci.slot < 19)` clause.
--
-- Counted 2026-09-02: 487 instances, 279 worn, 205 loose in bags, 3 in mail.
--
-- Re-runnable.

-- 1. Loose copies sitting in bags.
CREATE TEMPORARY TABLE zz_loose_kit (guid INT UNSIGNED NOT NULL PRIMARY KEY);

INSERT INTO zz_loose_kit (guid)
SELECT ii.guid
  FROM item_instance ii
  JOIN character_inventory ci ON ci.item = ii.guid
 WHERE ii.itemEntry BETWEEN 92000 AND 93999
   AND NOT (ci.bag = 0 AND ci.slot < 19);

-- 2. Copies attached to mail.
INSERT IGNORE INTO zz_loose_kit (guid)
SELECT ii.guid
  FROM item_instance ii
  JOIN mail_items mi ON mi.item_guid = ii.guid
 WHERE ii.itemEntry BETWEEN 92000 AND 93999;

-- 3. Orphans - an instance no container claims at all.
INSERT IGNORE INTO zz_loose_kit (guid)
SELECT ii.guid
  FROM item_instance ii
  LEFT JOIN character_inventory ci ON ci.item = ii.guid
  LEFT JOIN mail_items mi ON mi.item_guid = ii.guid
  LEFT JOIN auctionhouse a ON a.itemguid = ii.guid
 WHERE ii.itemEntry BETWEEN 92000 AND 93999
   AND ci.item IS NULL AND mi.item_guid IS NULL AND a.itemguid IS NULL;

SELECT CONCAT('field-kit instances to remove: ', COUNT(*)) AS result FROM zz_loose_kit;

DELETE ci FROM character_inventory ci JOIN zz_loose_kit z ON z.guid = ci.item;
DELETE mi FROM mail_items mi JOIN zz_loose_kit z ON z.guid = mi.item_guid;
DELETE ii FROM item_instance ii JOIN zz_loose_kit z ON z.guid = ii.guid;

DROP TEMPORARY TABLE zz_loose_kit;

SELECT CONCAT('field-kit instances remaining (all worn): ', COUNT(*)) AS result
  FROM item_instance WHERE itemEntry BETWEEN 92000 AND 93999;
