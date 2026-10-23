
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
DROP TABLE IF EXISTS `battleground_start_exit`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!50503 SET character_set_client = utf8mb4 */;
CREATE TABLE `battleground_start_exit` (
  `BgTypeId` int unsigned NOT NULL COMMENT 'battleground_template.ID',
  `TeamId` tinyint unsigned NOT NULL COMMENT '0 = the Alliance start, 1 = the Horde start',
  `X` float NOT NULL,
  `Y` float NOT NULL,
  `Z` float NOT NULL,
  `O` float NOT NULL DEFAULT '0',
  `Radius` float NOT NULL DEFAULT '25' COMMENT 'from the team start: who is still this close is moved',
  `DelayMs` int unsigned NOT NULL DEFAULT '0' COMMENT 'after the gates open',
  `BotsOnly` tinyint unsigned NOT NULL DEFAULT '0' COMMENT '1 = playerbots and their pets only',
  `Comment` varchar(255) NOT NULL DEFAULT '',
  PRIMARY KEY (`BgTypeId`,`TeamId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci COMMENT='Where a team is put when the gates open, see BattlegroundStartExit';
/*!40101 SET character_set_client = @saved_cs_client */;

LOCK TABLES `battleground_start_exit` WRITE;
/*!40000 ALTER TABLE `battleground_start_exit` DISABLE KEYS */;
INSERT INTO `battleground_start_exit` VALUES (881,0,4540.7,-1510.5,386.2,3.14159,25,0,0,'The Twisting Nether: north balcony (Alliance) to the platform\'s north edge'),(881,1,4460.7,-1510.5,386.2,0,25,0,0,'The Twisting Nether: south balcony (Horde) to the platform\'s south edge'),(886,0,777.68,1324.2,267.488,3.92699,25,0,0,'The Eye of Eternity: north-west pad (Alliance) onto the Eye'),(886,1,731.011,1277.54,267.488,0.7854,25,0,0,'The Eye of Eternity: south-east pad (Horde) onto the Eye');
/*!40000 ALTER TABLE `battleground_start_exit` ENABLE KEYS */;
UNLOCK TABLES;
/*!40103 SET TIME_ZONE=@OLD_TIME_ZONE */;

/*!40101 SET SQL_MODE=@OLD_SQL_MODE */;
/*!40014 SET FOREIGN_KEY_CHECKS=@OLD_FOREIGN_KEY_CHECKS */;
/*!40014 SET UNIQUE_CHECKS=@OLD_UNIQUE_CHECKS */;
/*!40101 SET CHARACTER_SET_CLIENT=@OLD_CHARACTER_SET_CLIENT */;
/*!40101 SET CHARACTER_SET_RESULTS=@OLD_CHARACTER_SET_RESULTS */;
/*!40101 SET COLLATION_CONNECTION=@OLD_COLLATION_CONNECTION */;
/*!40111 SET SQL_NOTES=@OLD_SQL_NOTES */;

