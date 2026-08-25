-- Flag every custom arena's WMO as outdoors.
--
-- Second half of 2026_08_06_07_dbc_custom_arenas_outdoors. The AreaTable
-- flags alone settle it for the open-air arenas; for the enclosed ones the
-- WMO group's own MOGP flags still say indoors, and WMOAreaTable bit 4 is the
-- explicit override that both client and server consult above them - exactly
-- as WMO 5282 needed for Violet Hold.
--
-- WMOIDs come from each WMO root file's MOHD.wmoID. Verified against the two
-- arenas sourced from stock lichking.MPQ (5110, 5406), which already carry
-- real rows - if the parse were wrong those would not have matched.
--
-- Replayable: the UPDATEs are idempotent and the INSERTs are guarded on
-- absence.

-- map 980   Tol'Viron Arena                WMOID 6831    (no rows - insert root)
INSERT INTO dbc.wmoareatable_lplus (ID, WMOID, NameSetID, WMOGroupID, SoundProviderPref, SoundProviderPrefUnderwater, AmbienceID, ZoneMusic, IntroSound, Flags, AreaTableID, AreaName_Lang_enUS, AreaName_Lang_Mask)
SELECT 51122, 6831, 0, -1, 0, 0, 0, 0, 0, 4, 6296, 'Tol''Viron Arena', 16712190
WHERE NOT EXISTS (SELECT 1 FROM dbc.wmoareatable_lplus WHERE WMOID = 6831);

-- map 982   Coliseum of Past Echoes        WMOID 75000   (no rows - insert root)
INSERT INTO dbc.wmoareatable_lplus (ID, WMOID, NameSetID, WMOGroupID, SoundProviderPref, SoundProviderPrefUnderwater, AmbienceID, ZoneMusic, IntroSound, Flags, AreaTableID, AreaName_Lang_enUS, AreaName_Lang_Mask)
SELECT 51123, 75000, 0, -1, 0, 0, 0, 0, 0, 4, 10026, 'Coliseum of Past Echoes', 16712190
WHERE NOT EXISTS (SELECT 1 FROM dbc.wmoareatable_lplus WHERE WMOID = 75000);

-- map 983   Imperial Arena of Thakraj      WMOID 25000   (no rows - insert root)
INSERT INTO dbc.wmoareatable_lplus (ID, WMOID, NameSetID, WMOGroupID, SoundProviderPref, SoundProviderPrefUnderwater, AmbienceID, ZoneMusic, IntroSound, Flags, AreaTableID, AreaName_Lang_enUS, AreaName_Lang_Mask)
SELECT 51124, 25000, 0, -1, 0, 0, 0, 0, 0, 4, 10028, 'Imperial Arena of Thakraj', 16712190
WHERE NOT EXISTS (SELECT 1 FROM dbc.wmoareatable_lplus WHERE WMOID = 25000);

-- map 984   Maldraxxus Coliseum            WMOID 13649   (no rows - insert root)
INSERT INTO dbc.wmoareatable_lplus (ID, WMOID, NameSetID, WMOGroupID, SoundProviderPref, SoundProviderPrefUnderwater, AmbienceID, ZoneMusic, IntroSound, Flags, AreaTableID, AreaName_Lang_enUS, AreaName_Lang_Mask)
SELECT 51125, 13649, 0, -1, 0, 0, 0, 0, 0, 4, 10032, 'Maldraxxus Coliseum', 16712190
WHERE NOT EXISTS (SELECT 1 FROM dbc.wmoareatable_lplus WHERE WMOID = 13649);

-- map 985   Nagrand Arena (Remastered)     WMOID 10834   (no rows - insert root)
INSERT INTO dbc.wmoareatable_lplus (ID, WMOID, NameSetID, WMOGroupID, SoundProviderPref, SoundProviderPrefUnderwater, AmbienceID, ZoneMusic, IntroSound, Flags, AreaTableID, AreaName_Lang_enUS, AreaName_Lang_Mask)
SELECT 51126, 10834, 0, -1, 0, 0, 0, 0, 0, 4, 10040, 'Nagrand Arena (Remastered)', 16712190
WHERE NOT EXISTS (SELECT 1 FROM dbc.wmoareatable_lplus WHERE WMOID = 10834);

-- map 986   Blade's Edge Arena (Remastered) WMOID 10872   (no rows - insert root)
INSERT INTO dbc.wmoareatable_lplus (ID, WMOID, NameSetID, WMOGroupID, SoundProviderPref, SoundProviderPrefUnderwater, AmbienceID, ZoneMusic, IntroSound, Flags, AreaTableID, AreaName_Lang_enUS, AreaName_Lang_Mask)
SELECT 51127, 10872, 0, -1, 0, 0, 0, 0, 0, 4, 10038, 'Blade''s Edge Arena (Remastered)', 16712190
WHERE NOT EXISTS (SELECT 1 FROM dbc.wmoareatable_lplus WHERE WMOID = 10872);

-- map 1007  Guardian's Hall                WMOID 50000   (no rows - insert root)
INSERT INTO dbc.wmoareatable_lplus (ID, WMOID, NameSetID, WMOGroupID, SoundProviderPref, SoundProviderPrefUnderwater, AmbienceID, ZoneMusic, IntroSound, Flags, AreaTableID, AreaName_Lang_enUS, AreaName_Lang_Mask)
SELECT 51128, 50000, 0, -1, 0, 0, 0, 0, 0, 4, 6137, 'Guardian''s Hall', 16712190
WHERE NOT EXISTS (SELECT 1 FROM dbc.wmoareatable_lplus WHERE WMOID = 50000);

