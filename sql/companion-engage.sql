-- Companion engagement policies: whether a companion gets into a fight at all.
--
-- Run once, against the characters database. The module probes for the column and warns
-- instead of pretending when it is missing.
--
-- auto    the module keeps out of it (playerbots decides)
-- mine    it fights only what you are fighting
-- joined  it joins fights you are already in, and starts none
-- never   it stays out of combat (it still defends itself)

ALTER TABLE cq_companion_custody
    ADD COLUMN engage VARCHAR(8) NOT NULL DEFAULT 'auto';
