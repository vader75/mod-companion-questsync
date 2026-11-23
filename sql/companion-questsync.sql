-- mod-companion-questsync - schema
--
-- Imports into the characters database (wl_characters on this realm):
--
--     mysql -u <user> -p wl_characters < companion-questsync.sql
--
-- Additive and idempotent. Do not import or delete historical compatibility
-- tables automatically: conflicting rows are reservations pending offline review.
-- Lifecycle operations also require companion-custody.sql.

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

-- Historical pool ledger. It is not authority to assign, rename, or reset a body.
CREATE TABLE IF NOT EXISTS `cq_pool` (
  `bot_guid`    INT UNSIGNED NOT NULL,
  `released_at` TIMESTAMP    NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `reason`      VARCHAR(32)  NOT NULL DEFAULT '',
  PRIMARY KEY (`bot_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
