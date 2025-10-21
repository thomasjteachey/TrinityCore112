#include "ABCommandScript.h"

#include "AutoBalanceMgr.h"
#include "Chat.h"
#include "Map.h"
#include "Player.h"
#include "RBAC.h"
#include "WorldSession.h"

using namespace Trinity::ChatCommands;

AutoBalanceCommandScript::AutoBalanceCommandScript() : CommandScript("AutoBalanceCommandScript") { }

ChatCommandTable AutoBalanceCommandScript::GetCommands() const
{
    static ChatCommandTable autoBalanceCommandTable =
    {
        { "setoffset", HandleSetOffsetCommand, rbac::RBAC_PERM_COMMAND_MODIFY, Console::Yes },
        { "getoffset", HandleGetOffsetCommand, rbac::RBAC_PERM_COMMAND_MODIFY, Console::Yes },
        { "mapstats",  HandleMapStatsCommand,  rbac::RBAC_PERM_COMMAND_DEBUG,  Console::No  }
    };

    static ChatCommandTable commandTable =
    {
        { "autobalance", autoBalanceCommandTable }
    };

    return commandTable;
}

bool AutoBalanceCommandScript::HandleSetOffsetCommand(ChatHandler* handler, int32 offset)
{
    AutoBalance::SetPlayerDifficultyOffset(offset);

    if (WorldSession* session = handler->GetSession())
        if (Player* player = session->GetPlayer())
            AutoBalance::NotifyPlayerEvent(player->GetMap());

    handler->PSendSysMessage("AutoBalance player difficulty offset set to %i.", offset);
    return true;
}

bool AutoBalanceCommandScript::HandleGetOffsetCommand(ChatHandler* handler)
{
    handler->PSendSysMessage("AutoBalance player difficulty offset is %i.", AutoBalance::GetPlayerDifficultyOffset());
    return true;
}

bool AutoBalanceCommandScript::HandleMapStatsCommand(ChatHandler* handler)
{
    WorldSession* session = handler->GetSession();
    if (!session)
    {
        handler->SendSysMessage("This command can only be used in game.");
        handler->SetSentErrorMessage(true);
        return false;
    }

    Player* player = session->GetPlayer();
    if (!player)
        return false;

    Map* map = player->GetMap();
    if (!map || !map->IsDungeon())
    {
        handler->SendSysMessage("AutoBalance map statistics are only available inside instances.");
        handler->SetSentErrorMessage(true);
        return false;
    }

    InstanceMap* instance = map->ToInstanceMap();
    uint32 actualPlayers = instance->GetPlayersCountExceptGMs();
    uint32 effectivePlayers = AutoBalance::GetEffectivePlayerCountForInstance(instance);
    float scale = AutoBalance::GetMapScale(instance);

    handler->PSendSysMessage("AutoBalance: %u active players, effective count %u, multiplier %.2f.", actualPlayers, effectivePlayers, scale);
    return true;
}
