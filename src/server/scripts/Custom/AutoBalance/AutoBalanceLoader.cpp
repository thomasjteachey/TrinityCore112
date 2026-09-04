#include "AutoBalance.h"
#include "AutoBalance/AutoBalanceCreature.h"
#include "Creature.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include <cmath>

namespace
{
    class DefaultAutoBalanceModule final : public ABModuleScript
    {
    public:
        DefaultAutoBalanceModule()
            : ABModuleScript("DefaultAutoBalanceModule")
        {
        }

        void OnMessage(AutoBalance::Message const& /*message*/) override { }
    };

    class AutoBalanceCreatureUpdateScript final : public AllCreatureScript
    {
    public:
        AutoBalanceCreatureUpdateScript()
            : AllCreatureScript("AutoBalanceCreatureUpdateScript")
        {
        }

        void OnAllCreatureUpdate(Creature* creature, uint32 /*diff*/) override
        {
            AutoBalance::ScaleCreature(creature);
        }
    };

    // Rewards follow the difficulty. AutoBalance scaled a dungeon's health and
    // damage to the size of the group but always paid a full group's experience
    // and gold, because the two reward modifiers it computes had no consumer -
    // only the debug command ever read them.
    //
    // Both multipliers come back empty unless the realm turns
    // AutoBalance.RewardScaling.XP / .Money on, so this changes nothing until
    // asked for.
    class AutoBalanceRewardScript final : public PlayerScript
    {
    public:
        AutoBalanceRewardScript()
            : PlayerScript("AutoBalanceRewardScript")
        {
        }

        void OnGiveXP(Player* /*player*/, uint32& amount, Unit* victim) override
        {
            if (!amount || !victim)
                return;

            if (Optional<float> const multiplier = AutoBalance::GetExperienceMultiplier(victim))
                amount = uint32(std::lround(double(amount) * double(*multiplier)));
        }

        void OnMoneyChanged(Player* player, int32& amount) override
        {
            // This hook sees every coin a player gains, so it has to be narrowed
            // to loot: vendor sales, quest rewards, mail and the auction house
            // must stay at face value. A loot GUID is only set while a corpse or
            // container is open, and resolving it to a creature drops chests and
            // fishing nodes, which AutoBalance never scaled in the first place.
            if (amount <= 0 || !player)
                return;

            ObjectGuid const lootGuid = player->GetLootGUID();
            if (lootGuid.IsEmpty() || !lootGuid.IsCreatureOrVehicle())
                return;

            Creature const* looted = ObjectAccessor::GetCreature(*player, lootGuid);
            if (!looted)
                return;

            if (Optional<float> const multiplier = AutoBalance::GetMoneyMultiplier(looted))
                amount = int32(std::lround(double(amount) * double(*multiplier)));
        }
    };
}

extern void AddAutoBalanceCommandScripts();

void AddAutoBalanceScripts()
{
    new DefaultAutoBalanceModule();
    new AutoBalanceCreatureUpdateScript();
    new AutoBalanceRewardScript();
    AddAutoBalanceCommandScripts();
}
