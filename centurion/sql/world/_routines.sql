
/*!40101 SET @OLD_CHARACTER_SET_CLIENT=@@CHARACTER_SET_CLIENT */;
/*!40101 SET @OLD_CHARACTER_SET_RESULTS=@@CHARACTER_SET_RESULTS */;
/*!40101 SET @OLD_COLLATION_CONNECTION=@@COLLATION_CONNECTION */;
/*!50503 SET NAMES utf8mb4 */;
/*!40103 SET @OLD_TIME_ZONE=@@TIME_ZONE */;
/*!40103 SET TIME_ZONE='+00:00' */;
/*!40014 SET @OLD_UNIQUE_CHECKS=@@UNIQUE_CHECKS, UNIQUE_CHECKS=0 */;
/*!40014 SET @OLD_FOREIGN_KEY_CHECKS=@@FOREIGN_KEY_CHECKS, FOREIGN_KEY_CHECKS=0 */;
/*!40101 SET @OLD_SQL_MODE=@@SQL_MODE, SQL_MODE='NO_AUTO_VALUE_ON_ZERO' */;
/*!40111 SET @OLD_SQL_NOTES=@@SQL_NOTES, SQL_NOTES=0 */;
/*!50003 DROP PROCEDURE IF EXISTS `build_classic_gossip_menu_port_list` */;
/*!50003 SET @saved_cs_client      = @@character_set_client */ ;
/*!50003 SET @saved_cs_results     = @@character_set_results */ ;
/*!50003 SET @saved_col_connection = @@collation_connection */ ;
/*!50003 SET character_set_client  = utf8mb4 */ ;
/*!50003 SET character_set_results = utf8mb4 */ ;
/*!50003 SET collation_connection  = utf8mb4_0900_ai_ci */ ;
/*!50003 SET @saved_sql_mode       = @@sql_mode */ ;
/*!50003 SET sql_mode              = 'ONLY_FULL_GROUP_BY,STRICT_TRANS_TABLES,NO_ZERO_IN_DATE,NO_ZERO_DATE,ERROR_FOR_DIVISION_BY_ZERO,NO_ENGINE_SUBSTITUTION' */ ;
DELIMITER ;;
CREATE PROCEDURE `build_classic_gossip_menu_port_list`()
BEGIN
    DECLARE rows_added INT DEFAULT 1;

    WHILE rows_added > 0 DO
        INSERT IGNORE INTO centurionworld.tmp_classic_gossip_menus_to_port (MenuID)
        SELECT
            gmo.action_menu_id
        FROM classicmangos.gossip_menu_option gmo
        JOIN centurionworld.tmp_classic_gossip_menus_to_port s
            ON s.MenuID = gmo.menu_id
        WHERE gmo.action_menu_id > 0;

        SET rows_added = ROW_COUNT();
    END WHILE;
END ;;
DELIMITER ;
/*!50003 SET sql_mode              = @saved_sql_mode */ ;
/*!50003 SET character_set_client  = @saved_cs_client */ ;
/*!50003 SET character_set_results = @saved_cs_results */ ;
/*!50003 SET collation_connection  = @saved_col_connection */ ;
/*!50003 DROP PROCEDURE IF EXISTS `flatten_classic_gossip_conditions` */;
/*!50003 SET @saved_cs_client      = @@character_set_client */ ;
/*!50003 SET @saved_cs_results     = @@character_set_results */ ;
/*!50003 SET @saved_col_connection = @@collation_connection */ ;
/*!50003 SET character_set_client  = utf8mb4 */ ;
/*!50003 SET character_set_results = utf8mb4 */ ;
/*!50003 SET collation_connection  = utf8mb4_0900_ai_ci */ ;
/*!50003 SET @saved_sql_mode       = @@sql_mode */ ;
/*!50003 SET sql_mode              = 'ONLY_FULL_GROUP_BY,STRICT_TRANS_TABLES,NO_ZERO_IN_DATE,NO_ZERO_DATE,ERROR_FOR_DIVISION_BY_ZERO,NO_ENGINE_SUBSTITUTION' */ ;
DELIMITER ;;
CREATE PROCEDURE `flatten_classic_gossip_conditions`()
BEGIN
    DECLARE rows_left INT DEFAULT 1;

    DECLARE vRowID BIGINT UNSIGNED;
    DECLARE vSourceType INT;
    DECLARE vSourceGroup INT;
    DECLARE vSourceEntry INT;
    DECLARE vElseGroup INT;
    DECLARE vConditionEntry INT;
    DECLARE vNegate TINYINT UNSIGNED;

    DECLARE vType INT;
    DECLARE vValue1 INT;
    DECLARE vValue2 INT;
    DECLARE vNewElseGroup INT;

    WHILE rows_left > 0 DO

        SELECT COUNT(*)
        INTO rows_left
        FROM centurionworld.tmp_classic_gossip_condition_pending p
        JOIN classicmangos.conditions c
            ON c.condition_entry = p.condition_entry
        WHERE c.type IN (-1, -2, -3);

        IF rows_left > 0 THEN

            SELECT
                p.RowID,
                p.SourceTypeOrReferenceId,
                p.SourceGroup,
                p.SourceEntry,
                p.ElseGroup,
                p.condition_entry,
                p.NegateFromParent,
                c.type,
                c.value1,
                c.value2
            INTO
                vRowID,
                vSourceType,
                vSourceGroup,
                vSourceEntry,
                vElseGroup,
                vConditionEntry,
                vNegate,
                vType,
                vValue1,
                vValue2
            FROM centurionworld.tmp_classic_gossip_condition_pending p
            JOIN classicmangos.conditions c
                ON c.condition_entry = p.condition_entry
            WHERE c.type IN (-1, -2, -3)
            ORDER BY p.RowID
            LIMIT 1;

            IF vType = -1 THEN

                IF vValue1 <> 0 THEN
                    UPDATE centurionworld.tmp_classic_gossip_condition_pending
                    SET condition_entry = vValue1
                    WHERE RowID = vRowID;
                ELSE
                    DELETE FROM centurionworld.tmp_classic_gossip_condition_pending
                    WHERE RowID = vRowID;
                END IF;

                IF vValue2 <> 0 THEN
                    INSERT INTO centurionworld.tmp_classic_gossip_condition_pending
                    (
                        SourceTypeOrReferenceId,
                        SourceGroup,
                        SourceEntry,
                        ElseGroup,
                        condition_entry,
                        NegateFromParent
                    )
                    VALUES
                    (
                        vSourceType,
                        vSourceGroup,
                        vSourceEntry,
                        vElseGroup,
                        vValue2,
                        vNegate
                    );
                END IF;

            ELSEIF vType = -2 THEN

                SELECT COALESCE(MAX(ElseGroup), 0) + 1
                INTO vNewElseGroup
                FROM centurionworld.tmp_classic_gossip_condition_pending
                WHERE SourceTypeOrReferenceId = vSourceType
                  AND SourceGroup = vSourceGroup
                  AND SourceEntry = vSourceEntry;

                INSERT INTO centurionworld.tmp_classic_gossip_condition_pending
                (
                    SourceTypeOrReferenceId,
                    SourceGroup,
                    SourceEntry,
                    ElseGroup,
                    condition_entry,
                    NegateFromParent
                )
                SELECT
                    SourceTypeOrReferenceId,
                    SourceGroup,
                    SourceEntry,
                    vNewElseGroup,
                    condition_entry,
                    NegateFromParent
                FROM centurionworld.tmp_classic_gossip_condition_pending
                WHERE SourceTypeOrReferenceId = vSourceType
                  AND SourceGroup = vSourceGroup
                  AND SourceEntry = vSourceEntry
                  AND ElseGroup = vElseGroup
                  AND RowID <> vRowID;

                IF vValue1 <> 0 THEN
                    UPDATE centurionworld.tmp_classic_gossip_condition_pending
                    SET condition_entry = vValue1
                    WHERE RowID = vRowID;
                ELSE
                    DELETE FROM centurionworld.tmp_classic_gossip_condition_pending
                    WHERE RowID = vRowID;
                END IF;

                IF vValue2 <> 0 THEN
                    INSERT INTO centurionworld.tmp_classic_gossip_condition_pending
                    (
                        SourceTypeOrReferenceId,
                        SourceGroup,
                        SourceEntry,
                        ElseGroup,
                        condition_entry,
                        NegateFromParent
                    )
                    VALUES
                    (
                        vSourceType,
                        vSourceGroup,
                        vSourceEntry,
                        vNewElseGroup,
                        vValue2,
                        vNegate
                    );
                END IF;

            ELSEIF vType = -3 THEN

                IF vValue1 <> 0 THEN
                    UPDATE centurionworld.tmp_classic_gossip_condition_pending
                    SET
                        condition_entry = vValue1,
                        NegateFromParent = CASE WHEN NegateFromParent = 0 THEN 1 ELSE 0 END
                    WHERE RowID = vRowID;
                ELSE
                    DELETE FROM centurionworld.tmp_classic_gossip_condition_pending
                    WHERE RowID = vRowID;
                END IF;

            END IF;

        END IF;

    END WHILE;
