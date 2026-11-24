-- Companion roles: what each companion is for.
--
-- Run once, against the characters database, before the worldserver restarts. The module
-- probes for the column and warns instead of pretending when it is missing.
--
-- "auto" is the default and the safe one: the module keeps its hands off that companion.
-- Nothing below changes movement or combat yet - the role is a record that the strategy
-- and etiquette layers will read.

ALTER TABLE cq_companion_custody
    ADD COLUMN role VARCHAR(16) NOT NULL DEFAULT 'auto';
