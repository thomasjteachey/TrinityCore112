-- Bind the four collision auras to their server-side registration scripts.
--
-- These bindings are what make PLAYERBOTS respect collision. Human players are
-- handled entirely by the client tweak (movement is client-authoritative, so
-- their own client clips them); bots have no client and are moved server-side by
-- the motion master, so the server has to know who is solid.
--
-- Each script tags the wearer with which rule the aura represents; the bot
-- movement code (BuildCollisionSafeDestination) then shortens any movement
-- segment that would enter a blocker. The spell ids live only here - the core
-- never hardcodes them.
--
-- Adding these does NOT change human-vs-human behaviour, which continues to be
-- decided client-side.
--
-- Replayable.

DELETE FROM `spell_script_names` WHERE `spell_id` IN (90210, 90211, 90212, 90213);
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
  (90210, 'spell_collision_block_enemies'),    -- Obstruction: one-way, enemies
  (90211, 'spell_collision_block_all'),        -- Immovable:   one-way, everyone
  (90212, 'spell_collision_mutual_enemies'),   -- Bodycheck:   mutual, enemies
  (90213, 'spell_collision_mutual_all');       -- Solid Form:  mutual, everyone