END ;;
DELIMITER ;
/*!50003 SET sql_mode              = @saved_sql_mode */ ;
/*!50003 SET character_set_client  = @saved_cs_client */ ;
/*!50003 SET character_set_results = @saved_cs_results */ ;
/*!50003 SET collation_connection  = @saved_col_connection */ ;
/*!50003 DROP PROCEDURE IF EXISTS `refresh_all_loot_templates_from_classicmangos` */;
/*!50003 SET @saved_cs_client      = @@character_set_client */ ;
/*!50003 SET @saved_cs_results     = @@character_set_results */ ;
/*!50003 SET @saved_col_connection = @@collation_connection */ ;
/*!50003 SET character_set_client  = utf8mb4 */ ;
/*!50003 SET character_set_results = utf8mb4 */ ;
/*!50003 SET collation_connection  = utf8mb4_0900_ai_ci */ ;
/*!50003 SET @saved_sql_mode       = @@sql_mode */ ;
/*!50003 SET sql_mode              = 'ONLY_FULL_GROUP_BY,STRICT_TRANS_TABLES,NO_ZERO_IN_DATE,NO_ZERO_DATE,ERROR_FOR_DIVISION_BY_ZERO,NO_ENGINE_SUBSTITUTION' */ ;
DELIMITER ;;
CREATE PROCEDURE `refresh_all_loot_templates_from_classicmangos`()
BEGIN
    DECLARE done INT DEFAULT 0;
    DECLARE v_table VARCHAR(128);
    DECLARE v_backup VARCHAR(128);
    DECLARE v_comment_expr VARCHAR(255);

    DECLARE cur CURSOR FOR
        SELECT s.table_name
        FROM information_schema.tables s
        JOIN information_schema.tables t
            ON t.table_schema = 'centurionworld'
           AND t.table_name = s.table_name
        JOIN information_schema.columns sc
            ON sc.table_schema = s.table_schema
           AND sc.table_name = s.table_name
        JOIN information_schema.columns tc
            ON tc.table_schema = t.table_schema
           AND tc.table_name = t.table_name
        WHERE s.table_schema = 'classicmangos'
          AND s.table_name LIKE '%\_loot_template'
          AND s.table_type = 'BASE TABLE'
          AND t.table_type = 'BASE TABLE'
        GROUP BY s.table_name
        HAVING
            SUM(LOWER(sc.column_name) = 'entry') > 0
            AND SUM(LOWER(sc.column_name) = 'item') > 0
            AND SUM(LOWER(sc.column_name) = 'chanceorquestchance') > 0
            AND SUM(LOWER(sc.column_name) = 'groupid') > 0
            AND SUM(LOWER(sc.column_name) = 'mincountorref') > 0
            AND SUM(LOWER(sc.column_name) = 'maxcount') > 0
            AND SUM(LOWER(sc.column_name) = 'condition_id') > 0

            AND SUM(LOWER(tc.column_name) = 'entry') > 0
            AND SUM(LOWER(tc.column_name) = 'item') > 0
            AND SUM(LOWER(tc.column_name) = 'reference') > 0
            AND SUM(LOWER(tc.column_name) = 'chance') > 0
            AND SUM(LOWER(tc.column_name) = 'questrequired') > 0
            AND SUM(LOWER(tc.column_name) = 'lootmode') > 0
            AND SUM(LOWER(tc.column_name) = 'groupid') > 0
            AND SUM(LOWER(tc.column_name) = 'mincount') > 0
            AND SUM(LOWER(tc.column_name) = 'maxcount') > 0
            AND SUM(LOWER(tc.column_name) = 'comment') > 0
        ORDER BY s.table_name;

    DECLARE CONTINUE HANDLER FOR NOT FOUND SET done = 1;

    OPEN cur;

    loot_loop: LOOP
        FETCH cur INTO v_table;

        IF done = 1 THEN
            LEAVE loot_loop;
        END IF;

        SELECT COALESCE(
            MAX(CASE WHEN LOWER(column_name) = 'comments' THEN CONCAT('c.`', column_name, '`') END),
            MAX(CASE WHEN LOWER(column_name) = 'comment' THEN CONCAT('c.`', column_name, '`') END),
            'NULL'
        )
        INTO v_comment_expr
        FROM information_schema.columns
        WHERE table_schema = 'classicmangos'
          AND table_name = v_table;

        SET v_backup = CONCAT(v_table, '_bak_classic_20260613');

        SET @sql = CONCAT(
            'DROP TABLE IF EXISTS `centurionworld`.`', v_backup, '`'
        );
        PREPARE stmt FROM @sql;
        EXECUTE stmt;
        DEALLOCATE PREPARE stmt;

        SET @sql = CONCAT(
            'CREATE TABLE `centurionworld`.`', v_backup, '` ',
            'LIKE `centurionworld`.`', v_table, '`'
        );
        PREPARE stmt FROM @sql;
        EXECUTE stmt;
        DEALLOCATE PREPARE stmt;

        SET @sql = CONCAT(
            'INSERT INTO `centurionworld`.`', v_backup, '` ',
            'SELECT * FROM `centurionworld`.`', v_table, '`'
        );
        PREPARE stmt FROM @sql;
        EXECUTE stmt;
        DEALLOCATE PREPARE stmt;

        SET @sql = CONCAT(
            'DELETE b ',
            'FROM `centurionworld`.`', v_table, '` b ',
            'JOIN ( ',
            '    SELECT DISTINCT entry ',
            '    FROM `classicmangos`.`', v_table, '` ',
            '    WHERE condition_id = 0 ',
            ') c ON c.entry = b.Entry'
        );
        PREPARE stmt FROM @sql;
        EXECUTE stmt;
        DEALLOCATE PREPARE stmt;

        SET @sql = CONCAT(
            'INSERT INTO `centurionworld`.`', v_table, '` ',
            '(Entry, Item, Reference, Chance, QuestRequired, LootMode, GroupId, MinCount, MaxCount, Comment) ',
            'SELECT ',
            '    c.entry AS Entry, ',
            '    c.item AS Item, ',
            '    CASE ',
            '        WHEN c.mincountOrRef < 0 THEN ABS(c.mincountOrRef) ',
            '        ELSE 0 ',
            '    END AS Reference, ',
            '    ABS(c.ChanceOrQuestChance) AS Chance, ',
            '    CASE ',
            '        WHEN c.ChanceOrQuestChance < 0 THEN 1 ',
            '        ELSE 0 ',
            '    END AS QuestRequired, ',
            '    1 AS LootMode, ',
            '    c.groupid AS GroupId, ',
            '    CASE ',
            '        WHEN c.mincountOrRef < 0 THEN 1 ',
            '        WHEN c.mincountOrRef < 1 THEN 1 ',
            '        ELSE c.mincountOrRef ',
            '    END AS MinCount, ',
            '    CASE ',
            '        WHEN c.maxcount < 1 THEN 1 ',
            '        ELSE c.maxcount ',
            '    END AS MaxCount, ',
            '    ', v_comment_expr, ' AS Comment ',
            'FROM `classicmangos`.`', v_table, '` c ',
            'WHERE c.condition_id = 0 ',
            'ON DUPLICATE KEY UPDATE ',
            '    Reference = VALUES(Reference), ',
            '    Chance = VALUES(Chance), ',
            '    QuestRequired = VALUES(QuestRequired), ',
            '    LootMode = VALUES(LootMode), ',
            '    GroupId = VALUES(GroupId), ',
            '    MinCount = VALUES(MinCount), ',
            '    MaxCount = VALUES(MaxCount), ',
            '    Comment = VALUES(Comment)'
        );
        PREPARE stmt FROM @sql;
        EXECUTE stmt;
        DEALLOCATE PREPARE stmt;

    END LOOP;

    CLOSE cur;
