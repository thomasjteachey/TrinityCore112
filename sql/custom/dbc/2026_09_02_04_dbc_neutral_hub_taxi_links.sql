-- Join the paired flight masters in every neutral hub, as Booty Bay already is.
--
-- Same shape as 2026_09_02_02_dbc_booty_bay_taxi_link.sql, applied to the rest
-- of the towns that have the problem. Each of these places has two taxi nodes a
-- few dozen yards apart, one per original faction network, and the two graphs
-- never touched - so a player boarding at one flight master could not reach any
-- destination served by the other, standing right beside them.
--
-- Every NPC involved is faction 35 here, so both will already talk to anybody;
-- only the routing was still split.
--
--   Gadgetzan            39 <-> 40   Bera Stonehammer (7823) / Bulkrek Ragefist (7824)
--   Everlook             52 <-> 53   Maethrya (11138)        / Yugrek (11139)
--   Light's Hope Chapel  67 <-> 68   Khaelyn Steelwing (12617) / Georgia (12636)
--   Cenarion Hold        72 <-> 73   Runk Windtamer (15178)  / Cloud Skydancer (15177)
--   Thorium Point        74 <-> 75   Lanie Reed (2941)       / Grisha (3305)
--
-- Moonglade (62/63) is deliberately NOT here. It looks like the same case from
-- the node table, but no flight master stands on either node - the only one in
-- the zone is Sindrayl, 350 yards away - so there is nothing to board and
-- nothing to join.
--
-- Three waypoints each way, as at Booty Bay: the two node positions with one
-- raised point between them, set about twenty yards above the higher end so the
-- ride arcs over the town instead of clipping through it.
--
-- IDs continue the Booty Bay block - TaxiPath ended at 1991 and TaxiPathNode at
-- 47005 - and neither DBC is ID-sorted, so appending is safe. TaxiPathNode IS
-- order-sensitive though: transports walk a path's nodes in FILE order, so the
-- generated file must come out sorted by (PathID, NodeIndex). dbcgen enforces
-- that now; regenerating without it is what broke the zeppelins.
--
-- Both binaries must be rebuilt to match, on the server AND in the client
-- patch: the client draws and animates the route from its own copy, so a path
-- the server knows and the client does not is a broken flight, not a missing
-- one.
--
-- Re-runnable.

DELETE FROM dbc.taxipath_bplus     WHERE ID     BETWEEN 1992 AND 2001;
DELETE FROM dbc.taxipathnode_bplus WHERE PathID BETWEEN 1992 AND 2001;

INSERT INTO dbc.taxipath_bplus (ID, FromTaxiNode, ToTaxiNode, Cost) VALUES
  (1992, 39, 40, 100),
  (1993, 40, 39, 100),
  (1994, 52, 53, 100),
  (1995, 53, 52, 100),
  (1996, 67, 68, 100),
  (1997, 68, 67, 100),
  (1998, 72, 73, 100),
  (1999, 73, 72, 100),
  (2000, 74, 75, 100),
  (2001, 75, 74, 100);

INSERT INTO dbc.taxipathnode_bplus
  (ID, PathID, NodeIndex, ContinentID, LocX, LocY, LocZ, Flags, Delay, ArrivalEventID, DepartureEventID)
VALUES
  -- Gadgetzan, Tanaris
  (47006, 1992, 0, 1, -7223.97, -3734.59,   8.39, 0, 0, 0, 0),
  (47007, 1992, 1, 1, -7136.43, -3757.48,  30.00, 0, 0, 0, 0),
  (47008, 1992, 2, 1, -7048.89, -3780.36,  10.19, 0, 0, 0, 0),
  (47009, 1993, 0, 1, -7048.89, -3780.36,  10.19, 0, 0, 0, 0),
  (47010, 1993, 1, 1, -7136.43, -3757.48,  30.00, 0, 0, 0, 0),
  (47011, 1993, 2, 1, -7223.97, -3734.59,   8.39, 0, 0, 0, 0),
  -- Everlook, Winterspring
  (47012, 1994, 0, 1,  6796.80, -4742.39, 701.50, 0, 0, 0, 0),
  (47013, 1994, 1, 1,  6804.93, -4676.76, 730.00, 0, 0, 0, 0),
  (47014, 1994, 2, 1,  6813.06, -4611.12, 710.67, 0, 0, 0, 0),
  (47015, 1995, 0, 1,  6813.06, -4611.12, 710.67, 0, 0, 0, 0),
  (47016, 1995, 1, 1,  6804.93, -4676.76, 730.00, 0, 0, 0, 0),
  (47017, 1995, 2, 1,  6796.80, -4742.39, 701.50, 0, 0, 0, 0),
  -- Light's Hope Chapel, Eastern Plaguelands
  (47018, 1996, 0, 0,  2271.09, -5340.80,  87.11, 0, 0, 0, 0),
  (47019, 1996, 1, 0,  2299.25, -5313.85, 107.00, 0, 0, 0, 0),
  (47020, 1996, 2, 0,  2327.41, -5286.89,  81.78, 0, 0, 0, 0),
  (47021, 1997, 0, 0,  2327.41, -5286.89,  81.78, 0, 0, 0, 0),
  (47022, 1997, 1, 0,  2299.25, -5313.85, 107.00, 0, 0, 0, 0),
  (47023, 1997, 2, 0,  2271.09, -5340.80,  87.11, 0, 0, 0, 0),
  -- Cenarion Hold, Silithus
  (47024, 1998, 0, 1, -6811.39,   836.74,  49.81, 0, 0, 0, 0),
  (47025, 1998, 1, 1, -6786.61,   804.39, 109.00, 0, 0, 0, 0),
  (47026, 1998, 2, 1, -6761.83,   772.03,  88.91, 0, 0, 0, 0),
  (47027, 1999, 0, 1, -6761.83,   772.03,  88.91, 0, 0, 0, 0),
  (47028, 1999, 1, 1, -6786.61,   804.39, 109.00, 0, 0, 0, 0),
  (47029, 1999, 2, 1, -6811.39,   836.74,  49.81, 0, 0, 0, 0),
  -- Thorium Point, Searing Gorge
  (47030, 2000, 0, 0, -6552.59, -1168.27, 309.31, 0, 0, 0, 0),
  (47031, 2000, 1, 0, -6553.76, -1134.16, 330.00, 0, 0, 0, 0),
  (47032, 2000, 2, 0, -6554.93, -1100.05, 309.57, 0, 0, 0, 0),
  (47033, 2001, 0, 0, -6554.93, -1100.05, 309.57, 0, 0, 0, 0),
  (47034, 2001, 1, 0, -6553.76, -1134.16, 330.00, 0, 0, 0, 0),
  (47035, 2001, 2, 0, -6552.59, -1168.27, 309.31, 0, 0, 0, 0);

-- To undo:
--   DELETE FROM dbc.taxipath_bplus     WHERE ID     BETWEEN 1992 AND 2001;
--   DELETE FROM dbc.taxipathnode_bplus WHERE PathID BETWEEN 1992 AND 2001;
-- (then rebuild both binaries and republish the patch)
