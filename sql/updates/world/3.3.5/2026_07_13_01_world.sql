-- Obsidian Colosseum: remove the flagstand lock as well as the pickup spell so the client uses instant gameobject interaction.
UPDATE `gameobject_template`
SET `Data0` = 0, `Data1` = 0, `castBarCaption` = ''
WHERE `entry` = 300206;