END ;;
DELIMITER ;
/*!50003 SET sql_mode              = @saved_sql_mode */ ;
/*!50003 SET character_set_client  = @saved_cs_client */ ;
/*!50003 SET character_set_results = @saved_cs_results */ ;
/*!50003 SET collation_connection  = @saved_col_connection */ ;
/*!50003 DROP PROCEDURE IF EXISTS `refresh_classic_quests_except_priest` */;
/*!50003 SET @saved_cs_client      = @@character_set_client */ ;
/*!50003 SET @saved_cs_results     = @@character_set_results */ ;
/*!50003 SET @saved_col_connection = @@collation_connection */ ;
/*!50003 SET character_set_client  = utf8mb4 */ ;
/*!50003 SET character_set_results = utf8mb4 */ ;
/*!50003 SET collation_connection  = utf8mb4_0900_ai_ci */ ;
/*!50003 SET @saved_sql_mode       = @@sql_mode */ ;
/*!50003 SET sql_mode              = 'ONLY_FULL_GROUP_BY,STRICT_TRANS_TABLES,NO_ZERO_IN_DATE,NO_ZERO_DATE,ERROR_FOR_DIVISION_BY_ZERO,NO_ENGINE_SUBSTITUTION' */ ;
DELIMITER ;;
CREATE PROCEDURE `refresh_classic_quests_except_priest`()
BEGIN
    DECLARE v_suffix VARCHAR(32);
    DECLARE v_has_bplus_gqs INT DEFAULT 0;
    DECLARE v_has_bplus_gqe INT DEFAULT 0;
    DECLARE v_has_classic_gqr INT DEFAULT 0;
    DECLARE v_has_classic_gir INT DEFAULT 0;
    DECLARE v_refreshed_count INT DEFAULT 0;
    DECLARE v_still_disabled_count INT DEFAULT 0;

    DECLARE EXIT HANDLER FOR SQLEXCEPTION
    BEGIN
        ROLLBACK;
        RESIGNAL;
    END;

    SET v_suffix = DATE_FORMAT(NOW(6), '%Y%m%d%H%i%s%f');

    SELECT COUNT(*) INTO v_has_bplus_gqs
    FROM information_schema.TABLES
    WHERE TABLE_SCHEMA = 'centurionworld'
      AND TABLE_NAME = 'gameobject_queststarter';

    SELECT COUNT(*) INTO v_has_bplus_gqe
    FROM information_schema.TABLES
    WHERE TABLE_SCHEMA = 'centurionworld'
      AND TABLE_NAME = 'gameobject_questender';

    SELECT COUNT(*) INTO v_has_classic_gqr
    FROM information_schema.TABLES
    WHERE TABLE_SCHEMA = 'classicmangos'
      AND TABLE_NAME = 'gameobject_questrelation';

    SELECT COUNT(*) INTO v_has_classic_gir
    FROM information_schema.TABLES
    WHERE TABLE_SCHEMA = 'classicmangos'
      AND TABLE_NAME = 'gameobject_involvedrelation';

    DROP TEMPORARY TABLE IF EXISTS tmp_classic_quests_refresh;

    CREATE TEMPORARY TABLE tmp_classic_quests_refresh (
        quest INT UNSIGNED NOT NULL PRIMARY KEY
    );

    INSERT IGNORE INTO tmp_classic_quests_refresh (quest)
    SELECT c.entry
    FROM classicmangos.quest_template c
    WHERE (c.RequiredClasses & 16) = 0
      AND c.entry NOT IN
      (
          5637, 5676, 5635, 6346, 5629, 5627,
          5658, 5679, 5652, 5680, 5645, 5674
      );

    SELECT COUNT(*) INTO v_refreshed_count
    FROM tmp_classic_quests_refresh;

    

    SET @sql = CONCAT('CREATE TABLE centurionworld.quest_template_bak_clq_', v_suffix, ' LIKE centurionworld.quest_template');
    PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;
    SET @sql = CONCAT('INSERT INTO centurionworld.quest_template_bak_clq_', v_suffix, ' SELECT * FROM centurionworld.quest_template');
    PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

    SET @sql = CONCAT('CREATE TABLE centurionworld.quest_template_addon_bak_clq_', v_suffix, ' LIKE centurionworld.quest_template_addon');
    PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;
    SET @sql = CONCAT('INSERT INTO centurionworld.quest_template_addon_bak_clq_', v_suffix, ' SELECT * FROM centurionworld.quest_template_addon');
    PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

    SET @sql = CONCAT('CREATE TABLE centurionworld.quest_offer_reward_bak_clq_', v_suffix, ' LIKE centurionworld.quest_offer_reward');
    PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;
    SET @sql = CONCAT('INSERT INTO centurionworld.quest_offer_reward_bak_clq_', v_suffix, ' SELECT * FROM centurionworld.quest_offer_reward');
    PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

    SET @sql = CONCAT('CREATE TABLE centurionworld.quest_request_items_bak_clq_', v_suffix, ' LIKE centurionworld.quest_request_items');
    PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;
    SET @sql = CONCAT('INSERT INTO centurionworld.quest_request_items_bak_clq_', v_suffix, ' SELECT * FROM centurionworld.quest_request_items');
    PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

    SET @sql = CONCAT('CREATE TABLE centurionworld.creature_queststarter_bak_clq_', v_suffix, ' LIKE centurionworld.creature_queststarter');
    PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;
    SET @sql = CONCAT('INSERT INTO centurionworld.creature_queststarter_bak_clq_', v_suffix, ' SELECT * FROM centurionworld.creature_queststarter');
    PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

    SET @sql = CONCAT('CREATE TABLE centurionworld.creature_questender_bak_clq_', v_suffix, ' LIKE centurionworld.creature_questender');
    PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;
    SET @sql = CONCAT('INSERT INTO centurionworld.creature_questender_bak_clq_', v_suffix, ' SELECT * FROM centurionworld.creature_questender');
    PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

    SET @sql = CONCAT('CREATE TABLE centurionworld.disables_bak_clq_', v_suffix, ' LIKE centurionworld.disables');
    PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;
    SET @sql = CONCAT('INSERT INTO centurionworld.disables_bak_clq_', v_suffix, ' SELECT * FROM centurionworld.disables');
    PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

    SET @sql = CONCAT('CREATE TABLE centurionworld.creature_template_bak_clq_', v_suffix, ' LIKE centurionworld.creature_template');
    PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;
    SET @sql = CONCAT('INSERT INTO centurionworld.creature_template_bak_clq_', v_suffix, ' SELECT * FROM centurionworld.creature_template');
    PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

    IF v_has_bplus_gqs > 0 THEN
        SET @sql = CONCAT('CREATE TABLE centurionworld.gameobject_queststarter_bak_clq_', v_suffix, ' LIKE centurionworld.gameobject_queststarter');
        PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;
        SET @sql = CONCAT('INSERT INTO centurionworld.gameobject_queststarter_bak_clq_', v_suffix, ' SELECT * FROM centurionworld.gameobject_queststarter');
        PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;
    END IF;

    IF v_has_bplus_gqe > 0 THEN
        SET @sql = CONCAT('CREATE TABLE centurionworld.gameobject_questender_bak_clq_', v_suffix, ' LIKE centurionworld.gameobject_questender');
        PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;
        SET @sql = CONCAT('INSERT INTO centurionworld.gameobject_questender_bak_clq_', v_suffix, ' SELECT * FROM centurionworld.gameobject_questender');
        PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;
    END IF;

    START TRANSACTION;

    INSERT INTO centurionworld.quest_template
    (
        ID, QuestType, QuestLevel, MinLevel, QuestSortID, QuestInfoID, SuggestedGroupNum,
        RequiredFactionId1, RequiredFactionId2, RequiredFactionValue1, RequiredFactionValue2,
        RewardNextQuest, RewardXPDifficulty, RewardMoney, RewardBonusMoney,
        RewardDisplaySpell, RewardSpell, RewardHonor, RewardKillHonor, StartItem, Flags,
        RequiredPlayerKills,
        RewardItem1, RewardAmount1, RewardItem2, RewardAmount2, RewardItem3, RewardAmount3,
        RewardItem4, RewardAmount4,
        ItemDrop1, ItemDropQuantity1, ItemDrop2, ItemDropQuantity2, ItemDrop3, ItemDropQuantity3,
        ItemDrop4, ItemDropQuantity4,
        RewardChoiceItemID1, RewardChoiceItemQuantity1,
        RewardChoiceItemID2, RewardChoiceItemQuantity2,
        RewardChoiceItemID3, RewardChoiceItemQuantity3,
        RewardChoiceItemID4, RewardChoiceItemQuantity4,
        RewardChoiceItemID5, RewardChoiceItemQuantity5,
        RewardChoiceItemID6, RewardChoiceItemQuantity6,
        POIContinent, POIx, POIy, POIPriority,
        RewardTitle, RewardTalents, RewardArenaPoints,
        RewardFactionID1, RewardFactionValue1, RewardFactionOverride1,
        RewardFactionID2, RewardFactionValue2, RewardFactionOverride2,
        RewardFactionID3, RewardFactionValue3, RewardFactionOverride3,
        RewardFactionID4, RewardFactionValue4, RewardFactionOverride4,
        RewardFactionID5, RewardFactionValue5, RewardFactionOverride5,
        TimeAllowed, AllowableRaces,
        LogTitle, LogDescription, QuestDescription, AreaDescription, QuestCompletionLog,
        RequiredNpcOrGo1, RequiredNpcOrGo2, RequiredNpcOrGo3, RequiredNpcOrGo4,
        RequiredNpcOrGoCount1, RequiredNpcOrGoCount2, RequiredNpcOrGoCount3, RequiredNpcOrGoCount4,
        RequiredItemId1, RequiredItemId2, RequiredItemId3, RequiredItemId4, RequiredItemId5, RequiredItemId6,
        RequiredItemCount1, RequiredItemCount2, RequiredItemCount3, RequiredItemCount4, RequiredItemCount5, RequiredItemCount6,
        Unknown0, ObjectiveText1, ObjectiveText2, ObjectiveText3, ObjectiveText4, VerifiedBuild
    )
    SELECT
        c.entry,
        CASE WHEN c.Method = 1 THEN 2 ELSE c.Method END,
        c.QuestLevel,
        c.MinLevel,
        c.ZoneOrSort,
        c.Type,
        c.SuggestedPlayers,
        c.RepObjectiveFaction,
        0,
        c.RepObjectiveValue,
        0,
        CASE WHEN c.NextQuestInChain < 0 THEN 0 ELSE c.NextQuestInChain END,
        1,
        c.RewOrReqMoney,
        c.RewMoneyMaxLevel,
        c.RewSpell,
        c.RewSpellCast,
        0,
        0,
        c.SrcItemId,
        c.QuestFlags,
        0,

        c.RewItemId1,
        c.RewItemCount1,
        c.RewItemId2,
        c.RewItemCount2,
        c.RewItemId3,
        c.RewItemCount3,
        c.RewItemId4,
        c.RewItemCount4,

        c.ReqSourceId1,
        c.ReqSourceCount1,
        c.ReqSourceId2,
        c.ReqSourceCount2,
        c.ReqSourceId3,
        c.ReqSourceCount3,
        c.ReqSourceId4,
        c.ReqSourceCount4,

        c.RewChoiceItemId1,
        c.RewChoiceItemCount1,
        c.RewChoiceItemId2,
        c.RewChoiceItemCount2,
        c.RewChoiceItemId3,
        c.RewChoiceItemCount3,
        c.RewChoiceItemId4,
        c.RewChoiceItemCount4,
        c.RewChoiceItemId5,
        c.RewChoiceItemCount5,
        c.RewChoiceItemId6,
        c.RewChoiceItemCount6,

        c.PointMapId,
        c.PointX,
        c.PointY,
        c.PointOpt,

        0,
        0,
        0,

        c.RewRepFaction1,
        c.RewRepValue1,
        0,
        c.RewRepFaction2,
        c.RewRepValue2,
        0,
        c.RewRepFaction3,
        c.RewRepValue3,
        0,
        c.RewRepFaction4,
        c.RewRepValue4,
        0,
        c.RewRepFaction5,
        c.RewRepValue5,
        0,

        c.LimitTime,
        c.RequiredRaces,

        COALESCE(c.Title, ''),
        COALESCE(c.Objectives, ''),
        COALESCE(c.Details, ''),
        COALESCE(c.EndText, ''),
        COALESCE(c.Objectives, ''),

        c.ReqCreatureOrGOId1,
        c.ReqCreatureOrGOId2,
        c.ReqCreatureOrGOId3,
        c.ReqCreatureOrGOId4,
        c.ReqCreatureOrGOCount1,
        c.ReqCreatureOrGOCount2,
        c.ReqCreatureOrGOCount3,
        c.ReqCreatureOrGOCount4,

        c.ReqItemId1,
        c.ReqItemId2,
        c.ReqItemId3,
        c.ReqItemId4,
        0,
        0,
        c.ReqItemCount1,
        c.ReqItemCount2,
        c.ReqItemCount3,
        c.ReqItemCount4,
        0,
        0,

        0,
        COALESCE(c.ObjectiveText1, ''),
        COALESCE(c.ObjectiveText2, ''),
        COALESCE(c.ObjectiveText3, ''),
        COALESCE(c.ObjectiveText4, ''),
        0
    FROM classicmangos.quest_template c
    JOIN tmp_classic_quests_refresh t ON t.quest = c.entry
    ON DUPLICATE KEY UPDATE
        QuestType = VALUES(QuestType),
        QuestLevel = VALUES(QuestLevel),
        MinLevel = VALUES(MinLevel),
        QuestSortID = VALUES(QuestSortID),
        QuestInfoID = VALUES(QuestInfoID),
        SuggestedGroupNum = VALUES(SuggestedGroupNum),
        RequiredFactionId1 = VALUES(RequiredFactionId1),
        RequiredFactionId2 = VALUES(RequiredFactionId2),
        RequiredFactionValue1 = VALUES(RequiredFactionValue1),
        RequiredFactionValue2 = VALUES(RequiredFactionValue2),
        RewardNextQuest = VALUES(RewardNextQuest),
        RewardMoney = VALUES(RewardMoney),
        RewardBonusMoney = VALUES(RewardBonusMoney),
        RewardDisplaySpell = VALUES(RewardDisplaySpell),
        RewardSpell = VALUES(RewardSpell),
        StartItem = VALUES(StartItem),
        Flags = VALUES(Flags),
        RewardItem1 = VALUES(RewardItem1),
        RewardAmount1 = VALUES(RewardAmount1),
        RewardItem2 = VALUES(RewardItem2),
        RewardAmount2 = VALUES(RewardAmount2),
        RewardItem3 = VALUES(RewardItem3),
        RewardAmount3 = VALUES(RewardAmount3),
        RewardItem4 = VALUES(RewardItem4),
        RewardAmount4 = VALUES(RewardAmount4),
        ItemDrop1 = VALUES(ItemDrop1),
        ItemDropQuantity1 = VALUES(ItemDropQuantity1),
        ItemDrop2 = VALUES(ItemDrop2),
        ItemDropQuantity2 = VALUES(ItemDropQuantity2),
        ItemDrop3 = VALUES(ItemDrop3),
        ItemDropQuantity3 = VALUES(ItemDropQuantity3),
        ItemDrop4 = VALUES(ItemDrop4),
        ItemDropQuantity4 = VALUES(ItemDropQuantity4),
        RewardChoiceItemID1 = VALUES(RewardChoiceItemID1),
        RewardChoiceItemQuantity1 = VALUES(RewardChoiceItemQuantity1),
        RewardChoiceItemID2 = VALUES(RewardChoiceItemID2),
        RewardChoiceItemQuantity2 = VALUES(RewardChoiceItemQuantity2),
        RewardChoiceItemID3 = VALUES(RewardChoiceItemID3),
        RewardChoiceItemQuantity3 = VALUES(RewardChoiceItemQuantity3),
        RewardChoiceItemID4 = VALUES(RewardChoiceItemID4),
        RewardChoiceItemQuantity4 = VALUES(RewardChoiceItemQuantity4),
        RewardChoiceItemID5 = VALUES(RewardChoiceItemID5),
        RewardChoiceItemQuantity5 = VALUES(RewardChoiceItemQuantity5),
        RewardChoiceItemID6 = VALUES(RewardChoiceItemID6),
        RewardChoiceItemQuantity6 = VALUES(RewardChoiceItemQuantity6),
        POIContinent = VALUES(POIContinent),
        POIx = VALUES(POIx),
        POIy = VALUES(POIy),
        POIPriority = VALUES(POIPriority),
        RewardFactionID1 = VALUES(RewardFactionID1),
        RewardFactionValue1 = VALUES(RewardFactionValue1),
        RewardFactionOverride1 = VALUES(RewardFactionOverride1),
        RewardFactionID2 = VALUES(RewardFactionID2),
        RewardFactionValue2 = VALUES(RewardFactionValue2),
        RewardFactionOverride2 = VALUES(RewardFactionOverride2),
        RewardFactionID3 = VALUES(RewardFactionID3),
        RewardFactionValue3 = VALUES(RewardFactionValue3),
        RewardFactionOverride3 = VALUES(RewardFactionOverride3),
        RewardFactionID4 = VALUES(RewardFactionID4),
        RewardFactionValue4 = VALUES(RewardFactionValue4),
        RewardFactionOverride4 = VALUES(RewardFactionOverride4),
        RewardFactionID5 = VALUES(RewardFactionID5),
        RewardFactionValue5 = VALUES(RewardFactionValue5),
        RewardFactionOverride5 = VALUES(RewardFactionOverride5),
        TimeAllowed = VALUES(TimeAllowed),
        AllowableRaces = VALUES(AllowableRaces),
        LogTitle = VALUES(LogTitle),
        LogDescription = VALUES(LogDescription),
        QuestDescription = VALUES(QuestDescription),
        AreaDescription = VALUES(AreaDescription),
        QuestCompletionLog = VALUES(QuestCompletionLog),
        RequiredNpcOrGo1 = VALUES(RequiredNpcOrGo1),
        RequiredNpcOrGo2 = VALUES(RequiredNpcOrGo2),
        RequiredNpcOrGo3 = VALUES(RequiredNpcOrGo3),
        RequiredNpcOrGo4 = VALUES(RequiredNpcOrGo4),
        RequiredNpcOrGoCount1 = VALUES(RequiredNpcOrGoCount1),
        RequiredNpcOrGoCount2 = VALUES(RequiredNpcOrGoCount2),
        RequiredNpcOrGoCount3 = VALUES(RequiredNpcOrGoCount3),
        RequiredNpcOrGoCount4 = VALUES(RequiredNpcOrGoCount4),
        RequiredItemId1 = VALUES(RequiredItemId1),
        RequiredItemId2 = VALUES(RequiredItemId2),
        RequiredItemId3 = VALUES(RequiredItemId3),
        RequiredItemId4 = VALUES(RequiredItemId4),
        RequiredItemId5 = VALUES(RequiredItemId5),
        RequiredItemId6 = VALUES(RequiredItemId6),
        RequiredItemCount1 = VALUES(RequiredItemCount1),
        RequiredItemCount2 = VALUES(RequiredItemCount2),
        RequiredItemCount3 = VALUES(RequiredItemCount3),
        RequiredItemCount4 = VALUES(RequiredItemCount4),
        RequiredItemCount5 = VALUES(RequiredItemCount5),
        RequiredItemCount6 = VALUES(RequiredItemCount6),
        ObjectiveText1 = VALUES(ObjectiveText1),
        ObjectiveText2 = VALUES(ObjectiveText2),
        ObjectiveText3 = VALUES(ObjectiveText3),
        ObjectiveText4 = VALUES(ObjectiveText4),
        VerifiedBuild = VALUES(VerifiedBuild);

    INSERT INTO centurionworld.quest_template_addon
    (
        ID, MaxLevel, AllowableClasses, SourceSpellID, PrevQuestID, NextQuestID,
        ExclusiveGroup, BreadcrumbForQuestId, RewardMailTemplateID, RewardMailDelay,
        RequiredSkillID, RequiredSkillPoints, RequiredMinRepFaction, RequiredMaxRepFaction,
        RequiredMinRepValue, RequiredMaxRepValue, ProvidedItemCount, SpecialFlags
    )
    SELECT
        c.entry,
        CASE WHEN c.MaxLevel = 255 THEN 0 ELSE c.MaxLevel END,
        c.RequiredClasses,
        c.SrcSpell,
        c.PrevQuestId,
        c.NextQuestId,
        c.ExclusiveGroup,
        CASE WHEN c.BreadcrumbForQuestId < 0 THEN 0 ELSE c.BreadcrumbForQuestId END,
        c.RewMailTemplateId,
        c.RewMailDelaySecs,
        c.RequiredSkill,
        c.RequiredSkillValue,
        c.RequiredMinRepFaction,
        c.RequiredMaxRepFaction,
        c.RequiredMinRepValue,
        c.RequiredMaxRepValue,
        c.SrcItemCount,
        c.SpecialFlags
    FROM classicmangos.quest_template c
    JOIN tmp_classic_quests_refresh t ON t.quest = c.entry
    ON DUPLICATE KEY UPDATE
        MaxLevel = VALUES(MaxLevel),
        AllowableClasses = VALUES(AllowableClasses),
        SourceSpellID = VALUES(SourceSpellID),
        PrevQuestID = VALUES(PrevQuestID),
        NextQuestID = VALUES(NextQuestID),
        ExclusiveGroup = VALUES(ExclusiveGroup),
        BreadcrumbForQuestId = VALUES(BreadcrumbForQuestId),
        RewardMailTemplateID = VALUES(RewardMailTemplateID),
        RewardMailDelay = VALUES(RewardMailDelay),
        RequiredSkillID = VALUES(RequiredSkillID),
        RequiredSkillPoints = VALUES(RequiredSkillPoints),
        RequiredMinRepFaction = VALUES(RequiredMinRepFaction),
        RequiredMaxRepFaction = VALUES(RequiredMaxRepFaction),
        RequiredMinRepValue = VALUES(RequiredMinRepValue),
        RequiredMaxRepValue = VALUES(RequiredMaxRepValue),
        ProvidedItemCount = VALUES(ProvidedItemCount),
        SpecialFlags = VALUES(SpecialFlags);

    INSERT INTO centurionworld.quest_offer_reward
    (
        ID, Emote1, Emote2, Emote3, Emote4,
        EmoteDelay1, EmoteDelay2, EmoteDelay3, EmoteDelay4,
        RewardText, VerifiedBuild
    )
    SELECT
        c.entry,
        c.OfferRewardEmote1,
        c.OfferRewardEmote2,
        c.OfferRewardEmote3,
        c.OfferRewardEmote4,
        c.OfferRewardEmoteDelay1,
        c.OfferRewardEmoteDelay2,
        c.OfferRewardEmoteDelay3,
        c.OfferRewardEmoteDelay4,
        COALESCE(c.OfferRewardText, ''),
        0
    FROM classicmangos.quest_template c
    JOIN tmp_classic_quests_refresh t ON t.quest = c.entry
    ON DUPLICATE KEY UPDATE
        Emote1 = VALUES(Emote1),
        Emote2 = VALUES(Emote2),
        Emote3 = VALUES(Emote3),
        Emote4 = VALUES(Emote4),
        EmoteDelay1 = VALUES(EmoteDelay1),
        EmoteDelay2 = VALUES(EmoteDelay2),
        EmoteDelay3 = VALUES(EmoteDelay3),
        EmoteDelay4 = VALUES(EmoteDelay4),
        RewardText = VALUES(RewardText),
        VerifiedBuild = VALUES(VerifiedBuild);

    INSERT INTO centurionworld.quest_request_items
    (
        ID, EmoteOnComplete, EmoteOnIncomplete, CompletionText, VerifiedBuild
    )
    SELECT
        c.entry,
        c.CompleteEmote,
        c.IncompleteEmote,
        COALESCE(c.RequestItemsText, ''),
        0
    FROM classicmangos.quest_template c
    JOIN tmp_classic_quests_refresh t ON t.quest = c.entry
    ON DUPLICATE KEY UPDATE
        EmoteOnComplete = VALUES(EmoteOnComplete),
        EmoteOnIncomplete = VALUES(EmoteOnIncomplete),
        CompletionText = VALUES(CompletionText),
        VerifiedBuild = VALUES(VerifiedBuild);

    DELETE qs
    FROM centurionworld.creature_queststarter qs
    JOIN tmp_classic_quests_refresh t ON t.quest = qs.quest;

    INSERT IGNORE INTO centurionworld.creature_queststarter (id, quest)
    SELECT cqr.id, cqr.quest
    FROM classicmangos.creature_questrelation cqr
    JOIN tmp_classic_quests_refresh t ON t.quest = cqr.quest;

    DELETE qe
    FROM centurionworld.creature_questender qe
    JOIN tmp_classic_quests_refresh t ON t.quest = qe.quest;

    INSERT IGNORE INTO centurionworld.creature_questender (id, quest)
    SELECT cir.id, cir.quest
    FROM classicmangos.creature_involvedrelation cir
    JOIN tmp_classic_quests_refresh t ON t.quest = cir.quest;

    IF v_has_bplus_gqs > 0 AND v_has_classic_gqr > 0 THEN
        DELETE gqs
        FROM centurionworld.gameobject_queststarter gqs
        JOIN tmp_classic_quests_refresh t ON t.quest = gqs.quest;

        INSERT IGNORE INTO centurionworld.gameobject_queststarter (id, quest)
        SELECT gqr.id, gqr.quest
        FROM classicmangos.gameobject_questrelation gqr
        JOIN tmp_classic_quests_refresh t ON t.quest = gqr.quest;
    END IF;

    IF v_has_bplus_gqe > 0 AND v_has_classic_gir > 0 THEN
        DELETE gqe
        FROM centurionworld.gameobject_questender gqe
        JOIN tmp_classic_quests_refresh t ON t.quest = gqe.quest;

        INSERT IGNORE INTO centurionworld.gameobject_questender (id, quest)
        SELECT gir.id, gir.quest
        FROM classicmangos.gameobject_involvedrelation gir
        JOIN tmp_classic_quests_refresh t ON t.quest = gir.quest;
    END IF;

    DELETE d
    FROM centurionworld.disables d
    JOIN tmp_classic_quests_refresh t ON t.quest = d.entry
    WHERE d.sourceType = 1;

    SELECT COUNT(*) INTO v_still_disabled_count
    FROM centurionworld.disables d
    JOIN tmp_classic_quests_refresh t ON t.quest = d.entry
    WHERE d.sourceType = 1;

    DROP TEMPORARY TABLE IF EXISTS tmp_classic_quest_npcs_refresh;

    CREATE TEMPORARY TABLE tmp_classic_quest_npcs_refresh (
        id INT UNSIGNED NOT NULL PRIMARY KEY
    );

    INSERT IGNORE INTO tmp_classic_quest_npcs_refresh (id)
    SELECT qs.id
    FROM centurionworld.creature_queststarter qs
    JOIN tmp_classic_quests_refresh t ON t.quest = qs.quest;

    INSERT IGNORE INTO tmp_classic_quest_npcs_refresh (id)
    SELECT qe.id
    FROM centurionworld.creature_questender qe
    JOIN tmp_classic_quests_refresh t ON t.quest = qe.quest;

    UPDATE centurionworld.creature_template ct
    JOIN tmp_classic_quest_npcs_refresh qnpc ON qnpc.id = ct.entry
    SET ct.npcflag = ct.npcflag | 2
    WHERE (ct.npcflag & 2) = 0;

    COMMIT;

    SELECT
        v_suffix AS backup_suffix,
        v_refreshed_count AS refreshed_classic_quests_excluding_priest,
        v_still_disabled_count AS still_disabled_refreshed_quests;
