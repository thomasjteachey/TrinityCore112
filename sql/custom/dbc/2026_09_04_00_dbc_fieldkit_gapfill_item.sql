-- Client Item.dbc rows for the field kit's gap-fill pieces.
--
-- item_template alone is not enough. The CLIENT resolves an item's class,
-- subclass, material, model and sheath from its own Item.dbc, and an id missing
-- there renders as "?", refuses to equip and cannot be linked in chat. The 1003
-- duplicates from 2026_09_02_01 already have their rows; these 104 do not.
--
-- Every value is read back out of the row the world migration just built, so the
-- two cannot disagree - and because each piece was cloned from real art,
-- DisplayInfoID already points at a model the client ships.
--
-- Run AFTER sql/custom/world/2026_09_04_00_world_fieldkit_gapfill.sql, then
-- rebuild the binary Item.dbc for the realm AND the copy inside the client
-- patch, or the client half of this is inert.
--
-- Re-runnable.

DELETE FROM dbc.item_lplus WHERE ID BETWEEN 93500 AND 93999;

INSERT INTO dbc.item_lplus
    (ID, ClassID, SubclassID, Sound_Override_Subclassid, Material, DisplayInfoID,
     InventoryType, SheatheType)
SELECT entry, class, subclass, -1, Material, displayid, InventoryType, sheath
  FROM bplusworld.item_template
 WHERE entry BETWEEN 93500 AND 93999;

SELECT CONCAT('client rows for the gap fill: ', COUNT(*)) AS result
  FROM dbc.item_lplus WHERE ID BETWEEN 93500 AND 93999;

SELECT CONCAT('gap-fill items with no client row (must be 0): ', COUNT(*)) AS result
  FROM bplusworld.item_template t
  LEFT JOIN dbc.item_lplus d ON d.ID = t.entry
 WHERE t.entry BETWEEN 93500 AND 93999 AND d.ID IS NULL;
