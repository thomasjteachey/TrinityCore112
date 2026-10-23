
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
DROP TABLE IF EXISTS `rift_league_item`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!50503 SET character_set_client = utf8mb4 */;
CREATE TABLE `rift_league_item` (
  `entry` int unsigned NOT NULL,
  `health` float NOT NULL DEFAULT '0',
  `mana` float NOT NULL DEFAULT '0',
  `damage` float NOT NULL DEFAULT '0',
  `ability_power` float NOT NULL DEFAULT '0',
  `armor` float NOT NULL DEFAULT '0',
  `resist` float NOT NULL DEFAULT '0',
  `attack_speed` float NOT NULL DEFAULT '0' COMMENT 'percent',
  `crit` float NOT NULL DEFAULT '0' COMMENT 'percent',
  `move_speed` float NOT NULL DEFAULT '0' COMMENT 'percent',
  `boots` tinyint unsigned NOT NULL DEFAULT '0' COMMENT 'only the fastest boots count',
  `health_regen` float NOT NULL DEFAULT '0' COMMENT 'per 5 sec',
  `mana_regen` float NOT NULL DEFAULT '0' COMMENT 'per 5 sec',
  `magic_pen` float NOT NULL DEFAULT '0',
  `life_steal` float NOT NULL DEFAULT '0' COMMENT 'percent of a basic attack''s damage, back as health',
  `comment` varchar(255) NOT NULL DEFAULT '',
  PRIMARY KEY (`entry`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci COMMENT='Summoner''s Rift shop items'' League (S3) stats';
/*!40101 SET character_set_client = @saved_cs_client */;

LOCK TABLES `rift_league_item` WRITE;
/*!40000 ALTER TABLE `rift_league_item` DISABLE KEYS */;
INSERT INTO `rift_league_item` VALUES (991000,0,0,0,0,0,0,0,0,7.69,1,0,0,0,0,'Boots of Speed (S3 325 g)'),(991001,0,0,10,0,0,0,0,0,0,0,0,0,0,0,'Long Sword (S3 400 g)'),(991002,0,0,0,20,0,0,0,0,0,0,0,0,0,0,'Amplifying Tome (S3 435 g)'),(991003,180,0,0,0,0,0,0,0,0,0,0,0,0,0,'Ruby Crystal (S3 475 g)'),(991004,0,200,0,0,0,0,0,0,0,0,0,0,0,0,'Sapphire Crystal (S3 400 g)'),(991005,0,0,0,0,15,0,0,0,0,0,0,0,0,0,'Cloth Armor (S3 300 g)'),(991006,0,0,0,0,0,20,0,0,0,0,0,0,0,0,'Null-Magic Mantle (S3 400 g)'),(991007,0,0,0,0,0,0,12,0,0,0,0,0,0,0,'Dagger (S3 400 g)'),(991008,0,0,0,0,0,0,0,8,0,0,0,0,0,0,'Brawler\'s Gloves (S3 400 g)'),(991009,0,0,0,0,0,0,0,0,0,0,5,0,0,0,'Rejuvenation Bead (S3 180 g)'),(991010,0,0,0,0,0,0,0,0,0,0,0,3,0,0,'Faerie Charm (S3 180 g)'),(991011,0,0,25,0,0,0,0,0,0,0,0,0,0,0,'Pickaxe (S3 875 g)'),(991012,0,0,45,0,0,0,0,0,0,0,0,0,0,0,'B. F. Sword (S3 1550 g)'),(991013,0,0,0,80,0,0,0,0,0,0,0,0,0,0,'Needlessly Large Rod (S3 1600 g)'),(991014,0,0,0,40,0,0,0,0,0,0,0,0,0,0,'Blasting Wand (S3 860 g)'),(991015,0,0,0,0,0,0,0,15,0,0,0,0,0,0,'Cloak of Agility (S3 730 g)'),(991016,0,0,0,0,40,0,0,0,0,0,0,0,0,0,'Chain Vest (S3 720 g)'),(991017,0,0,0,0,0,40,0,0,0,0,0,0,0,0,'Negatron Cloak (S3 720 g)'),(991018,380,0,0,0,0,0,0,0,0,0,0,0,0,0,'Giant\'s Belt (S3 1000 g)'),(991019,0,0,0,0,0,0,18,10,5,0,0,0,0,0,'Zeal (S3 1175 g)'),(991020,0,200,0,25,0,0,0,0,0,0,0,0,0,0,'Sheen (S3 1200 g)'),(991021,200,0,20,0,0,0,0,0,0,0,0,0,0,0,'Phage (S3 1465 g)'),(991022,200,300,0,0,0,0,0,0,0,0,0,0,0,0,'Catalyst the Protector (S3 1200 g)'),(991023,0,0,0,0,0,0,20,0,13.85,1,0,0,0,0,'Berserker\'s Greaves (S3 900 g)'),(991024,0,0,0,0,0,0,0,0,13.85,1,0,0,15,0,'Sorcerer\'s Shoes (S3 1100 g)'),(991025,60,0,0,15,0,0,0,0,0,0,0,3,0,0,'Doran\'s Ring (S3 400 g)'),(991026,0,0,0,120,0,0,0,0,0,0,0,0,0,0,'Rabadon\'s Deathcap (S3 3300 g)'),(991027,0,0,0,0,100,0,0,0,0,0,0,0,0,0,'Thornmail (S3 2200 g)'),(991028,1000,0,0,0,0,0,0,0,0,0,0,0,0,0,'Warmog\'s Armor (S3 2830 g)'),(991029,450,0,0,0,0,55,0,0,0,0,0,0,0,0,'Banshee\'s Veil (S3 2750 g)'),(991030,450,0,0,0,45,0,0,0,0,0,0,0,0,0,'Sunfire Cape (S3 2650 g)'),(991031,0,0,40,0,0,0,0,0,0,0,0,0,0,0,'Last Whisper (S3 2300 g)'),(991032,0,0,0,0,0,0,50,30,5,0,0,0,0,0,'Phantom Dancer (S3 2800 g)'),(991033,450,450,0,60,0,0,0,0,0,0,0,0,0,0,'Rod of Ages (S3 2800 g)'),(991034,0,0,0,0,50,40,0,0,0,0,0,0,0,0,'Guardian Angel (S3 2750 g)'),(991035,250,200,30,30,0,0,30,10,8,0,0,0,0,0,'Trinity Force (S3 3843 g)'),(991036,0,0,70,0,0,0,0,25,0,0,0,0,0,0,'Infinity Edge (S3 3800 g)'),(991037,0,0,0,120,50,0,0,0,0,0,0,0,0,0,'Zhonya\'s Hourglass (S3 3260 g)'),(991041,0,0,10,0,0,0,0,0,0,0,0,0,0,10,'Vampiric Scepter (S3 800 g)'),(991042,0,0,70,0,0,0,0,0,0,0,0,0,0,12,'The Bloodthirster (S3 3200 g)'),(991061,0,0,0,0,0,0,30,0,0,0,0,0,0,0,'Recurve Bow (S3 900 g)'),(991062,0,0,0,30,0,0,0,0,0,0,0,0,0,0,'Fiendish Codex (S3 820 g)'),(991063,200,0,0,0,0,0,0,0,0,0,0,0,0,0,'Kindlegem (S3 850 g)'),(991064,0,300,0,0,45,0,0,0,0,0,0,0,0,0,'Glacial Shroud (S3 1350 g)'),(991065,0,0,0,0,0,0,40,0,0,0,0,0,0,0,'Stinger (S3 1250 g)'),(991066,0,0,0,0,0,0,0,0,13.85,1,0,0,0,0,'Ionian Boots of Lucidity (S3 1000 g)'),(991067,0,0,25,0,0,0,0,0,0,0,0,0,0,0,'The Brutalizer (S3 1337 g)'),(991068,200,0,0,25,0,0,0,0,0,0,0,0,0,0,'Haunting Guise (S3 1485 g)'),(991069,0,0,0,70,0,0,0,0,0,0,0,0,0,0,'Void Staff (S3 2295 g)'),(991070,0,0,0,40,0,0,0,0,0,0,0,0,0,0,'Hextech Revolver (S3 1200 g)'),(991071,0,0,0,25,0,0,0,0,0,0,0,0,0,0,'Kage\'s Lucky Pick (S3 765 g)'),(991072,0,0,0,0,0,0,0,0,0,0,7,9,0,0,'Philosopher\'s Stone (S3 700 g)'),(991073,0,0,0,0,0,0,0,10,0,0,0,0,0,0,'Avarice Blade (S3 800 g)'),(991074,0,0,0,0,0,0,0,0,18.46,1,0,0,0,0,'Boots of Swiftness (S3 1000 g)'),(991075,0,0,0,0,0,0,0,0,32.31,1,0,0,0,0,'Boots of Mobility (S3 1000 g)'),(991076,0,0,0,0,0,25,0,0,13.85,1,0,0,0,0,'Mercury\'s Treads (S3 1200 g)'),(991077,0,0,0,0,25,0,0,0,13.85,1,0,0,0,0,'Ninja Tabi (S3 1000 g)'),(991078,0,0,0,0,20,0,0,0,0,0,0,0,0,0,'Emblem of Valor (S3 650 g)'),(991079,200,0,0,0,20,0,0,0,0,0,0,0,0,0,'Aegis of the Legion (S3 1900 g)'),(991080,0,0,0,0,0,0,0,0,0,0,0,0,0,0,'Mana Manipulator (S3 300 g)'),(991081,250,0,0,0,0,0,0,0,0,0,0,0,0,0,'Zeke\'s Herald (S3 2550 g)'),(991082,0,0,0,70,0,45,0,0,0,0,0,0,0,0,'Abyssal Scepter (S3 2560 g)'),(991083,0,0,0,0,50,0,0,0,0,0,0,0,0,0,'Warden\'s Mail (S3 1000 g)'),(991084,0,400,0,0,95,0,0,0,0,0,0,0,0,0,'Frozen Heart (S3 2900 g)'),(991085,0,0,0,0,45,0,0,15,0,0,0,0,0,0,'Atma\'s Impaler (S3 2300 g)'),(991086,0,0,0,0,0,25,0,0,0,0,0,7,0,0,'Chalice of Harmony (S3 880 g)'),(991087,0,0,0,60,0,40,0,0,0,0,0,15,0,0,'Athene\'s Unholy Grail (S3 2600 g)'),(991088,400,0,0,0,0,55,0,0,0,0,20,0,0,0,'Spirit Visage (S3 2625 g)'),(991089,200,0,0,0,0,45,0,0,0,0,0,0,0,0,'Spectre\'s Cowl (S3 1400 g)'),(991090,0,0,0,20,30,0,0,0,0,0,0,0,0,0,'Seeker\'s Armguard (S3 1160 g)'),(991091,0,0,0,20,0,0,0,0,0,0,0,0,0,0,'Mejai\'s Soulstealer (S3 1235 g)'),(991092,0,0,10,0,0,0,0,0,0,0,0,0,0,0,'Sword of the Occult (S3 1200 g)'),(991093,80,0,10,0,0,0,0,0,0,0,0,0,0,0,'Doran\'s Blade (S3 475 g)'),(991094,100,0,0,0,0,0,0,0,0,0,10,0,0,0,'Doran\'s Shield (S3 440 g)'),(991095,0,0,0,0,0,0,0,0,0,0,0,0,0,0,'Hunter\'s Machete (S3 300 g)'),(991096,0,0,0,0,25,0,0,0,0,0,0,0,0,0,'Madred\'s Razors (S3 700 g)'),(991097,0,0,0,0,0,0,0,0,0,0,14,7,0,0,'Spirit Stone (S3 700 g)'),(991098,500,0,0,0,0,0,0,0,0,0,14,7,0,0,'Spirit of the Ancient Golem (S3 2000 g)'),(991099,0,0,0,40,0,0,0,0,0,0,0,10,0,0,'Spirit of the Spectral Wraith (S3 2000 g)');
/*!40000 ALTER TABLE `rift_league_item` ENABLE KEYS */;
UNLOCK TABLES;
/*!40103 SET TIME_ZONE=@OLD_TIME_ZONE */;

/*!40101 SET SQL_MODE=@OLD_SQL_MODE */;
/*!40014 SET FOREIGN_KEY_CHECKS=@OLD_FOREIGN_KEY_CHECKS */;
/*!40014 SET UNIQUE_CHECKS=@OLD_UNIQUE_CHECKS */;
/*!40101 SET CHARACTER_SET_CLIENT=@OLD_CHARACTER_SET_CLIENT */;
/*!40101 SET CHARACTER_SET_RESULTS=@OLD_CHARACTER_SET_RESULTS */;
/*!40101 SET COLLATION_CONNECTION=@OLD_COLLATION_CONNECTION */;
/*!40111 SET SQL_NOTES=@OLD_SQL_NOTES */;

