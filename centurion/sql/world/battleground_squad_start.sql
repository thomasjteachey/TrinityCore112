
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
DROP TABLE IF EXISTS `battleground_squad_start`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!50503 SET character_set_client = utf8mb4 */;
CREATE TABLE `battleground_squad_start` (
  `BgTypeId` int unsigned NOT NULL COMMENT 'battleground_template.ID',
  `Squad` tinyint unsigned NOT NULL COMMENT '0..3, no gaps; even squads sit on the Alliance side, odd on the Horde side',
  `X` float NOT NULL,
  `Y` float NOT NULL,
  `Z` float NOT NULL,
  `O` float NOT NULL DEFAULT '0',
  `Name` varchar(32) COLLATE utf8mb4_unicode_ci NOT NULL DEFAULT '' COMMENT 'how the squad is named in announcements, e.g. North-East',
  `FlagSpell` int unsigned NOT NULL DEFAULT '0' COMMENT 'team flag aura its members carry in an arena (0 = the side gold/green one)',
  `Comment` varchar(255) COLLATE utf8mb4_unicode_ci NOT NULL DEFAULT '',
  PRIMARY KEY (`BgTypeId`,`Squad`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Squads of a battleground played by more than two teams (Battleground::IsSquadMatch)';
/*!40101 SET character_set_client = @saved_cs_client */;

LOCK TABLES `battleground_squad_start` WRITE;
/*!40000 ALTER TABLE `battleground_squad_start` DISABLE KEYS */;
INSERT INTO `battleground_squad_start` VALUES (887,0,-1176.3,1043.32,121,3.94887,'Gold',92322,'the 879 Alliance start (WorldSafeLocs 52614)'),(887,1,-1288.29,926.32,121,0.80727,'Green',92323,'the 879 Horde start (WorldSafeLocs 52615)'),(887,2,-1176.3,926.32,121,2.33432,'Blue',92320,'where 879 keeps Shadow Sight 1'),(887,3,-1288.29,1043.32,121,5.47591,'Purple',92321,'where 879 keeps Shadow Sight 2');
/*!40000 ALTER TABLE `battleground_squad_start` ENABLE KEYS */;
UNLOCK TABLES;
/*!40103 SET TIME_ZONE=@OLD_TIME_ZONE */;

/*!40101 SET SQL_MODE=@OLD_SQL_MODE */;
/*!40014 SET FOREIGN_KEY_CHECKS=@OLD_FOREIGN_KEY_CHECKS */;
/*!40014 SET UNIQUE_CHECKS=@OLD_UNIQUE_CHECKS */;
/*!40101 SET CHARACTER_SET_CLIENT=@OLD_CHARACTER_SET_CLIENT */;
/*!40101 SET CHARACTER_SET_RESULTS=@OLD_CHARACTER_SET_RESULTS */;
/*!40101 SET COLLATION_CONNECTION=@OLD_COLLATION_CONNECTION */;
/*!40111 SET SQL_NOTES=@OLD_SQL_NOTES */;