END ;;
DELIMITER ;
/*!50003 SET sql_mode              = @saved_sql_mode */ ;
/*!50003 SET character_set_client  = @saved_cs_client */ ;
/*!50003 SET character_set_results = @saved_cs_results */ ;
/*!50003 SET collation_connection  = @saved_col_connection */ ;
/*!50003 DROP PROCEDURE IF EXISTS `refresh_creature_loot_from_classicmangos` */;
/*!50003 SET @saved_cs_client      = @@character_set_client */ ;
/*!50003 SET @saved_cs_results     = @@character_set_results */ ;
/*!50003 SET @saved_col_connection = @@collation_connection */ ;
/*!50003 SET character_set_client  = utf8mb4 */ ;
/*!50003 SET character_set_results = utf8mb4 */ ;
/*!50003 SET collation_connection  = utf8mb4_0900_ai_ci */ ;
/*!50003 SET @saved_sql_mode       = @@sql_mode */ ;
/*!50003 SET sql_mode              = 'ONLY_FULL_GROUP_BY,STRICT_TRANS_TABLES,NO_ZERO_IN_DATE,NO_ZERO_DATE,ERROR_FOR_DIVISION_BY_ZERO,NO_ENGINE_SUBSTITUTION' */ ;
DELIMITER ;;
CREATE PROCEDURE `refresh_creature_loot_from_classicmangos`()
BEGIN
    

    DROP TABLE IF EXISTS centurionworld.creature_loot_template_bak_before_classic_refresh;

    CREATE TABLE centurionworld.creature_loot_template_bak_before_classic_refresh
    LIKE centurionworld.creature_loot_template;

    INSERT INTO centurionworld.creature_loot_template_bak_before_classic_refresh
    SELECT *
    FROM centurionworld.creature_loot_template;


    DELETE b
    FROM centurionworld.creature_loot_template b
    JOIN (
        SELECT DISTINCT entry
        FROM classicmangos.creature_loot_template
    ) c ON c.entry = b.Entry;


    INSERT INTO centurionworld.creature_loot_template
    (
        Entry,
        Item,
        Reference,
        Chance,
        QuestRequired,
        LootMode,
        GroupId,
        MinCount,
        MaxCount,
        Comment
    )
    SELECT
        c.entry AS Entry,
        c.item AS Item,

        CASE
            WHEN c.mincountOrRef < 0 THEN ABS(c.mincountOrRef)
            ELSE 0
        END AS Reference,

        ABS(c.ChanceOrQuestChance) AS Chance,

        CASE
            WHEN c.ChanceOrQuestChance < 0 THEN 1
            ELSE 0
        END AS QuestRequired,

        1 AS LootMode,

        c.groupid AS GroupId,

        CASE
            WHEN c.mincountOrRef < 0 THEN 1
            ELSE c.mincountOrRef
        END AS MinCount,

        c.maxcount AS MaxCount,

        c.comments AS Comment
    FROM classicmangos.creature_loot_template c
    WHERE c.condition_id = 0
    ON DUPLICATE KEY UPDATE
        Reference = VALUES(Reference),
        Chance = VALUES(Chance),
        QuestRequired = VALUES(QuestRequired),
        LootMode = VALUES(LootMode),
        GroupId = VALUES(GroupId),
        MinCount = VALUES(MinCount),
        MaxCount = VALUES(MaxCount),
        Comment = VALUES(Comment);

