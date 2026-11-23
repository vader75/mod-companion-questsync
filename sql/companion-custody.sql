-- Shared characters-DB schema for paired QuestSync and mod-wowlegends lifecycle.
-- Additive migration. Deliberately does NOT grant custody based on historical
-- bindings, account type, name prefixes or second-resolution release timestamps.
CREATE TABLE IF NOT EXISTS cq_companion_custody (
    bot_guid INT UNSIGNED NOT NULL PRIMARY KEY,
    module ENUM('cq', 'legacy') NOT NULL,
    body_account INT UNSIGNED NOT NULL,
    body_name VARCHAR(24) NOT NULL,
    owner_guid INT UNSIGNED NOT NULL DEFAULT 0,
    state ENUM('free', 'owned', 'retiring', 'retired') NOT NULL,
    tracking TINYINT UNSIGNED NOT NULL DEFAULT 0,
    reviewed_by VARCHAR(96) NOT NULL,
    changed_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6),
    KEY stock (module, state),
    KEY ownership (module, owner_guid, state),
    CONSTRAINT cq_custody_state CHECK (
        bot_guid > 0 AND body_account > 0 AND owner_guid <> bot_guid AND
        CHAR_LENGTH(body_name) > 0 AND CHAR_LENGTH(reviewed_by) > 0 AND
        ((state = 'free' AND owner_guid = 0 AND tracking = 0) OR
         (state = 'owned' AND owner_guid > 0 AND tracking IN (0, 1)) OR
         (state IN ('retiring', 'retired') AND owner_guid > 0 AND tracking = 0))
    )
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
