
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
DROP TABLE IF EXISTS `battleground_random_pool`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!50503 SET character_set_client = utf8mb4 */;
CREATE TABLE `battleground_random_pool` (
  `PoolBgTypeId` int unsigned NOT NULL COMMENT 'BattlegroundTypeId players queue for (6=All Arenas, 32=Random BG)',
  `MemberBgTypeId` int unsigned NOT NULL COMMENT 'BattlegroundTypeId that may be rolled',
  `Weight` double NOT NULL DEFAULT '1' COMMENT 'Relative selection weight; must be > 0',
  `Enabled` tinyint unsigned NOT NULL DEFAULT '1' COMMENT '0 disables without deleting',
  `Comment` varchar(64) NOT NULL DEFAULT '',
  PRIMARY KEY (`PoolBgTypeId`,`MemberBgTypeId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci COMMENT='Random battleground/arena selection pools';
/*!40101 SET character_set_client = @saved_cs_client */;

LOCK TABLES `battleground_random_pool` WRITE;
/*!40000 ALTER TABLE `battleground_random_pool` DISABLE KEYS */;
INSERT INTO `battleground_random_pool` VALUES (6,4,1,1,'Nagrand Arena'),(6,5,1,1,'Blade\'s Edge Arena'),(6,8,1,1,'Ruins of Lordaeron'),(6,10,1,1,'Dalaran Sewers'),(6,11,1,1,'The Ring of Valor'),(6,103,1,1,'Nefarian\'s Arena'),(6,870,1,1,'Tol\'Viron Arena'),(6,871,1,1,'The Tiger\'s Peak'),(6,872,1,1,'Colosseum of Past Echoes'),(6,873,1,1,'Imperial Arena of Thakraj'),(6,874,1,1,'Maldraxxus Colosseum'),(6,875,1,1,'Nagrand Arena (Remastered)'),(6,876,1,1,'Blade\'s Edge Arena (Remastered)'),(6,877,1,1,'Guardian\'s Hall'),(6,878,1,1,'Spark of Creator'),(6,879,1,1,'Baradin Hold Arena'),(6,880,1,0,'Obelisk of the Stars'),(6,881,1,1,'The Twisting Nether'),(6,882,1,1,'Black Rook Hold Arena'),(6,883,1,1,'Ashamane\'s Fall'),(6,884,1,0,'The Inventor\'s Library'),(6,885,1,0,'Amphitheater of Anguish'),(6,886,1,1,'The Eye of Eternity'),(6,888,1,1,'The Gamesman\'s Hall'),(889,103,1,1,'Nefarian\'s Arena'),(889,870,1,1,'Tol\'Viron Arena'),(889,871,1,1,'The Tiger\'s Peak'),(889,872,1,1,'Colosseum of Past Echoes'),(889,873,1,1,'Imperial Arena of Thakraj'),(889,874,1,1,'Maldraxxus Colosseum'),(889,875,1,1,'Nagrand Arena (remake)'),(889,876,1,1,'Blade\'s Edge Arena (remake)'),(889,877,1,1,'Guardian\'s Hall'),(889,878,1,1,'Spark of Creator'),(889,879,1,1,'Baradin Hold Arena'),(889,881,1,1,'The Twisting Nether'),(889,882,1,1,'Black Rook Hold Arena'),(889,883,1,1,'Ashamane\'s Fall'),(889,886,1,1,'The Eye of Eternity'),(889,888,1,1,'The Gamesman\'s Hall');
/*!40000 ALTER TABLE `battleground_random_pool` ENABLE KEYS */;
UNLOCK TABLES;
/*!40103 SET TIME_ZONE=@OLD_TIME_ZONE */;

/*!40101 SET SQL_MODE=@OLD_SQL_MODE */;
/*!40014 SET FOREIGN_KEY_CHECKS=@OLD_FOREIGN_KEY_CHECKS */;
/*!40014 SET UNIQUE_CHECKS=@OLD_UNIQUE_CHECKS */;
/*!40101 SET CHARACTER_SET_CLIENT=@OLD_CHARACTER_SET_CLIENT */;
/*!40101 SET CHARACTER_SET_RESULTS=@OLD_CHARACTER_SET_RESULTS */;
/*!40101 SET COLLATION_CONNECTION=@OLD_COLLATION_CONNECTION */;
/*!40111 SET SQL_NOTES=@OLD_SQL_NOTES */;

