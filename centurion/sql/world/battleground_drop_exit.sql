
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
DROP TABLE IF EXISTS `battleground_drop_exit`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!50503 SET character_set_client = utf8mb4 */;
CREATE TABLE `battleground_drop_exit` (
  `BgTypeId` int unsigned NOT NULL COMMENT 'battleground_template.ID',
  `Idx` tinyint unsigned NOT NULL DEFAULT '0',
  `IslandX` float NOT NULL,
  `IslandY` float NOT NULL,
  `IslandRadius` float NOT NULL COMMENT 'horizontal, from IslandX/Y',
  `IslandMinZ` float NOT NULL COMMENT 'below this a bot is already off the island',
  `EdgeX` float NOT NULL,
  `EdgeY` float NOT NULL,
  `EdgeZ` float NOT NULL,
  `LandingX` float NOT NULL,
  `LandingY` float NOT NULL,
  `LandingZ` float NOT NULL,
  `Comment` varchar(255) NOT NULL DEFAULT '',
  PRIMARY KEY (`BgTypeId`,`Idx`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci COMMENT='Ledges playerbots are put down from, see BattlegroundDropExit';
/*!40101 SET character_set_client = @saved_cs_client */;

LOCK TABLES `battleground_drop_exit` WRITE;
/*!40000 ALTER TABLE `battleground_drop_exit` DISABLE KEYS */;
INSERT INTO `battleground_drop_exit` VALUES (9,0,1436.75,109.66,0,0,1436.75,109.66,41.65,1438.33,107.73,25.82,'Strand of the Ancients: Gate of the Green Emerald wall, turret 0 - off the wall to the ground in front (15.8 yd drop)'),(9,1,1405.24,84.39,0,0,1405.24,84.39,41.38,1406.94,82.56,31.31,'Strand of the Ancients: Gate of the Green Emerald wall, turret 1 - off the wall to the ground in front (10.1 yd drop)'),(9,2,1422.38,-196.01,0,0,1422.38,-196.01,42.45,1423.68,-193.87,30.51,'Strand of the Ancients: Gate of the Blue Sapphire wall, turret 4 - off the wall to the ground in front (11.9 yd drop)'),(9,3,1455.17,-220.04,0,0,1455.17,-220.04,42.18,1456.6,-217.99,29.96,'Strand of the Ancients: Gate of the Blue Sapphire wall, turret 5 - off the wall to the ground in front (12.2 yd drop)'),(9,4,1232.8,-187.3,0,0,1232.8,-187.3,66.98,1239.1,-184.25,46.55,'Strand of the Ancients: Gate of the Red Sun wall, turret 6 - off the wall to the ground in front (20.4 yd drop)'),(9,5,1250.04,-223.89,0,0,1250.04,-223.89,66.98,1252.05,-222.41,54.93,'Strand of the Ancients: Gate of the Red Sun wall, turret 7 - off the wall to the ground in front (12.0 yd drop)'),(9,6,1236.64,92.03,0,0,1236.64,92.03,65.11,1238.8,90.76,51.35,'Strand of the Ancients: Gate of the Mauve Amethyst wall, turret 8 - off the wall to the ground in front (13.8 yd drop)'),(9,7,1215.55,57.53,0,0,1215.55,57.53,64.85,1217.74,56.33,52.69,'Strand of the Ancients: Gate of the Mauve Amethyst wall, turret 9 - off the wall to the ground in front (12.2 yd drop)'),(9,8,1069.19,-86.94,0,0,1069.19,-86.94,93.91,1071.69,-86.89,77.11,'Strand of the Ancients: Gate of the Yellow Moon wall, turret 2 - off the wall to the ground in front (16.8 yd drop)'),(9,9,1069.33,-127.51,0,0,1069.33,-127.51,96.58,1071.82,-127.29,78.65,'Strand of the Ancients: Gate of the Yellow Moon wall, turret 3 - off the wall to the ground in front (17.9 yd drop)'),(10,0,1352,817.25,21,12,1335.5,817.25,14.5,1326,817.25,3.6,'Dalaran Sewers: east start pipe (Alliance) - mouth to the ring'),(10,1,1230.5,765.1,21,12,1248.5,765.1,14.3,1258,765.1,3.6,'Dalaran Sewers: west start pipe (Horde) - mouth to the ring');
/*!40000 ALTER TABLE `battleground_drop_exit` ENABLE KEYS */;
UNLOCK TABLES;
/*!40103 SET TIME_ZONE=@OLD_TIME_ZONE */;

/*!40101 SET SQL_MODE=@OLD_SQL_MODE */;
/*!40014 SET FOREIGN_KEY_CHECKS=@OLD_FOREIGN_KEY_CHECKS */;
/*!40014 SET UNIQUE_CHECKS=@OLD_UNIQUE_CHECKS */;
/*!40101 SET CHARACTER_SET_CLIENT=@OLD_CHARACTER_SET_CLIENT */;
/*!40101 SET CHARACTER_SET_RESULTS=@OLD_CHARACTER_SET_RESULTS */;
/*!40101 SET COLLATION_CONNECTION=@OLD_COLLATION_CONNECTION */;
/*!40111 SET SQL_NOTES=@OLD_SQL_NOTES */;

