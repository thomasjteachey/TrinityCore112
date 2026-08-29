-- Quest reward items: give the ungated ones a required level.
--
-- A great many quest rewards ship with RequiredLevel = 0, so nothing stops a
-- level 12 character from wearing a level 45 quest reward handed to them (or
-- bought off the auction house, since nothing binds on this realm). Every
-- quest-awarded item that carries NO requirement of its own is pinned to
-- three levels below the quest that grants it.
--
-- Items already carrying a RequiredLevel are left exactly as they are.
--
-- An item granted by several quests takes the LOWEST of their levels, so a
-- reward shared between a low and a high quest never locks out the players
-- the low quest was written for. Quests with QuestLevel <= 0 (scaling and
-- event quests) carry no meaningful level and are skipped, and the result is
-- floored at 0 - the "no requirement" value - for the earliest quests.
--
-- Only items a character equips or uses are touched. Quest items (12), keys
-- (13), currency and tokens (15), recipes (9), money (10) and trade goods (7)
-- are left alone: a level requirement means nothing on them and would block
-- the quest turn-ins and instance doors they exist for.

UPDATE item_template it
JOIN
(
    SELECT itemId, MIN(QuestLevel) AS questLevel
    FROM
    (
        SELECT RewardItem1          AS itemId, QuestLevel FROM quest_template WHERE RewardItem1          > 0 AND QuestLevel > 0
        UNION ALL
        SELECT RewardItem2          AS itemId, QuestLevel FROM quest_template WHERE RewardItem2          > 0 AND QuestLevel > 0
        UNION ALL
        SELECT RewardItem3          AS itemId, QuestLevel FROM quest_template WHERE RewardItem3          > 0 AND QuestLevel > 0
        UNION ALL
        SELECT RewardItem4          AS itemId, QuestLevel FROM quest_template WHERE RewardItem4          > 0 AND QuestLevel > 0
        UNION ALL
        SELECT RewardChoiceItemID1  AS itemId, QuestLevel FROM quest_template WHERE RewardChoiceItemID1  > 0 AND QuestLevel > 0
        UNION ALL
        SELECT RewardChoiceItemID2  AS itemId, QuestLevel FROM quest_template WHERE RewardChoiceItemID2  > 0 AND QuestLevel > 0
        UNION ALL
        SELECT RewardChoiceItemID3  AS itemId, QuestLevel FROM quest_template WHERE RewardChoiceItemID3  > 0 AND QuestLevel > 0
        UNION ALL
        SELECT RewardChoiceItemID4  AS itemId, QuestLevel FROM quest_template WHERE RewardChoiceItemID4  > 0 AND QuestLevel > 0
        UNION ALL
        SELECT RewardChoiceItemID5  AS itemId, QuestLevel FROM quest_template WHERE RewardChoiceItemID5  > 0 AND QuestLevel > 0
        UNION ALL
        SELECT RewardChoiceItemID6  AS itemId, QuestLevel FROM quest_template WHERE RewardChoiceItemID6  > 0 AND QuestLevel > 0
    ) AS questRewards
    GROUP BY itemId
) AS q ON q.itemId = it.entry
SET it.RequiredLevel = GREATEST(q.questLevel - 3, 0)
WHERE it.RequiredLevel = 0
  AND it.class NOT IN (7, 9, 10, 12, 13, 15);
