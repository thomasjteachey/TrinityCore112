
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
DROP TABLE IF EXISTS `rift_league_item_unique`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!50503 SET character_set_client = utf8mb4 */;
CREATE TABLE `rift_league_item_unique` (
  `entry` int unsigned NOT NULL,
  `name` varchar(48) NOT NULL DEFAULT '' COMMENT 'empty = the item''s own; a League name never stacks with itself; * = every copy',
  `stat` varchar(32) NOT NULL,
  `value` float NOT NULL DEFAULT '0',
  `comment` varchar(255) NOT NULL DEFAULT '',
  PRIMARY KEY (`entry`,`name`,`stat`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci COMMENT='Summoner''s Rift shop items'' League (S3) UNIQUE effects, as stats RiftProgression knows';
/*!40101 SET character_set_client = @saved_cs_client */;

LOCK TABLES `rift_league_item_unique` WRITE;
/*!40000 ALTER TABLE `rift_league_item_unique` DISABLE KEYS */;
INSERT INTO `rift_league_item_unique` VALUES (991062,'','cdr',10,'Fiendish Codex'),(991063,'','cdr',10,'Kindlegem'),(991064,'','cdr',10,'Glacial Shroud'),(991065,'','cdr',10,'Stinger'),(991066,'','cdr',15,'Ionian Boots of Lucidity'),(991067,'','armor_pen',10,'The Brutalizer'),(991067,'','cdr',10,'The Brutalizer'),(991068,'Eyes of Pain','magic_pen',15,'Haunting Guise'),(991069,'','magic_pen_pct',35,'Void Staff'),(991070,'','spell_vamp',12,'Hextech Revolver'),(991071,'Lucky Shadow','gold10',4,'Kage\'s Lucky Pick'),(991072,'Transmute','gold10',5,'Philosopher\'s Stone'),(991073,'Avarice','gold10',3,'Avarice Blade'),(991073,'Greed','greed',2,'Avarice Blade'),(991074,'Slow Resist','slow_resist',25,'Boots of Swiftness'),(991075,'Mobility','ms_ooc',60,'Boots of Mobility'),(991076,'Tenacity','tenacity',35,'Mercury\'s Treads'),(991077,'','basic_block_pct',10,'Ninja Tabi'),(991078,'Valor','aura_ally_hp5',7,'Emblem of Valor'),(991079,'Legion','aura_ally_hp5',10,'Aegis of the Legion'),(991079,'Legion','aura_ally_mr',20,'Aegis of the Legion'),(991080,'Mana Warp','aura_ally_mp5',5,'Mana Manipulator'),(991081,'','aura_ally_ad',20,'Zeke\'s Herald'),(991081,'','aura_ally_ls',10,'Zeke\'s Herald'),(991081,'*','cdr',20,'Zeke\'s Herald'),(991082,'','aura_enemy_mr',20,'Abyssal Scepter'),(991083,'Cold Steel','cold_steel_as',15,'Warden\'s Mail'),(991084,'','aura_enemy_as',20,'Frozen Heart'),(991084,'*','cdr',20,'Frozen Heart'),(991085,'','ad_per_health',1.5,'Atma\'s Impaler'),(991086,'Mana Font','mana_font',1,'Chalice of Harmony'),(991087,'','takedown_mana_pct',12,'Athene\'s Unholy Grail'),(991087,'*','cdr',20,'Athene\'s Unholy Grail'),(991087,'Mana Font','mana_font',1,'Athene\'s Unholy Grail'),(991088,'','heal_amp',20,'Spirit Visage'),(991088,'*','cdr',20,'Spirit Visage'),(991089,'','champ_hit_regen',15,'Spectre\'s Cowl'),(991090,'','unit_kill_armor_ap',0.5,'Seeker\'s Armguard'),(991091,'','takedown_ap',8,'Mejai\'s Soulstealer'),(991092,'','takedown_ad',5,'Sword of the Occult'),(991093,'*','on_hit_heal',5,'Doran\'s Blade'),(991094,'','champ_basic_block',8,'Doran\'s Shield'),(991095,'Butcher','butcher',10,'Hunter\'s Machete'),(991095,'Maim','maim',10,'Hunter\'s Machete'),(991096,'Maim','maim',60,'Madred\'s Razors'),(991097,'Butcher','butcher',20,'Spirit Stone'),(991097,'Maim','maim',10,'Spirit Stone'),(991098,'*','cdr',10,'Spirit of the Ancient Golem'),(991098,'Butcher','butcher',30,'Spirit of the Ancient Golem'),(991098,'Tenacity','tenacity',35,'Spirit of the Ancient Golem'),(991099,'','spell_vamp',20,'Spirit of the Spectral Wraith'),(991099,'*','cdr',10,'Spirit of the Spectral Wraith'),(991099,'Butcher','butcher',30,'Spirit of the Spectral Wraith');
/*!40000 ALTER TABLE `rift_league_item_unique` ENABLE KEYS */;
UNLOCK TABLES;
/*!40103 SET TIME_ZONE=@OLD_TIME_ZONE */;

/*!40101 SET SQL_MODE=@OLD_SQL_MODE */;
/*!40014 SET FOREIGN_KEY_CHECKS=@OLD_FOREIGN_KEY_CHECKS */;
/*!40014 SET UNIQUE_CHECKS=@OLD_UNIQUE_CHECKS */;
/*!40101 SET CHARACTER_SET_CLIENT=@OLD_CHARACTER_SET_CLIENT */;
/*!40101 SET CHARACTER_SET_RESULTS=@OLD_CHARACTER_SET_RESULTS */;
/*!40101 SET COLLATION_CONNECTION=@OLD_COLLATION_CONNECTION */;
/*!40111 SET SQL_NOTES=@OLD_SQL_NOTES */;

