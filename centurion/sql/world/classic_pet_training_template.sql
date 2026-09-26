
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
DROP TABLE IF EXISTS `classic_pet_training_template`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!50503 SET character_set_client = utf8mb4 */;
CREATE TABLE `classic_pet_training_template` (
  `source_spell` int unsigned NOT NULL,
  `taught_spell` int unsigned NOT NULL,
  `source_name` varchar(128) CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci NOT NULL DEFAULT '',
  `taught_name` varchar(128) CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci NOT NULL DEFAULT '',
  `required_level` tinyint unsigned NOT NULL DEFAULT '1',
  `cost` smallint unsigned NOT NULL DEFAULT '0',
  `family_mask` bigint unsigned NOT NULL DEFAULT '0',
  `trainer_taught` tinyint unsigned NOT NULL DEFAULT '0',
  `wild_learned` tinyint unsigned NOT NULL DEFAULT '0',
  `previous_rank` int unsigned NOT NULL DEFAULT '0',
  `next_rank` int unsigned NOT NULL DEFAULT '0',
  `enabled` tinyint unsigned NOT NULL DEFAULT '1',
  PRIMARY KEY (`source_spell`),
  KEY `idx_taught_spell` (`taught_spell`),
  KEY `idx_family_mask` (`family_mask`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
/*!40101 SET character_set_client = @saved_cs_client */;

LOCK TABLES `classic_pet_training_template` WRITE;
/*!40000 ALTER TABLE `classic_pet_training_template` DISABLE KEYS */;
INSERT INTO `classic_pet_training_template` VALUES (1747,1742,'Cower','Cower',5,8,16781316,0,1,0,0,1),(1748,1753,'Cower','Cower',15,10,4100,0,1,0,0,1),(1749,1754,'Cower','Cower',25,12,16777220,0,1,0,0,1),(1750,1755,'Cower','Cower',35,14,16781316,0,1,0,0,1),(1751,1756,'Cower','Cower',45,16,16777220,0,1,0,0,1),(1853,2649,'Growl','Growl',1,0,0,1,0,0,0,1),(2975,16831,'Claw','Claw',32,13,68159892,0,1,0,0,1),(2976,16832,'Claw','Claw',40,17,68159892,0,1,0,0,1),(2977,3010,'Claw','Claw',48,21,68159892,0,1,0,0,1),(2980,16827,'Claw','Claw',1,1,68159892,0,1,0,0,1),(2981,16828,'Claw','Claw',8,4,68159892,0,1,0,0,1),(2982,16829,'Claw','Claw',16,7,68159892,0,1,0,0,1),(3666,3009,'Claw','Claw',56,25,68159892,0,1,0,0,1),(3667,16830,'Claw','Claw',24,10,68159892,0,1,0,0,1),(4195,4187,'Great Stamina','Great Stamina',10,5,0,1,0,0,0,1),(4196,4188,'Great Stamina','Great Stamina',12,10,0,1,0,0,0,1),(4197,4189,'Great Stamina','Great Stamina',18,15,0,1,0,0,0,1),(4198,4190,'Great Stamina','Great Stamina',24,25,0,1,0,0,0,1),(4199,4191,'Great Stamina','Great Stamina',30,50,0,1,0,0,0,1),(4200,4192,'Great Stamina','Great Stamina',36,75,0,1,0,0,0,1),(4201,4193,'Great Stamina','Great Stamina',42,100,0,1,0,0,0,1),(4202,4194,'Great Stamina','Great Stamina',48,125,0,1,0,0,0,1),(5048,5041,'Great Stamina','Great Stamina',54,150,0,1,0,0,0,1),(5049,5042,'Great Stamina','Great Stamina',60,185,0,1,0,0,0,1),(7370,7371,'Charge','Charge',1,5,32,0,1,0,0,1),(14922,14916,'Growl','Growl',10,0,0,1,0,0,0,1),(14923,14917,'Growl','Growl',20,0,0,1,0,0,0,1),(14924,14918,'Growl','Growl',30,0,0,1,0,0,0,1),(14925,14919,'Growl','Growl',40,0,0,1,0,0,0,1),(14926,14920,'Growl','Growl',50,0,0,1,0,0,0,1),(14927,14921,'Growl','Growl',60,0,0,1,0,0,0,1),(16698,16697,'Cower','Cower',55,18,16777220,0,1,0,0,1),(17254,17253,'Bite','Bite',1,1,186653438,0,1,0,0,1),(17262,17255,'Bite','Bite',8,4,186653438,0,1,0,0,1),(17263,17256,'Bite','Bite',16,7,186653438,0,1,0,0,1),(17264,17257,'Bite','Bite',24,10,186653438,0,1,0,0,1),(17265,17258,'Bite','Bite',32,13,186653438,0,1,0,0,1),(17266,17259,'Bite','Bite',40,17,186653438,0,1,0,0,1),(17267,17260,'Bite','Bite',48,21,186653438,0,1,0,0,1),(17268,17261,'Bite','Bite',56,25,186653438,0,1,0,0,1),(23100,23099,'Dash','Dash',30,15,33558566,0,1,0,0,1),(23111,23109,'Dash','Dash',40,20,33558566,0,1,0,0,1),(23112,23110,'Dash','Dash',50,25,33558566,0,1,0,0,1),(23146,23145,'Dive','Dive',30,15,218103936,0,1,0,0,1),(23149,23147,'Dive','Dive',40,20,218103936,0,1,0,0,1),(23150,23148,'Dive','Dive',50,25,218103936,0,1,0,0,1),(23163,23162,'Harass','Harass',1,0,0,0,0,0,0,0),(23166,23164,'Harass','Harass',1,0,0,0,0,0,0,0),(23167,23165,'Harass','Harass',1,0,0,0,0,0,0,0),(24424,24423,'Screech','Screech',8,10,83886208,0,1,0,0,1),(24440,23992,'Fire Resistance','Fire Resistance',20,5,0,1,0,0,0,1),(24441,24439,'Fire Resistance','Fire Resistance',30,15,0,1,0,0,0,1),(24451,24450,'Prowl','Prowl',30,15,4,0,1,0,0,1),(24454,24452,'Prowl','Prowl',40,20,4,0,1,0,0,1),(24455,24453,'Prowl','Prowl',50,25,4,0,1,0,0,1),(24463,24444,'Fire Resistance','Fire Resistance',40,45,0,1,0,0,0,1),(24464,24445,'Fire Resistance','Fire Resistance',50,90,0,1,0,0,0,1),(24475,24446,'Frost Resistance','Frost Resistance',20,5,0,1,0,0,0,1),(24476,24447,'Frost Resistance','Frost Resistance',30,15,0,1,0,0,0,1),(24477,24448,'Frost Resistance','Frost Resistance',40,45,0,1,0,0,0,1),(24478,24449,'Frost Resistance','Frost Resistance',50,90,0,1,0,0,0,1),(24490,24488,'Shadow Resistance','Shadow Resistance',20,5,0,1,0,0,0,1),(24494,24492,'Nature Resistance','Nature Resistance',20,5,0,1,0,0,0,1),(24495,24493,'Arcane Resistance','Arcane Resistance',20,5,0,1,0,0,0,1),(24508,24497,'Arcane Resistance','Arcane Resistance',30,15,0,1,0,0,0,1),(24509,24500,'Arcane Resistance','Arcane Resistance',40,45,0,1,0,0,0,1),(24510,24501,'Arcane Resistance','Arcane Resistance',50,90,0,1,0,0,0,1),(24511,24502,'Nature Resistance','Nature Resistance',30,15,0,1,0,0,0,1),(24512,24503,'Nature Resistance','Nature Resistance',40,45,0,1,0,0,0,1),(24513,24504,'Nature Resistance','Nature Resistance',50,90,0,1,0,0,0,1),(24514,24505,'Shadow Resistance','Shadow Resistance',30,15,0,1,0,0,0,1),(24515,24506,'Shadow Resistance','Shadow Resistance',40,45,0,1,0,0,0,1),(24516,24507,'Shadow Resistance','Shadow Resistance',50,90,0,1,0,0,0,1),(24547,24545,'Natural Armor','Natural Armor',10,1,0,1,0,0,0,1),(24556,24549,'Natural Armor','Natural Armor',12,5,0,1,0,0,0,1),(24557,24550,'Natural Armor','Natural Armor',18,10,0,1,0,0,0,1),(24558,24551,'Natural Armor','Natural Armor',24,15,0,1,0,0,0,1),(24559,24552,'Natural Armor','Natural Armor',30,25,0,1,0,0,0,1),(24560,24553,'Natural Armor','Natural Armor',36,50,0,1,0,0,0,1),(24561,24554,'Natural Armor','Natural Armor',42,75,0,1,0,0,0,1),(24562,24555,'Natural Armor','Natural Armor',48,100,0,1,0,0,0,1),(24580,24577,'Screech','Screech',24,15,83886208,0,1,0,0,1),(24581,24578,'Screech','Screech',40,20,83886208,0,1,0,0,1),(24582,24579,'Screech','Screech',56,25,83886208,0,1,0,0,1),(24584,24583,'Scorpid Poison','Scorpid Poison',24,15,1048576,0,1,0,0,1),(24588,24586,'Scorpid Poison','Scorpid Poison',40,20,1048576,0,1,0,0,1),(24589,24587,'Scorpid Poison','Scorpid Poison',56,25,1048576,0,1,0,0,1),(24599,24597,'Furious Howl','Furious Howl',56,25,2,0,1,0,0,1),(24607,24603,'Furious Howl','Furious Howl',40,20,2,0,1,0,0,1),(24608,24605,'Furious Howl','Furious Howl',24,15,2,0,1,0,0,1),(24609,24604,'Furious Howl','Furious Howl',10,10,2,0,1,0,0,1),(24631,24629,'Natural Armor','Natural Armor',54,125,0,1,0,0,0,1),(24632,24630,'Natural Armor','Natural Armor',60,150,0,1,0,0,0,1),(24641,24640,'Scorpid Poison','Scorpid Poison',8,10,1048576,0,1,0,0,1),(24845,24844,'Lightning Breath','Lightning Breath',1,1,134217728,0,1,0,0,1),(25013,25008,'Lightning Breath','Lightning Breath',12,5,134217728,0,1,0,0,1),(25014,25009,'Lightning Breath','Lightning Breath',24,10,134217728,0,1,0,0,1),(25015,25010,'Lightning Breath','Lightning Breath',36,15,134217728,0,1,0,0,1),(25016,25011,'Lightning Breath','Lightning Breath',48,20,134217728,0,1,0,0,1),(25017,25012,'Lightning Breath','Lightning Breath',60,25,134217728,0,1,0,0,1),(25077,25076,'Cobra Reflexes','Cobra Reflexes',1,0,2048,0,0,0,0,0),(26065,26064,'Shell Shield','Shell Shield',20,15,2097152,0,1,0,0,1),(26094,26090,'Thunderstomp','Thunderstomp',30,15,512,0,1,0,0,1),(26184,26177,'Charge','Charge',12,9,32,0,1,0,0,1),(26185,26178,'Charge','Charge',24,13,32,0,1,0,0,1),(26186,26179,'Charge','Charge',36,17,32,0,1,0,0,1),(26189,26187,'Thunderstomp','Thunderstomp',40,20,512,0,1,0,0,1),(26190,26188,'Thunderstomp','Thunderstomp',50,25,512,0,1,0,0,1),(26202,26201,'Charge','Charge',48,21,32,0,1,0,0,1),(28343,27685,'Charge','Charge',60,25,32,0,1,0,0,1);
/*!40000 ALTER TABLE `classic_pet_training_template` ENABLE KEYS */;
UNLOCK TABLES;
/*!40103 SET TIME_ZONE=@OLD_TIME_ZONE */;

/*!40101 SET SQL_MODE=@OLD_SQL_MODE */;
/*!40014 SET FOREIGN_KEY_CHECKS=@OLD_FOREIGN_KEY_CHECKS */;
/*!40014 SET UNIQUE_CHECKS=@OLD_UNIQUE_CHECKS */;
/*!40101 SET CHARACTER_SET_CLIENT=@OLD_CHARACTER_SET_CLIENT */;
/*!40101 SET CHARACTER_SET_RESULTS=@OLD_CHARACTER_SET_RESULTS */;
/*!40101 SET COLLATION_CONNECTION=@OLD_COLLATION_CONNECTION */;
/*!40111 SET SQL_NOTES=@OLD_SQL_NOTES */;

