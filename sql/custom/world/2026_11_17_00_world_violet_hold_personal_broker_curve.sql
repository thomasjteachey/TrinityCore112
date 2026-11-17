-- Personal-broker mode doubles its base reward from wave 10 onward.

DELETE FROM `trinity_string` WHERE `entry` = 20107;
INSERT INTO `trinity_string` (`entry`, `content_default`) VALUES
  (20107, 'The Hold grows generous: beginning with wave %u''s clear, every player receives two private boon brokers after each wave.');
