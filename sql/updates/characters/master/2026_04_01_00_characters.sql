CREATE TABLE IF NOT EXISTS `playerbot_bg_bootstrap_queue` (
  `id` bigint unsigned NOT NULL AUTO_INCREMENT,
  `requested_at` datetime NOT NULL,
  `team_id` tinyint unsigned NOT NULL,
  `battleground_type_id` smallint unsigned NOT NULL,
  `bot_name_prefix` varchar(32) COLLATE utf8mb4_unicode_ci NOT NULL,
  `state` varchar(16) COLLATE utf8mb4_unicode_ci NOT NULL DEFAULT 'queued',
  `attempts` tinyint unsigned NOT NULL DEFAULT 0,
  `processed_at` datetime DEFAULT NULL,
  `last_error` varchar(255) COLLATE utf8mb4_unicode_ci NOT NULL DEFAULT '',
  PRIMARY KEY (`id`),
  KEY `idx_state_requested` (`state`, `requested_at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
