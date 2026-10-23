
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
DROP TABLE IF EXISTS `battleground_bounds`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!50503 SET character_set_client = utf8mb4 */;
CREATE TABLE `battleground_bounds` (
  `BgTypeId` int unsigned NOT NULL COMMENT 'battleground_template.ID',
  `MinZ` float NOT NULL COMMENT 'below this height a combatant, pet or totem has fallen out of the arena',
  `ProbeZ` float NOT NULL DEFAULT '0' COMMENT 'look down from here for the floor at the return point; 0 = do not look',
  `KeepXY` tinyint unsigned NOT NULL DEFAULT '0' COMMENT '1 = put them back at their own X/Y, at SafeZ',
  `SafeX` float NOT NULL,
  `SafeY` float NOT NULL,
  `SafeZ` float NOT NULL,
  `GraceMs` int unsigned NOT NULL DEFAULT '0' COMMENT 'after the gates open, before any of this applies',
  `Boundary` varchar(4096) NOT NULL DEFAULT '' COMMENT 'optional outline "x y, x y, ..." in world X/Y; leaving it returns you just inside',
  `Comment` varchar(255) NOT NULL DEFAULT '',
  PRIMARY KEY (`BgTypeId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci COMMENT='Where a battleground''s combatants and their pets may be once the gates open';
/*!40101 SET character_set_client = @saved_cs_client */;

LOCK TABLES `battleground_bounds` WRITE;
/*!40000 ALTER TABLE `battleground_bounds` DISABLE KEYS */;
INSERT INTO `battleground_bounds` VALUES (10,0,0,0,1292,760,7,0,'','Dalaran Sewers: below Blizzard\'s own under-floor line (area trigger 5326) - back onto the south ring'),(11,27,0,1,763.5,-284,28.28,20500,'','Ring of Valor: still in a lift shaft once the elevators have risen - lifted onto the floor where it stands'),(877,-10,0,0,536.55,805.72,1.5,0,'','Guardian\'s Hall: off the floor into the void - centre'),(878,-10,0,0,527.59,801,2,0,'','Spark of Creator: off the floor - centre'),(881,380,391,0,4500.7,-1510.5,387,0,'4453.5 -1557.0, 4456.0 -1548.5, 4456.5 -1515.0, 4459.0 -1513.5, 4455.0 -1512.0, 4457.0 -1487.5, 4453.5 -1464.0, 4474.0 -1467.0, 4495.5 -1466.5, 4499.0 -1465.0, 4528.0 -1467.0, 4547.0 -1464.0, 4544.0 -1483.0, 4544.0 -1501.5, 4546.0 -1510.0, 4544.0 -1520.0, 4544.0 -1536.0, 4547.0 -1557.0, 4528.0 -1554.0, 4501.0 -1556.0, 4474.0 -1554.0','The Twisting Nether: off the platform - back onto it at the nearest edge'),(884,945,959.5,0,8147.76,-865.23,958,0,'8216.3 -823.3, 8216.3 -829.3, 8214.3 -832.8, 8205.3 -836.3, 8201.3 -853.3, 8183.8 -848.8, 8181.3 -849.8, 8179.3 -860.3, 8194.8 -864.3, 8195.8 -866.8, 8192.8 -867.8, 8188.8 -884.8, 8190.3 -889.8, 8174.8 -886.3, 8171.8 -895.8, 8173.3 -898.8, 8191.3 -902.8, 8186.8 -921.3, 8192.8 -926.8, 8193.3 -933.3, 8190.8 -937.3, 8182.3 -940.3, 8178.8 -955.8, 8140.3 -946.8, 8132.8 -973.8, 8121.8 -971.3, 8115.3 -977.8, 8107.3 -976.3, 8104.3 -972.3, 8103.3 -966.8, 8092.3 -964.3, 8097.8 -940.3, 8102.3 -938.8, 8106.3 -933.3, 8106.3 -924.8, 8095.3 -913.3, 8082.3 -908.8, 8066.8 -907.3, 8063.8 -905.3, 8063.3 -901.8, 8071.8 -898.8, 8077.8 -893.3, 8079.8 -882.8, 8087.3 -878.8, 8095.3 -870.3, 8096.8 -862.8, 8106.8 -863.3, 8104.8 -860.8, 8105.3 -856.8, 8107.8 -846.8, 8110.8 -845.8, 8109.8 -843.3, 8104.8 -842.3, 8107.3 -836.3, 8107.8 -812.3, 8111.8 -805.8, 8119.8 -803.8, 8132.8 -803.8, 8141.3 -795.3, 8144.8 -784.8, 8144.8 -778.8, 8139.3 -766.3, 8139.3 -759.3, 8145.3 -740.8, 8146.8 -740.8, 8141.8 -753.3, 8143.3 -755.3, 8158.8 -758.8, 8162.8 -753.3, 8170.8 -752.3, 8176.3 -757.8, 8177.3 -763.3, 8184.3 -764.8, 8176.8 -795.8, 8212.8 -805.3, 8209.8 -817.3, 8213.8 -819.3','The Inventor\'s Library: off the terrace (north stairs, south hillside, parapet) - nearest edge'),(885,266,281,0,5778.51,-2983.11,274.5,0,'5824.5 -2988.5, 5817.5 -2993.0, 5820.5 -3000.0, 5817.0 -3003.5, 5822.0 -3011.0, 5814.5 -3014.5, 5811.5 -3013.0, 5803.5 -3017.5, 5801.0 -3014.0, 5791.0 -3015.5, 5783.5 -3022.5, 5784.5 -3026.5, 5777.0 -3029.5, 5776.0 -3033.0, 5768.0 -3036.5, 5765.5 -3028.0, 5763.5 -3027.0, 5760.0 -3029.0, 5756.5 -3021.5, 5749.0 -3024.5, 5744.5 -3015.0, 5747.0 -3011.5, 5744.0 -3006.5, 5741.0 -3007.5, 5736.0 -2997.0, 5742.5 -2992.0, 5739.5 -2985.0, 5743.5 -2982.0, 5740.5 -2977.0, 5736.0 -2978.5, 5728.5 -2961.5, 5789.0 -2933.0, 5797.0 -2949.5, 5793.0 -2952.5, 5794.5 -2956.5, 5796.0 -2958.0, 5800.0 -2956.5, 5804.0 -2963.5, 5811.0 -2960.5, 5816.0 -2971.0, 5813.5 -2974.5, 5816.5 -2979.5, 5819.5 -2978.0','Amphitheater of Anguish: out of the pit (tunnels under the ring, the ring itself) - nearest edge'),(886,240,285,0,754.345,1300.87,266.669,0,'804.3 1300.9, 803.6 1309.6, 801.3 1318.0, 797.6 1325.9, 792.6 1333.0, 786.5 1339.2, 779.3 1344.2, 771.4 1347.9, 763.0 1350.1, 754.3 1350.9, 745.7 1350.1, 737.2 1347.9, 729.3 1344.2, 722.2 1339.2, 716.0 1333.0, 711.0 1325.9, 707.4 1318.0, 705.1 1309.6, 704.3 1300.9, 705.1 1292.2, 707.4 1283.8, 711.0 1275.9, 716.0 1268.7, 722.2 1262.6, 729.3 1257.6, 737.2 1253.9, 745.7 1251.6, 754.3 1250.9, 763.0 1251.6, 771.4 1253.9, 779.3 1257.6, 786.5 1262.6, 792.6 1268.7, 797.6 1275.9, 801.3 1283.8, 803.6 1292.2','The Eye of Eternity: off the Eye (or left on a pad) - back onto it at the nearest edge'),(888,216,0,0,-11080.7,-1876.25,221.17,0,'-11134.2 -1870.5, -11124.8 -1863.0, -11129.2 -1857.6, -11114.3 -1845.7, -11124.5 -1832.9, -11113.1 -1823.8, -11102.9 -1836.6, -11088.0 -1824.8, -11083.7 -1830.3, -11074.3 -1822.8, -11027.2 -1882.0, -11036.6 -1889.5, -11032.2 -1894.9, -11047.1 -1906.8, -11041.8 -1913.5, -11053.2 -1922.6, -11058.5 -1915.9, -11073.4 -1927.7, -11077.8 -1922.2, -11087.2 -1929.7','The Gamesman\'s Hall: off the hall, its balconies and stairways - back onto the middle of the board');
/*!40000 ALTER TABLE `battleground_bounds` ENABLE KEYS */;
UNLOCK TABLES;
/*!40103 SET TIME_ZONE=@OLD_TIME_ZONE */;

/*!40101 SET SQL_MODE=@OLD_SQL_MODE */;
/*!40014 SET FOREIGN_KEY_CHECKS=@OLD_FOREIGN_KEY_CHECKS */;
/*!40014 SET UNIQUE_CHECKS=@OLD_UNIQUE_CHECKS */;
/*!40101 SET CHARACTER_SET_CLIENT=@OLD_CHARACTER_SET_CLIENT */;
/*!40101 SET CHARACTER_SET_RESULTS=@OLD_CHARACTER_SET_RESULTS */;
/*!40101 SET COLLATION_CONNECTION=@OLD_COLLATION_CONNECTION */;
/*!40111 SET SQL_NOTES=@OLD_SQL_NOTES */;

