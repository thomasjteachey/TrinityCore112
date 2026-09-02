-- Join the two Booty Bay flight masters so the cross-faction networks meet.
--
-- Booty Bay has two taxi nodes about fifty-five yards apart:
--   18  (-14444.3, 509.62, 26.2)  served by Gringer (2858)
--   19  (-14473.0, 464.15, 36.43) served by Gyll (2859)
--
-- Both NPCs sit on faction 35 here, so either will talk to anybody - but their
-- taxi graphs never touched, so no route could cross between them. Flight
-- routing is graph-based: a player can only reach a destination that is
-- connected to the node they take off from. Adding a path in each direction
-- joins what were two separate networks into one.
--
-- (A third node, 9, also names itself Booty Bay but has no paths at all and is
-- left alone. Node 34 is the ship transport, not a flight master.)
--
-- The hop is short, so three waypoints each way: the two node positions with a
-- single raised point between them, high enough that the ride arcs over the
-- town rather than clipping through the buildings on the way.
--
-- IDs come from clear headroom - TaxiPath tops out at 1978 and TaxiPathNode at
-- 46874 - and neither DBC is ID-sorted, so appending is safe.
--
-- The binary TaxiPath.dbc and TaxiPathNode.dbc must be rebuilt to match, on the
-- server AND in the client patch: the client draws and animates the route from
-- its own copy, so a path the server knows and the client does not is a broken
-- flight rather than a missing one.
--
-- Re-runnable.

DELETE FROM dbc.taxipath_bplus     WHERE ID IN (1990, 1991);
DELETE FROM dbc.taxipathnode_bplus WHERE PathID IN (1990, 1991);

INSERT INTO dbc.taxipath_bplus (ID, FromTaxiNode, ToTaxiNode, Cost) VALUES
  (1990, 18, 19, 100),
  (1991, 19, 18, 100);

INSERT INTO dbc.taxipathnode_bplus
  (ID, PathID, NodeIndex, ContinentID, LocX, LocY, LocZ, Flags, Delay, ArrivalEventID, DepartureEventID)
VALUES
  -- 18 -> 19
  (47000, 1990, 0, 0, -14444.3, 509.62, 26.20, 0, 0, 0, 0),
  (47001, 1990, 1, 0, -14458.0, 487.00, 55.00, 0, 0, 0, 0),
  (47002, 1990, 2, 0, -14473.0, 464.15, 36.43, 0, 0, 0, 0),
  -- 19 -> 18
  (47003, 1991, 0, 0, -14473.0, 464.15, 36.43, 0, 0, 0, 0),
  (47004, 1991, 1, 0, -14458.0, 487.00, 55.00, 0, 0, 0, 0),
  (47005, 1991, 2, 0, -14444.3, 509.62, 26.20, 0, 0, 0, 0);

-- To undo:
--   DELETE FROM dbc.taxipath_bplus     WHERE ID IN (1990, 1991);
--   DELETE FROM dbc.taxipathnode_bplus WHERE PathID IN (1990, 1991);
-- (then rebuild both binaries and republish the patch)
