CREATE TABLE IF NOT EXISTS `custom_hidden_itemset_bonus` (
    `id` INT UNSIGNED NOT NULL AUTO_INCREMENT,
    `itemset_id` INT UNSIGNED NOT NULL,
    `required_count` TINYINT UNSIGNED NOT NULL,
    `spell_to_apply` INT UNSIGNED NOT NULL,
    PRIMARY KEY (`id`),
    UNIQUE KEY `uniq_set` (`itemset_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
