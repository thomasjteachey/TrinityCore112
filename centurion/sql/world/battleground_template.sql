
/*!40101 SET @OLD_CHARACTER_SET_CLIENT=@@CHARACTER_SET_CLIENT */;
/*!40101 SET @OLD_CHARACTER_SET_RESULTS=@@CHARACTER_SET_RESULTS */;
/*!40101 SET @OLD_COLLATION_CONNECTION=@@COLLATION_CONNECTION */;
/*!50503 SET NAMES utf8mb4 */;
/*!40103 SET @OLD_TIME_ZONE=@@TIME_ZONE */;
/*!40103 SET TIME_ZONE='+00:00' */;
/*!40014 SET @OLD_UNIQUE_CHECKS=@@UNIQUE_CHECKS, UNIQUE_CHECKS=0 */;
/*!40014 SET @OLD_FOREIGN_KEY_CHECKS=@@FOREIGN_KEY_CHECKS, FOREIGN_KEY_CHECKS=0 */;
/*!40101 SET @OLD_SQL_MODE=@@SQL_MODE, SQL_MODE='NO_AUTO_VALUE_ON_ZERO' */;
/*!40111 SET @OLD_SQL_NOTES=@@SQL_NOTES, SQL_NOTES=0 */;
DROP TABLE IF EXISTS `battleground_template`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!50503 SET character_set_client = utf8mb4 */;
CREATE TABLE `battleground_template` (
  `ID` int unsigned NOT NULL DEFAULT '0',
  `MinPlayersPerTeam` smallint unsigned NOT NULL DEFAULT '0',
  `MaxPlayersPerTeam` smallint unsigned NOT NULL DEFAULT '0',
  `MinLvl` tinyint unsigned NOT NULL DEFAULT '0',
  `MaxLvl` tinyint unsigned NOT NULL DEFAULT '0',
  `AllianceStartLoc` int unsigned NOT NULL,
  `AllianceStartO` float NOT NULL,
  `HordeStartLoc` int unsigned NOT NULL,
  `HordeStartO` float NOT NULL,
  `StartMaxDist` float NOT NULL DEFAULT '0',
  `Weight` tinyint unsigned NOT NULL DEFAULT '1',
  `ScriptName` char(64) CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci NOT NULL DEFAULT '',
  `Comment` char(32) CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci NOT NULL,
  PRIMARY KEY (`ID`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
/*!40101 SET character_set_client = @saved_cs_client */;

LOCK TABLES `battleground_template` WRITE;
/*!40000 ALTER TABLE `battleground_template` DISABLE KEYS */;
INSERT INTO `battleground_template` VALUES (1,20,40,80,80,611,3.16312,610,0.715504,100,1,'','Alterac Valley'),(2,5,10,10,80,769,3.14159,770,0.151581,75,1,'','Warsong Gulch'),(3,8,15,10,60,890,3.91571,889,0.813671,75,1,'','Arathi Basin'),(4,0,2,10,80,929,0,936,3.14159,0,1,'BGReplayBGScript','Nagrand Arena'),(5,0,2,10,80,939,0,940,3.14159,0,1,'BGReplayBGScript','Blades\'s Edge Arena'),(6,0,2,10,80,0,0,0,0,0,1,'BGReplayBGScript','All Arena'),(7,8,15,80,80,1103,3.03123,1104,0.055761,75,1,'','Eye of The Storm'),(8,0,2,10,80,1258,0,1259,3.14159,0,1,'BGReplayBGScript','Ruins of Lordaeron'),(9,7,15,80,80,1367,0,1368,0,0,1,'','Strand of the Ancients'),(10,5,5,10,80,1362,0,1363,3.14159,0,1,'BGReplayBGScript','Dalaran Sewers'),(11,5,5,10,80,1364,0,1365,0,0,1,'BGReplayBGScript','The Ring of Valor'),(30,20,40,80,80,1485,0,1486,3.16124,200,1,'','Isle of Conquest'),(31,5,10,71,80,1731,0,1730,3,120,1,'','Slavery Valley'),(32,10,10,80,80,0,0,0,0,0,1,'','Random battleground'),(100,2,5,10,69,51890,0,51891,3.14159,40,1,'','Scarlet Chapel'),(101,2,10,10,69,52300,1.54331,52301,4.73988,30,1,'','Blackrock Throne'),(102,2,20,60,69,52320,0.07828,52321,3.21202,30,1,'','Obsidian Colosseum'),(103,0,2,10,80,52410,3.73855,52411,5.42715,0,1,'BGReplayBGScript','Nefarian\'s Arena'),(104,2,10,1,80,52500,6.15124,52501,2.50524,40,1,'','Tanaris Deathmatch'),(105,1,40,10,69,52520,0.027,52521,3.211,0,1,'','The Violet Hold Gauntlet'),(108,5,10,10,80,1726,2.57218,1727,6.16538,120,1,'','Twin Peaks'),(120,5,10,1,60,1739,0,1738,0,75,1,'','Battle for Gilneas'),(870,0,5,10,80,4136,3.14159,4137,0,0,1,'BGReplayBGScript','Tol\'Viron Arena'),(871,5,5,10,80,4535,0,4534,0,0,1,'BGReplayBGScript','The Tiger\'s Peak Arena'),(872,0,5,10,80,52600,6.26357,52601,3.11411,0,1,'BGReplayBGScript','Colosseum of Past Echoes'),(873,0,5,10,80,52602,5.81038,52603,2.66878,0,1,'BGReplayBGScript','Imperial Arena of Thakraj'),(874,0,5,10,80,52604,1.5654,52605,4.70699,0,1,'BGReplayBGScript','Maldraxxus Colosseum'),(875,0,5,10,80,52606,5.19907,52607,2.05747,0,1,'BGReplayBGScript','Nagrand Arena (Remastered)'),(876,0,5,10,80,52608,4.9734,52609,1.83181,0,1,'BGReplayBGScript','Blade\'s Edge Arena (Remastered)'),(877,0,5,10,80,52610,1.57063,52611,4.71222,0,1,'BGReplayBGScript','Guardian\'s Hall'),(878,0,5,10,80,52612,4.71277,52613,1.57118,0,1,'BGReplayBGScript','Spark of Creator'),(879,0,5,10,80,52614,3.94887,52615,0.807273,0,1,'BGReplayBGScript','Baradin Hold Arena'),(880,0,5,10,80,52616,4.65047,52617,1.50888,0,1,'BGReplayBGScript','Obelisk of the Stars'),(881,0,5,10,80,52618,3.13606,52619,6.27766,0,1,'BGReplayBGScript','The Twisting Nether'),(882,0,5,10,80,52620,0.271261,52621,3.41285,0,1,'BGReplayBGScript','Black Rook Hold Arena'),(883,0,5,10,80,52622,4.64055,52623,1.49896,0,1,'BGReplayBGScript','Ashamane\'s Fall'),(884,0,5,10,80,52624,1.31895,52625,4.46054,0,1,'BGReplayBGScript','The Inventor\'s Library'),(885,0,5,10,80,52626,3.57646,52627,0.434869,0,1,'BGReplayBGScript','Amphitheater of Anguish');
/*!40000 ALTER TABLE `battleground_template` ENABLE KEYS */;
UNLOCK TABLES;
/*!40103 SET TIME_ZONE=@OLD_TIME_ZONE */;

/*!40101 SET SQL_MODE=@OLD_SQL_MODE */;
/*!40014 SET FOREIGN_KEY_CHECKS=@OLD_FOREIGN_KEY_CHECKS */;
/*!40014 SET UNIQUE_CHECKS=@OLD_UNIQUE_CHECKS */;
/*!40101 SET CHARACTER_SET_CLIENT=@OLD_CHARACTER_SET_CLIENT */;
/*!40101 SET CHARACTER_SET_RESULTS=@OLD_CHARACTER_SET_RESULTS */;
/*!40101 SET COLLATION_CONNECTION=@OLD_COLLATION_CONNECTION */;
/*!40111 SET SQL_NOTES=@OLD_SQL_NOTES */;

