#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "BattlegroundQueue.h"
#include "Group.h"
#include "GroupReference.h"
#include "Miscellaneous/AddonRequestRoutes.h"
#include "Miscellaneous/TournamentMode.h"
#include "Opcodes.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "ScriptedCreature.h"
#include "ScriptedGossip.h"
#include "SharedDefines.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <DBCStores.h>
#include "StringConvert.h"
#include "Util.h"
#include <algorithm>
#include <string>

namespace
{
constexpr uint32 ACTION_QUEUE_SCARLET_REGULAR = GOSSIP_ACTION_INFO_DEF + 1;
constexpr uint32 ACTION_QUEUE_SCARLET_ALLIANCE = GOSSIP_ACTION_INFO_DEF + 2;
constexpr uint32 ACTION_QUEUE_SCARLET_HORDE = GOSSIP_ACTION_INFO_DEF + 3;
constexpr uint32 ACTION_QUEUE_BLACKROCK_REGULAR = GOSSIP_ACTION_INFO_DEF + 4;
constexpr uint32 ACTION_QUEUE_BLACKROCK_ALLIANCE = GOSSIP_ACTION_INFO_DEF + 5;
constexpr uint32 ACTION_QUEUE_BLACKROCK_HORDE = GOSSIP_ACTION_INFO_DEF + 6;
constexpr uint32 ACTION_CLOSE = GOSSIP_ACTION_INFO_DEF + 7;
constexpr uint32 ACTION_QUEUE_OBSIDIAN_REGULAR = GOSSIP_ACTION_INFO_DEF + 8;
constexpr uint32 ACTION_QUEUE_OBSIDIAN_ALLIANCE = GOSSIP_ACTION_INFO_DEF + 9;
constexpr uint32 ACTION_QUEUE_OBSIDIAN_HORDE = GOSSIP_ACTION_INFO_DEF + 10;
// 11-17 were Tanaris Deathmatch, the Violet Hold and Temple of Kotmogu, which
// Whitemane no longer offers.

void SendQueueError(Player* player, char const* text)
{
    CloseGossipMenuFor(player);
    if (WorldSession* session = player ? player->GetSession() : nullptr)
        session->SendNotification("%s", text);
}

bool QueueSinglePlayer(Player* player, BattlegroundTypeId bgTypeId, uint32 forcedTeam)
{
    if (!player || player->IsInCustomGameLobby() || !player->GetBGAccessByLevel(bgTypeId) || !player->HasFreeBattlegroundQueueId())
        return false;

    Battleground* bgTemplate = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId);
    if (!bgTemplate)
        return false;

    BattlegroundQueueTypeId const queueTypeId = BattlegroundMgr::BGQueueTypeId(bgTypeId, 0);
    if (queueTypeId == BATTLEGROUND_QUEUE_NONE)
        return false;

    if (player->GetBattlegroundQueueIndex(queueTypeId) < PLAYER_MAX_BATTLEGROUND_QUEUES)
        return false;

    PvPDifficultyEntry const* bracketEntry = GetBattlegroundBracketByLevel(bgTemplate->GetMapId(), player->GetLevel());
    if (!bracketEntry)
        return false;

    uint32 const previousBgTeam = player->GetBGTeamOverride();
    if (forcedTeam == TEAM_ALLIANCE)
        player->SetBGTeam(ALLIANCE);
    else if (forcedTeam == TEAM_HORDE)
        player->SetBGTeam(HORDE);
    else
        player->SetBGTeam(0);

    BattlegroundQueue& bgQueue = sBattlegroundMgr->GetBattlegroundQueue(queueTypeId);
    GroupQueueInfo* ginfo = bgQueue.AddGroup(player, nullptr, bgTypeId, bracketEntry, 0, false, false, 0, 0);

    player->SetBGTeam(previousBgTeam);

    if (!ginfo)
        return false;

    player->AddBattlegroundQueueId(queueTypeId);
    sBattlegroundMgr->ScheduleQueueUpdate(ginfo->ArenaMatchmakerRating, ginfo->ArenaType, queueTypeId, bgTypeId, bracketEntry->GetBracketId());
    return true;
}

