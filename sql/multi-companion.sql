-- Several companions per owner.
--
-- `wowlegends_companion` has `owner_guid` as its PRIMARY KEY, so it can only ever hold
-- one row per player. That is a schema rule, not a module limit: the lookup reads every
-- row the table has, and the sweep loops them. This script creates a table that can hold
-- them, carries over the bindings that already exist, and is safe to run twice.
--
-- Run against the characters database (wl_characters here), then point the module at it:
--
--     CompanionQuestSync.BindingTable = wowlegends_companion_multi
--
-- and `.reload config`. Reverting is the same one line back plus a reload; nothing is
-- written to the old table, so the old bindings are still there.

CREATE TABLE IF NOT EXISTS `wowlegends_companion_multi` (
  `owner_guid`     INT UNSIGNED NOT NULL,
  `bot_guid`       INT UNSIGNED NOT NULL,
  `companion_name` VARCHAR(24)  NOT NULL,
  `created_at`     TIMESTAMP    NOT NULL DEFAULT CURRENT_TIMESTAMP,
  KEY `owner` (`owner_guid`),
  KEY `bot`   (`bot_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Carry over what is already bound. IGNORE keeps this idempotent.
INSERT IGNORE INTO `wowlegends_companion_multi` (`owner_guid`, `bot_guid`, `companion_name`)
SELECT `owner_guid`, `bot_guid`, `companion_name`
  FROM `wowlegends_companion`;

-- One bot bound once. Two rows for the same bot means two sweeps levelling and
-- re-questing one character against each other, and two companions whispering with the
-- same name. The module warns about duplicates too, but a clean table is cheaper.
SELECT `bot_guid`, COUNT(*) AS `owners`
  FROM `wowlegends_companion_multi`
 GROUP BY `bot_guid`
HAVING `owners` > 1;

-- Four companions per owner is the ceiling: a party of five with the player. Anything
-- beyond that is ignored (and logged) rather than silently swept.
SELECT `owner_guid`, COUNT(*) AS `companions`
  FROM `wowlegends_companion_multi`
 GROUP BY `owner_guid`
HAVING `companions` > 4;
