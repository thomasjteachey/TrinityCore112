#include "Config.h"
#include "Player.h"
#include "Position.h"
#include "ScriptMgr.h"
#include "World.h"
#include "WorldSession.h"
#include <string>

namespace
{
uint32 constexpr StockadesMapId = 34;
uint32 constexpr StockadesFailMapId = 0;
Position const StockadesFailTeleport = { -8762.38f, 848.01f, 86.3139f, 0.0f };

bool IsFeatureEnabled()
{
    return sConfigMgr->GetOption<bool>("PvPvEDungeon.Stockades.Enable", false);
}

bool ShouldHandle(Player const* player)
{
    return IsFeatureEnabled() && player && !player->IsGameMaster();
}

bool IsInsideStockades(Player const* player)
{
    return player && player->GetMapId() == StockadesMapId;
}

std::string GetEliminationMessage()
{
    return sConfigMgr->GetOption<std::string>(
        "PvPvEDungeon.Stockades.EliminationMessage",
        "The Stockades PvPvE dungeon ended while you were offline; you have been eliminated.");
}

void TeleportPlayerOut(Player* player)
{
    if (!player)
        return;

    if (!player->IsBeingTeleported())
        player->TeleportTo(StockadesFailMapId, StockadesFailTeleport.GetPositionX(), StockadesFailTeleport.GetPositionY(),
            StockadesFailTeleport.GetPositionZ(), StockadesFailTeleport.GetOrientation());
}

void NotifyElimination(Player* player)
{
    if (WorldSession* session = player->GetSession())
    {
        std::string const message = GetEliminationMessage();
        if (!message.empty())
            session->SendNotification("%s", message.c_str());
    }
}

void HandleUnsafeLogout(Player* player)
{
    if (!player)
        return;

    player->RemoveAtLoginFlag(AT_LOGIN_PVPVE_STOCKADES_SAFE_RESUME);
    NotifyElimination(player);
    TeleportPlayerOut(player);
}
}

class StockadesPvPvEPlayerScript : public PlayerScript
{
public:
    StockadesPvPvEPlayerScript() : PlayerScript("StockadesPvPvEPlayerScript") { }

    void OnLogin(Player* player, bool /*firstLogin*/) override
    {
        if (!ShouldHandle(player))
        {
            if (player && player->HasAtLoginFlag(AT_LOGIN_PVPVE_STOCKADES_SAFE_RESUME))
                player->RemoveAtLoginFlag(AT_LOGIN_PVPVE_STOCKADES_SAFE_RESUME);
            return;
        }

        if (!IsInsideStockades(player))
        {
            if (player->HasAtLoginFlag(AT_LOGIN_PVPVE_STOCKADES_SAFE_RESUME))
                player->RemoveAtLoginFlag(AT_LOGIN_PVPVE_STOCKADES_SAFE_RESUME);
            return;
        }

        if (player->HasAtLoginFlag(AT_LOGIN_PVPVE_STOCKADES_SAFE_RESUME))
        {
            player->RemoveAtLoginFlag(AT_LOGIN_PVPVE_STOCKADES_SAFE_RESUME);
            return;
        }

        HandleUnsafeLogout(player);
    }

    void OnLogout(Player* player) override
    {
        if (!ShouldHandle(player))
        {
            if (player && player->HasAtLoginFlag(AT_LOGIN_PVPVE_STOCKADES_SAFE_RESUME))
                player->RemoveAtLoginFlag(AT_LOGIN_PVPVE_STOCKADES_SAFE_RESUME);
            return;
        }

        if (IsInsideStockades(player))
        {
            if (sWorld->IsShuttingDown())
                player->RemoveAtLoginFlag(AT_LOGIN_PVPVE_STOCKADES_SAFE_RESUME);
            else
                player->SetAtLoginFlag(AT_LOGIN_PVPVE_STOCKADES_SAFE_RESUME);
            return;
        }

        if (player->HasAtLoginFlag(AT_LOGIN_PVPVE_STOCKADES_SAFE_RESUME))
            player->RemoveAtLoginFlag(AT_LOGIN_PVPVE_STOCKADES_SAFE_RESUME);
    }
};

void AddSC_stockades_pvpve()
{
    new StockadesPvPvEPlayerScript();
}