// Queue an entire party as a single GroupQueueInfo, mirroring the stock
// CMSG_BATTLEMASTER_JOIN group path. Every member must individually be
// eligible; if any is not, nothing is queued at all rather than leaving a
// half-queued party.
bool QueueGroup(Player* leader, Group* group, BattlegroundTypeId bgTypeId, uint32 forcedTeam)
{
    if (!leader || !group)
        return false;

    Battleground* bgTemplate = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId);
    if (!bgTemplate)
        return false;

    BattlegroundQueueTypeId const queueTypeId = BattlegroundMgr::BGQueueTypeId(bgTypeId, 0);
    if (queueTypeId == BATTLEGROUND_QUEUE_NONE)
        return false;

    PvPDifficultyEntry const* bracketEntry = GetBattlegroundBracketByLevel(bgTemplate->GetMapId(), leader->GetLevel());
    if (!bracketEntry)
        return false;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member->IsInCustomGameLobby() || !member->GetBGAccessByLevel(bgTypeId) ||
            !member->HasFreeBattlegroundQueueId() ||
            member->GetBattlegroundQueueIndex(queueTypeId) < PLAYER_MAX_BATTLEGROUND_QUEUES)
            return false;

        // The tournament and world queue pools never share a match.
        if (Tournament::QueuesInTournamentPool(member) != Tournament::QueuesInTournamentPool(leader))
            return false;
    }

    // The forced side is read off the LEADER inside AddGroup, and it applies to
    // the whole resulting entry, so only the leader's override needs setting.
    uint32 const previousBgTeam = leader->GetBGTeamOverride();
    if (forcedTeam == TEAM_ALLIANCE)
        leader->SetBGTeam(ALLIANCE);
    else if (forcedTeam == TEAM_HORDE)
        leader->SetBGTeam(HORDE);
    else
        leader->SetBGTeam(0);

    BattlegroundQueue& bgQueue = sBattlegroundMgr->GetBattlegroundQueue(queueTypeId);
    bool const isPremade = group->GetMembersCount() >= bgTemplate->GetMinPlayersPerTeam();
    GroupQueueInfo* ginfo = bgQueue.AddGroup(leader, group, bgTypeId, bracketEntry, 0, false, isPremade, 0, 0);

    leader->SetBGTeam(previousBgTeam);

    if (!ginfo)
        return false;

    uint32 const avgTime = bgQueue.GetAverageQueueWaitTime(ginfo, bracketEntry->GetBracketId());
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member)
            continue;

        uint32 const queueSlot = member->AddBattlegroundQueueId(queueTypeId);

        WorldPacket data;
        sBattlegroundMgr->BuildBattlegroundStatusPacket(&data, bgTemplate, queueSlot, STATUS_WAIT_QUEUE,
            avgTime, 0, ginfo->ArenaType, 0);
        member->SendDirectMessage(&data);
    }

    sBattlegroundMgr->ScheduleQueueUpdate(ginfo->ArenaMatchmakerRating, ginfo->ArenaType, queueTypeId, bgTypeId, bracketEntry->GetBracketId());
    return true;
}

