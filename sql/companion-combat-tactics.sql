-- Run once against the characters database. Per owned body, the three base-3
-- digits are Tank First, Gap Closers, and Guard Flanks, least significant first.
-- 0 inherits the realm policy; 1 is off; 2 is on (when safe for Gap Closers).
ALTER TABLE cq_companion_custody
    ADD COLUMN combat_tactics TINYINT UNSIGNED NOT NULL DEFAULT 0;
