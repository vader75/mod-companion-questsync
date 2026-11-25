-- Companion role lock: how recently a role was changed.
--
-- Run once, against the characters database, after companion-roles.sql. Without it the role
-- command still works, it just cannot rate-limit changes - the module logs one warning and says
-- so rather than failing.
--
-- Why it exists: a role is not a checkbox. It drives strategies, and (with the loadout work) a
-- respec and a gear swap, and flipping it five times in ten seconds is how a companion ends up
-- respeccing in a doorway. One change per CompanionQuestSync.RoleChangeCooldown (default 60 s),
-- and only out of combat.

ALTER TABLE cq_companion_custody
    ADD COLUMN role_changed_at INT UNSIGNED NOT NULL DEFAULT 0;
