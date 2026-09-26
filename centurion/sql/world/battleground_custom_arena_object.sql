
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
DROP TABLE IF EXISTS `battleground_custom_arena_object`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!50503 SET character_set_client = utf8mb4 */;
CREATE TABLE `battleground_custom_arena_object` (
  `Id` int unsigned NOT NULL AUTO_INCREMENT,
  `BgTypeId` int unsigned NOT NULL COMMENT 'BattlegroundTypeId, must be a data-driven arena',
  `GoEntry` int unsigned NOT NULL COMMENT 'gameobject_template.entry',
  `X` float NOT NULL DEFAULT '0',
  `Y` float NOT NULL DEFAULT '0',
  `Z` float NOT NULL DEFAULT '0',
  `Orientation` float NOT NULL DEFAULT '0',
  `Rotation0` float NOT NULL DEFAULT '0',
  `Rotation1` float NOT NULL DEFAULT '0',
  `Rotation2` float NOT NULL DEFAULT '0',
  `Rotation3` float NOT NULL DEFAULT '1',
  `ObjectType` tinyint unsigned NOT NULL DEFAULT '0' COMMENT '0=door, 1=buff',
  `Comment` varchar(64) NOT NULL DEFAULT '',
  PRIMARY KEY (`Id`),
  KEY `idx_bgtype` (`BgTypeId`,`ObjectType`)
) ENGINE=InnoDB AUTO_INCREMENT=49 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci COMMENT='Gates and buffs for data-driven custom arenas';
/*!40101 SET character_set_client = @saved_cs_client */;

