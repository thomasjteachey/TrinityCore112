
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
DROP TABLE IF EXISTS `rift_league_unit`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!50503 SET character_set_client = utf8mb4 */;
CREATE TABLE `rift_league_unit` (
  `kind` varchar(32) NOT NULL,
  `health` float NOT NULL DEFAULT '0',
  `health_growth` float NOT NULL DEFAULT '0',
  `damage` float NOT NULL DEFAULT '0',
  `damage_growth` float NOT NULL DEFAULT '0',
  `damage_cap` float NOT NULL DEFAULT '0',
  `attack_speed` float NOT NULL DEFAULT '1',
  `armor` float NOT NULL DEFAULT '0',
  `armor_growth` float NOT NULL DEFAULT '0',
  `armor_cap` float NOT NULL DEFAULT '0',
  `resist` float NOT NULL DEFAULT '0',
  `resist_growth` float NOT NULL DEFAULT '0',
  `resist_cap` float NOT NULL DEFAULT '0',
  `growth_start` float NOT NULL DEFAULT '0' COMMENT 'minutes of League game time before growth starts',
  `growth_step` float NOT NULL DEFAULT '0' COMMENT 'minutes per growth step; 0 = the growth is per minute, continuous',
  `bonus_armor` float NOT NULL DEFAULT '0',
  `bonus_armor_fade_start` float NOT NULL DEFAULT '0',
  `bonus_armor_fade_end` float NOT NULL DEFAULT '0',
  `backdoor_armor` float NOT NULL DEFAULT '0',
  `backdoor_resist` float NOT NULL DEFAULT '0',
  `damage_vs_champions` float NOT NULL DEFAULT '1',
  `damage_vs_structures` float NOT NULL DEFAULT '1',
  `turret_damage_taken` float NOT NULL DEFAULT '1',
  `armor_pen_pct` float NOT NULL DEFAULT '0',
  `comment` varchar(255) NOT NULL DEFAULT '',
  PRIMARY KEY (`kind`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci COMMENT='Summoner''s Rift units in League (S3) units: RiftLeague converts them';
/*!40101 SET character_set_client = @saved_cs_client */;

LOCK TABLES `rift_league_unit` WRITE;
/*!40000 ALTER TABLE `rift_league_unit` DISABLE KEYS */;
INSERT INTO `rift_league_unit` VALUES ('ancient_krug',1200,48,54,1.6,0,0.613,12,0,0,-10,0,0,0,0,0,0,0,0,0,1,1,1,0,'S3 Golem (big)'),('baron',8800,140,460,0,0,0.208,120,0,0,70,0,0,0,0,0,0,0,0,0,1,1,1,0,'S3 Baron Nashor'),('blue_sentinel',1400,90,65,1.825,0,0.613,20,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,0,'S3 Ancient Golem'),('caster',280,15,23,2,0,0.67,0,1.25,0,0,2,0,0,3,0,0,0,0,0,0.85,1.5,1,0,'S3 caster minion'),('crimson_raptor',1000,39,35,0.75,0,0.638,15,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,0,'S3 Wraith'),('dragon',3500,240,145,0,0,0.658,21,0,0,30,0,0,0,0,0,0,0,0,0,1,1,1,0,'S3 Dragon'),('greater_murk_wolf',1100,40,35,0.7,0,0.679,9,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,0,'S3 Giant Wolf'),('gromp',1000,39,35,0.75,0,0.638,15,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,0,'no S3 camp: the S3 Wraith\'s numbers'),('inhibitor',4000,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,0,'S3 inhibitor'),('krug',300,37,30,1.3,0,0.613,12,0,0,-10,0,0,0,0,0,0,0,0,0,1,1,1,0,'S3 Golem (small)'),('melee',445,20,12,1,0,1.25,0,2,0,0,1.25,0,0,3,0,0,0,0,0,0.85,1.5,1,0,'S3 melee minion'),('murk_wolf',200,25,8,0.43,0,0.679,9,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,0,'S3 Lesser Wolf (attack speed, armor: the Giant Wolf\'s)'),('nexus',5500,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,0,'S3 nexus'),('raptor',90,10.8,6,0.198,0,0.638,5,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,0,'S3 Lesser Wraith x 3/5 (five raptors for three wraiths)'),('red_brambleback',1400,90,65,1.825,0,0.625,20,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,0,'S3 Lizard Elder'),('rift_scuttler',1000,39,0,0,0,0.638,15,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,0,'no S3 camp: the S3 Wraith\'s health, harmless'),('siege',700,27,40,3,0,1,15,3,0,0,3,0,0,3,0,0,0,0,0,0.85,1.5,0.7,0,'S3 siege minion (not spawned yet)'),('super',1500,200,190,10,0,0.694,30,0,0,-30,0,0,0,3,0,0,0,0,0,0.85,1.5,0.7,0,'S3 super minion (not spawned yet)'),('turret_inhibitor',2550,0,190,4,310,0.833,67,1,97,100,1,130,8,0,0,0,0,150,150,1,1,1,30,'S3 inhibitor turret'),('turret_inner',2550,0,170,4,250,0.833,60,1,80,100,1,120,8,0,0,0,0,150,150,1,1,1,30,'S3 inner turret'),('turret_nexus',2425,0,180,4,300,0.833,65,1,95,100,1,130,8,0,0,0,0,150,150,1,1,1,30,'S3 nexus turret'),('turret_outer',2550,0,152,4,184,0.833,60,0,0,100,0,0,0,0,60,4,8,150,150,1,1,1,30,'S3 outer turret');
/*!40000 ALTER TABLE `rift_league_unit` ENABLE KEYS */;
UNLOCK TABLES;
/*!40103 SET TIME_ZONE=@OLD_TIME_ZONE */;

/*!40101 SET SQL_MODE=@OLD_SQL_MODE */;
/*!40014 SET FOREIGN_KEY_CHECKS=@OLD_FOREIGN_KEY_CHECKS */;
/*!40014 SET UNIQUE_CHECKS=@OLD_UNIQUE_CHECKS */;
/*!40101 SET CHARACTER_SET_CLIENT=@OLD_CHARACTER_SET_CLIENT */;
/*!40101 SET CHARACTER_SET_RESULTS=@OLD_CHARACTER_SET_RESULTS */;
/*!40101 SET COLLATION_CONNECTION=@OLD_COLLATION_CONNECTION */;
/*!40111 SET SQL_NOTES=@OLD_SQL_NOTES */;

