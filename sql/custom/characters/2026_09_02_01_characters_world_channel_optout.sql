-- Remembers the characters who have deliberately left the World channel.
--
-- World is a default channel (ChatChannels.dbc id 27, CHANNEL_DBC_FLAG_INITIAL),
-- so everyone is in it without doing anything - and Player::UpdateLocalChannels
-- rejoins every eligible default channel on each zone change. For General that
-- is harmless, because the channel it rejoins is the new zone's. For one global
-- channel it means a player who types /leave World is put back moments later,
-- so the leave has to be recorded to mean anything.
--
-- A row here means "stay out". Joining the channel again deletes the row.
--
-- Only ever read for characters that reach the auto-join, once per session, so
-- an empty table costs a single indexed miss per login.
--
-- Re-runnable.

CREATE TABLE IF NOT EXISTS `character_world_channel_optout` (
  `guid` INT UNSIGNED NOT NULL COMMENT 'Character low GUID that left the World channel',
  PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
  COMMENT='Characters who opted out of the default World chat channel';
