-- Quiet: playerbots' own threat reduction, per companion.
--
-- Dropping the tank strategy stops a companion grabbing aggro on purpose, but its damage still
-- builds threat. "quiet" turns on the threat strategy, at the cost of damage, so it stays off
-- unless asked for (the card's Quiet button, or `.companionsync quiet <name> on`).

ALTER TABLE cq_companion_custody
    ADD COLUMN quiet TINYINT UNSIGNED NOT NULL DEFAULT 0;