LOCK TABLES `battleground_custom_arena_object` WRITE;
/*!40000 ALTER TABLE `battleground_custom_arena_object` DISABLE KEYS */;
INSERT INTO `battleground_custom_arena_object` VALUES (1,872,185483,8026.25,-2749.97,1133.77,4.63387,0,0,-0.734317,0.678807,0,'coliseumarena gate 1 (surveyed)'),(2,872,185483,8128.83,-2748.27,1133.85,4.68099,0,0,-0.71812,0.695919,0,'coliseumarena gate 2 (surveyed)'),(3,872,184663,8074.24,-2712.76,1134.08,4.70847,0,0,0.708492,-0.705719,1,'coliseumarena shadowsight 1'),(4,872,184664,8076.93,-2788.57,1134.22,1.61005,0,0,0.720848,0.693093,1,'coliseumarena shadowsight 2'),(5,873,185483,488.07,-878.306,27.7091,4.75009,0,0,-0.693652,0.720311,0,'nerubianarena gate 1 (surveyed)'),(6,873,185483,599.511,-941.21,27.7028,4.71478,0,0,-0.706261,0.707952,0,'nerubianarena gate 2 (surveyed)'),(7,873,184663,544.371,-884.355,26.7965,4.67549,0,0,0.720031,-0.693941,1,'nerubianarena shadowsight 1'),(8,873,184664,544.132,-931.262,26.0333,4.71083,0,0,0.707657,-0.706556,1,'nerubianarena shadowsight 2'),(9,874,185483,2849.81,2201.16,3260.18,0.281295,0,0,-0.140184,-0.990125,0,'maldraxxuscoliseum gate 1 (surveyed)'),(10,874,185483,2849.4,2304.35,3259.94,5.99117,0,0,-0.145491,0.98936,0,'maldraxxuscoliseum gate 2 (surveyed)'),(11,874,184663,2876.32,2252.89,3260.4,3.08122,0,0,0.999544,0.030182,1,'maldraxxuscoliseum shadowsight 1'),(12,874,184664,2787.44,2254.39,3260.43,0.002457,0,0,0.001228,0.999999,1,'maldraxxuscoliseum shadowsight 2'),(13,875,300400,-2019.26,6609.55,12.717,0.400356,0,0,-0.198844,-0.980031,0,'nagrandarena2 gate 1 (surveyed)'),(14,875,300401,-2067.86,6699.41,12.6588,3.63227,0,0,-0.970055,0.242886,0,'nagrandarena2 gate 2 (surveyed)'),(15,875,184663,-1995.35,6679.22,13.0716,3.61656,0,0,0.971933,-0.235257,1,'nagrandarena2 shadowsight 1'),(16,875,184664,-2090.86,6629.11,12.8695,0.722366,0,0,0.353381,0.935479,1,'nagrandarena2 shadowsight 2'),(17,876,183971,2778.05,6053.39,-3.14541,2.16953,0,0,-0.884193,-0.467121,0,'bladesedgearena2b gate 1 (surveyed)'),(18,876,183973,2796.86,5954.02,-3.06822,2.32267,0,0,-0.917336,-0.398114,0,'bladesedgearena2b gate 2 (surveyed)'),(19,876,184663,2768.12,5986.67,-4.40502,0.775437,0,0,0.378077,0.925774,1,'bladesedgearena2b shadowsight 1'),(20,876,184664,2808.45,6020.85,-4.12446,3.80317,0,0,0.945787,-0.324789,1,'bladesedgearena2b shadowsight 2'),(21,877,185483,536.769,752.417,0.505373,3.1415,0,0,-1,-0.000049,0,'karazhanarena gate 1 (surveyed)'),(22,877,185483,536.464,858.864,0.504717,3.20429,0,0,-0.999509,0.031343,0,'karazhanarena gate 2 (surveyed)'),(23,877,184663,564.335,805.875,-0.05997,3.18865,0,0,0.999723,-0.023529,1,'karazhanarena shadowsight 1'),(24,877,184664,508.64,805.706,-0.059966,6.27917,0,0,0.002006,-0.999998,1,'karazhanarena shadowsight 2'),(25,878,185483,505.72,838.528,0.872084,0.522682,0,0,-0.258376,-0.966044,0,'ulduararena gate 1 (surveyed)'),(26,878,185483,507.538,764.469,0.872084,2.50191,0,0,-0.949285,-0.314417,0,'ulduararena gate 2 (surveyed)'),(27,878,184663,557.732,837.495,0.87077,3.86466,0,0,0.935356,-0.353707,1,'ulduararena shadowsight 1'),(28,878,184664,556.42,768.009,0.871984,2.38811,0,0,0.929868,0.367894,1,'ulduararena shadowsight 2'),(29,879,185483,-1184.7,1034.54,120.2,3.94887,0,0,0.919639,-0.392765,0,'BaradinHoldArena gate 1 (derived from the team starts)'),(30,879,185483,-1279.89,935.095,120.2,0.807273,0,0,0.392765,0.919639,0,'BaradinHoldArena gate 2 (derived from the team starts)'),(31,880,185483,-9255.25,-1484.67,67.2,4.65047,0,0,0.728655,-0.684881,0,'obeliskofthestarts gate 1 (derived from the team starts)'),(32,880,185483,-9263.36,-1615.61,67.2,1.50888,0,0,0.684881,0.728655,0,'obeliskofthestarts gate 2 (derived from the team starts)'),(33,881,185483,4556.79,-1428.41,386.2,3.13606,0,0,0.999996,0.002764,0,'thetwistingnether gate 1 (derived from the team starts)'),(34,881,185483,4441.47,-1427.78,386.2,6.27766,0,0,0.002764,-0.999996,0,'thetwistingnether gate 2 (derived from the team starts)'),(35,882,185483,1383.61,1258.04,33.2505,5.00723,0,0,-0.595572,0.803302,0,'BlackrookHoldArena gate 1 (surveyed)'),(36,882,185483,1384.17,1232.46,33.2548,4.42605,0,0,-0.800763,0.598981,0,'BlackrookHoldArena gate 2 (surveyed)'),(37,882,185483,1450.32,1277.05,33.2335,4.97189,0,0,-0.609672,0.792654,0,'BlackrookHoldArena gate 3 (surveyed)'),(38,882,185483,1464.2,1254.33,33.2384,2.44289,0,0,-0.939595,-0.342289,0,'BlackrookHoldArena gate 4 (surveyed)'),(39,882,184663,1429.63,1238.82,34.1074,2.32905,0,0,0.918601,0.395186,1,'BlackrookHoldArena shadowsight 1'),(40,882,184664,1424.6,1202.65,32.0943,1.66934,0,0,0.741073,0.671424,1,'BlackrookHoldArena shadowsight 2'),(41,883,300002,3539.14,5482.91,325.49,4.77411,0,0,-0.684951,0.72859,0,'valsharaharena gate 1 (surveyed)'),(42,883,300002,3548.85,5591.53,325.514,4.68378,0,0,-0.717148,0.696921,0,'valsharaharena gate 2 (surveyed)'),(43,883,184663,3493.61,5540.87,323.021,6.16031,0,0,0.061397,-0.998113,1,'valsharaharena shadowsight 1'),(44,883,184664,3604.44,5532.03,325.364,3.04229,0,0,0.998768,0.049632,1,'valsharaharena shadowsight 2'),(45,884,185483,8119.26,-946.297,957.2,1.31895,0,0,0.612701,0.790315,0,'ulduaroutarena gate 1 (derived from the team starts)'),(46,884,185483,8161.44,-782.383,957.2,4.46054,0,0,0.790315,-0.612701,0,'ulduaroutarena gate 2 (derived from the team starts)'),(47,885,185483,5807.42,-2979.89,273.2,3.57646,0,0,0.976454,-0.215725,0,'gundrakarena gate 1 (derived from the team starts)'),(48,885,185483,5753.15,-3005.11,273.2,0.434869,0,0,0.215725,0.976454,0,'gundrakarena gate 2 (derived from the team starts)');
/*!40000 ALTER TABLE `battleground_custom_arena_object` ENABLE KEYS */;
UNLOCK TABLES;
/*!40103 SET TIME_ZONE=@OLD_TIME_ZONE */;

/*!40101 SET SQL_MODE=@OLD_SQL_MODE */;
/*!40014 SET FOREIGN_KEY_CHECKS=@OLD_FOREIGN_KEY_CHECKS */;
/*!40014 SET UNIQUE_CHECKS=@OLD_UNIQUE_CHECKS */;
/*!40101 SET CHARACTER_SET_CLIENT=@OLD_CHARACTER_SET_CLIENT */;
/*!40101 SET CHARACTER_SET_RESULTS=@OLD_CHARACTER_SET_RESULTS */;
/*!40101 SET COLLATION_CONNECTION=@OLD_COLLATION_CONNECTION */;
/*!40111 SET SQL_NOTES=@OLD_SQL_NOTES */;