END ;;
DELIMITER ;
/*!50003 SET sql_mode              = @saved_sql_mode */ ;
/*!50003 SET character_set_client  = @saved_cs_client */ ;
/*!50003 SET character_set_results = @saved_cs_results */ ;
/*!50003 SET collation_connection  = @saved_col_connection */ ;
/*!50003 DROP PROCEDURE IF EXISTS `spBarracksMakeItems` */;
/*!50003 SET @saved_cs_client      = @@character_set_client */ ;
/*!50003 SET @saved_cs_results     = @@character_set_results */ ;
/*!50003 SET @saved_col_connection = @@collation_connection */ ;
/*!50003 SET character_set_client  = utf8mb4 */ ;
/*!50003 SET character_set_results = utf8mb4 */ ;
/*!50003 SET collation_connection  = utf8mb4_0900_ai_ci */ ;
/*!50003 SET @saved_sql_mode       = @@sql_mode */ ;
/*!50003 SET sql_mode              = 'ONLY_FULL_GROUP_BY,STRICT_TRANS_TABLES,NO_ZERO_IN_DATE,NO_ZERO_DATE,ERROR_FOR_DIVISION_BY_ZERO,NO_ENGINE_SUBSTITUTION' */ ;
DELIMITER ;;
CREATE PROCEDURE `spBarracksMakeItems`()
BEGIN
select 'remaking item table';
	drop table if exists barracksworld.item_template;
    create table barracksworld.item_template like trinityworld.item_template;
    alter table barracksworld.item_template 
    ;
    alter table barracksworld.item_template modify scriptName char(64); 
    insert into barracksworld.item_template select wit.* from trinityworld.item_template wit;

