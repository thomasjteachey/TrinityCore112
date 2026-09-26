
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
DROP TABLE IF EXISTS `quest_pool_template`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!50503 SET character_set_client = utf8mb4 */;
CREATE TABLE `quest_pool_template` (
  `poolId` int unsigned NOT NULL,
  `numActive` int unsigned NOT NULL COMMENT 'Number of indices to have active at any time',
  `description` varchar(255) CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci DEFAULT NULL,
  PRIMARY KEY (`poolId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
/*!40101 SET character_set_client = @saved_cs_client */;

LOCK TABLES `quest_pool_template` WRITE;
/*!40000 ALTER TABLE `quest_pool_template` DISABLE KEYS */;
INSERT INTO `quest_pool_template` VALUES (348,1,'Public Relations Agent <Crown Chemical Co.> - Daily Quests'),(349,1,'High Crusader Adelard - Daily Quests'),(350,1,'Toxic Tolerance Daily-Quests'),(351,1,'Toxic Tolerance Daily-Quests'),(352,1,'The Rokk <Master of Cooking> - Daily Quests'),(353,1,'Old Man Barlowned - Daily Quests'),(354,1,'Gretta the Arbiter - Daily Quests'),(356,1,'Wind Trader Zhareem - Daily Quests'),(357,1,'Nether-Stalker Mah\'duun - Daily Quests'),(359,1,'Oracle Soo-nee Dailies'),(360,1,'Oracle Soo-dow Dailies'),(361,1,'Rejek Dailies'),(362,1,'Vekgar Dailies'),(363,1,'Narasi Snowdawn <The Silver Covenant> - Daily Quests'),(364,1,'Savinia Loresong <The Silver Covenant> - Daily Quests'),(365,1,'Girana the Blooded <The Sunreavers> - Daily Quests'),(366,1,'Tylos Dawnrunner <The Sunreavers> - Daily Quests'),(367,1,'Crusader Silverdawn Dailies'),(370,1,'Troll Patrol Daily Quests'),(5662,1,'TournamentDaily Sunreavers'),(5663,1,'TournamentDaily Orks'),(5664,1,'TournamentDaily Trolls'),(5665,1,'TournamentDaily Taurens'),(5666,1,'TournamentDaily Undeads '),(5667,1,'TournamentDaily Bloodelfs'),(5668,1,'TournamentDaily Convenant'),(5669,1,'TournamentDaily Humans'),(5670,1,'TournamentDaily Dwarfs'),(5671,1,'TournamentDaily Gnomes'),(5672,1,'TournamentDaily Nightelfs'),(5673,1,'TournamentDaily Dranei'),(5674,1,'DalaranCookingDaily Ally'),(5675,1,'DalaranCookingDaily Horde'),(5676,1,'DalaranFishingDaily'),(5677,1,'DalaranJewelcraftingDaily'),(5678,1,'Raiding weeklies'),(5679,1,'General ICC 10man pool'),(5680,1,'General ICC 25man pool'),(5707,1,'Wintergrasp weekly quests (Alliance, attackers)'),(5708,1,'Wintergrasp weekly quests (Alliance, defenders)'),(5709,1,'Wintergrasp weekly quests (Horde, attackers)'),(5710,1,'Wintergrasp weekly quests (Horde, defenders)');
/*!40000 ALTER TABLE `quest_pool_template` ENABLE KEYS */;
UNLOCK TABLES;
/*!40103 SET TIME_ZONE=@OLD_TIME_ZONE */;

/*!40101 SET SQL_MODE=@OLD_SQL_MODE */;
/*!40014 SET FOREIGN_KEY_CHECKS=@OLD_FOREIGN_KEY_CHECKS */;
/*!40014 SET UNIQUE_CHECKS=@OLD_UNIQUE_CHECKS */;
/*!40101 SET CHARACTER_SET_CLIENT=@OLD_CHARACTER_SET_CLIENT */;
/*!40101 SET CHARACTER_SET_RESULTS=@OLD_CHARACTER_SET_RESULTS */;
/*!40101 SET COLLATION_CONNECTION=@OLD_COLLATION_CONNECTION */;
/*!40111 SET SQL_NOTES=@OLD_SQL_NOTES */;

