
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
DROP TABLE IF EXISTS `rift_league_champion`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!50503 SET character_set_client = utf8mb4 */;
CREATE TABLE `rift_league_champion` (
  `class` tinyint unsigned NOT NULL,
  `role` varchar(16) NOT NULL DEFAULT '',
  `health` float NOT NULL DEFAULT '0',
  `health_per_level` float NOT NULL DEFAULT '0',
  `armor` float NOT NULL DEFAULT '0',
  `armor_per_level` float NOT NULL DEFAULT '0',
  `resist` float NOT NULL DEFAULT '0',
  `resist_per_level` float NOT NULL DEFAULT '0',
  `damage` float NOT NULL DEFAULT '0',
  `damage_per_level` float NOT NULL DEFAULT '0',
  `attack_speed` float NOT NULL DEFAULT '0.625',
  `attack_speed_per_level` float NOT NULL DEFAULT '0' COMMENT 'a fraction of the base per level',
  `mana` float NOT NULL DEFAULT '0',
  `mana_per_level` float NOT NULL DEFAULT '0',
  `rune_damage` float NOT NULL DEFAULT '0',
  `rune_armor` float NOT NULL DEFAULT '0',
  `rune_resist_per_level` float NOT NULL DEFAULT '0',
  `rune_ability_power` float NOT NULL DEFAULT '0',
  `rune_ability_power_per_level` float NOT NULL DEFAULT '0',
  `rune_magic_pen` float NOT NULL DEFAULT '0',
  `comment` varchar(255) NOT NULL DEFAULT '',
  PRIMARY KEY (`class`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci COMMENT='Summoner''s Rift: each class''s League (S3) base stats (stat at level l = base + per_level * l)';
/*!40101 SET character_set_client = @saved_cs_client */;

LOCK TABLES `rift_league_champion` WRITE;
/*!40000 ALTER TABLE `rift_league_champion` DISABLE KEYS */;
INSERT INTO `rift_league_champion` VALUES (1,'fighter',447,94.6667,18.3333,3.3167,30,1.25,54.8667,3.4167,0.6335,0.0323,222.5,37.5,15.26,12.66,1.333,0,0,0,'Warrior: S3 fighter average (Garen, Jax, Malphite) + a fighter rune page'),(2,'fighter',447,94.6667,18.3333,3.3167,30,1.25,54.8667,3.4167,0.6335,0.0323,222.5,37.5,15.26,12.66,1.333,0,0,0,'Paladin: S3 fighter average (Garen, Jax, Malphite) + a fighter rune page'),(3,'marksman',406.667,81.3333,13.1667,3.3,30,0,46.6,2.95,0.6463,0.0367,213.333,36,15.26,12.66,1.333,0,0,0,'Hunter: S3 marksman average (Ashe, Caitlyn, MissFortune) + a marksman rune page'),(4,'fighter',447,94.6667,18.3333,3.3167,30,1.25,54.8667,3.4167,0.6335,0.0323,222.5,37.5,15.26,12.66,1.333,0,0,0,'Rogue: S3 fighter average (Garen, Jax, Malphite) + a fighter rune page'),(5,'mage',372,81,9.75,3.95,30,0,50,2.8125,0.6019,0.0174,250,52.5,0,12.66,0,14.85,1.53,8.5,'Priest: S3 mage average (Annie, Ryze) + a mage rune page'),(6,'fighter',447,94.6667,18.3333,3.3167,30,1.25,54.8667,3.4167,0.6335,0.0323,222.5,37.5,15.26,12.66,1.333,0,0,0,'Death Knight: S3 fighter average (Garen, Jax, Malphite) + a fighter rune page'),(7,'fighter',447,94.6667,18.3333,3.3167,30,1.25,54.8667,3.4167,0.6335,0.0323,222.5,37.5,15.26,12.66,1.333,0,0,0,'Shaman: S3 fighter average (Garen, Jax, Malphite) + a fighter rune page'),(8,'mage',372,81,9.75,3.95,30,0,50,2.8125,0.6019,0.0174,250,52.5,0,12.66,0,14.85,1.53,8.5,'Mage: S3 mage average (Annie, Ryze) + a mage rune page'),(9,'mage',372,81,9.75,3.95,30,0,50,2.8125,0.6019,0.0174,250,52.5,0,12.66,0,14.85,1.53,8.5,'Warlock: S3 mage average (Annie, Ryze) + a mage rune page'),(11,'fighter',447,94.6667,18.3333,3.3167,30,1.25,54.8667,3.4167,0.6335,0.0323,222.5,37.5,15.26,12.66,1.333,0,0,0,'Druid: S3 fighter average (Garen, Jax, Malphite) + a fighter rune page');
/*!40000 ALTER TABLE `rift_league_champion` ENABLE KEYS */;
UNLOCK TABLES;
/*!40103 SET TIME_ZONE=@OLD_TIME_ZONE */;

/*!40101 SET SQL_MODE=@OLD_SQL_MODE */;
/*!40014 SET FOREIGN_KEY_CHECKS=@OLD_FOREIGN_KEY_CHECKS */;
/*!40014 SET UNIQUE_CHECKS=@OLD_UNIQUE_CHECKS */;
/*!40101 SET CHARACTER_SET_CLIENT=@OLD_CHARACTER_SET_CLIENT */;
/*!40101 SET CHARACTER_SET_RESULTS=@OLD_CHARACTER_SET_RESULTS */;
/*!40101 SET COLLATION_CONNECTION=@OLD_COLLATION_CONNECTION */;
/*!40111 SET SQL_NOTES=@OLD_SQL_NOTES */;

