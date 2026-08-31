-- Restore hostile factions on quest kill-credit targets.
--
-- The B+ world carries 9347 creatures on faction 35 (friendly to everyone)
-- where stock TrinityCore has them on a real faction.  For most that is
-- harmless, but 126 of them are the kill-credit target of a quest: friendly
-- to everyone means unattackable, which means the credit can never be earned
-- and the quest cannot be completed at all.  Ashenvale Outrunner (12856,
-- quest 6503 "Ashenvale Outrunners") is the reported case; the rest were
-- found the same way and are swept here so it is done once.
--
-- Every value below is that creature's stock TrinityCore faction, read from
-- the reference world database on the same box - not a guess.
--
-- Scope: ONLY creatures that are both a quest kill target AND differ from
-- stock.  The other ~9200 faction-35 creatures (city guards, vendors,
-- ambient NPCs) are deliberately left alone.

UPDATE creature_template SET faction=11 WHERE entry=26212;  -- Captain Gryan Stoutmantle
UPDATE creature_template SET faction=1174 WHERE entry=11878;  -- Nathanos Blightcaller
UPDATE creature_template SET faction=12 WHERE entry=12423;  -- Guard Roberts
UPDATE creature_template SET faction=12 WHERE entry=17240;  -- Admiral Odesyus
UPDATE creature_template SET faction=120 WHERE entry=2487;  -- Fleet Master Seahorn
UPDATE creature_template SET faction=120 WHERE entry=2496;  -- Baron Revilgaz
UPDATE creature_template SET faction=1214 WHERE entry=13154;  -- Commander Louis Philips
UPDATE creature_template SET faction=1214 WHERE entry=13597;  -- Frostwolf Explosives Expert
UPDATE creature_template SET faction=1216 WHERE entry=13320;  -- Commander Karl Philips
UPDATE creature_template SET faction=1216 WHERE entry=13598;  -- Stormpike Explosives Expert
UPDATE creature_template SET faction=122 WHERE entry=6177;  -- Narm Faulk
UPDATE creature_template SET faction=123 WHERE entry=2265;  -- Hillsbrad Apprentice Blacksmith
UPDATE creature_template SET faction=123 WHERE entry=6172;  -- Henze Faulk
UPDATE creature_template SET faction=124 WHERE entry=16330;  -- Sentinel Spy
UPDATE creature_template SET faction=126 WHERE entry=40257;  -- Troll Citizen
UPDATE creature_template SET faction=126 WHERE entry=40260;  -- Troll Volunteer
UPDATE creature_template SET faction=1314 WHERE entry=27493;  -- Sergeant Bonesnap
UPDATE creature_template SET faction=1315 WHERE entry=27121;  -- Blackriver Credit
UPDATE creature_template SET faction=14 WHERE entry=11836;  -- Captured Rabid Thistle Bear
UPDATE creature_template SET faction=14 WHERE entry=5676;  -- Summoned Voidwalker
UPDATE creature_template SET faction=15 WHERE entry=24117;  -- Lurielle
UPDATE creature_template SET faction=150 WHERE entry=4979;  -- Theramore Guard
UPDATE creature_template SET faction=1514 WHERE entry=12856;  -- Ashenvale Outrunner
UPDATE creature_template SET faction=1576 WHERE entry=2691;  -- Highvale Outrunner
UPDATE creature_template SET faction=1576 WHERE entry=8564;  -- Ranger
UPDATE creature_template SET faction=1603 WHERE entry=15938;  -- Eversong Ranger
UPDATE creature_template SET faction=1604 WHERE entry=15941;  -- Apprentice Ralen
UPDATE creature_template SET faction=1604 WHERE entry=15945;  -- Apprentice Meledor
UPDATE creature_template SET faction=1604 WHERE entry=16206;  -- Apprentice Varnis
UPDATE creature_template SET faction=1604 WHERE entry=16208;  -- Apothecary Enith
UPDATE creature_template SET faction=1604 WHERE entry=16209;  -- Ranger Vedoran
UPDATE creature_template SET faction=1604 WHERE entry=17119;  -- Ithania
UPDATE creature_template SET faction=1604 WHERE entry=17226;  -- Viera Sunwhisper
UPDATE creature_template SET faction=1604 WHERE entry=17768;  -- Blood Knight Stillblade
UPDATE creature_template SET faction=1638 WHERE entry=16483;  -- Draenei Survivor
UPDATE creature_template SET faction=1638 WHERE entry=17116;  -- Exarch Menelaous
UPDATE creature_template SET faction=1638 WHERE entry=17681;  -- Expedition Researcher
UPDATE creature_template SET faction=1651 WHERE entry=16847;  -- Debilitated Mag'har Grunt
UPDATE creature_template SET faction=1652 WHERE entry=18428;  -- Mag'har Prisoner
UPDATE creature_template SET faction=1659 WHERE entry=17900;  -- Ashyen
UPDATE creature_template SET faction=1659 WHERE entry=17901;  -- Keleth
UPDATE creature_template SET faction=1667 WHERE entry=17290;  -- Captain Alina
UPDATE creature_template SET faction=1669 WHERE entry=17296;  -- Captain Boneshatter
UPDATE creature_template SET faction=1685 WHERE entry=17440;  -- High Chief Stillpine
UPDATE creature_template SET faction=1685 WHERE entry=17682;  -- Princess Stillpine
UPDATE creature_template SET faction=1694 WHERE entry=17551;  -- Tavara
UPDATE creature_template SET faction=1722 WHERE entry=18369;  -- Corki
UPDATE creature_template SET faction=1722 WHERE entry=20812;  -- Corki
UPDATE creature_template SET faction=1731 WHERE entry=20071;  -- Wind Trader Marid
UPDATE creature_template SET faction=1732 WHERE entry=19266;  -- Private Imarion
UPDATE creature_template SET faction=1732 WHERE entry=2694;  -- Highvale Ranger
UPDATE creature_template SET faction=1732 WHERE entry=31304;  -- Dying Soldier
UPDATE creature_template SET faction=1734 WHERE entry=19606;  -- Grek
UPDATE creature_template SET faction=1735 WHERE entry=18384;  -- Malukaz
UPDATE creature_template SET faction=1735 WHERE entry=19265;  -- Scout Makha
UPDATE creature_template SET faction=1770 WHERE entry=30152;  -- Bruor Ironbane
UPDATE creature_template SET faction=1770 WHERE entry=36955;  -- Lady Jaina Proudmoore
UPDATE creature_template SET faction=1770 WHERE entry=37554;  -- Lady Sylvanas Windrunner
UPDATE creature_template SET faction=1892 WHERE entry=24124;  -- Captured Valgarde Prisoner (PROXY)
UPDATE creature_template SET faction=1892 WHERE entry=24820;  -- Iron Dwarf Relic
UPDATE creature_template SET faction=1892 WHERE entry=24824;  -- Iron Dwarf Relic
UPDATE creature_template SET faction=1892 WHERE entry=25248;  -- "Salty" John Thorpe
UPDATE creature_template SET faction=1892 WHERE entry=25827;  -- Tom Hegger
UPDATE creature_template SET faction=1892 WHERE entry=26885;  -- Mountaineer Kilian
UPDATE creature_template SET faction=1892 WHERE entry=27341;  -- Helpless Villager Proxy
UPDATE creature_template SET faction=1892 WHERE entry=27359;  -- Trapped Wintergarde Villager
UPDATE creature_template SET faction=1897 WHERE entry=23778;  -- Dark Ranger Lyana
UPDATE creature_template SET faction=190 WHERE entry=25342;  -- Dead Caravan Guard
UPDATE creature_template SET faction=1922 WHERE entry=24211;  -- Freed Winterhoof Longrunner
UPDATE creature_template SET faction=1922 WHERE entry=26179;  -- Taunka'le Refugee
UPDATE creature_template SET faction=1922 WHERE entry=26810;  -- Roanauk Icemist
UPDATE creature_template SET faction=1922 WHERE entry=27221;  -- Tormak the Scarred
UPDATE creature_template SET faction=1928 WHERE entry=27376;  -- Deathguard Schneider
UPDATE creature_template SET faction=1928 WHERE entry=27378;  -- Senior Scrivener Barriga
UPDATE creature_template SET faction=1928 WHERE entry=27379;  -- Engineer Burke
UPDATE creature_template SET faction=1928 WHERE entry=27381;  -- Chancellor Amai
UPDATE creature_template SET faction=1929 WHERE entry=23998;  -- Deathstalker Razael
UPDATE creature_template SET faction=1973 WHERE entry=25773;  -- Fizzcrank Survivor
UPDATE creature_template SET faction=1973 WHERE entry=25828;  -- Guard Mitchells
UPDATE creature_template SET faction=1974 WHERE entry=27588;  -- 7th Legion Elite
UPDATE creature_template SET faction=1974 WHERE entry=27788;  -- Injured 7th Legion Soldier
UPDATE creature_template SET faction=1978 WHERE entry=30381;  -- Xarantaur
UPDATE creature_template SET faction=1981 WHERE entry=25425;  -- Farseer Grimwalker's Spirit
UPDATE creature_template SET faction=2007 WHERE entry=28701;  -- Timothy Jones
UPDATE creature_template SET faction=2032 WHERE entry=27676;  -- Silverbrook Defender
UPDATE creature_template SET faction=2050 WHERE entry=28532;  -- Bloodrose Datura
UPDATE creature_template SET faction=2060 WHERE entry=29043;  -- Rejek
UPDATE creature_template SET faction=2070 WHERE entry=28042;  -- Captain Brandon
UPDATE creature_template SET faction=2070 WHERE entry=28043;  -- Captain Grondel
UPDATE creature_template SET faction=2070 WHERE entry=28044;  -- Captain Rupert
UPDATE creature_template SET faction=2070 WHERE entry=28205;  -- Alchemist Finklestein
UPDATE creature_template SET faction=2070 WHERE entry=29455;  -- Gerk
UPDATE creature_template SET faction=29 WHERE entry=10556;  -- Lazy Peon
UPDATE creature_template SET faction=29 WHERE entry=12430;  -- Grunt Kor'ja
UPDATE creature_template SET faction=29 WHERE entry=39757;  -- Cultist Kagarn
UPDATE creature_template SET faction=29 WHERE entry=39758;  -- Cultist Agtar
UPDATE creature_template SET faction=29 WHERE entry=39760;  -- Cultist Tokka
UPDATE creature_template SET faction=29 WHERE entry=39763;  -- Cultist Rokaga
UPDATE creature_template SET faction=474 WHERE entry=23797;  -- Moxie Steelgrille
UPDATE creature_template SET faction=474 WHERE entry=7583;  -- Sprinkle
UPDATE creature_template SET faction=55 WHERE entry=12427;  -- Mountaineer Dolf
UPDATE creature_template SET faction=64 WHERE entry=39623;  -- Gnome Citizen
UPDATE creature_template SET faction=68 WHERE entry=12428;  -- Deathguard Kel
UPDATE creature_template SET faction=7 WHERE entry=11627;  -- Tamed Kodo
UPDATE creature_template SET faction=7 WHERE entry=22112;  -- Karynaku
UPDATE creature_template SET faction=7 WHERE entry=29032;  -- Malar Bravehorn
UPDATE creature_template SET faction=79 WHERE entry=8518;  -- Rynthariel the Keymaster
UPDATE creature_template SET faction=794 WHERE entry=16031;  -- Ysida Harmon
UPDATE creature_template SET faction=794 WHERE entry=16254;  -- Field Marshal Chambers
UPDATE creature_template SET faction=80 WHERE entry=12429;  -- Sentinel Shaya
UPDATE creature_template SET faction=83 WHERE entry=10668;  -- Beaten Corpse
UPDATE creature_template SET faction=83 WHERE entry=11680;  -- Horde Scout
UPDATE creature_template SET faction=83 WHERE entry=11681;  -- Horde Deforester
UPDATE creature_template SET faction=83 WHERE entry=11684;  -- Warsong Shredder
UPDATE creature_template SET faction=83 WHERE entry=17304;  -- Overseer Gorthak
UPDATE creature_template SET faction=855 WHERE entry=10978;  -- Legacki
UPDATE creature_template SET faction=875 WHERE entry=1268;  -- Ozzie Togglevolt
UPDATE creature_template SET faction=875 WHERE entry=17243;  -- Engineer "Spark" Overgrind
UPDATE creature_template SET faction=875 WHERE entry=39466;  -- Motivated Citizen
UPDATE creature_template SET faction=875 WHERE entry=6119;  -- Tog Rustsprocket
UPDATE creature_template SET faction=875 WHERE entry=7955;  -- Milli Featherwhistle
UPDATE creature_template SET faction=88 WHERE entry=2344;  -- Dun Garok Mountaineer
UPDATE creature_template SET faction=88 WHERE entry=2404;  -- Blacksmith Verringtan
UPDATE creature_template SET faction=894 WHERE entry=23602;  -- Deserter Agitator
UPDATE creature_template SET faction=894 WHERE entry=23720;  -- Theramore Prisoner
UPDATE creature_template SET faction=90 WHERE entry=6268;  -- Summoned Felhunter
