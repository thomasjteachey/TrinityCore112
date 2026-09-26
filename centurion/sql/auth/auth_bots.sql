-- The bot accounts. Bots log in inside the worldserver, never through authserver, so their
-- passwords are random bytes nobody knows.
INSERT INTO `account` (`id`, `username`, `salt`, `verifier`, `email`, `reg_mail`, `last_ip`, `expansion`) VALUES
(76, 'PLAYERBOTONE', RANDOM_BYTES(32), RANDOM_BYTES(32), '', '', '127.0.0.1', 2),
(77, 'PLAYERBOTTWO', RANDOM_BYTES(32), RANDOM_BYTES(32), '', '', '127.0.0.1', 2),
(78, 'PLAYERBOTTHREE', RANDOM_BYTES(32), RANDOM_BYTES(32), '', '', '127.0.0.1', 2),
(79, 'PLAYERBOTFOUR', RANDOM_BYTES(32), RANDOM_BYTES(32), '', '', '127.0.0.1', 2);
INSERT INTO `realmcharacters` (`realmid`, `acctid`, `numchars`) VALUES
(1, 76, 2),
(1, 77, 115),
(1, 78, 115),
(1, 79, 27);
