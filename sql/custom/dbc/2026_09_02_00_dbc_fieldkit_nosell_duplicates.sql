-- Client Item.dbc rows for the non-sellable field-kit duplicates.
--
-- Pairs with sql/custom/world/2026_09_02_01_world_fieldkit_nosell_duplicates.sql,
-- which creates entries 92000-93002 in item_template. A hand-made item without
-- a matching Item.dbc row shows a "?" icon and cannot be equipped at all, so
-- these rows are not optional - see reference_custom_item_client_dbc.
--
-- Every field is copied from the source item, because a duplicate must look and
-- behave exactly like the item it copies. Item.dbc carries no price, so nothing
-- here needs to change for the SellPrice-zero part; that lives in item_template.
--
-- After running this, Item.dbc must be regenerated and propagated to all five
-- places: the local patch-enUS-A, the server's published patch-enUS-A with the
-- version bumped, the server's data/dbc, the local data/dbc/bplus, and this
-- mirror.
--
-- Re-runnable.

DELETE FROM dbc.item_lplus WHERE ID BETWEEN 92000 AND 93999;

INSERT INTO dbc.item_lplus
    (ID, ClassID, SubclassID, Sound_Override_Subclassid, Material, DisplayInfoID, InventoryType, SheatheType)
SELECT m.kit_entry, i.ClassID, i.SubclassID, i.Sound_Override_Subclassid,
       i.Material, i.DisplayInfoID, i.InventoryType, i.SheatheType
FROM bplusworld.zz_fieldkit_map m
JOIN dbc.item_lplus i ON i.ID = m.source_entry;

-- To undo:
--   DELETE FROM dbc.item_lplus WHERE ID BETWEEN 92000 AND 93999;
