
-- One realm on this machine. Change address/localAddress/port to match your worldserver.conf
-- (RealmID, WorldServerPort). gamebuild 12342 is the Centurion client's build.
INSERT INTO `realmlist` (`id`,`name`,`address`,`localAddress`,`localSubnetMask`,`port`,`icon`,`flag`,`timezone`,`allowedSecurityLevel`,`population`,`gamebuild`) VALUES (1,'Centurion','127.0.0.1','127.0.0.1','255.255.255.0',8085,1,0,1,0,0,12342);
