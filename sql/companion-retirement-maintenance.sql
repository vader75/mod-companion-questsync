-- OPTIONAL, during authorized clean-stop maintenance only, with all world
-- processes and external account/character writers stopped. Not a save barrier.
-- Requires both canonical binding tables and InnoDB; see docs/companion-custody.md.
-- This retires a body permanently; it does not wipe it, rename it, or recycle it.
DELIMITER $$
DROP PROCEDURE IF EXISTS cq_finalize_companion_retirement$$
CREATE PROCEDURE cq_finalize_companion_retirement(
    IN p_module VARCHAR(8), IN p_bot INT UNSIGNED, IN p_owner INT UNSIGNED,
    IN p_account INT UNSIGNED, IN p_name VARCHAR(24))
BEGIN
    -- Parameters inherit the caller's connection collation, which need not match the
    -- ledger's utf8mb4_0900_ai_ci columns; comparing them directly raises error 1267.
    DECLARE v_module VARCHAR(8) CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci;
    DECLARE v_name VARCHAR(24) CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci;
    DECLARE EXIT HANDLER FOR SQLEXCEPTION
    BEGIN
        ROLLBACK;
        RESIGNAL;
    END;

    SET v_module = p_module;
    SET v_name = p_name;

    START TRANSACTION;
    IF v_module NOT IN ('cq', 'legacy') OR v_module IS NULL OR p_bot = p_owner THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Invalid retirement identity';
    END IF;
    -- Require old renamed tables to be reconciled first; a row in one of them is
    -- still a reservation. Do not guess how to interpret a compatibility table.
    IF EXISTS (SELECT 1 FROM information_schema.tables WHERE table_schema = DATABASE()
               AND table_name = 'wowlegends_companion_multi') THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Review and archive the old multi-binding table first';
    END IF;
    UPDATE cq_companion_custody x JOIN characters c ON c.guid = x.bot_guid
    JOIN characters o ON o.guid = x.owner_guid
    SET x.state = 'retired'
    WHERE x.module = v_module AND x.state = 'retiring' AND x.tracking = 0
      AND x.bot_guid = p_bot AND x.owner_guid = p_owner AND x.body_account = p_account
      AND c.account = p_account AND c.online = 0
      AND BINARY x.body_name = BINARY v_name AND BINARY c.name = BINARY v_name;
    IF ROW_COUNT() <> 1 THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Unverified or non-offline retirement';
    END IF;
    IF v_module = 'cq' THEN
        IF EXISTS (SELECT 1 FROM wowlegends_companion WHERE bot_guid = p_bot) THEN
            SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Conflicting legacy reservation';
        END IF;
        DELETE FROM cq_bindings WHERE bot_guid = p_bot AND owner_guid = p_owner AND BINARY companion_name = BINARY v_name;
    ELSE
        IF EXISTS (SELECT 1 FROM cq_bindings WHERE bot_guid = p_bot) THEN
            SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Conflicting QuestSync reservation';
        END IF;
        DELETE FROM wowlegends_companion WHERE bot_guid = p_bot AND owner_guid = p_owner AND BINARY companion_name = BINARY v_name;
    END IF;
    IF ROW_COUNT() <> 1 THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Binding mismatch; retirement rolled back';
    END IF;
    COMMIT;
END$$
DELIMITER ;
