
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
DROP TABLE IF EXISTS `classic_pet_trainer_spell_template`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!50503 SET character_set_client = utf8mb4 */;
CREATE TABLE `classic_pet_trainer_spell_template` (
  `source_spell` int unsigned NOT NULL,
  `taught_spell` int unsigned NOT NULL,
  `source_name` varchar(128) CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci NOT NULL DEFAULT '',
  `taught_name` varchar(128) CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci NOT NULL DEFAULT '',
  `required_level` tinyint unsigned NOT NULL DEFAULT '1',
  `cost` smallint unsigned NOT NULL DEFAULT '0',
  `trainer_spellcost` int NOT NULL DEFAULT '0'
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
/*!40101 SET character_set_client = @saved_cs_client */;

LOCK TABLES `classic_pet_trainer_spell_template` WRITE;
/*!40000 ALTER TABLE `classic_pet_trainer_spell_template` DISABLE KEYS */;
INSERT INTO `classic_pet_trainer_spell_template` VALUES (1853,2649,'Growl','Growl',1,0,1),(4195,4187,'Great Stamina','Great Stamina',10,0,0),(4196,4188,'Great Stamina','Great Stamina',12,0,0),(4197,4189,'Great Stamina','Great Stamina',18,0,0),(4198,4190,'Great Stamina','Great Stamina',24,0,0),(4199,4191,'Great Stamina','Great Stamina',30,0,0),(4200,4192,'Great Stamina','Great Stamina',36,0,0),(4201,4193,'Great Stamina','Great Stamina',42,0,0),(4202,4194,'Great Stamina','Great Stamina',48,0,0),(5048,5041,'Great Stamina','Great Stamina',54,0,0),(5049,5042,'Great Stamina','Great Stamina',60,0,0),(14922,14916,'Growl','Growl',10,0,1),(14923,14917,'Growl','Growl',20,0,0),(14924,14918,'Growl','Growl',30,0,0),(14925,14919,'Growl','Growl',40,0,0),(14926,14920,'Growl','Growl',50,0,0),(14927,14921,'Growl','Growl',60,0,0),(24440,23992,'Fire Resistance','Fire Resistance',20,0,0),(24441,24439,'Fire Resistance','Fire Resistance',30,0,0),(24463,24444,'Fire Resistance','Fire Resistance',40,0,0),(24464,24445,'Fire Resistance','Fire Resistance',50,0,0),(24475,24446,'Frost Resistance','Frost Resistance',20,0,0),(24476,24447,'Frost Resistance','Frost Resistance',30,0,0),(24477,24448,'Frost Resistance','Frost Resistance',40,0,0),(24478,24449,'Frost Resistance','Frost Resistance',50,0,0),(24490,24488,'Shadow Resistance','Shadow Resistance',20,0,0),(24494,24492,'Nature Resistance','Nature Resistance',20,0,0),(24495,24493,'Arcane Resistance','Arcane Resistance',20,0,0),(24508,24497,'Arcane Resistance','Arcane Resistance',30,0,0),(24509,24500,'Arcane Resistance','Arcane Resistance',40,0,0),(24510,24501,'Arcane Resistance','Arcane Resistance',50,0,0),(24511,24502,'Nature Resistance','Nature Resistance',30,0,0),(24512,24503,'Nature Resistance','Nature Resistance',40,0,0),(24513,24504,'Nature Resistance','Nature Resistance',50,0,0),(24514,24505,'Shadow Resistance','Shadow Resistance',30,0,0),(24515,24506,'Shadow Resistance','Shadow Resistance',40,0,0),(24516,24507,'Shadow Resistance','Shadow Resistance',50,0,0),(24547,24545,'Natural Armor','Natural Armor',10,0,0),(24556,24549,'Natural Armor','Natural Armor',12,0,0),(24557,24550,'Natural Armor','Natural Armor',18,0,0),(24558,24551,'Natural Armor','Natural Armor',24,0,0),(24559,24552,'Natural Armor','Natural Armor',30,0,0),(24560,24553,'Natural Armor','Natural Armor',36,0,0),(24561,24554,'Natural Armor','Natural Armor',42,0,0),(24562,24555,'Natural Armor','Natural Armor',48,0,0),(24631,24629,'Natural Armor','Natural Armor',54,0,0),(24632,24630,'Natural Armor','Natural Armor',60,0,0);
/*!40000 ALTER TABLE `classic_pet_trainer_spell_template` ENABLE KEYS */;
UNLOCK TABLES;
/*!40103 SET TIME_ZONE=@OLD_TIME_ZONE */;

/*!40101 SET SQL_MODE=@OLD_SQL_MODE */;
/*!40014 SET FOREIGN_KEY_CHECKS=@OLD_FOREIGN_KEY_CHECKS */;
/*!40014 SET UNIQUE_CHECKS=@OLD_UNIQUE_CHECKS */;
/*!40101 SET CHARACTER_SET_CLIENT=@OLD_CHARACTER_SET_CLIENT */;
/*!40101 SET CHARACTER_SET_RESULTS=@OLD_CHARACTER_SET_RESULTS */;
/*!40101 SET COLLATION_CONNECTION=@OLD_COLLATION_CONNECTION */;
/*!40111 SET SQL_NOTES=@OLD_SQL_NOTES */;

