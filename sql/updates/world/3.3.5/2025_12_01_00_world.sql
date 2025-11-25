-- Add missing reset notification strings so players see informative messages
DELETE FROM `trinity_string` WHERE `entry` IN (215, 216);
INSERT INTO `trinity_string` (`entry`, `content_default`, `content_loc1`, `content_loc2`, `content_loc3`, `content_loc4`, `content_loc5`, `content_loc6`, `content_loc7`, `content_loc8`)
VALUES
(215, 'Your spells have been reset.', NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL),
(216, 'Your talents have been reset.', NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