// Whitemane's queue: the party leader for the party (one queue entry, sized to
// a side), or the player alone; forcedTeam TEAM_NEUTRAL for either side.
bool QueueWhitemaneBattleground(Player* player, BattlegroundTypeId bgTypeId, char const* battlegroundName, uint32 forcedTeam)
{
    Group* group = player->GetGroup();
    if (group && !group->IsLeader(player->GetGUID()))
    {
        SendQueueError(player, "Only the party leader can queue your group.");
        return true;
    }

    if (!group)
    {
        if (!QueueSinglePlayer(player, bgTypeId, forcedTeam))
        {
            std::string error = "Unable to queue for ";
            error += battlegroundName;
            error += " right now.";
            SendQueueError(player, error.c_str());
        }
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

    // A party bigger than one team can hold must be turned away here.
    // QueueGroup enters it as ONE GroupQueueInfo and BattlegroundQueue
    // only ever seats a group whole, so an oversized party waits for a
    // number of free slots its side can never have - it sits in the
    // queue forever with no error. The honor-tab path is guarded by
    // Group::CanJoinBattlegroundQueue (BattlemasterList MaxGroupSize);
    // this NPC bypasses that check, so apply both limits ourselves.
    // Reachable since Scarlet Chapel dropped to a 5-player cap: a
    // six-man party is an ordinary thing to walk up with.
    if (Battleground const* sizeTemplate = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId))
    {
        uint32 partyCap = sizeTemplate->GetMaxPlayersPerTeam();
        if (BattlemasterListEntry const* bl = sBattlemasterListStore.LookupEntry(bgTypeId))
            partyCap = std::min(partyCap, bl->MaxGroupSize);

        if (partyCap && group->GetMembersCount() > partyCap)
        {
            std::string error = battlegroundName;
            error += " holds only ";
            error += std::to_string(partyCap);
            error += " per side - your party is too large.";
            SendQueueError(player, error.c_str());
            return true;
        }
    }

    // Queue the party as ONE queue entry. Calling QueueSinglePlayer per
    // member (as this used to) created a separate solo GroupQueueInfo
    // for each of them, so the queue never knew they were together -
    // and AddGroup's balancing deliberately pushes consecutive solo
    // entries to opposite sides, which is exactly what scattered
    // parties across both teams. One entry also lets the group-aware
    // logic in BattlegroundQueue work: it evicts bots to make room
    // rather than admitting a group partially or splitting it.
    if (!QueueGroup(player, group, bgTypeId, forcedTeam))
    {
        std::string error = "Unable to queue your group for ";
        error += battlegroundName;
        error += " right now.";
        SendQueueError(player, error.c_str());
    }
    else
        CloseGossipMenuFor(player);

    return true;
}

