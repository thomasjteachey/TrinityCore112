-- Make every Violet Hold outdoor-classification input agree.
--
-- ADT files do not have an indoor/outdoor bit. All 6,400 MCNK terrain chunks
-- in DalaranPrison point at AreaTable 4415, so that row is the ADT-side input.
-- Map 1608 also has custom fallback area 30608. Clear AREA_FLAG_INSIDE
-- (0x02000000) and set AREA_FLAG_OUTSIDE (0x04000000) on both.
--
-- WMOAreaTable Flags bit 4 is the explicit treat-as-outdoors override used by
-- both client and server above the WMO group's own MOGP flags. Clear bit 2
-- (inside) while setting bit 4 on the root and every group row for WMO 5282.
-- These stock rows have AreaTableID 0; point them at outdoor AreaTable 4415 so
-- the client does not fall back to the stock WMO's indoor classification when
-- enabling mounts and other outdoors-only action buttons.
--
-- The binary DBCs are normalized by tools/violet_hold/vhr_dbc.py. Client WMO
-- rendering metadata is handled separately by vhr_outdoors.py: MOHD, MOGI,
-- MOGP, and the A/interior/exterior render-batch counts must change together.
-- Setting only MOGP EXTERIOR culls the hold because all 16 batches remain in
-- the interior range. The server VMAP deliberately remains stock.
--
-- Replayable.
UPDATE dbc.areatable_lplus
SET Flags = (Flags & ~33554432) | 67108864
WHERE ID IN (4415, 30608);

UPDATE dbc.wmoareatable_lplus
SET Flags = (Flags & ~2) | 4,
    AreaTableID = 4415
WHERE WMOID = 5282;
