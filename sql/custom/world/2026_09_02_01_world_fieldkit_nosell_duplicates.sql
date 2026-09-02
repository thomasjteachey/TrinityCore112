-- Non-sellable duplicates of every item the hardcore field kit can issue.
--
-- The kit reissues a full set of white (or grey, below level 15) gear on every
-- death, so the gear it hands out must not be a gold faucet: dying repeatedly
-- and vendoring the replacements would print money. Setting SellPrice on the
-- real items is not an option - the same items drop from mobs and sit on
-- vendors, and those copies must still sell normally.
--
-- So the kit issues its own duplicates. Each is a byte-for-byte copy of the
-- original with SellPrice and BuyPrice zeroed, which means the CLIENT shows a
-- sell price of zero in the tooltip and the merchant refuses it - the ordinary
-- behaviour of a worthless item, with nothing new to explain to the player.
-- Same name, same display, same stats, same required level, so it is
-- indistinguishable in play from the item it copies.
--
-- Entries live at 92000-93002, inside a reserved 92000-93999 block chosen
-- because it is free in both item_template (max real entry 100998) and in the
-- client Item.dbc mirror dbc.item_lplus. zz_fieldkit_map records
-- source -> duplicate so this is idempotent and reversible, and so the client
-- Item.dbc rows can be generated from the same pairing.
--
-- The selection below is the field-kit filter from
-- src/server/scripts/Custom/custom_barracks_hardcore.cpp BuildWhiteKitCacheOnce
-- transcribed into SQL: white or grey, obtainable somewhere in the world, no
-- scaffolding names, classic ids only, equippable armour or weapon, nothing
-- gated by skill/spell/reputation/honour/zone, nothing quest-bound, nothing
-- with a duration. It yields 645 white and 358 grey items, 1003 in all.
--
-- NOTE the scaffolding markers barely bite here: of the 1010 items that pass
-- everything else, the name filters remove only 7. That is expected - the
-- markers exist to catch developer junk, and this pool is already restricted to
-- items the world demonstrably produces.
--
-- Re-runnable: the map and the duplicates are rebuilt from scratch each time.

-- Pin sql_mode. Under NO_BACKSLASH_ESCAPES a backslash in a string literal is
-- not an escape, which silently changes what any regex below means - the first
-- run of this file matched 1003 items instead of 364 for exactly that reason.
-- The filters avoid backslashes entirely now as well, belt and braces.
SET SESSION sql_mode = '';

DROP TABLE IF EXISTS zz_fieldkit_obtainable;
CREATE TABLE zz_fieldkit_obtainable (id INT NOT NULL PRIMARY KEY);
INSERT IGNORE INTO zz_fieldkit_obtainable SELECT DISTINCT item   FROM npc_vendor;
INSERT IGNORE INTO zz_fieldkit_obtainable SELECT DISTINCT Item   FROM creature_loot_template;
INSERT IGNORE INTO zz_fieldkit_obtainable SELECT DISTINCT Item   FROM gameobject_loot_template;
INSERT IGNORE INTO zz_fieldkit_obtainable SELECT DISTINCT Item   FROM reference_loot_template;
INSERT IGNORE INTO zz_fieldkit_obtainable SELECT DISTINCT Item   FROM item_loot_template;
INSERT IGNORE INTO zz_fieldkit_obtainable SELECT DISTINCT itemid FROM playercreateinfo_item;

-- Clear any previous run before renumbering, or the old duplicates linger.
DELETE FROM item_template WHERE entry BETWEEN 92000 AND 93999;

DROP TABLE IF EXISTS zz_fieldkit_map;
CREATE TABLE zz_fieldkit_map (
    source_entry INT NOT NULL PRIMARY KEY,
    kit_entry    INT NOT NULL UNIQUE
);

SET @n := 91999;
INSERT INTO zz_fieldkit_map (source_entry, kit_entry)
SELECT t.entry, (@n := @n + 1)
FROM item_template t
JOIN zz_fieldkit_obtainable o ON o.id = t.entry
WHERE t.Quality IN (0, 1)
  AND t.entry <= 24000
  AND t.class IN (2, 4)
  AND t.InventoryType <> 0
  AND t.RequiredLevel <= 60
  AND t.ItemLevel <= 70
  AND t.RequiredSkill = 0
  AND t.requiredspell = 0
  AND t.RequiredReputationFaction = 0
  AND t.requiredhonorrank = 0
  AND t.area = 0
  AND t.Map = 0
  AND t.bonding <> 4
  AND t.duration = 0
  AND LOWER(t.name) NOT LIKE '%[ph]%'
  AND LOWER(t.name) NOT REGEXP 'crobinson|monster |old |deprecated|unused|placeholder'
  AND LOWER(t.name) NOT REGEXP '(^|[^a-z])test([^a-z]|$)'
ORDER BY t.entry;

-- Copy through a scratch table so every column comes along without having to
-- name a hundred and fifty of them.
DROP TEMPORARY TABLE IF EXISTS zz_fieldkit_new;
CREATE TEMPORARY TABLE zz_fieldkit_new LIKE item_template;
INSERT INTO zz_fieldkit_new
    SELECT * FROM item_template WHERE entry IN (SELECT source_entry FROM zz_fieldkit_map);

UPDATE zz_fieldkit_new n
    JOIN zz_fieldkit_map m ON m.source_entry = n.entry
    SET n.entry = m.kit_entry,
        n.SellPrice = 0,
        n.BuyPrice = 0;

INSERT INTO item_template SELECT * FROM zz_fieldkit_new;
DROP TEMPORARY TABLE zz_fieldkit_new;

DROP TABLE zz_fieldkit_obtainable;

-- To undo:
--   DELETE FROM item_template WHERE entry BETWEEN 92000 AND 93999;
--   DROP TABLE zz_fieldkit_map;
-- (and drop the matching rows from dbc.item_lplus, then regenerate Item.dbc)
