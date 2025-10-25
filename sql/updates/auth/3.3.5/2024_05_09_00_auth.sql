DELETE FROM `rbac_linked_permissions` WHERE `id`=196 AND `linkedId`=882;
DELETE FROM `rbac_permissions` WHERE `id`=882;
INSERT INTO `rbac_permissions` (`id`, `name`) VALUES (882, 'Command: reload autobalance');
INSERT INTO `rbac_linked_permissions` (`id`, `linkedId`) VALUES (196, 882);
