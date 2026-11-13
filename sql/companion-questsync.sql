-- mod-companion-questsync - schema
--
-- Imports into the characters database (wl_characters on this realm):
--
--     mysql -u <user> -p wl_characters < companion-questsync.sql
--
-- Safe to run more than once. Everything here is idempotent, and the migration at the
-- bottom moves data out of the old `wowlegends_companion_*` names this module used
-- before it stopped borrowing another module's namespace.

-- Bindings this module owns: which body belongs to which owner, under which name.
-- A body can be bound once; an owner can bind several (the cap is a config option, 4).
CREATE TABLE IF NOT EXISTS `cq_bindings` (
  `owner_guid`     INT UNSIGNED NOT NULL,
  `bot_guid`       INT UNSIGNED NOT NULL,
  `companion_name` VARCHAR(24)  NOT NULL,
  `created_at`     TIMESTAMP    NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`owner_guid`, `bot_guid`),
  UNIQUE KEY `bot` (`bot_guid`),
  KEY `owner` (`owner_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- The module's own pool ledger: bodies this module released, and when. Retention is 180
-- days, pruned by `.companionadmin integrity fix`.
CREATE TABLE IF NOT EXISTS `cq_pool` (
  `bot_guid`    INT UNSIGNED NOT NULL,
  `released_at` TIMESTAMP    NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `reason`      VARCHAR(32)  NOT NULL DEFAULT '',
  PRIMARY KEY (`bot_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ---------------------------------------------------------------------------
-- migration from the old names
-- ---------------------------------------------------------------------------

-- 1. an existing installation keeps its rows: copy them across if the old table is there
SET @old_bindings := (SELECT COUNT(*) FROM information_schema.tables
                       WHERE table_schema = DATABASE() AND table_name = 'wowlegends_companion_multi');
SET @sql := IF(@old_bindings > 0,
  'INSERT IGNORE INTO `cq_bindings` (owner_guid, bot_guid, companion_name, created_at)
     SELECT owner_guid, bot_guid, companion_name, created_at FROM `wowlegends_companion_multi`',
  'DO 0');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @old_pool := (SELECT COUNT(*) FROM information_schema.tables
                   WHERE table_schema = DATABASE() AND table_name = 'wowlegends_companion_pool');
SET @sql := IF(@old_pool > 0,
  'INSERT IGNORE INTO `cq_pool` (bot_guid, released_at, reason)
     SELECT bot_guid, released_at, reason FROM `wowlegends_companion_pool`',
  'DO 0');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

-- 2. drop the old tables once their rows are across. Nothing else may still name them,
--    so this belongs with the config change (BindingTable = cq_bindings,
--    PoolTable = cq_pool), not before it.
SET @sql := IF(@old_bindings > 0, 'DROP TABLE `wowlegends_companion_multi`', 'DO 0');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @sql := IF(@old_pool > 0, 'DROP TABLE `wowlegends_companion_pool`', 'DO 0');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

-- 3. keys for tables created by an older version of this file (no primary key, a plain
--    'bot' index): without them a body could be bound twice.
SET @plain_bot := (SELECT COUNT(*) FROM information_schema.statistics
                    WHERE table_schema = DATABASE() AND table_name = 'cq_bindings'
                      AND index_name = 'bot' AND non_unique = 1);
SET @sql := IF(@plain_bot > 0, 'ALTER TABLE `cq_bindings` DROP INDEX `bot`', 'DO 0');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @unique_bot := (SELECT COUNT(*) FROM information_schema.statistics
                     WHERE table_schema = DATABASE() AND table_name = 'cq_bindings'
                       AND column_name = 'bot_guid' AND non_unique = 0);
SET @sql := IF(@unique_bot = 0, 'ALTER TABLE `cq_bindings` ADD UNIQUE KEY `bot` (`bot_guid`)', 'DO 0');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @has_pk := (SELECT COUNT(*) FROM information_schema.statistics
                 WHERE table_schema = DATABASE() AND table_name = 'cq_bindings'
                   AND index_name = 'PRIMARY');
SET @sql := IF(@has_pk = 0,
  'ALTER TABLE `cq_bindings` ADD PRIMARY KEY (`owner_guid`, `bot_guid`)', 'DO 0');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;
