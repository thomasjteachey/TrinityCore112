CREATE TABLE IF NOT EXISTS `account_bank_item` (
  `accountId` int unsigned NOT NULL,
  `slot` smallint unsigned NOT NULL,
  `item_guid` int unsigned NOT NULL,
  PRIMARY KEY (`accountId`,`slot`),
  UNIQUE KEY `idx_account_bank_item_guid` (`item_guid`),
  CONSTRAINT `fk_account_bank_item_guid` FOREIGN KEY (`item_guid`) REFERENCES `item_instance` (`guid`) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Account wide bank slots';
