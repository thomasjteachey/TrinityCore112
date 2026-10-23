
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
DROP TABLE IF EXISTS `rift_ability_whitelist`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!50503 SET character_set_client = utf8mb4 */;
CREATE TABLE `rift_ability_whitelist` (
  `class` tinyint unsigned NOT NULL,
  `name` varchar(64) NOT NULL,
  `free` tinyint unsigned NOT NULL DEFAULT '0',
  PRIMARY KEY (`class`,`name`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci COMMENT='Summoner''s Rift: the class spells skill picks can learn';
/*!40101 SET character_set_client = @saved_cs_client */;

LOCK TABLES `rift_ability_whitelist` WRITE;
/*!40000 ALTER TABLE `rift_ability_whitelist` DISABLE KEYS */;
INSERT INTO `rift_ability_whitelist` VALUES (1,'Battle Stance',1),(1,'Berserker Rage',0),(1,'Berserker Stance',1),(1,'Bloodrage',0),(1,'Charge',0),(1,'Cleave',0),(1,'Defensive Stance',1),(1,'Demoralizing Shout',0),(1,'Disarm',0),(1,'Execute',0),(1,'Hamstring',0),(1,'Heroic Strike',0),(1,'Intercept',0),(1,'Intimidating Shout',0),(1,'Overpower',0),(1,'Pummel',0),(1,'Recklessness',0),(1,'Rend',0),(1,'Retaliation',0),(1,'Revenge',0),(1,'Shield Bash',0),(1,'Shield Block',0),(1,'Shield Wall',0),(1,'Slam',0),(1,'Sunder Armor',0),(1,'Thunder Clap',0),(1,'Whirlwind',0),(2,'Cleanse',0),(2,'Concentration Aura',0),(2,'Divine Intervention',0),(2,'Divine Protection',0),(2,'Divine Shield',0),(2,'Exorcism',0),(2,'Flash of Light',0),(2,'Hammer of Justice',0),(2,'Hammer of Wrath',0),(2,'Hand of Freedom',0),(2,'Hand of Protection',0),(2,'Hand of Sacrifice',0),(2,'Holy Light',0),(2,'Holy Wrath',0),(2,'Judgement',0),(2,'Judgement*',0),(2,'Lay on Hands',0),(2,'Purify',0),(2,'Seal of Justice',0),(2,'Seal of Light',0),(2,'Seal of Righteousness',0),(2,'Seal of the Crusader',0),(2,'Seal of Wisdom',0),(2,'Turn Undead',0),(3,'Arcane Shot',0),(3,'Aspect of the Cheetah',0),(3,'Aspect of the Hawk',0),(3,'Aspect of the Monkey',0),(3,'Aspect of the Pack',0),(3,'Auto Shot',1),(3,'Call Pet',1),(3,'Concussive Shot',0),(3,'Dismiss Pet',1),(3,'Explosive Trap',0),(3,'Feed Pet',1),(3,'Feign Death',0),(3,'Flare',0),(3,'Freezing Trap',0),(3,'Frost Trap',0),(3,'Hunter\'s Mark',0),(3,'Immolation Trap',0),(3,'Mend Pet',0),(3,'Mongoose Bite',0),(3,'Multi-Shot',0),(3,'Rapid Fire',0),(3,'Raptor Strike',0),(3,'Revive Pet',1),(3,'Scare Beast',0),(3,'Scorpid Sting',0),(3,'Serpent Sting',0),(3,'Tame Beast',1),(3,'Tranquilizing Shot',0),(3,'Viper Sting',0),(3,'Volley',0),(3,'Wing Clip',0),(4,'Ambush',0),(4,'Backstab',0),(4,'Blind',0),(4,'Cheap Shot',0),(4,'Crippling Poison*',1),(4,'Deadly Poison*',1),(4,'Distract',0),(4,'Evasion',0),(4,'Eviscerate',0),(4,'Expose Armor',0),(4,'Feint',0),(4,'Garrote',0),(4,'Gouge',0),(4,'Instant Poison*',1),(4,'Kick',0),(4,'Kidney Shot',0),(4,'Mind-numbing Poison*',1),(4,'Rupture',0),(4,'Sap',0),(4,'Sinister Strike',0),(4,'Sprint',0),(4,'Stealth',0),(4,'Vanish',0),(4,'Wound Poison*',1),(5,'Abolish Disease',0),(5,'Cure Disease',0),(5,'Desperate Prayer',0),(5,'Devouring Plague',0),(5,'Dispel Magic',0),(5,'Elune\'s Grace',0),(5,'Fade',0),(5,'Fear Ward',0),(5,'Feedback',0),(5,'Flash Heal',0),(5,'Greater Heal',0),(5,'Heal',0),(5,'Hex of Weakness',0),(5,'Holy Fire',0),(5,'Inner Fire',0),(5,'Mana Burn',0),(5,'Mind Blast',0),(5,'Mind Control',0),(5,'Power Word: Shield',0),(5,'Prayer of Healing',0),(5,'Psychic Scream',0),(5,'Renew',0),(5,'Shackle Undead',0),(5,'Shadow Word: Pain',0),(5,'Shadowguard',0),(5,'Smite',0),(5,'Starshards',0),(5,'Touch of Weakness',0),(7,'Chain Heal',0),(7,'Chain Lightning',0),(7,'Cure Disease',0),(7,'Cure Poison',0),(7,'Disease Cleansing Totem',0),(7,'Earth Shock',0),(7,'Earthbind Totem',0),(7,'Fire Nova Totem',0),(7,'Flame Shock',0),(7,'Flametongue Weapon',0),(7,'Frost Shock',0),(7,'Frostbrand Weapon',0),(7,'Ghost Wolf',0),(7,'Grounding Totem',0),(7,'Healing Wave',0),(7,'Lesser Healing Wave',0),(7,'Lightning Bolt',0),(7,'Lightning Shield',0),(7,'Magma Totem',0),(7,'Poison Cleansing Totem',0),(7,'Purge',0),(7,'Reincarnation',0),(7,'Rockbiter Weapon',0),(7,'Searing Totem',0),(7,'Stoneclaw Totem',0),(7,'Tremor Totem',0),(7,'Windfury Totem',0),(7,'Windfury Weapon',0),(8,'Arcane Explosion',0),(8,'Arcane Missiles',0),(8,'Blink',0),(8,'Blizzard',0),(8,'Cone of Cold',0),(8,'Counterspell',0),(8,'Evocation',0),(8,'Fire Blast',0),(8,'Fire Ward',0),(8,'Fireball',0),(8,'Flamestrike',0),(8,'Frost Armor',0),(8,'Frost Nova',0),(8,'Frost Ward',0),(8,'Frostbolt',0),(8,'Ice Armor',0),(8,'Mage Armor',0),(8,'Mana Shield',0),(8,'Polymorph',0),(8,'Remove Lesser Curse',0),(8,'Scorch',0),(9,'Banish',0),(9,'Corruption',0),(9,'Create Healthstone*',0),(9,'Create Soulstone*',0),(9,'Curse of Agony',0),(9,'Curse of Recklessness',0),(9,'Curse of Shadow',0),(9,'Curse of the Elements',0),(9,'Curse of Tongues',0),(9,'Curse of Weakness',0),(9,'Death Coil',0),(9,'Demon Armor',0),(9,'Demon Skin',0),(9,'Drain Life',0),(9,'Drain Mana',0),(9,'Drain Soul',0),(9,'Fear',0),(9,'Hellfire',0),(9,'Howl of Terror',0),(9,'Immolate',0),(9,'Inferno',0),(9,'Life Tap',0),(9,'Rain of Fire',0),(9,'Ritual of Doom',0),(9,'Searing Pain',0),(9,'Shadow Bolt',0),(9,'Shadow Ward',0),(9,'Soul Fire',0),(9,'Summon Felhunter',1),(9,'Summon Imp',1),(9,'Summon Succubus',1),(9,'Summon Voidwalker',1),(11,'Abolish Poison',0),(11,'Barkskin',0),(11,'Bash',0),(11,'Bear Form',1),(11,'Cat Form',1),(11,'Claw',0),(11,'Cure Poison',0),(11,'Dash',0),(11,'Demoralizing Roar',0),(11,'Dire Bear Form',1),(11,'Enrage',0),(11,'Entangling Roots',0),(11,'Faerie Fire',0),(11,'Faerie Fire (Feral)',0),(11,'Ferocious Bite',0),(11,'Frenzied Regeneration',0),(11,'Healing Touch',0),(11,'Hibernate',0),(11,'Hurricane',0),(11,'Innervate',0),(11,'Maul',0),(11,'Moonfire',0),(11,'Nature\'s Grasp',0),(11,'Pounce',0),(11,'Prowl',0),(11,'Rake',0),(11,'Ravage',0),(11,'Rebirth',0),(11,'Regrowth',0),(11,'Rejuvenation',0),(11,'Remove Curse',0),(11,'Rip',0),(11,'Shred',0),(11,'Starfire',0),(11,'Swipe',0),(11,'Swipe (Bear)',0),(11,'Tranquility',0),(11,'Travel Form',0),(11,'Wrath',0);
/*!40000 ALTER TABLE `rift_ability_whitelist` ENABLE KEYS */;
UNLOCK TABLES;
/*!40103 SET TIME_ZONE=@OLD_TIME_ZONE */;

/*!40101 SET SQL_MODE=@OLD_SQL_MODE */;
/*!40014 SET FOREIGN_KEY_CHECKS=@OLD_FOREIGN_KEY_CHECKS */;
/*!40014 SET UNIQUE_CHECKS=@OLD_UNIQUE_CHECKS */;
/*!40101 SET CHARACTER_SET_CLIENT=@OLD_CHARACTER_SET_CLIENT */;
/*!40101 SET CHARACTER_SET_RESULTS=@OLD_CHARACTER_SET_RESULTS */;
/*!40101 SET COLLATION_CONNECTION=@OLD_COLLATION_CONNECTION */;
/*!40111 SET SQL_NOTES=@OLD_SQL_NOTES */;

