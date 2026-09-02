-- Auto-awarded field-kit gear reads as artifact quality and says what it is.
--
-- The kit issues copies of ordinary white and grey items, so on the character
-- sheet it was indistinguishable from gear the player had actually earned. That
-- is exactly wrong for something the server hands out on every death and
-- destroys the moment it is replaced: it invites people to treat a loaner as
-- kit worth keeping, and to wonder where it went when it vanishes.
--
-- Quality 6 (ITEM_QUALITY_ARTIFACT) is used as the marker. Nothing else on this
-- realm uses it, so the colour is unambiguous - and being a quality no real drop
-- carries, it cannot be confused with an upgrade. Note the client paints
-- artifact as a pale gold rather than a true red; there is no redder tier
-- available, quality 7 is heirloom and paints the same.
--
-- IMPORTANT - the grey/white split is NOT cosmetic, and this erases it.
-- Centurion.Hardcore.FieldKit.GreyUntilLevel hands the earliest levels grey
-- gear, which is genuinely weaker than white, and the two tiers were told apart
-- purely by item_template.Quality. They are interleaved across the whole
-- 92000-93002 range, so no id range can stand in for it. Once every duplicate
-- reads 6, that signal is gone from the duplicate itself.
--
-- zz_fieldkit_map.source_entry is what saves it: the ORIGINAL item still
-- carries its own quality, and BuildWhiteKitCacheOnce now classifies each
-- duplicate by its source rather than by itself. Do not drop that table, and do
-- not renumber the duplicates without rebuilding it - doing either silently
-- promotes every level 1-14 character from grey kit to white.
--
-- Re-runnable.

SET SESSION sql_mode = '';

UPDATE item_template
   SET Quality = 6,
       description = 'Temporal issue. Awarded automatically, and destroyed when you replace it.'
 WHERE entry BETWEEN 92000 AND 93999;

-- Sanity: every duplicate should still resolve to a source whose quality tells
-- the kit which tier it belongs to.
SELECT CONCAT('duplicates now artifact-quality: ', COUNT(*)) AS result
  FROM item_template WHERE entry BETWEEN 92000 AND 93999 AND Quality = 6;

SELECT CONCAT('duplicates with no source row (must be 0): ', COUNT(*)) AS result
  FROM item_template t
  LEFT JOIN zz_fieldkit_map m ON m.kit_entry = t.entry
 WHERE t.entry BETWEEN 92000 AND 93999 AND m.kit_entry IS NULL;

SELECT CONCAT('grey tier still resolvable from sources: ', COUNT(*)) AS result
  FROM zz_fieldkit_map m
  JOIN item_template s ON s.entry = m.source_entry
 WHERE s.Quality = 0;

-- To undo, restoring each duplicate's quality and description from its source:
--   UPDATE item_template t
--     JOIN zz_fieldkit_map m ON m.kit_entry = t.entry
--     JOIN item_template s ON s.entry = m.source_entry
--      SET t.Quality = s.Quality, t.description = s.description
--    WHERE t.entry BETWEEN 92000 AND 93999;