select 'reverting item attributes to classic';
update barracksworld.item_template it join classicmangos.item_template vit on it.entry = vit.entry
set it.class = vit.class,it.subclass = vit.subclass,it.name = vit.name,it.displayid = vit.displayid,it.Quality = vit.Quality,it.Flags = vit.Flags,it.BuyCount = vit.BuyCount,
it.BuyPrice = vit.BuyPrice,it.SellPrice = vit.SellPrice,it.InventoryType = vit.InventoryType,it.AllowableClass = vit.AllowableClass,it.AllowableRace = vit.AllowableRace,
it.ItemLevel = vit.ItemLevel,it.RequiredLevel = vit.RequiredLevel,it.RequiredSkill = vit.RequiredSkill,it.RequiredSkillRank = vit.RequiredSkillRank,
it.requiredspell = vit.requiredspell,it.requiredhonorrank = vit.requiredhonorrank,it.RequiredCityRank = vit.RequiredCityRank,
it.RequiredReputationFaction = vit.RequiredReputationFaction,it.RequiredReputationRank = vit.RequiredReputationRank,it.maxcount = vit.maxcount,it.stackable = vit.stackable,
it.ContainerSlots = vit.ContainerSlots,it.stat_type1 = vit.stat_type1,it.stat_value1 = vit.stat_value1,it.stat_type2 = vit.stat_type2,it.stat_value2 = vit.stat_value2,
it.stat_type3 = vit.stat_type3,it.stat_value3 = vit.stat_value3,it.stat_type4 = vit.stat_type4,it.stat_value4 = vit.stat_value4,it.stat_type5 = vit.stat_type5,
it.stat_value5 = vit.stat_value5,it.stat_type6 = vit.stat_type6,it.stat_value6 = vit.stat_value6,it.stat_type7 = vit.stat_type7,it.stat_value7 = vit.stat_value7,
it.stat_type8 = vit.stat_type8,it.stat_value8 = vit.stat_value8,it.stat_type9 = vit.stat_type9,it.stat_value9 = vit.stat_value9,it.stat_type10 = vit.stat_type10,
it.stat_value10 = vit.stat_value10,it.dmg_min1 = vit.dmg_min1,it.dmg_max1 = vit.dmg_max1,it.dmg_type1 = vit.dmg_type1,it.dmg_min2 = vit.dmg_min2,it.dmg_max2 = vit.dmg_max2,
it.dmg_type2 = vit.dmg_type2,it.armor = vit.armor,it.holy_res = vit.holy_res,it.fire_res = vit.fire_res,it.nature_res = vit.nature_res,it.frost_res = vit.frost_res,
it.shadow_res = vit.shadow_res,it.arcane_res = vit.arcane_res,it.delay = vit.delay,it.ammo_type = vit.ammo_type,it.RangedModRange = vit.RangedModRange,it.spellid_1 = vit.spellid_1,
it.spelltrigger_1 = vit.spelltrigger_1,it.spellcharges_1 = vit.spellcharges_1,it.spellppmRate_1 = vit.spellppmRate_1,it.spellcooldown_1 = vit.spellcooldown_1,
it.spellcategory_1 = vit.spellcategory_1,it.spellcategorycooldown_1 = vit.spellcategorycooldown_1,it.spellid_2 = vit.spellid_2,it.spelltrigger_2 = vit.spelltrigger_2,
it.spellcharges_2 = vit.spellcharges_2,it.spellppmRate_2 = vit.spellppmRate_2,it.spellcooldown_2 = vit.spellcooldown_2,it.spellcategory_2 = vit.spellcategory_2,
it.spellcategorycooldown_2 = vit.spellcategorycooldown_2,it.spellid_3 = vit.spellid_3,it.spelltrigger_3 = vit.spelltrigger_3,it.spellcharges_3 = vit.spellcharges_3,
it.spellppmRate_3 = vit.spellppmRate_3,it.spellcooldown_3 = vit.spellcooldown_3,it.spellcategory_3 = vit.spellcategory_3,it.spellcategorycooldown_3 = vit.spellcategorycooldown_3,
it.spellid_4 = vit.spellid_4,it.spelltrigger_4 = vit.spelltrigger_4,it.spellcharges_4 = vit.spellcharges_4,it.spellppmRate_4 = vit.spellppmRate_4,
it.spellcooldown_4 = vit.spellcooldown_4,it.spellcategory_4 = vit.spellcategory_4,it.spellcategorycooldown_4 = vit.spellcategorycooldown_4,it.spellid_5 = vit.spellid_5,
it.spelltrigger_5 = vit.spelltrigger_5,it.spellcharges_5 = vit.spellcharges_5,it.spellppmRate_5 = vit.spellppmRate_5,it.spellcooldown_5 = vit.spellcooldown_5,
it.spellcategory_5 = vit.spellcategory_5,it.spellcategorycooldown_5 = vit.spellcategorycooldown_5,it.bonding = vit.bonding,it.description = vit.description,it.PageText = vit.PageText,
it.LanguageID = vit.LanguageID,it.PageMaterial = vit.PageMaterial,it.startquest = vit.startquest,it.lockid = vit.lockid,it.Material = vit.Material,it.sheath = vit.sheath,
it.RandomProperty = vit.RandomProperty,it.block = vit.block,it.itemset = vit.itemset,it.MaxDurability = vit.MaxDurability,it.area = vit.area,it.Map = vit.Map,
it.BagFamily = vit.BagFamily,it.ScriptName = vit.ScriptName,it.DisenchantID = vit.DisenchantID,it.FoodType = vit.FoodType,it.minMoneyLoot = vit.minMoneyLoot,
it.maxMoneyLoot = vit.maxMoneyLoot
where (it.requiredSkill != 762) 
;
    
select 'arena items';   
    
    update barracksworld.item_template
    set flags = flags + 2097152
    where entry in
    (
		3776,8079,8928,8985,9186,10922,14530,22895,
        19013, 19012, 9421, 
        8008, 
        9449 
    );
    
