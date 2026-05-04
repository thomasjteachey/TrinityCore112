#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "BattlegroundQueue.h"
#include "Group.h"
#include "GroupReference.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "ScriptedCreature.h"
#include "ScriptedGossip.h"
#include "SharedDefines.h"
#include "WorldSession.h"
#include <DBCStores.h>

namespace
{
constexpr uint32 ACTION_QUEUE_REGULAR = GOSSIP_ACTION_INFO_DEF + 1;
constexpr uint32 ACTION_QUEUE_ALLIANCE = GOSSIP_ACTION_INFO_DEF + 2;
constexpr uint32 ACTION_QUEUE_HORDE = GOSSIP_ACTION_INFO_DEF + 3;
constexpr uint32 ACTION_CLOSE = GOSSIP_ACTION_INFO_DEF + 4;

void SendQueueError(Player* player, char const* text)
{
    CloseGossipMenuFor(player);
    if (WorldSession* session = player ? player->GetSession() : nullptr)
        session->SendNotification("%s", text);
}

bool QueueSinglePlayer(Player* player, uint32 forcedTeam)
{
    if (!player || !player->GetBGAccessByLevel(BATTLEGROUND_SCM) || !player->HasFreeBattlegroundQueueId())
        return false;

    Battleground* bgTemplate = sBattlegroundMgr->GetBattlegroundTemplate(BATTLEGROUND_SCM);
    if (!bgTemplate)
        return false;

    BattlegroundQueueTypeId const queueTypeId = BattlegroundMgr::BGQueueTypeId(BATTLEGROUND_SCM, 0);
    if (queueTypeId == BATTLEGROUND_QUEUE_NONE)
        return false;

    if (player->GetBattlegroundQueueIndex(queueTypeId) < PLAYER_MAX_BATTLEGROUND_QUEUES)
        return false;

    PvPDifficultyEntry const* bracketEntry = GetBattlegroundBracketByLevel(bgTemplate->GetMapId(), player->GetLevel());
    if (!bracketEntry)
        return false;

    uint32 const previousBgTeam = player->GetBGTeam();
    if (forcedTeam == TEAM_ALLIANCE)
        player->SetBGTeam(ALLIANCE);
    else if (forcedTeam == TEAM_HORDE)
        player->SetBGTeam(HORDE);

    BattlegroundQueue& bgQueue = sBattlegroundMgr->GetBattlegroundQueue(queueTypeId);
    GroupQueueInfo* ginfo = bgQueue.AddGroup(player, nullptr, BATTLEGROUND_SCM, bracketEntry, 0, false, false, 0, 0);

    player->SetBGTeam(previousBgTeam);

    if (!ginfo)
        return false;

    player->AddBattlegroundQueueId(queueTypeId);
    sBattlegroundMgr->ScheduleQueueUpdate(ginfo->ArenaMatchmakerRating, ginfo->ArenaType, queueTypeId, BATTLEGROUND_SCM, bracketEntry->GetBracketId());
    return true;
}
}

class npc_scarlet_chapel_queue : public CreatureScript
{
public:
    npc_scarlet_chapel_queue() : CreatureScript("npc_scarlet_chapel_queue") { }

    struct npc_scarlet_chapel_queueAI : public ScriptedAI
    {
        npc_scarlet_chapel_queueAI(Creature* creature) : ScriptedAI(creature) { }

        bool OnGossipHello(Player* player) override
        {
            ClearGossipMenuFor(player);
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Queue for Scarlet Chapel", GOSSIP_SENDER_MAIN, ACTION_QUEUE_REGULAR);
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Queue for Scarlet Chapel (Force Alliance side)", GOSSIP_SENDER_MAIN, ACTION_QUEUE_ALLIANCE);
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Queue for Scarlet Chapel (Force Horde side)", GOSSIP_SENDER_MAIN, ACTION_QUEUE_HORDE);
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Maybe later", GOSSIP_SENDER_MAIN, ACTION_CLOSE);
            SendGossipMenuFor(player, player->GetGossipTextId(me), me);
            return true;
        }

        bool OnGossipSelect(Player* player, uint32, uint32 gossipListId) override
        {
            uint32 const action = player->PlayerTalkClass->GetGossipOptionAction(gossipListId);
            switch (action)
            {
                case ACTION_QUEUE_REGULAR: return HandleQueue(player, TEAM_NEUTRAL);
                case ACTION_QUEUE_ALLIANCE: return HandleQueue(player, TEAM_ALLIANCE);
                case ACTION_QUEUE_HORDE: return HandleQueue(player, TEAM_HORDE);
                case ACTION_CLOSE: CloseGossipMenuFor(player); return true;
                default: return false;
            }
        }

        bool HandleQueue(Player* player, uint32 forcedTeam)
        {
            Group* group = player->GetGroup();
            if (group && !group->IsLeader(player->GetGUID()))
            {
                SendQueueError(player, "Only the party leader can queue your group.");
                return true;
            }

            if (!group)
            {
                if (!QueueSinglePlayer(player, forcedTeam))
                    SendQueueError(player, "Unable to queue for Scarlet Chapel right now.");
                else
                    CloseGossipMenuFor(player);
                return true;
            }

            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (!member)
                {
                    SendQueueError(player, "All party members must be online.");
                    return true;
                }
            }

            bool anyFailed = false;
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (!QueueSinglePlayer(member, forcedTeam))
                    anyFailed = true;
            }

            if (anyFailed)
                SendQueueError(player, "One or more party members could not be queued.");
            else
                CloseGossipMenuFor(player);

            return true;
        }
    };

    CreatureAI* GetAI(Creature* creature) const override
    {
        return new npc_scarlet_chapel_queueAI(creature);
    }
};

void AddSC_npc_scarlet_chapel_queue()
{
    new npc_scarlet_chapel_queue();
}
