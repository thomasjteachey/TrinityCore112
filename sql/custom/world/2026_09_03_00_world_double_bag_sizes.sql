-- Double every bag's capacity on Barracks Plus.
--
-- ContainerSlots is the number of slots a container item offers. It is server
-- side only: the client learns a bag's size from CONTAINER_FIELD_NUM_SLOTS on
-- the object, not from any DBC, so nothing needs republishing to the launcher.
--
-- CAPPED AT 36, which is not a style choice - Bag.h defines
--
--     #define MAX_BAG_SIZE 36                 // 2.0.12
--
-- and Bag itself allocates a fixed m_bagslot[MAX_BAG_SIZE]. A template claiming
-- more than 36 would be writing past the end of that array, so the cap is a
-- hard engine limit rather than a balance decision.
--
-- The consequence is worth knowing: only bags of 18 slots or fewer actually
-- double. Everything from 18 upward lands on 36, so the current spread of
-- 18/20/22/24/28/30/32 collapses into a single size. The top end of bag
-- progression effectively disappears - a 6-slot starter bag doubling to 12 is
-- a real upgrade, while a 20 and a 32 become indistinguishable.
--
-- Applied to bplusworld only, as asked.
--
-- Re-runnable in the sense that it will not exceed the cap, but NOT idempotent:
-- running it twice doubles twice. The backup table below is the way back.

CREATE TABLE IF NOT EXISTS `item_template_bak_bagsize_20260903` AS
SELECT entry, ContainerSlots FROM `item_template` WHERE class = 1;

UPDATE `item_template`
   SET ContainerSlots = LEAST(ContainerSlots * 2, 36)
 WHERE class = 1
   AND ContainerSlots > 0;

-- To undo:
--   UPDATE item_template t
--     JOIN item_template_bak_bagsize_20260903 b ON b.entry = t.entry
--      SET t.ContainerSlots = b.ContainerSlots;