select 'done making items!';
END ;;
DELIMITER ;
/*!50003 SET sql_mode              = @saved_sql_mode */ ;
/*!50003 SET character_set_client  = @saved_cs_client */ ;
/*!50003 SET character_set_results = @saved_cs_results */ ;
/*!50003 SET collation_connection  = @saved_col_connection */ ;
/*!50003 DROP PROCEDURE IF EXISTS `spBarracksMakeTrainers` */;
/*!50003 SET @saved_cs_client      = @@character_set_client */ ;
/*!50003 SET @saved_cs_results     = @@character_set_results */ ;
/*!50003 SET @saved_col_connection = @@collation_connection */ ;
/*!50003 SET character_set_client  = utf8mb4 */ ;
/*!50003 SET character_set_results = utf8mb4 */ ;
/*!50003 SET collation_connection  = utf8mb4_0900_ai_ci */ ;
/*!50003 SET @saved_sql_mode       = @@sql_mode */ ;
/*!50003 SET sql_mode              = 'ONLY_FULL_GROUP_BY,STRICT_TRANS_TABLES,NO_ZERO_IN_DATE,NO_ZERO_DATE,ERROR_FOR_DIVISION_BY_ZERO,NO_ENGINE_SUBSTITUTION' */ ;
DELIMITER ;;
CREATE PROCEDURE `spBarracksMakeTrainers`()
BEGIN
  DECLARE old_safe_updates INT DEFAULT 0;

  
  DECLARE EXIT HANDLER FOR SQLEXCEPTION
  BEGIN
    ROLLBACK;
    SET SQL_SAFE_UPDATES = old_safe_updates;
    SELECT 'ERROR: transaction rolled back' AS msg;
  END;

  START TRANSACTION;

  
  SET old_safe_updates = @@SQL_SAFE_UPDATES;
  SET SQL_SAFE_UPDATES = 0;

  
  SET SESSION group_concat_max_len = 1024*1024;

  
  DROP TEMPORARY TABLE IF EXISTS tmp_mapped_trainers;
  CREATE TEMPORARY TABLE tmp_mapped_trainers ENGINE=InnoDB AS
  SELECT DISTINCT cdt.TrainerId
  FROM barracksworld.creature_default_trainer cdt;

  
  DROP TEMPORARY TABLE IF EXISTS tmp_trainer_signatures;
  CREATE TEMPORARY TABLE tmp_trainer_signatures (
    TrainerId INT UNSIGNED NOT NULL PRIMARY KEY,
    Sig       BINARY(16)   NOT NULL
  ) ENGINE=InnoDB;

  INSERT INTO tmp_trainer_signatures (TrainerId, Sig)
  SELECT
    ts.TrainerId,
    UNHEX(MD5(
      GROUP_CONCAT(
        CONCAT_WS(':',
          ts.SpellId,
          ts.ReqLevel,
          ts.ReqSkillLine,
          ts.ReqSkillRank,
          ts.ReqAbility1,
          ts.ReqAbility2,
          ts.ReqAbility3,
          ts.MoneyCost
        )
        ORDER BY ts.SpellId, ts.ReqLevel, ts.ReqSkillLine, ts.ReqSkillRank,
                 ts.ReqAbility1, ts.ReqAbility2, ts.ReqAbility3, ts.MoneyCost
        SEPARATOR ';'
      )
    )) AS Sig
  FROM barracksworld.trainer_spell ts
  JOIN tmp_mapped_trainers mt ON mt.TrainerId = ts.TrainerId
  GROUP BY ts.TrainerId;

  
  DROP TEMPORARY TABLE IF EXISTS tmp_unique_sets;
  CREATE TEMPORARY TABLE tmp_unique_sets (
    CanonicalId  INT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    Sig          BINARY(16)   NOT NULL,
    RepTrainerId INT UNSIGNED NOT NULL,
    Type         TINYINT UNSIGNED NOT NULL,
    Requirement  INT UNSIGNED NOT NULL
  ) ENGINE=InnoDB;

  
  ALTER TABLE tmp_unique_sets AUTO_INCREMENT = 500000;

  INSERT INTO tmp_unique_sets (Sig, RepTrainerId, Type, Requirement)
  SELECT
    s.Sig,
    MIN(s.TrainerId)            AS RepTrainerId,
    COALESCE(MIN(tr.Type), 0)   AS Type,
    COALESCE(MIN(tr.Requirement), 0) AS Requirement
  FROM tmp_trainer_signatures s
  JOIN barracksworld.trainer tr ON tr.Id = s.TrainerId
  GROUP BY s.Sig;

  
  DROP TEMPORARY TABLE IF EXISTS tmp_old_to_new;
  CREATE TEMPORARY TABLE tmp_old_to_new (
    OldTrainerId INT UNSIGNED NOT NULL PRIMARY KEY,
    CanonicalId  INT UNSIGNED NOT NULL
  ) ENGINE=InnoDB;

  INSERT INTO tmp_old_to_new (OldTrainerId, CanonicalId)
  SELECT s.TrainerId, u.CanonicalId
  FROM tmp_trainer_signatures s
  JOIN tmp_unique_sets u ON u.Sig = s.Sig;

  
  INSERT INTO barracksworld.trainer (Id, Type, Requirement, Greeting, VerifiedBuild)
  SELECT
    u.CanonicalId,
    u.Type,
    u.Requirement,
    NULL,
    0
  FROM tmp_unique_sets u
  ON DUPLICATE KEY UPDATE
    Type         = VALUES(Type),
    Requirement  = VALUES(Requirement),
    Greeting     = VALUES(Greeting),
    VerifiedBuild= VALUES(VerifiedBuild);

  
  DELETE ts
  FROM barracksworld.trainer_spell ts
  JOIN tmp_unique_sets u ON u.CanonicalId = ts.TrainerId;

  INSERT INTO barracksworld.trainer_spell
    (TrainerId, SpellId, MoneyCost, ReqSkillLine, ReqSkillRank,
     ReqAbility1, ReqAbility2, ReqAbility3, ReqLevel, VerifiedBuild)
  SELECT
    u.CanonicalId,
    rs.SpellId, rs.MoneyCost, rs.ReqSkillLine, rs.ReqSkillRank,
    rs.ReqAbility1, rs.ReqAbility2, rs.ReqAbility3,
    rs.ReqLevel,
    0
  FROM tmp_unique_sets u
  JOIN barracksworld.trainer_spell rs ON rs.TrainerId = u.RepTrainerId;

  
  UPDATE barracksworld.creature_default_trainer cdt
  JOIN tmp_old_to_new m ON m.OldTrainerId = cdt.TrainerId
  SET cdt.TrainerId = m.CanonicalId;

  
  DELETE ts
  FROM barracksworld.trainer_spell ts
  LEFT JOIN barracksworld.creature_default_trainer cdt
    ON cdt.TrainerId = ts.TrainerId
  WHERE cdt.TrainerId IS NULL;

  
  DELETE tr
  FROM barracksworld.trainer tr
  LEFT JOIN barracksworld.creature_default_trainer cdt
    ON cdt.TrainerId = tr.Id
  WHERE cdt.TrainerId IS NULL;

  
  UPDATE barracksworld.creature_template ct
  JOIN barracksworld.creature_default_trainer cdt
    ON cdt.CreatureId = ct.entry
  SET ct.npcflag = ct.npcflag | 0x20;

  COMMIT;

  
  SET SQL_SAFE_UPDATES = old_safe_updates;

  SELECT
    (SELECT COUNT(*) FROM barracksworld.trainer)                         AS trainer_rows,
    (SELECT COUNT(*) FROM barracksworld.trainer_spell)                   AS trainer_spell_rows,
    (SELECT COUNT(*) FROM barracksworld.creature_default_trainer)        AS mappings;

END ;;
DELIMITER ;
/*!50003 SET sql_mode              = @saved_sql_mode */ ;
/*!50003 SET character_set_client  = @saved_cs_client */ ;
/*!50003 SET character_set_results = @saved_cs_results */ ;
/*!50003 SET collation_connection  = @saved_col_connection */ ;
/*!50003 DROP PROCEDURE IF EXISTS `spBarracksMakeTrainers_FromClassicMangos` */;
/*!50003 SET @saved_cs_client      = @@character_set_client */ ;
/*!50003 SET @saved_cs_results     = @@character_set_results */ ;
/*!50003 SET @saved_col_connection = @@collation_connection */ ;
/*!50003 SET character_set_client  = utf8mb4 */ ;
/*!50003 SET character_set_results = utf8mb4 */ ;
/*!50003 SET collation_connection  = utf8mb4_0900_ai_ci */ ;
/*!50003 SET @saved_sql_mode       = @@sql_mode */ ;
/*!50003 SET sql_mode              = 'ONLY_FULL_GROUP_BY,STRICT_TRANS_TABLES,NO_ZERO_IN_DATE,NO_ZERO_DATE,ERROR_FOR_DIVISION_BY_ZERO,NO_ENGINE_SUBSTITUTION' */ ;
DELIMITER ;;
CREATE PROCEDURE `spBarracksMakeTrainers_FromClassicMangos`()
BEGIN
  DECLARE old_safe_updates INT DEFAULT 0;

  DECLARE EXIT HANDLER FOR SQLEXCEPTION
  BEGIN
    ROLLBACK;
    SET SQL_SAFE_UPDATES = old_safe_updates;
    SELECT 'ERROR: transaction rolled back' AS msg;
  END;

  START TRANSACTION;

  SET old_safe_updates = @@SQL_SAFE_UPDATES;
  SET SQL_SAFE_UPDATES = 0;

  SET SESSION group_concat_max_len = 1024*1024;

  
  DROP TEMPORARY TABLE IF EXISTS tmp_mapped_creatures;
  CREATE TEMPORARY TABLE tmp_mapped_creatures ENGINE=InnoDB AS
  SELECT DISTINCT cdt.CreatureId, cdt.TrainerId AS OldTrainerId
  FROM barracksworld.creature_default_trainer cdt;

  DROP TEMPORARY TABLE IF EXISTS tmp_creatures_with_classic;
  CREATE TEMPORARY TABLE tmp_creatures_with_classic ENGINE=InnoDB AS
  SELECT DISTINCT mc.CreatureId, mc.OldTrainerId
  FROM tmp_mapped_creatures mc
  JOIN classicmangos.npc_trainer nt ON nt.entry = mc.CreatureId;

  
  DROP TEMPORARY TABLE IF EXISTS tmp_creature_signatures;
  CREATE TEMPORARY TABLE tmp_creature_signatures (
    CreatureId   MEDIUMINT UNSIGNED NOT NULL PRIMARY KEY,
    OldTrainerId INT UNSIGNED        NOT NULL,
    Sig          BINARY(16)          NOT NULL
  ) ENGINE=InnoDB;

  INSERT INTO tmp_creature_signatures (CreatureId, OldTrainerId, Sig)
  SELECT
    c.CreatureId,
    c.OldTrainerId,
    UNHEX(MD5(
      GROUP_CONCAT(
        CONCAT_WS(':',
          nt.spell,
          nt.reqlevel,
          nt.reqskill,
          nt.reqskillvalue,
          COALESCE(nt.ReqAbility1, 0),
          COALESCE(nt.ReqAbility2, 0),
          COALESCE(nt.ReqAbility3, 0),
          nt.spellcost
          
        )
        ORDER BY
          nt.spell, nt.reqlevel, nt.reqskill, nt.reqskillvalue,
          COALESCE(nt.ReqAbility1, 0), COALESCE(nt.ReqAbility2, 0), COALESCE(nt.ReqAbility3, 0),
          nt.spellcost
        SEPARATOR ';'
      )
    )) AS Sig
  FROM tmp_creatures_with_classic c
  JOIN classicmangos.npc_trainer nt
    ON nt.entry = c.CreatureId
  GROUP BY c.CreatureId, c.OldTrainerId;

  
  DROP TEMPORARY TABLE IF EXISTS tmp_unique_sets;
  CREATE TEMPORARY TABLE tmp_unique_sets (
    CanonicalId    INT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    Sig            BINARY(16)   NOT NULL,
    RepCreatureId  MEDIUMINT UNSIGNED NOT NULL,
    RepOldTrainerId INT UNSIGNED NOT NULL,
    Type           TINYINT UNSIGNED NOT NULL,
    Requirement    INT UNSIGNED NOT NULL
  ) ENGINE=InnoDB;

  ALTER TABLE tmp_unique_sets AUTO_INCREMENT = 500000;

  INSERT INTO tmp_unique_sets (Sig, RepCreatureId, RepOldTrainerId, Type, Requirement)
  SELECT
    s.Sig,
    MIN(s.CreatureId) AS RepCreatureId,
    MIN(s.OldTrainerId) AS RepOldTrainerId,
    COALESCE(MIN(t.Type), 0) AS Type,
    COALESCE(MIN(t.Requirement), 0) AS Requirement
  FROM tmp_creature_signatures s
  LEFT JOIN barracksworld.trainer t
    ON t.Id = s.OldTrainerId
  GROUP BY s.Sig;

  
  DROP TEMPORARY TABLE IF EXISTS tmp_creature_to_new;
  CREATE TEMPORARY TABLE tmp_creature_to_new (
    CreatureId  MEDIUMINT UNSIGNED NOT NULL PRIMARY KEY,
    CanonicalId INT UNSIGNED       NOT NULL
  ) ENGINE=InnoDB;

  INSERT INTO tmp_creature_to_new (CreatureId, CanonicalId)
  SELECT s.CreatureId, u.CanonicalId
  FROM tmp_creature_signatures s
  JOIN tmp_unique_sets u ON u.Sig = s.Sig;

  
  INSERT INTO barracksworld.trainer (Id, Type, Requirement, Greeting, VerifiedBuild)
  SELECT
    u.CanonicalId,
    u.Type,
    u.Requirement,
    NULL,
    0
  FROM tmp_unique_sets u
  ON DUPLICATE KEY UPDATE
    Type          = VALUES(Type),
    Requirement   = VALUES(Requirement),
    Greeting      = VALUES(Greeting),
    VerifiedBuild = VALUES(VerifiedBuild);

  
  DELETE ts
  FROM barracksworld.trainer_spell ts
  JOIN tmp_unique_sets u ON u.CanonicalId = ts.TrainerId;

  INSERT INTO barracksworld.trainer_spell
    (TrainerId, SpellId, MoneyCost, ReqSkillLine, ReqSkillRank,
     ReqAbility1, ReqAbility2, ReqAbility3, ReqLevel, VerifiedBuild)
  SELECT DISTINCT
    u.CanonicalId,
    nt.spell,
    nt.spellcost,
    nt.reqskill,
    nt.reqskillvalue,
    COALESCE(nt.ReqAbility1, 0),
    COALESCE(nt.ReqAbility2, 0),
    COALESCE(nt.ReqAbility3, 0),
    nt.reqlevel,
    0
  FROM tmp_unique_sets u
  JOIN classicmangos.npc_trainer nt
    ON nt.entry = u.RepCreatureId;

  
  UPDATE barracksworld.creature_default_trainer cdt
  JOIN tmp_creature_to_new m ON m.CreatureId = cdt.CreatureId
  SET cdt.TrainerId = m.CanonicalId;

  
  DELETE ts
  FROM barracksworld.trainer_spell ts
  LEFT JOIN barracksworld.creature_default_trainer cdt
    ON cdt.TrainerId = ts.TrainerId
  WHERE cdt.TrainerId IS NULL;

  
  DELETE tr
  FROM barracksworld.trainer tr
  LEFT JOIN barracksworld.creature_default_trainer cdt
    ON cdt.TrainerId = tr.Id
  WHERE cdt.TrainerId IS NULL;

  
  UPDATE barracksworld.creature_template ct
  JOIN barracksworld.creature_default_trainer cdt
    ON cdt.CreatureId = ct.entry
  SET ct.npcflag = ct.npcflag | 0x20;

  COMMIT;

  SET SQL_SAFE_UPDATES = old_safe_updates;

  
  SELECT
    (SELECT COUNT(*) FROM barracksworld.trainer) AS trainer_rows,
    (SELECT COUNT(*) FROM barracksworld.trainer_spell) AS trainer_spell_rows,
    (SELECT COUNT(*) FROM barracksworld.creature_default_trainer) AS mappings,
    (SELECT COUNT(*)
     FROM barracksworld.creature_default_trainer cdt
     JOIN barracksworld.trainer_spell ts ON ts.TrainerId = cdt.TrainerId
     LEFT JOIN classicmangos.npc_trainer nt
       ON nt.entry = cdt.CreatureId
      AND nt.spell = ts.SpellId
     WHERE nt.spell IS NULL
    ) AS spells_not_in_classic_by_spellid;

