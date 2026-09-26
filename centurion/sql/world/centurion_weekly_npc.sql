
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
DROP TABLE IF EXISTS `centurion_weekly_npc`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!50503 SET character_set_client = utf8mb4 */;
CREATE TABLE `centurion_weekly_npc` (
  `entry` int unsigned NOT NULL,
  `board` tinyint unsigned NOT NULL,
  `place` tinyint unsigned NOT NULL DEFAULT '0',
  `comment` varchar(255) COLLATE utf8mb4_unicode_ci NOT NULL DEFAULT '',
  PRIMARY KEY (`entry`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Centurion: NPCs dressed as the weekly leaderboards. board 1 = weekly honor (place 1-3), board 2 = most deaths in the zone the NPC stands in';
/*!40101 SET character_set_client = @saved_cs_client */;

LOCK TABLES `centurion_weekly_npc` WRITE;
/*!40000 ALTER TABLE `centurion_weekly_npc` DISABLE KEYS */;
INSERT INTO `centurion_weekly_npc` VALUES (110017,1,2,'Trash at his feet (L+ 110017)'),(920103,1,1,'Warchief of Gurubashi (L+ 31412)'),(920118,1,3,'Xabt target dummies (L+ 32666)'),(920300,2,0,'inn corpse, spawn 214170'),(920301,2,0,'inn corpse, spawn 214171'),(920302,2,0,'inn corpse, spawn 214172'),(920303,2,0,'inn corpse, spawn 214173'),(920304,2,0,'inn corpse, spawn 214174'),(920305,2,0,'inn corpse, spawn 214175'),(920306,2,0,'inn corpse, spawn 214176'),(920307,2,0,'inn corpse, spawn 214177'),(920308,2,0,'inn corpse, spawn 214178'),(920309,2,0,'inn corpse, spawn 214179'),(920310,2,0,'inn corpse, spawn 214180'),(920311,2,0,'inn corpse, spawn 214181'),(920312,2,0,'inn corpse, spawn 214182'),(920313,2,0,'inn corpse, spawn 214183'),(920314,2,0,'inn corpse, spawn 214184'),(920315,2,0,'inn corpse, spawn 214185'),(920316,2,0,'inn corpse, spawn 214186'),(920317,2,0,'inn corpse, spawn 214187'),(920318,2,0,'inn corpse, spawn 214188'),(920319,2,0,'inn corpse, spawn 214189'),(920320,2,0,'inn corpse, spawn 214190'),(920321,2,0,'inn corpse, spawn 214191'),(920322,2,0,'inn corpse, spawn 214192'),(920323,2,0,'inn corpse, spawn 214193'),(920324,2,0,'inn corpse, spawn 214194'),(920325,2,0,'inn corpse, spawn 214195'),(920326,2,0,'inn corpse, spawn 214196'),(920327,2,0,'inn corpse, spawn 214197'),(920328,2,0,'inn corpse, spawn 214198'),(920329,2,0,'inn corpse, spawn 214199'),(920330,2,0,'inn corpse, spawn 214200'),(920331,2,0,'inn corpse, spawn 214201'),(920332,2,0,'inn corpse, spawn 214202'),(920333,2,0,'inn corpse, spawn 214203'),(920334,2,0,'inn corpse, spawn 214204'),(920335,2,0,'inn corpse, spawn 214205'),(920336,2,0,'inn corpse, spawn 214206'),(920337,2,0,'inn corpse, spawn 214207'),(920338,2,0,'inn corpse, spawn 214208'),(920339,2,0,'inn corpse, spawn 214209'),(920340,2,0,'inn corpse, spawn 214210'),(920341,2,0,'inn corpse, spawn 214211'),(920342,2,0,'inn corpse, spawn 214212'),(920343,2,0,'inn corpse, spawn 214213'),(920344,2,0,'inn corpse, spawn 214214'),(920345,2,0,'inn corpse, spawn 214215'),(920346,2,0,'inn corpse, spawn 214216'),(920347,2,0,'inn corpse, spawn 214217'),(920348,2,0,'inn corpse, spawn 214218'),(920349,2,0,'inn corpse, spawn 214219'),(920350,2,0,'inn corpse, spawn 214220'),(920351,2,0,'inn corpse, spawn 214221'),(920352,2,0,'inn corpse, spawn 214222'),(920353,2,0,'inn corpse, spawn 214223'),(920354,2,0,'inn corpse, spawn 214224'),(920355,2,0,'inn corpse, spawn 214225'),(920356,2,0,'inn corpse, spawn 214226'),(920357,2,0,'inn corpse, spawn 214227'),(920358,2,0,'inn corpse, spawn 214228'),(920359,2,0,'inn corpse, spawn 214229'),(920360,2,0,'inn corpse, spawn 214230'),(920361,2,0,'inn corpse, spawn 214231'),(920362,2,0,'inn corpse, spawn 214232'),(920363,2,0,'inn corpse, spawn 214233'),(920364,2,0,'inn corpse, spawn 214234'),(920365,2,0,'inn corpse, spawn 214235'),(920366,2,0,'inn corpse, spawn 214236'),(920367,2,0,'inn corpse, spawn 214237'),(920368,2,0,'inn corpse, spawn 214238'),(920369,2,0,'inn corpse, spawn 214239'),(920370,2,0,'inn corpse, spawn 214240'),(920371,2,0,'inn corpse, spawn 214241'),(920372,2,0,'inn corpse, spawn 214242'),(920373,2,0,'inn corpse, spawn 214243'),(920374,2,0,'inn corpse, spawn 214244'),(920375,2,0,'inn corpse, spawn 214245'),(920376,2,0,'inn corpse, spawn 214246'),(920377,2,0,'inn corpse, spawn 214247'),(920378,2,0,'inn corpse, spawn 214248'),(920379,2,0,'inn corpse, spawn 214249'),(920380,2,0,'inn corpse, spawn 214250'),(920381,2,0,'inn corpse, spawn 214251'),(920382,2,0,'inn corpse, spawn 214252'),(920383,2,0,'inn corpse, spawn 214253'),(920384,2,0,'inn corpse, spawn 214254'),(920385,2,0,'inn corpse, spawn 214255'),(920386,2,0,'inn corpse, spawn 214256'),(920387,2,0,'inn corpse, spawn 214257'),(920388,2,0,'inn corpse, spawn 214258'),(920389,2,0,'inn corpse, spawn 214259'),(920390,2,0,'inn corpse, spawn 214260'),(920391,2,0,'inn corpse, spawn 214261'),(920392,2,0,'inn corpse, spawn 214262'),(920393,2,0,'inn corpse, spawn 214263'),(920394,2,0,'inn corpse, spawn 214264'),(920395,2,0,'inn corpse, spawn 214265'),(920396,2,0,'inn corpse, spawn 214266'),(920397,2,0,'inn corpse, spawn 214267'),(920398,2,0,'inn corpse, spawn 214268'),(920399,2,0,'inn corpse, spawn 214269'),(920400,2,0,'inn corpse, spawn 214270'),(920401,2,0,'inn corpse, spawn 214271'),(920402,2,0,'inn corpse, spawn 214272'),(920403,2,0,'inn corpse, spawn 214273'),(920404,2,0,'inn corpse, spawn 214274'),(920405,2,0,'inn corpse, spawn 214275'),(920406,2,0,'inn corpse, spawn 214276'),(920407,2,0,'inn corpse, spawn 214277'),(920408,2,0,'inn corpse, spawn 214278'),(920409,2,0,'inn corpse, spawn 214279'),(920410,2,0,'inn corpse, spawn 214280'),(920411,2,0,'inn corpse, spawn 214281'),(920412,2,0,'inn corpse, spawn 214282'),(920413,2,0,'inn corpse, spawn 214283'),(920414,2,0,'inn corpse, spawn 214284'),(920415,2,0,'inn corpse, spawn 214285'),(920416,2,0,'inn corpse, spawn 214286'),(920417,2,0,'inn corpse, spawn 214287'),(920418,2,0,'inn corpse, spawn 214288'),(920419,2,0,'inn corpse, spawn 214289'),(920420,2,0,'inn corpse, spawn 214290'),(920421,2,0,'inn corpse, spawn 214291'),(920422,2,0,'inn corpse, spawn 214292'),(920423,2,0,'inn corpse, spawn 214293'),(920424,2,0,'inn corpse, spawn 900001'),(920425,2,0,'inn corpse, spawn 900002');
/*!40000 ALTER TABLE `centurion_weekly_npc` ENABLE KEYS */;
UNLOCK TABLES;
/*!40103 SET TIME_ZONE=@OLD_TIME_ZONE */;

/*!40101 SET SQL_MODE=@OLD_SQL_MODE */;
/*!40014 SET FOREIGN_KEY_CHECKS=@OLD_FOREIGN_KEY_CHECKS */;
/*!40014 SET UNIQUE_CHECKS=@OLD_UNIQUE_CHECKS */;
/*!40101 SET CHARACTER_SET_CLIENT=@OLD_CHARACTER_SET_CLIENT */;
/*!40101 SET CHARACTER_SET_RESULTS=@OLD_CHARACTER_SET_RESULTS */;
/*!40101 SET COLLATION_CONNECTION=@OLD_COLLATION_CONNECTION */;
/*!40111 SET SQL_NOTES=@OLD_SQL_NOTES */;

