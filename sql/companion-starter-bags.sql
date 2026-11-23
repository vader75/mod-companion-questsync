-- Optional starter grant, characters DB. Apply once after companion-custody.sql
-- and before booting the aligned Playerbots/QuestSync build. No backfill.
-- NULL = not armed (including all existing companions); empty = pending;
-- nonempty = consumed, permanently tied to the custody tombstone for this body.
ALTER TABLE cq_companion_custody
    ADD COLUMN starter_bag_attempt CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NULL DEFAULT NULL;