-- The hourly Gurubashi chest never closed, and never paid out.
--
-- Both symptoms are one omission. gameobject_template 179697 ships with an
-- empty ScriptName, so go_custom_gurubashi_hourly_chest was never attached to
-- the object - and that script is the only thing that hands over the Marks of
-- Honor and then despawns the chest afterwards. The C++ side has been compiled
-- and registered the whole time; nothing ever bound it to the game object, so
-- OnLootStateChanged could not fire and the chest sat there looking lootable
-- for as long as it lived.

UPDATE `gameobject_template`
SET `ScriptName` = 'go_custom_gurubashi_hourly_chest'
WHERE `entry` = 179697;