END ;;
DELIMITER ;
/*!50003 SET sql_mode              = @saved_sql_mode */ ;
/*!50003 SET character_set_client  = @saved_cs_client */ ;
/*!50003 SET character_set_results = @saved_cs_results */ ;
/*!50003 SET collation_connection  = @saved_col_connection */ ;
/*!50003 DROP PROCEDURE IF EXISTS `spBarracksResetQuests` */;
/*!50003 SET @saved_cs_client      = @@character_set_client */ ;
/*!50003 SET @saved_cs_results     = @@character_set_results */ ;
/*!50003 SET @saved_col_connection = @@collation_connection */ ;
/*!50003 SET character_set_client  = utf8mb4 */ ;
/*!50003 SET character_set_results = utf8mb4 */ ;
/*!50003 SET collation_connection  = utf8mb4_0900_ai_ci */ ;
/*!50003 SET @saved_sql_mode       = @@sql_mode */ ;
/*!50003 SET sql_mode              = 'ONLY_FULL_GROUP_BY,STRICT_TRANS_TABLES,NO_ZERO_IN_DATE,NO_ZERO_DATE,ERROR_FOR_DIVISION_BY_ZERO,NO_ENGINE_SUBSTITUTION' */ ;
DELIMITER ;;
CREATE PROCEDURE `spBarracksResetQuests`()
BEGIN
    DECLARE v_old_fk INT DEFAULT @@FOREIGN_KEY_CHECKS;
    SET FOREIGN_KEY_CHECKS = 0;

    
    DROP TABLE IF EXISTS barracksworld.quest_template;
    CREATE TABLE barracksworld.quest_template LIKE trinityworld.quest_template;
    INSERT INTO barracksworld.quest_template
    SELECT * FROM trinityworld.quest_template;

    DROP TABLE IF EXISTS barracksworld.quest_poi;
    CREATE TABLE barracksworld.quest_poi LIKE trinityworld.quest_poi;
    INSERT INTO barracksworld.quest_poi
    SELECT * FROM trinityworld.quest_poi;

    DROP TABLE IF EXISTS barracksworld.quest_poi_points;
    CREATE TABLE barracksworld.quest_poi_points LIKE trinityworld.quest_poi_points;
    INSERT INTO barracksworld.quest_poi_points
    SELECT * FROM trinityworld.quest_poi_points;

    
    DROP TEMPORARY TABLE IF EXISTS tmp_classic_quest_ids;
    CREATE TEMPORARY TABLE tmp_classic_quest_ids
    SELECT DISTINCT qt.entry AS questId
    FROM classicmangos.quest_template qt;

    

    
    DELETE FROM barracksworld.quest_poi
    WHERE QuestID IN (SELECT questId FROM tmp_classic_quest_ids);

    INSERT INTO barracksworld.quest_poi
    (QuestID, id, ObjectiveIndex, MapID, WorldMapAreaId, Floor, Priority, Flags, VerifiedBuild)
    SELECT
        cp.questId                      AS QuestID,
        cp.poiId                        AS id,
        cp.objIndex                     AS ObjectiveIndex,
        cp.mapId                        AS MapID,
        cp.mapAreaId                    AS WorldMapAreaId,
        cp.floorId                      AS Floor,
        cp.unk3                         AS Priority,      
        cp.unk4                         AS Flags,         
        NULL                            AS VerifiedBuild
    FROM classicmangos.quest_poi cp
    INNER JOIN tmp_classic_quest_ids q ON q.questId = cp.questId;

    
    DELETE FROM barracksworld.quest_poi_points
    WHERE QuestID IN (SELECT questId FROM tmp_classic_quest_ids);

    
    DROP TEMPORARY TABLE IF EXISTS tmp_cpp;
    CREATE TEMPORARY TABLE tmp_cpp (
        questId INT NOT NULL,
        poiId   INT NOT NULL,
        x       INT NOT NULL,
        y       INT NOT NULL
    ) ENGINE=InnoDB;

    INSERT INTO tmp_cpp (questId, poiId, x, y)
    SELECT qpp.questId, qpp.poiId, qpp.x, qpp.y
    FROM classicmangos.quest_poi_points qpp
    INNER JOIN tmp_classic_quest_ids q ON q.questId = qpp.questId;

    
    DROP TEMPORARY TABLE IF EXISTS tmp_cpp_ord;
    CREATE TEMPORARY TABLE tmp_cpp_ord ENGINE=InnoDB AS
    SELECT
        questId, poiId, x, y,
        ROW_NUMBER() OVER (
          PARTITION BY questId, poiId
          ORDER BY x ASC, y ASC, questId ASC, poiId ASC
        ) - 1 AS idx2   
    FROM tmp_cpp;

    INSERT INTO barracksworld.quest_poi_points
    (QuestID, Idx1, Idx2, X, Y, VerifiedBuild)
    SELECT
        questId            AS QuestID,
        poiId              AS Idx1,
        idx2               AS Idx2,
        x                  AS X,
        y                  AS Y,
        NULL               AS VerifiedBuild
    FROM tmp_cpp_ord;

    
    DROP TEMPORARY TABLE IF EXISTS tmp_cpp_ord;
    DROP TEMPORARY TABLE IF EXISTS tmp_cpp;
    DROP TEMPORARY TABLE IF EXISTS tmp_classic_quest_ids;

    SET FOREIGN_KEY_CHECKS = v_old_fk;
END ;;
DELIMITER ;
/*!50003 SET sql_mode              = @saved_sql_mode */ ;
/*!50003 SET character_set_client  = @saved_cs_client */ ;
/*!50003 SET character_set_results = @saved_cs_results */ ;
/*!50003 SET collation_connection  = @saved_col_connection */ ;
/*!40103 SET TIME_ZONE=@OLD_TIME_ZONE */;

/*!40101 SET SQL_MODE=@OLD_SQL_MODE */;
/*!40014 SET FOREIGN_KEY_CHECKS=@OLD_FOREIGN_KEY_CHECKS */;
/*!40014 SET UNIQUE_CHECKS=@OLD_UNIQUE_CHECKS */;
/*!40101 SET CHARACTER_SET_CLIENT=@OLD_CHARACTER_SET_CLIENT */;
/*!40101 SET CHARACTER_SET_RESULTS=@OLD_CHARACTER_SET_RESULTS */;
/*!40101 SET COLLATION_CONNECTION=@OLD_COLLATION_CONNECTION */;
/*!40111 SET SQL_NOTES=@OLD_SQL_NOTES */;