-- map 1008  Spark of Creator               WMOID 100504  (no rows - insert root)
INSERT INTO dbc.wmoareatable_lplus (ID, WMOID, NameSetID, WMOGroupID, SoundProviderPref, SoundProviderPrefUnderwater, AmbienceID, ZoneMusic, IntroSound, Flags, AreaTableID, AreaName_Lang_enUS, AreaName_Lang_Mask)
SELECT 51129, 100504, 0, -1, 0, 0, 0, 0, 0, 4, 6138, 'Spark of Creator', 16712190
WHERE NOT EXISTS (SELECT 1 FROM dbc.wmoareatable_lplus WHERE WMOID = 100504);

-- map 1134  The Tiger's Peak               WMOID 7324    (no rows - insert root)
INSERT INTO dbc.wmoareatable_lplus (ID, WMOID, NameSetID, WMOGroupID, SoundProviderPref, SoundProviderPrefUnderwater, AmbienceID, ZoneMusic, IntroSound, Flags, AreaTableID, AreaName_Lang_enUS, AreaName_Lang_Mask)
SELECT 51130, 7324, 0, -1, 0, 0, 0, 0, 0, 4, 6732, 'The Tiger''s Peak', 16712190
WHERE NOT EXISTS (SELECT 1 FROM dbc.wmoareatable_lplus WHERE WMOID = 7324);

-- map 1401  Baradin Hold Arena             WMOID 6007    (no rows - insert root)
INSERT INTO dbc.wmoareatable_lplus (ID, WMOID, NameSetID, WMOGroupID, SoundProviderPref, SoundProviderPrefUnderwater, AmbienceID, ZoneMusic, IntroSound, Flags, AreaTableID, AreaName_Lang_enUS, AreaName_Lang_Mask)
SELECT 51131, 6007, 0, -1, 0, 0, 0, 0, 0, 4, 10109, 'Baradin Hold Arena', 16712190
WHERE NOT EXISTS (SELECT 1 FROM dbc.wmoareatable_lplus WHERE WMOID = 6007);

-- map 1402  Obelisk of the Stars           WMOID 5959    (no rows - insert root)
INSERT INTO dbc.wmoareatable_lplus (ID, WMOID, NameSetID, WMOGroupID, SoundProviderPref, SoundProviderPrefUnderwater, AmbienceID, ZoneMusic, IntroSound, Flags, AreaTableID, AreaName_Lang_enUS, AreaName_Lang_Mask)
SELECT 51132, 5959, 0, -1, 0, 0, 0, 0, 0, 4, 8463, 'Obelisk of the Stars', 16712190
WHERE NOT EXISTS (SELECT 1 FROM dbc.wmoareatable_lplus WHERE WMOID = 5959);

-- map 1403  The Twisting Nether            WMOID 50001   (no rows - insert root)
INSERT INTO dbc.wmoareatable_lplus (ID, WMOID, NameSetID, WMOGroupID, SoundProviderPref, SoundProviderPrefUnderwater, AmbienceID, ZoneMusic, IntroSound, Flags, AreaTableID, AreaName_Lang_enUS, AreaName_Lang_Mask)
SELECT 51133, 50001, 0, -1, 0, 0, 0, 0, 0, 4, 10051, 'The Twisting Nether', 16712190
WHERE NOT EXISTS (SELECT 1 FROM dbc.wmoareatable_lplus WHERE WMOID = 50001);

-- map 1504  Black Rook Hold Arena          WMOID 10359   (no rows - insert root)
INSERT INTO dbc.wmoareatable_lplus (ID, WMOID, NameSetID, WMOGroupID, SoundProviderPref, SoundProviderPrefUnderwater, AmbienceID, ZoneMusic, IntroSound, Flags, AreaTableID, AreaName_Lang_enUS, AreaName_Lang_Mask)
SELECT 51134, 10359, 0, -1, 0, 0, 0, 0, 0, 4, 10057, 'Black Rook Hold Arena', 16712190
WHERE NOT EXISTS (SELECT 1 FROM dbc.wmoareatable_lplus WHERE WMOID = 10359);

-- map 1552  Ashamane's Fall                WMOID 10620   (no rows - insert root)
INSERT INTO dbc.wmoareatable_lplus (ID, WMOID, NameSetID, WMOGroupID, SoundProviderPref, SoundProviderPrefUnderwater, AmbienceID, ZoneMusic, IntroSound, Flags, AreaTableID, AreaName_Lang_enUS, AreaName_Lang_Mask)
SELECT 51135, 10620, 0, -1, 0, 0, 0, 0, 0, 4, 10053, 'Ashamane''s Fall', 16712190
WHERE NOT EXISTS (SELECT 1 FROM dbc.wmoareatable_lplus WHERE WMOID = 10620);

-- map 1683  The Inventor's Library         WMOID 5406    (6 existing rows)
UPDATE dbc.wmoareatable_lplus SET Flags = (Flags & ~2) | 4, AreaTableID = 8443 WHERE WMOID = 5406;

-- map 1684  Amphitheater of Anguish        WMOID 5110    (44 existing rows)
UPDATE dbc.wmoareatable_lplus SET Flags = (Flags & ~2) | 4, AreaTableID = 8442 WHERE WMOID = 5110;

