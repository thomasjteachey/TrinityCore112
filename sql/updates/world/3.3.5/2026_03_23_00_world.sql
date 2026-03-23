-- Warsong Gulch: use timeout-specific flag return text without player placeholder
DELETE FROM `broadcast_text` WHERE `ID` IN (910000, 910001);
INSERT INTO `broadcast_text` (`ID`, `LanguageID`, `Text`, `Text1`, `EmoteID1`, `EmoteID2`, `EmoteID3`, `EmoteDelay1`, `EmoteDelay2`, `EmoteDelay3`, `SoundEntriesID`, `EmotesID`, `Flags`, `VerifiedBuild`) VALUES
(910000, 0, 'The Alliance Flag was returned to its base!', '', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
(910001, 0, 'The Horde Flag was returned to its base!', '', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