// The Battlemaster's window (CENTURION_ArenaMaster), owner 2026-10-29: "add a
// similar bg frame with a bg battlemaster (get rid of whitemane and aethalas)".
//   BGMASTER:JOIN:<battlemaster guid>:<bgTypeId>:<group 0|1>:<side N|A|H>
// Whitemane's three take a side and go through her queue; every other
// battleground through the stock join, as from the battlemaster's own list.
void HandleBattlemasterRequest(Player* player, std::string_view argument)
{
    std::vector<std::string_view> const parts = Trinity::Tokenize(argument, ':', true);
    if (!player || !player->GetSession() || parts.size() != 5 || parts[0] != "JOIN")
        return;

    Optional<uint64> const raw = Trinity::StringTo<uint64>(parts[1]);
    Optional<uint32> const type = Trinity::StringTo<uint32>(parts[2]);
    if (!raw || !type)
        return;

    ObjectGuid const master(*raw);
    if (!player->GetNPCIfCanInteractWith(master, UNIT_NPC_FLAG_BATTLEMASTER))
    {
        player->GetSession()->SendNotification("Speak to a Battlemaster first.");
        return;
    }

    BattlegroundTypeId const bgTypeId = BattlegroundTypeId(*type);
    bool const group = parts[3] == "1";
    char const* whitemane = bgTypeId == BATTLEGROUND_SCM ? "Scarlet Chapel"
        : bgTypeId == BATTLEGROUND_BRT ? "Blackrock Throne"
        : bgTypeId == BATTLEGROUND_OBC ? "Obsidian Colosseum" : nullptr;
    if (whitemane)
    {
        uint32 const team = parts[4] == "A" ? TEAM_ALLIANCE : parts[4] == "H" ? TEAM_HORDE : TEAM_NEUTRAL;
        // Alone or as the party, as she always did: a party is queued whole.
        if (group || !player->GetGroup())
            QueueWhitemaneBattleground(player, bgTypeId, whitemane, team);
        else if (!QueueSinglePlayer(player, bgTypeId, team))
            SendQueueError(player, (std::string("Unable to queue for ") + whitemane + " right now.").c_str());
        return;
    }

    WorldPacket data(CMSG_BATTLEMASTER_JOIN, 8 + 4 + 4 + 1);
    data << master;
    data << uint32(bgTypeId);
    data << uint32(0);                                      // first available
    data << uint8(group ? 1 : 0);
    player->GetSession()->HandleBattlemasterJoinOpcode(data);
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
            // A battleground is offered only once its data is loaded: code can
            // ship ahead of its rows, and QueueSinglePlayer would refuse it anyway.
            auto queueable = [](BattlegroundTypeId type) { return sBattlegroundMgr->GetBattlegroundTemplate(type) != nullptr; };
            if (queueable(BATTLEGROUND_SCM))
            {
                AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Queue for Scarlet Chapel", GOSSIP_SENDER_MAIN, ACTION_QUEUE_SCARLET_REGULAR);
                AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Queue for Scarlet Chapel (Force Alliance side)", GOSSIP_SENDER_MAIN, ACTION_QUEUE_SCARLET_ALLIANCE);
                AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Queue for Scarlet Chapel (Force Horde side)", GOSSIP_SENDER_MAIN, ACTION_QUEUE_SCARLET_HORDE);
            }
            if (queueable(BATTLEGROUND_BRT))
            {
                AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Queue for Blackrock Throne", GOSSIP_SENDER_MAIN, ACTION_QUEUE_BLACKROCK_REGULAR);
                AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Queue for Blackrock Throne (Force Alliance side)", GOSSIP_SENDER_MAIN, ACTION_QUEUE_BLACKROCK_ALLIANCE);
                AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Queue for Blackrock Throne (Force Horde side)", GOSSIP_SENDER_MAIN, ACTION_QUEUE_BLACKROCK_HORDE);
            }
            if (queueable(BATTLEGROUND_OBC))
            {
                AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Queue for Obsidian Colosseum", GOSSIP_SENDER_MAIN, ACTION_QUEUE_OBSIDIAN_REGULAR);
                AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Queue for Obsidian Colosseum (Force Alliance side)", GOSSIP_SENDER_MAIN, ACTION_QUEUE_OBSIDIAN_ALLIANCE);
                AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Queue for Obsidian Colosseum (Force Horde side)", GOSSIP_SENDER_MAIN, ACTION_QUEUE_OBSIDIAN_HORDE);
            }
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Maybe later", GOSSIP_SENDER_MAIN, ACTION_CLOSE);
            SendGossipMenuFor(player, player->GetGossipTextId(me), me);
            return true;
        }

        bool OnGossipSelect(Player* player, uint32, uint32 gossipListId) override
        {
            uint32 const action = player->PlayerTalkClass->GetGossipOptionAction(gossipListId);
            switch (action)
            {
                case ACTION_QUEUE_SCARLET_REGULAR: return HandleQueue(player, BATTLEGROUND_SCM, "Scarlet Chapel", TEAM_NEUTRAL);
                case ACTION_QUEUE_SCARLET_ALLIANCE: return HandleQueue(player, BATTLEGROUND_SCM, "Scarlet Chapel", TEAM_ALLIANCE);
                case ACTION_QUEUE_SCARLET_HORDE: return HandleQueue(player, BATTLEGROUND_SCM, "Scarlet Chapel", TEAM_HORDE);
                case ACTION_QUEUE_BLACKROCK_REGULAR: return HandleQueue(player, BATTLEGROUND_BRT, "Blackrock Throne", TEAM_NEUTRAL);
                case ACTION_QUEUE_BLACKROCK_ALLIANCE: return HandleQueue(player, BATTLEGROUND_BRT, "Blackrock Throne", TEAM_ALLIANCE);
                case ACTION_QUEUE_BLACKROCK_HORDE: return HandleQueue(player, BATTLEGROUND_BRT, "Blackrock Throne", TEAM_HORDE);
                case ACTION_QUEUE_OBSIDIAN_REGULAR: return HandleQueue(player, BATTLEGROUND_OBC, "Obsidian Colosseum", TEAM_NEUTRAL);
                case ACTION_QUEUE_OBSIDIAN_ALLIANCE: return HandleQueue(player, BATTLEGROUND_OBC, "Obsidian Colosseum", TEAM_ALLIANCE);
                case ACTION_QUEUE_OBSIDIAN_HORDE: return HandleQueue(player, BATTLEGROUND_OBC, "Obsidian Colosseum", TEAM_HORDE);
                case ACTION_CLOSE: CloseGossipMenuFor(player); return true;
                default: return false;
            }
        }

        bool HandleQueue(Player* player, BattlegroundTypeId bgTypeId, char const* battlegroundName, uint32 forcedTeam)
        {
            return QueueWhitemaneBattleground(player, bgTypeId, battlegroundName, forcedTeam);
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
    AddonRequestRoutes::Register("BGMASTER", &HandleBattlemasterRequest);
}
