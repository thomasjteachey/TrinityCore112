-- Flag every custom arena as outdoors.
--
-- Same problem Violet Hold had (see 2026_08_06_06_dbc_violet_hold_wmo_outdoors),
-- and the same first half of the fix.
--
-- All 24 arena AreaTable rows ship with Flags = 65664, which sets neither
-- AREA_FLAG_INSIDE (0x02000000) nor AREA_FLAG_OUTSIDE (0x04000000). With
-- neither bit present the client falls back to the WMO group's own indoor
-- classification, so an arena built inside a WMO reads as indoors and the
-- outdoors-only action buttons stay dark.
--
-- Clearing INSIDE and setting OUTSIDE makes the area itself authoritative.
-- This is sufficient on its own for the open-air arenas, where the player is
-- never inside a WMO group.
--
-- NOT sufficient by itself for the enclosed ones (Baradin Hold, Black Rook
-- Hold, Guardian's Hall/Karazhan, Spark of Creator/Ulduar). Those also need the
-- WMOAreaTable half - clear bit 2, set bit 4, point AreaTableID at the outdoor
-- area - exactly as WMO 5282 got for Violet Hold. That is keyed on each arena's
-- WMOID, which lives in the WMO root files rather than in any DBC we already
-- hold, so it is deliberately not attempted here.
--
-- Replayable.

UPDATE dbc.areatable_lplus
SET Flags = (Flags & ~33554432) | 67108864
WHERE ContinentID IN (
    982,   -- Coliseum of Past Echoes
    983,   -- Imperial Arena of Thakraj
    984,   -- Maldraxxus Coliseum
    985,   -- Nagrand Arena (Remastered)
    986,   -- Blade's Edge Arena (Remastered)
    1007,  -- Guardian's Hall
    1008,  -- Spark of Creator
    1401,  -- Baradin Hold Arena
    1402,  -- Obelisk of the Stars
    1403,  -- The Twisting Nether
    1504,  -- Black Rook Hold Arena
    1552,  -- Ashamane's Fall
    1683,  -- The Inventor's Library
    1684   -- Amphitheater of Anguish
);

-- The two arenas that predate this work, kept in step so every arena on the
-- realm answers the indoor/outdoor question the same way.
UPDATE dbc.areatable_lplus
SET Flags = (Flags & ~33554432) | 67108864
WHERE ContinentID IN (
    980,   -- Tol'Viron Arena
    1134   -- The Tiger's Peak
);
