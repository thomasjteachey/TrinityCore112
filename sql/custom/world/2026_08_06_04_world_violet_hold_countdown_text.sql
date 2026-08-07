-- Violet Hold wave countdown, shown as a raid-warning style notification.
--
-- The wave prep window was announced once as a chat line, which is easy to miss
-- in a fight. This is the string the per-second countdown formats, delivered
-- through PSendMessageToAll with CHAT_MSG_RAID_BOSS_EMOTE so the client renders
-- it in the centre of the screen like a boss emote / raid warning rather than
-- burying it in the chat frame.
--
-- 20100 is clear of everything in use (trinity_string currently tops out at
-- 20077).
--
-- Replayable.

DELETE FROM trinity_string WHERE entry = 20100;
INSERT INTO trinity_string (entry, content_default) VALUES
  (20100, 'Next wave in %u...');
