/*
 * Violet Hold boons - the broker NPC and the aura scripts behind the two
 * chance-based boons and the level marker. See
 * game/Miscellaneous/VioletHoldBoons.h for the design and the boon table.
 */

#include "Battleground.h"
#include "BattlegroundVHR.h"
#include "CellImpl.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "Containers.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Map.h"
#include "Player.h"
#include "Random.h"
#include "RBAC.h"
#include "ScriptedCreature.h"
#include "ScriptedGossip.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "AddonRequestRoutes.h"
#include "DBCStores.h"
#include "GameTime.h"
#include "ItemTemplate.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "TemporarySummon.h"
#include "Util.h"
#include "VioletHoldBoons.h"
#include "VioletHoldClassBoons.h"
#include "VioletHoldTuning.h"
#include "WorldSession.h"
#include "WrappedSpells.h"

#include <algorithm>
#include <iterator>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace VioletHoldBoons;
using namespace Trinity::ChatCommands;

namespace
{
constexpr uint32 ACTION_PICK_BASE = GOSSIP_ACTION_INFO_DEF + 100;   // + uint8(Boon)
constexpr uint32 ACTION_REROLL = GOSSIP_ACTION_INFO_DEF + 500;
constexpr uint32 ACTION_TEST_PREVIOUS = GOSSIP_ACTION_INFO_DEF + 501;
constexpr uint32 ACTION_TEST_NEXT = GOSSIP_ACTION_INFO_DEF + 502;
// The shop's wallet line: does nothing but show the menu again.
constexpr uint32 ACTION_SHOP_WALLET = GOSSIP_ACTION_INFO_DEF + 503;
// "Send the next wave": any one player starts it, or calls it in on top of
// the live one (BattlegroundVHR::StartNextWave).
constexpr uint32 ACTION_SHOP_READY = GOSSIP_ACTION_INFO_DEF + 504;
constexpr size_t TEST_OFFERS_PER_PAGE = 10;

// At most one all-boons test broker per GM character. A stale entry is harmless
// and is replaced the next time that character runs the command.
std::unordered_map<ObjectGuid, ObjectGuid> TestBrokerByOwner;

// "%s takes the %s." - see sql/custom/world/2026_08_15_00_world_violet_hold_boons.sql
constexpr uint32 STRING_BOON_TAKEN = 20101;

// The broker's own run, or nullptr when he is somewhere he should not be.
BattlegroundVHR* GetRun(Creature const* broker)
{
    Map* map = broker->GetMap();
    BattlegroundMap* bgMap = map ? map->ToBattlegroundMap() : nullptr;
    if (!bgMap)
        return nullptr;

    Battleground* bg = bgMap->GetBG();
    if (!bg || bg->GetTypeID() != BATTLEGROUND_VHR)
        return nullptr;

    return static_cast<BattlegroundVHR*>(bg);
}

// Only the party may trade with him: the clones are Players too, and on the
// other team.
bool MayTrade(Player const* player, BattlegroundVHR const* run, ObjectGuid brokerGuid = ObjectGuid::Empty)
{
    if (!player || !run)
        return false;

    WorldSession const* session = player->GetSession();
    if (!session || session->IsTransientPlayerSession())
        return false;

    if (player->GetBattleground() != run)
        return false;

    return player->GetBGTeam() == run->GetHumanTeam()
        && (!brokerGuid || run->MayUseBoonBroker(player, brokerGuid));
}

BattlegroundVHR* RunOfPlayer(Player const* player)
{
    Battleground* bg = player ? player->GetBattleground() : nullptr;
    if (!bg || bg->GetTypeID() != BATTLEGROUND_VHR)
        return nullptr;
    return static_cast<BattlegroundVHR*>(bg);
}

// --- The card draft (CENTURION_VioletHold addon, BoonDraft.lua) -------------
//
// The broker's offers as a hand of cards. Everything the cards show comes from
// the same tables the gossip lines do; the gossip menu is still sent after the
// cards, so a player without the addon trades exactly as before, and the addon
// closes that menu when it has the cards up.
//
// Server -> addon, CCGAME whispers, fields split on '^' (Field() keeps them
// out of the text), <broker> written the way UnitGUID writes it:
//   VHBOON:DEAL:<broker>^<wave>^<final wave>^<greed>^<abundance>^<hoarder>^<second opinions>^<common only>[^<name>^<class id>]...
//   VHBOON:CARD:<broker>^<code>^<kind B|S|I>^<tier 1-6>^<run-wide>^<greed card>^<icon S<spell>|I<item>>
//               ^<name>^<headline>^<text>^<held>^<after>^<cap>^<unit>^<PickResult for you>[^<who can take it>]...
//   VHBOON:DECK:<broker>[^<tier>^<name>^<value>]...      (several lines when long)
//   VHBOON:SHOW:<broker>                                 the hand is complete
//   VHBOON:LOOK:<broker>^<name>^<code>                   a party member is looking at a card
//   VHBOON:TAKEN:<broker>^<name>^<code>                  the deal is over
//   VHBOON:GONE:<broker>                                 the broker left with nothing taken
//   VHBOON:REROLLED:<broker>^<name>                       its fixed hand was replaced
// The shop keeper (a run with Centurion.VioletHold.Shop.Enable) sends the same
// DEAL ... SHOW block, plus, right after DEAL and after each CARD:
//   VHBOON:SHOP:<keeper>^<your shards>^<your shards per wave>^<your shards per killing blow>^<reroll %>^<discount %>
//               ^<your reroll cost>^<your free rerolls left>^<your locks in use>^<your locks>^<your interest next wave>
//   VHBOON:PRICE:<keeper>^<code>^<your price now>^<full price>^<waves discounted>^<locked by>^<locked by you 1|0>
//   VHBOON:WAVE:<keeper>^<next wave waiting for the party 1|0>^<ready>^<needed>^<you ready 1|0>    (before SHOW)
// and the addon may send VHBOON:READY:<keeper>:0 ("I'm ready").
// and sends the whole block again to everyone with the window open whenever
// the board or the wallets change. A shop sends no TAKEN: buying a card keeps
// the window open on the restocked board.
// Addon -> server, CCGAMEREQ whispers to self:
//   VHBOON:PICK:<broker>:<code>      (at the shop keeper, the same as BUY)
//   VHBOON:BUY:<keeper>:<code>
//   VHBOON:LOCK:<keeper>:<code>      (Boon of Safekeeping: lock, or unlock your own)
//   VHBOON:LOOK:<broker>:<code>
//   VHBOON:REROLL:<broker>:0         (at the shop keeper: reroll the unlocked cards)
// Codes are Offer::Encode(); CODE_NONE is "looking at nothing" - at the shop
// keeper, also "the window is closed".
constexpr uint32 CODE_NONE = 999;
// One addon line stays inside what a chat message may carry.
constexpr size_t MAX_LINE = 250;
// A party member's "looking at" relay, at most this often.
constexpr uint32 LOOK_INTERVAL_MS = 150;

std::string BrokerTag(ObjectGuid guid)
{
    return Trinity::StringFormat("0x{:016X}", guid.GetRawValue());
}

ObjectGuid ParseBrokerTag(std::string_view tag)
{
    if (tag.size() > 2 && tag[0] == '0' && (tag[1] == 'x' || tag[1] == 'X'))
        tag.remove_prefix(2);
    Optional<uint64> raw = Trinity::StringTo<uint64>(tag, 16);
    return raw ? ObjectGuid(*raw) : ObjectGuid::Empty;
}

std::string Field(std::string_view text)
{
    std::string out(text);
    for (char& c : out)
        if (c == '^' || c == '|' || c == '\t' || c == '\n' || c == '\r')
            c = ' ';
    return out;
}

bool IsRunWideBoon(Boon boon, Player const* viewer = nullptr)
{
    if (BattlegroundVHR* run = RunOfPlayer(viewer); run && run->UsesPersonalBrokers())
        if (boon == Boon::Greed || boon == Boon::Hoarder || boon == Boon::SecondOpinion || boon == Boon::Crucible)
            return false;
    return boon == Boon::Greed || boon == Boon::Hoarder || boon == Boon::Fellowship || boon == Boon::Abundance
        || boon == Boon::SecondOpinion || boon == Boon::Crucible || boon == Boon::Bazaar || boon == Boon::Safekeeping;
}

// The card's frame colour, read off the roll weight so a card that looks rare
// is rare in the pool: 1 common, 2 uncommon, 3 rare, 4 class spell, 5 legendary.
// A very-rare boon (weight 0, rarer than any legendary) wears the legendary frame.
// The class boons are tier 6, artifact: the top frame (BoonDraft.lua's RARITY[6]).
uint8 RarityTier(Offer offer)
{
    return GetRarityTier(offer);
}

char const* OfferName(Offer offer)
{
    return offer.kind == Offer::Kind::Boon ? GetBoon(Boon(offer.index)).name
         : offer.kind == Offer::Kind::ClassSpell ? GetClassSpell(offer.index).name
         : GetItemGrant(offer.index).name;
}

// "8 shards", "6 shards, was 8", "free" - what a shop card costs this viewer,
// for the gossip line.
std::string PriceText(VioletHoldShop::Slot const& slot, Player const* viewer)
{
    uint32 const price = VioletHoldShop::GetPriceFor(slot, viewer);
    if (!price)
        return "free";
    if (price < slot.basePrice)
        return Trinity::StringFormat("{} shards, was {}", price, slot.basePrice);
    return Trinity::StringFormat("{} shard{}", price, price == 1 ? "" : "s");
}

// Who holds a shop card's lock, by name ("" when nobody, or when they left).
std::string LockerName(VioletHoldShop::Slot const& slot)
{
    if (!slot.lockedBy)
        return "";
    if (Player const* locker = ObjectAccessor::FindPlayer(slot.lockedBy))
        return locker->GetName();
    return "";
}

// "Boon of the Vampire" -> "Vampire", for the deck strip.
std::string ShortBoonName(char const* name)
{
    std::string_view view(name);
    for (std::string_view prefix : { std::string_view("Boon of the "), std::string_view("Boon of ") })
        if (StringStartsWith(view, prefix))
            return std::string(view.substr(prefix.size()));
    return std::string(view);
}

// Every human member of the run, except `except`.
void SendToParty(BattlegroundVHR const* run, std::string const& line, Player const* except = nullptr)
{
    for (auto const& itr : run->GetPlayers())
    {
        if (itr.second.Team != run->GetHumanTeam() || (except && itr.first == except->GetGUID()))
            continue;
        if (Player* member = ObjectAccessor::FindPlayer(itr.first))
            if (member->GetSession() && !member->GetSession()->IsTransientPlayerSession())
                Battleground::SendSquadFeedLine(member, line);
    }
}

void CollectBrokerRoster(BattlegroundVHR const* run, ObjectGuid brokerGuid, std::vector<Player const*>& out)
{
    if (ObjectGuid const ownerGuid = run->GetBoonBrokerOwner(brokerGuid))
    {
        if (Player const* owner = ObjectAccessor::FindPlayer(ownerGuid))
            out.push_back(owner);
        return;
    }
    run->CollectHumanPlayers(out);
}

void SendToBrokerAudience(BattlegroundVHR const* run, ObjectGuid brokerGuid, std::string const& line,
    Player const* except = nullptr)
{
    if (ObjectGuid const ownerGuid = run->GetBoonBrokerOwner(brokerGuid))
    {
        if ((!except || except->GetGUID() != ownerGuid))
            if (Player* owner = ObjectAccessor::FindPlayer(ownerGuid))
                Battleground::SendSquadFeedLine(owner, line);
        return;
    }
    SendToParty(run, line, except);
}

std::string CardLine(Player const* viewer, std::string const& tag, Offer offer, bool greedCard,
    std::vector<Player const*> const& roster)
{
    char kind = 'B';
    bool runWide = false;
    std::string icon, name, headline, text, unit;
    uint32 held = 0, after = 0, cap = 0;

    switch (offer.kind)
    {
        case Offer::Kind::Item:
        {
            ItemGrantInfo const& info = GetItemGrant(offer.index);
            kind = 'I';
            icon = "I" + std::to_string(info.itemEntry);
            name = info.name;
            if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(info.itemEntry))
                name = proto->Name1;
            headline = "Legendary";
            SpellItemEnchantmentEntry const* enchant = sSpellItemEnchantmentStore.LookupEntry(info.enchantId);
            if (enchant && *enchant->Name[LOCALE_enUS])
                headline = enchant->Name[LOCALE_enUS];
            text = "Lost when you leave the Hold.";
            break;
        }
        case Offer::Kind::ClassSpell:
        {
            ClassSpellInfo const& info = GetClassSpell(offer.index);
            kind = 'S';
            icon = "S" + std::to_string(ResolveRank(info.spellId, viewer->GetLevel()));
            name = info.name;
            headline = "Learn";
            text = "Lost when you leave the Hold.";
            break;
        }
        default:
        {
            BoonInfo const& info = GetBoon(Boon(offer.index));
            icon = "S" + std::to_string(GetBoonSpellFor(info.boon, viewer->GetClass()));
            name = info.name;
            runWide = IsRunWideBoon(info.boon, viewer);
            held = GetStacks(viewer, info.boon);
            cap = GetMaxStacks(info);
            uint8 const grantStacks = GetGrantStacks(info);
            after = std::min<uint32>(held + grantStacks, cap);
            unit = info.unit;
            text = info.summary;
            // Tempo's stacks read as ms, Devastation's as 15% each.
            uint32 const perStack = DisplayPerStack(info.boon);
            held *= perStack;
            after *= perStack;
            cap *= perStack;
            if (info.boon == Boon::Tempo)
                headline = Trinity::StringFormat("+{} ms", uint32(grantStacks) * perStack);
            if (BattlegroundVHR* run = RunOfPlayer(viewer); run && run->UsesPersonalBrokers())
                switch (info.boon)
                {
                    case Boon::Greed:        text = "extra option at each of your brokers from now on"; break;
                    case Boon::Hoarder:      text = "one extra private broker after every wave from now on (up to two)"; break;
                    case Boon::SecondOpinion:text = "three whole-hand rerolls for your brokers this run"; break;
                    case Boon::Crucible:     text = "a deathless wave calls you an extra common-boon broker"; break;
                    default: break;
                }
            if (cap > 1 && info.boon != Boon::Tempo)
                headline = Trinity::StringFormat("+{}{}", uint32(grantStacks) * perStack, info.unit);
            if (info.boon == Boon::Healthstone)
                if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(GetHealthstoneFor(viewer->GetLevel())))
                    text = proto->Name1 + ", " + info.summary;
            break;
        }
    }

    PickResult const status = CanTake(viewer, offer);

    std::string const head = Trinity::StringFormat("VHBOON:CARD:{}^{}^{}^{}^{}^{}^{}^{}^{}^", tag, offer.Encode(), kind,
        uint32(RarityTier(offer)), runWide ? 1 : 0, greedCard ? 1 : 0, icon, Field(name), Field(headline));
    std::string tail = Trinity::StringFormat("^{}^{}^{}^{}^{}", held, after, cap, Field(unit), uint32(status));

    // A card that is not yours says whose it is.
    if (status != PickResult::Ok)
        for (Player const* member : roster)
            if (member && member != viewer && CanTake(member, offer) == PickResult::Ok && tail.size() < 80)
                tail += "^" + Field(member->GetName());

    text = Field(text);
    size_t const room = MAX_LINE > head.size() + tail.size() ? MAX_LINE - head.size() - tail.size() : 0;
    if (text.size() > room)
        text.resize(room);

    return head + text + tail;
}

// What the viewer already holds this run: boons with their stacks, the class
// spells the broker taught (knowledge marker) and the legendaries he handed
// out. Run-wide boons are left to the DEAL line.
void SendDeck(Player* viewer, std::string const& tag)
{
    std::vector<std::string> entries;

    for (uint8 i = 0; i < uint8(Boon::Max); ++i)
    {
        BoonInfo const& info = GetBoon(Boon(i));
        if (IsRunWideBoon(info.boon, viewer))
            continue;
        uint8 const stacks = GetStacks(viewer, info.boon);
        if (!stacks)
            continue;

        Offer offer;
        offer.kind = Offer::Kind::Boon;
        offer.index = i;
        std::string value;
        if (info.boon == Boon::Tempo)
            value = Trinity::StringFormat("{} ms", uint32(stacks) * TEMPO_MS_PER_STACK);
        else if (info.maxStacks > 1)
            value = *info.unit ? Trinity::StringFormat("{}{}", uint32(stacks) * DisplayPerStack(info.boon), info.unit) : Trinity::StringFormat("x{}", uint32(stacks));
        entries.push_back(Trinity::StringFormat("{}^{}^{}", uint32(RarityTier(offer)), Field(ShortBoonName(info.name)), Field(value)));
    }

    if (Aura const* marker = viewer->GetAura(SPELL_KNOWLEDGE_MARKER))
        for (uint8 slot = 0; slot < MAX_SPELL_EFFECTS; ++slot)
            if (AuraEffect const* eff = marker->GetEffect(slot))
                if (uint32 const tableSpellId = uint32(eff->GetBaseAmount()))
                    for (uint8 i = 0; i < CLASS_SPELL_COUNT; ++i)
                        if (GetClassSpell(i).spellId == tableSpellId)
                            entries.push_back(Trinity::StringFormat("4^{}^learned", Field(GetClassSpell(i).name)));

    Aura const* cache = viewer->GetAura(SPELL_CACHE_MARKER);
    for (uint8 i = 0; i < ITEM_GRANT_COUNT; ++i)
    {
        ItemGrantInfo const& info = GetItemGrant(i);
        bool owned = false;
        if (info.runOnly)
            owned = viewer->HasItemCount(info.itemEntry, 1, true);
        else if (cache && i < MAX_SPELL_EFFECTS)
            if (AuraEffect const* eff = cache->GetEffect(i))
                owned = eff->GetBaseAmount() != 0;
        if (owned)
            entries.push_back(Trinity::StringFormat("5^{}^", Field(info.name)));
    }

    std::string const start = "VHBOON:DECK:" + tag;
    std::string line = start;
    for (std::string const& entry : entries)
    {
        if (line.size() + 1 + entry.size() > MAX_LINE && line.size() > start.size())
        {
            Battleground::SendSquadFeedLine(viewer, line);
            line = start;
        }
        line += "^" + entry;
    }
    Battleground::SendSquadFeedLine(viewer, line);
}

struct npc_violet_hold_boon_brokerAI : public ScriptedAI
{
    npc_violet_hold_boon_brokerAI(Creature* creature) : ScriptedAI(creature), _rolled(false), _consumed(false) { }

    void Reset() override
    {
        me->SetReactState(REACT_PASSIVE);
    }

    uint32 ConfigureForTesting(Player* owner)
    {
        _testOwner = owner ? owner->GetGUID() : ObjectGuid::Empty;
        _offers.clear();
        _looks.clear();
        _lookTimes.clear();
        _testPage = 0;
        _consumed = false;

        uint32 eligible = 0;
        if (owner)
            for (uint16 index = 0; index < uint16(Boon::Max); ++index)
            {
                Offer offer{ Offer::Kind::Boon, uint8(index) };
                _offers.push_back(offer);
                if (CanTake(owner, offer) == PickResult::Ok)
                    ++eligible;
            }

        _rolled = true;
        return eligible;
    }

    bool OnGossipHello(Player* player) override
    {
        BattlegroundVHR* run = GetRun(me);
        if (!MayTradeWith(player, run) || _consumed)
        {
            CloseGossipMenuFor(player);
            return true;
        }

        if (run->IsShopKeeper(me->GetGUID()))
            return ShopHello(player, run);

        // A full class-eligible pool is far wider than the addon card frame.
        // Keep the normal production broker on cards, and present only this GM
        // test broker as a compact paged gossip list.
        if (_testOwner)
            return ShowTestPage(player, run);

        // The board minus whatever has gone dead since it was rolled: a
        // unique boon somebody claimed, a class spell its only owner
        // learned, a legendary already handed out. Lines that merely do
        // not fit THIS reader stay - they are still somebody's - and
        // DescribeOffer labels them.
        std::vector<Offer> const offers = LiveOffersFor(run);
        if (offers.empty())
        {
            // Nothing here helps anyone any more. This broker can never be
            // consumed by a pick, and his slot is one of a fixed few, so
            // retire him now and let the next wave put a fresh one out.
            if (WorldSession* session = player->GetSession())
                session->SendNotification("The broker has nothing left that would help anyone here.");
            CloseGossipMenuFor(player);
            _consumed = true;
            SendAudience(run, "VHBOON:GONE:" + BrokerTag(me->GetGUID()));
            // Frees the slot and removes the creature at the end of the
            // update. Nothing below may touch `me` after this.
            Retire(run);
            return true;
        }

        // The cards first, so the addon has them when the menu arrives.
        SendDeal(player, run, offers);

        ClearGossipMenuFor(player);
        for (Offer const& offer : offers)
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, DescribeOffer(player, offer),
                GOSSIP_SENDER_MAIN, ACTION_PICK_BASE + offer.Encode());
        if (!_testOwner && run->GetSecondOpinionCharges(player))
            AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                Trinity::StringFormat("Ask for a second opinion ({} left)", uint32(run->GetSecondOpinionCharges(player))),
                GOSSIP_SENDER_MAIN, ACTION_REROLL);

        SendGossipMenuFor(player, NPC_TEXT_BOON_BROKER, me->GetGUID());
        return true;
    }

    bool OnGossipSelect(Player* player, uint32 /*menuId*/, uint32 gossipListId) override
    {
        uint32 const action = player->PlayerTalkClass->GetGossipOptionAction(gossipListId);
        CloseGossipMenuFor(player);

        if (BattlegroundVHR* run = GetRun(me); run && run->IsShopKeeper(me->GetGUID()))
        {
            Offer offer;
            if (action == ACTION_SHOP_READY)
                Ready(player);
            else if (action == ACTION_REROLL)
                ShopReroll(player, run);
            else if (action >= ACTION_PICK_BASE && action < ACTION_REROLL && Offer::Decode(action - ACTION_PICK_BASE, offer))
                Buy(player, offer);
            // Back to the counter, whichever line it was.
            OnGossipHello(player);
            return true;
        }

        if (_testOwner && action == ACTION_TEST_PREVIOUS)
        {
            if (_testPage)
                --_testPage;
            OnGossipHello(player);
            return true;
        }
        if (_testOwner && action == ACTION_TEST_NEXT)
        {
            ++_testPage;
            OnGossipHello(player);
            return true;
        }

        if (action == ACTION_REROLL)
        {
            if (Reroll(player))
                OnGossipHello(player);
            return true;
        }

        Offer offer;
        if (action < ACTION_PICK_BASE || !Offer::Decode(action - ACTION_PICK_BASE, offer))
            return true;

        Pick(player, offer);
        return true;
    }

    bool Reroll(Player* player)
    {
        BattlegroundVHR* run = GetRun(me);
        if (run && run->IsShopKeeper(me->GetGUID()))
        {
            ShopReroll(player, run);
            return false;   // the board went out to every viewer already
        }
        if (_testOwner || !MayTradeWith(player, run) || _consumed || !_rolled
            || !player->GetNPCIfCanInteractWith(me->GetGUID(), UNIT_NPC_FLAG_GOSSIP)
            || !run->SpendSecondOpinion(player))
            return false;

        _offers.clear();
        _looks.clear();
        _rolled = false;
        SendAudience(run, Trinity::StringFormat("VHBOON:REROLLED:{}^{}", BrokerTag(me->GetGUID()), Field(player->GetName())));
        return true;
    }

    // Both ways in - a gossip line and a card - end here.
    void Pick(Player* player, Offer offer)
    {
        BattlegroundVHR* run = GetRun(me);
        // A card addon that predates the shop asks the keeper for a pick.
        if (run && run->IsShopKeeper(me->GetGUID()))
        {
            Buy(player, offer);
            return;
        }
        if (!MayTradeWith(player, run) || _consumed)
            return;

        // The pick has to be one of the offers this broker is actually
        // showing, so a hand-crafted packet cannot shop the whole list.
        std::vector<Offer> const& offers = OffersFor(run);
        if (std::find(offers.begin(), offers.end(), offer) == offers.end())
            return;

        PickResult const result = Take(player, offer);
        if (result != PickResult::Ok)
        {
            // Wrong class, already known, capped: the broker stays for
            // whoever it does fit. Nothing was changed.
            if (WorldSession* session = player->GetSession())
                session->SendNotification("%s", GetPickResultText(result));
            if (_testOwner)
                OnGossipHello(player);
            return;
        }

        char const* name = OfferName(offer);
        if (WorldSession* session = player->GetSession())
            session->SendNotification("You receive %s.", name);
        if (_testOwner)
        {
            // Unlike a real draft, the test broker remains so the GM can take
            // another card. LiveOffersFor removes capped/unique cards and keeps
            // stackable cards until their real cap is reached.
            OnGossipHello(player);
            return;
        }

        _consumed = true;
        if (!run->UsesPersonalBrokers())
            run->PSendMessageToAll(STRING_BOON_TAKEN, CHAT_MSG_BG_SYSTEM_NEUTRAL, nullptr, player->GetName().c_str(), name);
        SendAudience(run, Trinity::StringFormat("VHBOON:TAKEN:{}^{}^{}", BrokerTag(me->GetGUID()), Field(player->GetName()), offer.Encode()));

        // Frees the slot and removes the creature at the end of the update.
        // Nothing below may touch `me` after this.
        run->ConsumeBoonBroker(me->GetGUID());
    }

    // A party member's cursor moved to another card (CODE_NONE: put down).
    // Told to the rest of the party so two people do not reach for the same one.
    void Look(Player* player, uint32 code)
    {
        BattlegroundVHR* run = GetRun(me);
        if (!MayTradeWith(player, run) || _consumed)
            return;

        Offer offer;
        if (code != CODE_NONE && !Offer::Decode(code, offer))
            return;

        uint32 const now = GameTime::GetGameTimeMS();
        uint32& last = _lookTimes[player->GetGUID()];
        // Putting the cards down always goes through, or a badge could stick.
        if (code != CODE_NONE && last && now - last < LOOK_INTERVAL_MS)
            return;
        last = now;

        if (code == CODE_NONE)
            _looks.erase(player->GetGUID());
        else
            _looks[player->GetGUID()] = code;

        // At the shop keeper the cards going down is the window closing: no
        // more board updates for that player until they open it again.
        if (run->IsShopKeeper(me->GetGUID()))
        {
            if (code == CODE_NONE)
                run->GetShop().RemoveViewer(player->GetGUID());
            else
                run->GetShop().AddViewer(player->GetGUID());
        }

        SendAudience(run, Trinity::StringFormat("VHBOON:LOOK:{}^{}^{}", BrokerTag(me->GetGUID()), Field(player->GetName()), code), player);
    }

    // BattlegroundVHR::NotifyShopChanged: a wave was cleared (shards paid,
    // cards rerolled or discounted).
    void DoAction(int32 action) override
    {
        if (action != BG_VHR_ACTION_SHOP_CHANGED)
            return;
        if (BattlegroundVHR* run = GetRun(me); run && run->IsShopKeeper(me->GetGUID()))
            SendShopToViewers(run);
    }

    // One card bought at the shop keeper - the gossip line, a card's Buy
    // button and an old addon's Take all end here. Nothing changes unless the
    // card is still on the board, the buyer can pay and the grant takes.
    void Buy(Player* player, Offer offer)
    {
        BattlegroundVHR* run = GetRun(me);
        if (!run || !run->IsShopKeeper(me->GetGUID()) || !MayTradeWith(player, run))
            return;

        VioletHoldShop& shop = run->GetShop();
        WorldSession* session = player->GetSession();
        int32 const index = shop.FindSlot(offer);
        if (index < 0)
        {
            // Somebody bought it first, or a wave rerolled it.
            if (session)
                session->SendNotification("That is no longer for sale.");
            SendShopBoard(player, run);
            return;
        }

        uint32 const price = VioletHoldShop::GetPriceFor(shop.GetSlots()[size_t(index)], player);
        uint32 const shards = shop.GetShards(player->GetGUID());
        // A refusal still sends the board back, so the card addon's Buy
        // button stops waiting on an answer.
        if (shards < price)
        {
            if (session)
                session->SendNotification("You need %u more shards.", price - shards);
            SendShopBoard(player, run);
            return;
        }

        PickResult const result = Take(player, offer);
        if (result != PickResult::Ok)
        {
            if (session)
                session->SendNotification("%s", GetPickResultText(result));
            SendShopBoard(player, run);
            return;
        }

        shop.Spend(player->GetGUID(), price);
        char const* name = OfferName(offer);
        if (session)
            session->SendNotification("You buy %s for %u shards.", name, price);

        std::vector<Player const*> roster;
        CollectRoster(run, roster);
        if (roster.size() > 1)
            run->PSendMessageToAll(STRING_BOON_TAKEN, CHAT_MSG_BG_SYSTEM_NEUTRAL, nullptr, player->GetName().c_str(), name);

        // The empty slot is drawn again at once. What was just bought may also
        // have closed another card for good (a unique boon, a spell).
        shop.Restock(roster, size_t(index));
        shop.RestockDeadSlots(roster);
        shop.AddViewer(player->GetGUID());
        SendShopToViewers(run);
    }

    // The board's Reroll: every card nobody has locked is drawn again, for
    // Shop.RerollCost shards or one of the payer's free Gambler rerolls.
    void ShopReroll(Player* player, BattlegroundVHR* run)
    {
        if (!run->IsShopKeeper(me->GetGUID()) || !MayTradeWith(player, run))
            return;
        WorldSession* session = player->GetSession();
        if (!player->GetNPCIfCanInteractWith(me->GetGUID(), UNIT_NPC_FLAG_GOSSIP))
        {
            if (session)
                session->SendNotification("You are too far away from the broker.");
            SendShopBoard(player, run);
            return;
        }

        VioletHoldShop& shop = run->GetShop();
        uint32 const cost = shop.GetRerollCostFor(player);
        std::vector<Player const*> roster;
        CollectRoster(run, roster);
        if (!shop.Reroll(player, roster))
        {
            if (session)
                session->SendNotification("You need %u more shards to reroll.", cost - std::min(cost, shop.GetShards(player->GetGUID())));
            SendShopBoard(player, run);
            return;
        }

        if (roster.size() > 1)
            for (Player const* member : roster)
                if (member && member != player && member->GetSession())
                    ChatHandler(member->GetSession()).SendSysMessage(
                        Trinity::StringFormat("{} rerolled the shop.", player->GetName()).c_str());
        shop.AddViewer(player->GetGUID());
        SendShopToViewers(run);
    }

    // "I'm ready" - from the gossip line or the shop window's Ready button.
    void Ready(Player* player)
    {
        BattlegroundVHR* run = GetRun(me);
        if (!run || !run->IsShopKeeper(me->GetGUID()) || !MayTradeWith(player, run))
            return;
        std::string const reply = run->StartNextWave(player);
        if (WorldSession* session = player->GetSession())
            session->SendNotification("%s", reply.c_str());
        run->GetShop().AddViewer(player->GetGUID());
        SendShopToViewers(run);
    }

    // Boon of Safekeeping: lock a card on the board with one of the party's
    // shared locks, or take any lock off it.
    void ToggleLock(Player* player, Offer offer)
    {
        BattlegroundVHR* run = GetRun(me);
        if (!run || !run->IsShopKeeper(me->GetGUID()) || !MayTradeWith(player, run))
            return;

        VioletHoldShop& shop = run->GetShop();
        WorldSession* session = player->GetSession();
        switch (shop.ToggleLock(player, offer))
        {
            case VioletHoldShop::LockResult::Locked:
            case VioletHoldShop::LockResult::Unlocked:
                SendShopToViewers(run);
                return;
            case VioletHoldShop::LockResult::NoLocks:
                if (session)
                    session->SendNotification("Locking a card takes the Boon of Safekeeping.");
                break;
            case VioletHoldShop::LockResult::AllInUse:
                if (session)
                    session->SendNotification("All %u of the party's locks are in use.", shop.GetLocksMax());
                break;
            default:
                break;
        }
        SendShopBoard(player, run);
    }

private:
    // The shop keeper's counter: the board as cards (for the addon), then the
    // same board as a gossip list with prices (for everyone without it).
    bool ShopHello(Player* player, BattlegroundVHR* run)
    {
        VioletHoldShop& shop = run->GetShop();
        std::vector<Player const*> roster;
        CollectRoster(run, roster);

        shop.AddViewer(player->GetGUID());
        if (shop.RestockDeadSlots(roster))
            SendShopToViewers(run);
        else
            SendShopBoard(player, run);

        ClearGossipMenuFor(player);
        AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG,
            Trinity::StringFormat("You have {} shards. A cleared wave pays {}, a killing blow {}.",
                shop.GetShards(player->GetGUID()), shop.GetWavePayFor(player), shop.GetKillPayFor(player)),
            GOSSIP_SENDER_MAIN, ACTION_SHOP_WALLET);
        for (VioletHoldShop::Slot const& slot : shop.GetSlots())
        {
            if (!slot.filled)
                continue;
            std::string const locker = LockerName(slot);
            AddGossipItemFor(player, GOSSIP_ICON_VENDOR,
                Trinity::StringFormat("[{}] {}{}", PriceText(slot, player), DescribeOffer(player, slot.offer),
                    locker.empty() ? "" : " - locked by " + locker),
                GOSSIP_SENDER_MAIN, ACTION_PICK_BASE + slot.offer.Encode());
        }
        uint32 const rerollCost = shop.GetRerollCostFor(player);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT,
            rerollCost ? Trinity::StringFormat("Reroll the unlocked cards ({} shards)", rerollCost)
                       : std::string("Reroll the unlocked cards (free)"),
            GOSSIP_SENDER_MAIN, ACTION_REROLL);
        switch (run->GetNextWaveCall())
        {
            case BattlegroundVHR::NextWaveCall::Start:
                AddGossipItemFor(player, GOSSIP_ICON_BATTLE,
                    Trinity::StringFormat("Send wave {} in", run->GetWaveNumber()),
                    GOSSIP_SENDER_MAIN, ACTION_SHOP_READY);
                break;
            case BattlegroundVHR::NextWaveCall::CallEarly:
                AddGossipItemFor(player, GOSSIP_ICON_BATTLE,
                    Trinity::StringFormat("Call wave {} in now, on top of this one", run->GetWaveNumber() + 1),
                    GOSSIP_SENDER_MAIN, ACTION_SHOP_READY);
                break;
            default:
                break;
        }

        SendGossipMenuFor(player, NPC_TEXT_BOON_BROKER, me->GetGUID());
        return true;
    }

    // The board as the card addon reads it: the broker's DEAL ... SHOW block,
    // with the viewer's shards and each card's price added.
    void SendShopBoard(Player* viewer, BattlegroundVHR* run)
    {
        std::string const tag = BrokerTag(me->GetGUID());
        std::vector<Player const*> roster;
        CollectRoster(run, roster);
        VioletHoldShop const& shop = run->GetShop();

        std::string deal = Trinity::StringFormat("VHBOON:DEAL:{}^{}^{}^0^{}^0^0^0", tag, run->GetWaveNumber(),
            uint32(BG_VHR_FINAL_WAVE), uint32(run->GetBonusRunesPerWave()));
        for (Player const* member : roster)
            if (member && deal.size() < MAX_LINE - 20)
                deal += Trinity::StringFormat("^{}^{}", Field(member->GetName()), uint32(member->GetClass()));
        Battleground::SendSquadFeedLine(viewer, deal);
        uint32 const interest = shop.GetInterestFor(viewer);
        Battleground::SendSquadFeedLine(viewer, Trinity::StringFormat("VHBOON:SHOP:{}^{}^{}^{}^{}^{}^{}^{}^{}^{}^{}", tag,
            shop.GetShards(viewer->GetGUID()), shop.GetWavePayFor(viewer) - interest, shop.GetKillPayFor(viewer),
            VioletHoldShop::GetRerollChancePercent(), VioletHoldShop::GetDiscountPercent(),
            shop.GetRerollCostFor(viewer), shop.GetFreeRerollsLeft(viewer),
            shop.GetLocksUsed(), shop.GetLocksMax(), interest));

        for (VioletHoldShop::Slot const& slot : shop.GetSlots())
        {
            if (!slot.filled)
                continue;
            Battleground::SendSquadFeedLine(viewer, CardLine(viewer, tag, slot.offer, false, roster));
            // ...^<locked by>^<mine>^<may unlock>: the locks are the party's,
            // so anyone may take one off (addons up to 1.00177 ignore it).
            Battleground::SendSquadFeedLine(viewer, Trinity::StringFormat("VHBOON:PRICE:{}^{}^{}^{}^{}^{}^{}^{}", tag,
                slot.offer.Encode(), VioletHoldShop::GetPriceFor(slot, viewer), slot.basePrice, uint32(slot.discounts),
                Field(LockerName(slot)), slot.lockedBy == viewer->GetGUID() ? 1 : 0, slot.lockedBy ? 1 : 0));
        }

        // The window's next-wave button:
        //   WAVE:<tag>^<shown>^0^1^0^<call>^<wave>
        // call 1 = send the waiting wave in, 2 = call the next one in on top
        // of this fight; wave = the wave it brings. Fields 3-5 (ready, needed,
        // mine) are what addons up to 1.00176 read for the old party vote:
        // "0 of 1, not mine" draws their button as "Start next wave".
        {
            BattlegroundVHR::NextWaveCall const call = run->GetNextWaveCall();
            uint32 const wave = call == BattlegroundVHR::NextWaveCall::CallEarly ? run->GetWaveNumber() + 1 : run->GetWaveNumber();
            Battleground::SendSquadFeedLine(viewer, Trinity::StringFormat("VHBOON:WAVE:{}^{}^0^1^0^{}^{}", tag,
                call != BattlegroundVHR::NextWaveCall::None ? 1 : 0, uint32(call), wave));
        }

        SendDeck(viewer, tag);
        Battleground::SendSquadFeedLine(viewer, "VHBOON:SHOW:" + tag);

        for (auto const& [guid, code] : _looks)
            if (guid != viewer->GetGUID())
                if (Player const* member = ObjectAccessor::FindPlayer(guid))
                    Battleground::SendSquadFeedLine(viewer,
                        Trinity::StringFormat("VHBOON:LOOK:{}^{}^{}", tag, Field(member->GetName()), code));
    }

    // Everyone with the window open gets the board again. Whoever has left
    // the run (or cannot trade here any more) stops being sent it.
    void SendShopToViewers(BattlegroundVHR* run)
    {
        VioletHoldShop& shop = run->GetShop();
        GuidSet const viewers = shop.GetViewers();
        for (ObjectGuid guid : viewers)
        {
            Player* viewer = ObjectAccessor::FindPlayer(guid);
            if (!viewer || !MayTradeWith(viewer, run))
            {
                shop.RemoveViewer(guid);
                continue;
            }
            SendShopBoard(viewer, run);
        }
    }

    bool ShowTestPage(Player* player, BattlegroundVHR* run)
    {
        std::vector<Offer> const offers = LiveOffersFor(run);
        if (offers.empty())
        {
            if (WorldSession* session = player->GetSession())
                session->SendNotification("You have no eligible boons left on this test broker.");
            CloseGossipMenuFor(player);
            _consumed = true;
            Retire(run);
            return true;
        }

        size_t const pages = (offers.size() + TEST_OFFERS_PER_PAGE - 1) / TEST_OFFERS_PER_PAGE;
        if (_testPage >= pages)
            _testPage = uint32(pages - 1);
        size_t const first = size_t(_testPage) * TEST_OFFERS_PER_PAGE;
        size_t const last = std::min(offers.size(), first + TEST_OFFERS_PER_PAGE);

        ClearGossipMenuFor(player);
        if (_testPage)
            AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                Trinity::StringFormat("Previous page ({}/{})", _testPage, pages),
                GOSSIP_SENDER_MAIN, ACTION_TEST_PREVIOUS);
        for (size_t i = first; i < last; ++i)
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, DescribeOffer(player, offers[i]),
                GOSSIP_SENDER_MAIN, ACTION_PICK_BASE + offers[i].Encode());
        if (last < offers.size())
            AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                Trinity::StringFormat("Next page ({}/{})", _testPage + 2, pages),
                GOSSIP_SENDER_MAIN, ACTION_TEST_NEXT);

        SendGossipMenuFor(player, NPC_TEXT_BOON_BROKER, me->GetGUID());
        return true;
    }

    bool MayTradeWith(Player const* player, BattlegroundVHR const* run) const
    {
        if (!_testOwner)
            return MayTrade(player, run, me->GetGUID());
        return player && player->GetGUID() == _testOwner && MayTrade(player, run);
    }

    void CollectRoster(BattlegroundVHR const* run, std::vector<Player const*>& out) const
    {
        if (_testOwner)
        {
            if (Player const* owner = ObjectAccessor::FindPlayer(_testOwner))
                out.push_back(owner);
            return;
        }
        CollectBrokerRoster(run, me->GetGUID(), out);
    }

    void SendAudience(BattlegroundVHR const* run, std::string const& line, Player const* except = nullptr) const
    {
        if (_testOwner)
        {
            if ((!except || except->GetGUID() != _testOwner))
                if (Player* owner = ObjectAccessor::FindPlayer(_testOwner))
                    Battleground::SendSquadFeedLine(owner, line);
            return;
        }
        SendToBrokerAudience(run, me->GetGUID(), line, except);
    }

    void Retire(BattlegroundVHR* run)
    {
        if (_testOwner)
        {
            auto itr = TestBrokerByOwner.find(_testOwner);
            if (itr != TestBrokerByOwner.end() && itr->second == me->GetGUID())
                TestBrokerByOwner.erase(itr);
            me->DespawnOrUnsummon();
            return;
        }
        run->ConsumeBoonBroker(me->GetGUID());
    }

    void SendDeal(Player* viewer, BattlegroundVHR* run, std::vector<Offer> const& live)
    {
        std::string const tag = BrokerTag(me->GetGUID());
        std::vector<Player const*> roster;
        CollectRoster(run, roster);

        std::string deal = Trinity::StringFormat("VHBOON:DEAL:{}^{}^{}^{}^{}^{}^{}^{}", tag, run->GetWaveNumber(), uint32(BG_VHR_FINAL_WAVE),
            uint32(_testOwner ? 0 : run->GetBrokerBonusOffers(viewer)), uint32(run->GetBonusRunesPerWave()), uint32(run->GetBonusBrokersPerWave(viewer)),
            uint32(run->GetSecondOpinionCharges(viewer)), run->IsCommonOnlyBroker(me->GetGUID()) ? 1 : 0);
        for (Player const* member : roster)
            if (member && deal.size() < MAX_LINE - 20)
                deal += Trinity::StringFormat("^{}^{}", Field(member->GetName()), uint32(member->GetClass()));
        Battleground::SendSquadFeedLine(viewer, deal);

        // Past the first OFFERS_PER_BROKER of the roll is what Greed added.
        std::vector<Offer> const& rolled = OffersFor(run);
        for (Offer const& offer : live)
        {
            size_t const position = size_t(std::find(rolled.begin(), rolled.end(), offer) - rolled.begin());
            Battleground::SendSquadFeedLine(viewer, CardLine(viewer, tag, offer, !_testOwner && position >= OFFERS_PER_BROKER, roster));
        }

        SendDeck(viewer, tag);
        Battleground::SendSquadFeedLine(viewer, "VHBOON:SHOW:" + tag);

        // Where everyone else's cursor already is.
        for (auto const& [guid, code] : _looks)
            if (guid != viewer->GetGUID())
                if (Player const* member = ObjectAccessor::FindPlayer(guid))
                    Battleground::SendSquadFeedLine(viewer,
                        Trinity::StringFormat("VHBOON:LOOK:{}^{}^{}", tag, Field(member->GetName()), code));
    }

    // Rolled once, on the first visit, against the party as it stands then,
    // and shown unchanged to everyone after - closing and reopening the
    // window is not a reroll, and a hunter ability stays on the board for
    // the hunter even if a mage looked first.
    // The cached board, filtered to what at least one member of the team
    // could still take. Recomputed on every visit: the roll is fixed, but
    // whether an offer is still worth anything is not.
    std::vector<Offer> LiveOffersFor(BattlegroundVHR const* run)
    {
        std::vector<Player const*> roster;
        CollectRoster(run, roster);

        std::vector<Offer> live;
        for (Offer const& offer : OffersFor(run))
            for (Player const* member : roster)
                if (member && CanTake(member, offer) == PickResult::Ok)
                {
                    live.push_back(offer);
                    break;
                }

        return live;
    }

    std::vector<Offer> const& OffersFor(BattlegroundVHR const* run)
    {
        if (!_rolled)
        {
            std::vector<Player const*> roster;
            CollectRoster(run, roster);
            // Boon of Greed widens every broker rolled after it.
            Player const* owner = roster.empty() ? nullptr : roster.front();
            _offers = RollOffers(roster, uint8(std::min<uint32>(OFFERS_PER_BROKER + run->GetBrokerBonusOffers(owner), 12)),
                run->IsCommonOnlyBroker(me->GetGUID()));
            _rolled = true;
        }
        return _offers;
    }

    std::vector<Offer> _offers;
    std::unordered_map<ObjectGuid, uint32> _looks;
    std::unordered_map<ObjectGuid, uint32> _lookTimes;
    ObjectGuid _testOwner;
    uint32 _testPage = 0;
    bool _rolled;
    bool _consumed;
};

// CCGAMEREQ VHBOON:PICK:<broker>:<code> and VHBOON:LOOK:<broker>:<code>, from
// the card draft. A pick passes the same interaction check a gossip click does
// (alive, in reach of the broker), then the same Pick().
void HandleBoonRequest(Player* sender, std::string_view argument)
{
    std::vector<std::string_view> const parts = Trinity::Tokenize(argument, ':', false);
    if (parts.size() != 3)
        return;

    ObjectGuid const brokerGuid = ParseBrokerTag(parts[1]);
    Optional<uint32> const code = Trinity::StringTo<uint32>(parts[2]);
    if (brokerGuid.IsEmpty() || !code)
        return;

    BattlegroundVHR* run = RunOfPlayer(sender);
    if (!MayTrade(sender, run))
        return;

    Creature* broker = ObjectAccessor::GetCreature(*sender, brokerGuid);
    npc_violet_hold_boon_brokerAI* ai = broker ? dynamic_cast<npc_violet_hold_boon_brokerAI*>(broker->AI()) : nullptr;
    bool const buying = parts[0] == "BUY";
    if (!ai || GetRun(broker) != run)
    {
        // Gone already (taken by someone without the addon, or retired):
        // put the sender's cards away.
        if (parts[0] == "PICK" || buying)
            Battleground::SendSquadFeedLine(sender, "VHBOON:GONE:" + std::string(parts[1]));
        return;
    }

    if (parts[0] == "LOOK")
    {
        ai->Look(sender, *code);
        return;
    }

    if (parts[0] == "REROLL")
    {
        if (ai->Reroll(sender))
            ai->OnGossipHello(sender);
        return;
    }

    if (parts[0] == "READY")
    {
        ai->Ready(sender);
        return;
    }

    if (parts[0] == "LOCK")
    {
        Offer offer;
        if (Offer::Decode(*code, offer))
            ai->ToggleLock(sender, offer);
        return;
    }

    if (parts[0] != "PICK" && !buying)
        return;

    if (!sender->GetNPCIfCanInteractWith(brokerGuid, UNIT_NPC_FLAG_GOSSIP))
    {
        if (WorldSession* session = sender->GetSession())
            session->SendNotification("You are too far away from the broker.");
        return;
    }

    Offer offer;
    if (!Offer::Decode(*code, offer))
        return;

    if (buying)
        ai->Buy(sender, offer);
    else
        ai->Pick(sender, offer);
}
}

class npc_violet_hold_boon_broker : public CreatureScript
{
public:
    npc_violet_hold_boon_broker() : CreatureScript("npc_violet_hold_boon_broker") { }

    CreatureAI* GetAI(Creature* creature) const override
    {
        return new npc_violet_hold_boon_brokerAI(creature);
    }
};

// `.vhr broker`: a private, persistent test draft containing every boon the
// invoking GM can legally take at that moment. It deliberately uses CanTake,
// Take, and the normal gossip interaction so it tests the production grant
// path. One successful pick does not consume this special broker.
class violet_hold_boon_commands : public CommandScript
{
public:
    violet_hold_boon_commands() : CommandScript("violet_hold_boon_commands") { }

    ChatCommandTable GetCommands() const override
    {
        // Shop testing. All of them act on the run you are in.
        static ChatCommandTable shopTable =
        {
            { "start",  HandleShopStart,  rbac::RBAC_PERM_COMMAND_GM, Console::No },
            { "shards", HandleShopShards, rbac::RBAC_PERM_COMMAND_GM, Console::No },
            { "wave",   HandleShopWave,   rbac::RBAC_PERM_COMMAND_GM, Console::No },
            { "reroll", HandleShopReroll, rbac::RBAC_PERM_COMMAND_GM, Console::No },
            { "info",   HandleShopInfo,   rbac::RBAC_PERM_COMMAND_GM, Console::No },
        };
        static ChatCommandTable vhrTable =
        {
            { "broker", HandleBroker, rbac::RBAC_PERM_COMMAND_GM, Console::No },
            { "shop",   shopTable },
            { "event",  HandleEvent,  rbac::RBAC_PERM_COMMAND_GM, Console::No },
            { "next",   HandleNext,   rbac::RBAC_PERM_COMMAND_GM, Console::No },
            // Balancing: no run needed, so they work from the console too.
            { "tune",   HandleTune,   rbac::RBAC_PERM_COMMAND_GM, Console::Yes },
            { "boon",   HandleBoon,   rbac::RBAC_PERM_COMMAND_GM, Console::Yes },
        };
        static ChatCommandTable commandTable =
        {
            { "vhr", vhrTable },
        };
        return commandTable;
    }

    static bool HandleBroker(ChatHandler* handler)
    {
        WorldSession* session = handler->GetSession();
        Player* player = session ? session->GetPlayer() : nullptr;
        BattlegroundVHR* run = RunOfPlayer(player);
        if (!player || !MayTrade(player, run))
        {
            handler->SendSysMessage("You must be a real party member inside an active Violet Hold run.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        auto old = TestBrokerByOwner.find(player->GetGUID());
        if (old != TestBrokerByOwner.end())
        {
            if (Creature* broker = ObjectAccessor::GetCreature(*player, old->second))
            {
                Battleground::SendSquadFeedLine(player, "VHBOON:GONE:" + BrokerTag(broker->GetGUID()));
                broker->DespawnOrUnsummon();
            }
            TestBrokerByOwner.erase(old);
        }

        Position spot = player->GetNearPosition(2.0f, 0.0f);
        spot.SetOrientation(spot.GetAbsoluteAngle(player));
        TempSummon* broker = player->SummonCreature(NPC_BOON_BROKER, spot, TEMPSUMMON_MANUAL_DESPAWN);
        npc_violet_hold_boon_brokerAI* ai = broker ? dynamic_cast<npc_violet_hold_boon_brokerAI*>(broker->AI()) : nullptr;
        if (!ai)
        {
            if (broker)
                broker->DespawnOrUnsummon();
            handler->SendSysMessage("The Violet Hold boon broker could not be spawned.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        uint32 const count = ai->ConfigureForTesting(player);
        if (!count)
        {
            broker->DespawnOrUnsummon();
            handler->SendSysMessage("You currently have no eligible boons to test.");
            return true;
        }

        TestBrokerByOwner[player->GetGUID()] = broker->GetGUID();
        handler->PSendSysMessage("Spawned a private test broker with %u eligible boons. It remains after each pick; run .vhr broker again to replace it.", count);
        return true;
    }

    // The run the command's user is a party member of, or nullptr (with the
    // reason sent). With needShop, also nullptr unless the run has the shop.
    static BattlegroundVHR* ShopRun(ChatHandler* handler, bool needShop = true)
    {
        WorldSession* session = handler->GetSession();
        Player* player = session ? session->GetPlayer() : nullptr;
        BattlegroundVHR* run = RunOfPlayer(player);
        if (!player || !MayTrade(player, run))
        {
            handler->SendSysMessage("You must be a real party member inside an active Violet Hold run.");
            handler->SetSentErrorMessage(true);
            return nullptr;
        }
        if (needShop && !run->UsesShop())
        {
            handler->SendSysMessage("This run has no shop. .vhr shop start opens one.");
            handler->SetSentErrorMessage(true);
            return nullptr;
        }
        return run;
    }

    // `.vhr tune`                 every knob and its value
    // `.vhr tune <filter>`        the knobs whose name contains it (shop, event, release...)
    // `.vhr tune <knob> <value>`  set it live (until restart); "reset" undoes it
    // `.vhr tune reset`           drop every live override (knobs and boons)
    static bool HandleTune(ChatHandler* handler, Optional<std::string> knob, Optional<std::string> value)
    {
        if (knob && value)
        {
            handler->SendSysMessage(VioletHoldTuning::Set(*knob, *value).c_str());
            return true;
        }
        if (knob && *knob == "reset")
        {
            VioletHoldTuning::ResetAll();
            handler->SendSysMessage("Every live Violet Hold override is gone; config values and defaults apply.");
            return true;
        }
        std::vector<std::string> lines;
        VioletHoldTuning::List(knob ? *knob : std::string(), lines);
        if (lines.empty())
            handler->SendSysMessage("No knob matches that. .vhr tune lists them all.");
        for (std::string const& line : lines)
            handler->SendSysMessage(line.c_str());
        return true;
    }

    // `.vhr boon <name>`                    its weight and shop price
    // `.vhr boon <name> weight <w>`         roll weight (8-10 = plain common, 0 = never offered)
    // `.vhr boon <name> price <shards>`     its shop price
    // `.vhr boon <name> tier <1-6>`         its card tier: colour, price, and odds (1 common .. 4 epic .. 6 artifact)
    // `.vhr boon <name> reset`
    static bool HandleBoon(ChatHandler* handler, std::string name, Optional<std::string> what, Optional<std::string> value)
    {
        handler->SendSysMessage(VioletHoldTuning::BoonCommand(name, what ? *what : std::string(), value ? *value : std::string()).c_str());
        return true;
    }

    // `.vhr next`: start the next wave now (BattlegroundVHR::SkipToNextWave).
    static bool HandleNext(ChatHandler* handler)
    {
        BattlegroundVHR* run = ShopRun(handler, false);
        if (!run)
            return false;
        handler->SendSysMessage(run->SkipToNextWave());
        return true;
    }

    // `.vhr event <name>`: the next wave of the run you are in gets this wave
    // event, whatever the roll would have said (bloodmoon, quicksilver,
    // giants, swarm, goldrush, bounty, mirror).
    static bool HandleEvent(ChatHandler* handler, std::string name)
    {
        BattlegroundVHR* run = ShopRun(handler, false);
        if (!run)
            return false;

        std::string key;
        for (char c : name)
            if (c != ' ' && c != '_' && c != '-')
                key += char(std::tolower(static_cast<unsigned char>(c)));

        static std::pair<char const*, VhrWaveEvent> const kNames[] =
        {
            { "bloodmoon", VhrWaveEvent::BloodMoon }, { "quicksilver", VhrWaveEvent::Quicksilver },
            { "giants", VhrWaveEvent::Giants }, { "swarm", VhrWaveEvent::Swarm },
            { "goldrush", VhrWaveEvent::GoldRush }, { "bounty", VhrWaveEvent::Bounty },
            { "mirror", VhrWaveEvent::MirrorMatch }, { "mirrormatch", VhrWaveEvent::MirrorMatch },
            { "doppelganger", VhrWaveEvent::Doppelganger }, { "doppel", VhrWaveEvent::Doppelganger },
        };
        for (auto const& [label, event] : kNames)
            if (key == label)
            {
                run->ForceNextWaveEvent(event);
                handler->PSendSysMessage("The next wave will be %s.", BattlegroundVHR::GetWaveEventName(event));
                return true;
            }

        handler->SendSysMessage("Events: bloodmoon, quicksilver, giants, swarm, goldrush, bounty, mirror, doppelganger.");
        handler->SetSentErrorMessage(true);
        return false;
    }

    // `.vhr shop start`: this run becomes a shop run now, even with
    // Centurion.VioletHold.Shop.Enable off - so the shop can be tried on a
    // realm without turning it on for everybody.
    static bool HandleShopStart(ChatHandler* handler)
    {
        BattlegroundVHR* run = ShopRun(handler, false);
        if (!run)
            return false;
        if (!run->StartShop())
        {
            handler->SendSysMessage("This run already has the shop.");
            return true;
        }
        handler->PSendSysMessage("The shop is open for this run: the broker stands on the entrance landing with %u cards. No more brokers will come this run.",
            uint32(run->GetShop().GetSlots().size()));
        return true;
    }

    // `.vhr shop shards <amount>`: give your target (a party member) or
    // yourself shards; a negative amount takes them away.
    static bool HandleShopShards(ChatHandler* handler, int32 amount)
    {
        BattlegroundVHR* run = ShopRun(handler);
        if (!run)
            return false;

        Player* target = handler->getSelectedPlayerOrSelf();
        if (!target || !MayTrade(target, run))
            target = handler->GetSession()->GetPlayer();

        VioletHoldShop& shop = run->GetShop();
        if (amount > 0)
            shop.AddShards(target->GetGUID(), uint32(amount));
        else if (amount < 0)
            shop.Spend(target->GetGUID(), std::min<uint32>(uint32(-int64(amount)), shop.GetShards(target->GetGUID())));

        handler->PSendSysMessage("%s now has %u shards.", target->GetName().c_str(), shop.GetShards(target->GetGUID()));
        run->NotifyShopChanged();
        return true;
    }

    // `.vhr shop wave`: what a cleared wave does to the shop, without
    // clearing one - everybody is paid Shop.ShardsPerWave and every card
    // rerolls or gets cheaper.
    static bool HandleShopWave(ChatHandler* handler)
    {
        BattlegroundVHR* run = ShopRun(handler);
        if (!run)
            return false;

        VioletHoldShop& shop = run->GetShop();
        std::vector<Player const*> roster;
        run->CollectHumanPlayers(roster);

        std::vector<VioletHoldShop::Slot> const before = shop.GetSlots();
        uint32 interest = 0;
        uint32 paidMe = 0;
        for (Player const* member : roster)
            if (member)
            {
                uint32 const paid = shop.PayWave(member, interest);
                if (member == handler->GetSession()->GetPlayer())
                    paidMe = paid;
            }
        shop.AgeBoard(roster);

        uint32 rerolled = 0, cheaper = 0;
        std::vector<VioletHoldShop::Slot> const& after = shop.GetSlots();
        for (size_t i = 0; i < after.size() && i < before.size(); ++i)
        {
            if (!before[i].filled || !after[i].filled)
                continue;
            if (!(before[i].offer == after[i].offer))
                ++rerolled;
            else if (after[i].discounts > before[i].discounts)
                ++cheaper;
        }

        handler->PSendSysMessage("Shop wave: everyone was paid a wave (you got %u shards); %u card(s) rerolled, %u got %u%% cheaper.",
            paidMe, rerolled, cheaper, VioletHoldShop::GetDiscountPercent());
        run->NotifyShopChanged();
        return true;
    }

    // `.vhr shop reroll`: a whole new board now.
    static bool HandleShopReroll(ChatHandler* handler)
    {
        BattlegroundVHR* run = ShopRun(handler);
        if (!run)
            return false;

        VioletHoldShop& shop = run->GetShop();
        std::vector<Player const*> roster;
        run->CollectHumanPlayers(roster);
        for (size_t i = 0; i < shop.GetSlots().size(); ++i)
            shop.Restock(roster, i);

        handler->SendSysMessage("The shop board is redrawn.");
        run->NotifyShopChanged();
        return true;
    }

    // `.vhr shop info`: the board and everybody's shards.
    static bool HandleShopInfo(ChatHandler* handler)
    {
        BattlegroundVHR* run = ShopRun(handler);
        if (!run)
            return false;

        VioletHoldShop const& shop = run->GetShop();
        handler->PSendSysMessage("Shop, wave %u: %u slots; %u shards a wave, %u a killing blow; %u%% reroll, %u%% cheaper.",
            run->GetWaveNumber(), uint32(shop.GetSlots().size()), VioletHoldShop::GetShardsPerWave(),
            VioletHoldShop::GetShardsPerKillingBlow(), VioletHoldShop::GetRerollChancePercent(), VioletHoldShop::GetDiscountPercent());

        uint32 index = 0;
        for (VioletHoldShop::Slot const& slot : shop.GetSlots())
        {
            ++index;
            if (!slot.filled)
            {
                handler->PSendSysMessage("  %u. (empty)", index);
                continue;
            }
            std::string const locker = LockerName(slot);
            handler->PSendSysMessage("  %u. %s - %u shards (full %u, %u wave(s) discounted)%s", index, OfferName(slot.offer),
                VioletHoldShop::GetPrice(slot), slot.basePrice, uint32(slot.discounts),
                locker.empty() ? "" : (" - locked by " + locker).c_str());
        }

        std::vector<Player const*> roster;
        run->CollectHumanPlayers(roster);
        for (Player const* member : roster)
            if (member)
                handler->PSendSysMessage("  %s: %u shards", member->GetName().c_str(), shop.GetShards(member->GetGUID()));
        return true;
    }
};

// Boon of Flurry (90238): SPELL_AURA_PROC_TRIGGER_SPELL firing 90250 (add one
// extra attack) at 1% per stack. spell_proc gives it melee-only proc flags
// at 100% and this script supplies the real chance from the stacked amount.
class spell_vhr_boon_flurry : public AuraScript
{
    PrepareAuraScript(spell_vhr_boon_flurry);

    bool CheckProc(ProcEventInfo& /*eventInfo*/)
    {
        AuraEffect const* eff = GetEffect(EFFECT_0);
        return eff && roll_chance_i(eff->GetAmount());
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_vhr_boon_flurry::CheckProc);
    }
};

namespace
{
// The T2 set wrappers: Shadow Bolt (Felflame Bolt 90318, custom_t2_shaman_
// warlock.cpp) and Serpent Sting (Cinderbite 90334, custom_t2_druid_hunter.cpp)
// are no longer what the class learns. The spellbook holds a DUMMY wrapper per
// rank; its hit casts the real damage as an INNER spell - the stock rank or a
// clone of it - which nobody "knows". Echoes keys on the spellbook, so without
// this table it would skip the wrapper (a dummy, nothing to double - correct)
// and then skip the inner cast too (unknown id - wrong), and neither spell
// would ever echo for anyone, wearer or not. Inner id -> the wrapper that
// stands for it in the spellbook: the shared table in WrappedSpells.h, which
// since every class holds wrappers also covers Consecration, Sprint, Purge and
// the totems - stock spells that used to echo as known ranks.
uint32 WrapperForInnerSpell(uint32 spellId)
{
    return WrappedSpells::GetWrapper(spellId);
}

// Set for the duration of the echo's own CastSpell. TRIGGERED_FULL_MASK casts
// directly, so the echoed spell's cast-phase proc - which is the very event
// Echoes listens to, carrying the very id that just passed CheckProc - arrives
// here synchronously, while HandleProc is still on the stack. The plain
// "triggered casts never echo" rule used to be the whole guard; now that the
// wrapped inner spells are allowed through it triggered, the echo of one of
// them would echo itself again. thread_local because map updates run on a
// thread pool: a cast never crosses threads, but two maps echo concurrently.
thread_local bool s_echoInFlight = false;
}

// Boon of Echoes (90239): a dummy with cast-phase proc flags for damage-class
// magic/none spells. When it fires, the spell that was just cast is cast again
// at the same targets, free and instant.
class spell_vhr_boon_echoes : public AuraScript
{
    PrepareAuraScript(spell_vhr_boon_echoes);

    static bool IsDamageOrHeal(SpellInfo const* info)
    {
        for (SpellEffectInfo const& effect : info->GetEffects())
        {
            if (!effect.IsEffect())
                continue;

            switch (effect.Effect)
            {
                case SPELL_EFFECT_SCHOOL_DAMAGE:
                case SPELL_EFFECT_HEALTH_LEECH:
                case SPELL_EFFECT_HEAL:
                case SPELL_EFFECT_HEAL_PCT:
                case SPELL_EFFECT_HEAL_MECHANICAL:
                    return true;
                case SPELL_EFFECT_APPLY_AURA:
                case SPELL_EFFECT_APPLY_AREA_AURA_PARTY:
                case SPELL_EFFECT_APPLY_AREA_AURA_RAID:
                    switch (effect.ApplyAuraName)
                    {
                        case SPELL_AURA_PERIODIC_DAMAGE:
                        case SPELL_AURA_PERIODIC_DAMAGE_PERCENT:
                        case SPELL_AURA_PERIODIC_HEAL:
                        case SPELL_AURA_PERIODIC_LEECH:
                        case SPELL_AURA_OBS_MOD_HEALTH:
                            return true;
                        default:
                            break;
                    }
                    break;
                default:
                    break;
            }
        }
        return false;
    }

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        Spell const* spell = eventInfo.GetProcSpell();
        SpellInfo const* info = eventInfo.GetSpellInfo();
        if (!spell || !info)
            return false;

        // Never echo an echo (see s_echoInFlight).
        if (s_echoInFlight)
            return false;

        // Item uses (potions, healthstones, explosives) are NOT triggered casts
        // and would otherwise be doubled for free - out. A consumed charge has
        // already nulled m_CastItem by the time the cast-phase proc runs
        // (TakeCastItem precedes it), so the reliable test is the spellbook
        // below: only a spell the caster actually knows may echo.
        if (spell->m_CastItem || spell->m_castItemGUID)
            return false;

        Player const* casterPlayer = GetTarget() ? GetTarget()->ToPlayer() : nullptr;
        if (!casterPlayer)
            return false;

        // Leave triggered casts (procs, other scripts' inner casts) alone. The
        // one exception is the inner cast of a wrapped rank (WrapperForInnerSpell):
        // the Serpent Sting wrapper launches its sting TRIGGERED_FULL_MASK, and
        // that sting IS the hunter's cast - it has to be allowed through, and
        // only it. (Shadow Bolt's wrapper casts its clone TRIGGERED_NONE, so
        // the clone never needed this; the exception is harmless for it.)
        // NB: Aura::GetProcEffectMask drops triggered sources before any script
        // runs unless 90239's spell_proc row carries PROC_ATTR_TRIGGERED_CAN_PROC
        // - the Serpent Sting half of this depends on that row.
        uint32 const wrapper = WrapperForInnerSpell(info->Id);
        if (spell->IsTriggered() && !wrapper)
            return false;

        // The spellbook test. A wrapped inner spell is never known itself; the
        // wrapper that stands for it is.
        if (!casterPlayer->HasActiveSpell(info->Id) && !(wrapper && casterPlayer->HasActiveSpell(wrapper)))
            return false;

        if (info->IsChanneled() || info->IsPassive() || info->NeedsComboPoints())
            return false;

        if (IsBoonSpell(info->Id))
            return false;

        if (!IsDamageOrHeal(info))
            return false;

        AuraEffect const* eff = GetEffect(EFFECT_0);
        return eff && roll_chance_i(eff->GetAmount());
    }

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();

        Spell const* spell = eventInfo.GetProcSpell();
        Unit* caster = GetTarget();
        if (!spell || !caster)
            return;

        // For a wrapped rank this is the INNER spell (clone or stock rank): the
        // wrapper itself was refused by IsDamageOrHeal, so the echo re-fires
        // the damage without a second dummy, cost, or cast bar.
        SpellCastTargets targets = spell->m_targets;
        s_echoInFlight = true;
        caster->CastSpell(std::move(targets), spell->GetSpellInfo()->Id, TRIGGERED_FULL_MASK);
        s_echoInFlight = false;
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_vhr_boon_echoes::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_vhr_boon_echoes::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Boon of Ascension (90247): the marker whose base amount is the level to go
// back to. VioletHoldBoons::StripAll does the rollback deliberately, before
// removing this; this is the safety net for the marker vanishing any other
// way while the character is in the world (a GM .unaura, a spell reset).
// A LOGOUT must not trigger it - the levels have to survive a relog into the
// run - and in this fork logout cleanup removes auras BEFORE RemoveFromWorld
// (Unit::CleanupBeforeRemoveFromMap -> DefensiveCleanupAurasBeforeDelete),
// after the character was already saved, so IsInWorld alone is not enough:
// the session's logout flag is what says "this is a logout".
class spell_vhr_boon_ascension : public AuraScript
{
    PrepareAuraScript(spell_vhr_boon_ascension);

    void HandleRemove(AuraEffect const* aurEff, AuraEffectHandleModes /*mode*/)
    {
        Player* player = GetTarget() ? GetTarget()->ToPlayer() : nullptr;
        if (!player || !player->IsInWorld())
            return;

        WorldSession const* session = player->GetSession();
        if (!session || session->PlayerLogout())
            return;

        int32 const baseLevel = aurEff->GetBaseAmount();
        if (baseLevel > 0 && baseLevel < int32(player->GetLevel()))
            RollbackBorrowedLevels(player, uint8(baseLevel));
    }

    void Register() override
    {
        AfterEffectRemove += AuraEffectRemoveFn(spell_vhr_boon_ascension::HandleRemove, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Boon Broker's Cache (90257): each effect's base amount is the low guid of a
// weapon the broker handed out. Same safety net as the Ascension marker: if
// the aura goes any way other than StripAll while the character is in the
// world (and not logging out), the weapon goes with it - otherwise a stray
// `.unaura all` would leave someone a permanent Thunderfury.
class spell_vhr_boon_cache : public AuraScript
{
    PrepareAuraScript(spell_vhr_boon_cache);

    void HandleRemove(AuraEffect const* aurEff, AuraEffectHandleModes /*mode*/)
    {
        Player* player = GetTarget() ? GetTarget()->ToPlayer() : nullptr;
        if (!player || !player->IsInWorld())
            return;

        WorldSession const* session = player->GetSession();
        if (!session || session->PlayerLogout())
            return;

        DestroyGrantedItemByGuidLow(player, uint32(aurEff->GetBaseAmount()));
    }

    void Register() override
    {
        AfterEffectRemove += AuraEffectRemoveFn(spell_vhr_boon_cache::HandleRemove, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_vhr_boon_cache::HandleRemove, EFFECT_1, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_vhr_boon_cache::HandleRemove, EFFECT_2, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

namespace
{
// Living hostile units within `radius` of `around` that `attacker` may hit,
// excluding `around`. Same rule as the game-lib Overkill splash.
std::vector<Unit*> HostilesAround(Unit* around, Unit* attacker, float radius)
{
    std::vector<Unit*> out;
    if (!around || !attacker || !around->IsInWorld())
        return out;

    std::list<Unit*> found;
    Trinity::AnyUnfriendlyUnitInObjectRangeCheck check(around, attacker, radius);
    Trinity::UnitListSearcher<Trinity::AnyUnfriendlyUnitInObjectRangeCheck> searcher(around, found, check);
    Cell::VisitAllObjects(around, searcher, radius);

    for (Unit* unit : found)
        if (unit != around && unit->IsAlive() && attacker->IsValidAttackTarget(unit))
            out.push_back(unit);
    return out;
}

// The proc-fired spells never feed the procs again (they carry
// CANT_TRIGGER_PROC too; this is the belt to that brace).
bool IsBoonTriggeredSpell(SpellInfo const* spellInfo)
{
    if (!spellInfo)
        return false;
    switch (spellInfo->Id)
    {
        case SPELL_RICOCHET_DAMAGE:
        case SPELL_OVERKILL_DAMAGE:
        case SPELL_REFLECTION_DAMAGE:
        case SPELL_VAMPIRE_HEAL:
        case SPELL_BOON_EXTRA_ATTACK_SWING:
            return true;
        default:
            return false;
    }
}

uint32 ProcDamage(ProcEventInfo const& eventInfo)
{
    DamageInfo const* damageInfo = eventInfo.GetDamageInfo();
    return damageInfo ? damageInfo->GetDamage() : 0;
}
}

// 90288 Boon of Ricochet: stacks% chance for a damaging hit to leap to the
// nearest other enemy within RICOCHET_RADIUS of the victim for half.
class spell_vhr_boon_ricochet : public AuraScript
{
    PrepareAuraScript(spell_vhr_boon_ricochet);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        if (IsBoonTriggeredSpell(eventInfo.GetSpellInfo()))
            return false;
        // The boons are death-persistent and procs run after Kill(): a hit
        // that lands on a holder's corpse must not fire from it.
        if (!GetTarget()->IsAlive())
            return false;
        if (!ProcDamage(eventInfo) || !eventInfo.GetProcTarget())
            return false;
        return roll_chance_i(GetStackAmount());
    }

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();

        Unit* caster = GetTarget();
        Unit* victim = eventInfo.GetProcTarget();
        uint32 const damage = ProcDamage(eventInfo);
        if (!caster || !victim || !damage)
            return;

        Unit* next = nullptr;
        float best = 0.0f;
        for (Unit* candidate : HostilesAround(victim, caster, RICOCHET_RADIUS))
        {
            float const dist = victim->GetExactDist2d(candidate);
            if (!next || dist < best)
            {
                next = candidate;
                best = dist;
            }
        }
        if (!next)
            return;

        CastSpellExtraArgs args(TRIGGERED_FULL_MASK);
        args.AddSpellBP0(int32(std::max<uint32>(1, damage / 2)));
        caster->CastSpell(next, SPELL_RICOCHET_DAMAGE, args);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_vhr_boon_ricochet::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_vhr_boon_ricochet::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// 90290 Boon of Reflection: stacks% of any damage taken (physical or magic,
// direct or periodic - PROC_FLAG_TAKEN_DAMAGE) thrown back at the attacker.
class spell_vhr_boon_reflection : public AuraScript
{
    PrepareAuraScript(spell_vhr_boon_reflection);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        if (IsBoonTriggeredSpell(eventInfo.GetSpellInfo()))
            return false;
        Unit* attacker = eventInfo.GetActor();
        return GetTarget()->IsAlive() && attacker && attacker != GetTarget() && attacker->IsAlive() && ProcDamage(eventInfo);
    }

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();

        Unit* attacker = eventInfo.GetActor();
        uint32 const reflected = CalculatePct(ProcDamage(eventInfo), uint32(GetStackAmount()));
        if (!attacker || !reflected)
            return;

        CastSpellExtraArgs args(TRIGGERED_FULL_MASK);
        args.AddSpellBP0(int32(reflected));
        GetTarget()->CastSpell(attacker, SPELL_REFLECTION_DAMAGE, args);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_vhr_boon_reflection::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_vhr_boon_reflection::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// 90291 Boon of the Vampire: stacks% of damage dealt returned as health.
class spell_vhr_boon_vampire : public AuraScript
{
    PrepareAuraScript(spell_vhr_boon_vampire);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        if (IsBoonTriggeredSpell(eventInfo.GetSpellInfo()))
            return false;
        return GetTarget()->IsAlive() && ProcDamage(eventInfo) > 0;
    }

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();

        uint32 const heal = CalculatePct(ProcDamage(eventInfo), uint32(GetStackAmount()));
        if (!heal)
            return;

        CastSpellExtraArgs args(TRIGGERED_FULL_MASK);
        args.AddSpellBP0(int32(heal));
        GetTarget()->CastSpell(GetTarget(), SPELL_VAMPIRE_HEAL, args);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_vhr_boon_vampire::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_vhr_boon_vampire::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// --- third batch ------------------------------------------------------------
// Every proc below has a spell_proc row (damage done, direct and periodic, and
// for Divinity and Clearcasting healing done too; triggered spells may proc).

namespace
{
// The healing a proc event carries: all of it, or only what landed (not overheal).
uint32 ProcHeal(ProcEventInfo const& eventInfo, bool effectiveOnly)
{
    HealInfo const* healInfo = eventInfo.GetHealInfo();
    if (!healInfo)
        return 0;
    return effectiveOnly ? healInfo->GetEffectiveHeal() : healInfo->GetHeal();
}
}

// 90413 Boon of Divinity: stacks% of the damage and the effective healing the
// holder does goes into an absorb shield (90419) on the holder, which never
// holds more than DIVINITY_SHIELD_CAP_PCT of max health. Each top-up restarts
// its 30 sec. A shield that is used up is gone, and the next proc starts a new one.
class spell_vhr_boon_divinity : public AuraScript
{
    PrepareAuraScript(spell_vhr_boon_divinity);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        if (IsBoonTriggeredSpell(eventInfo.GetSpellInfo()))
            return false;
        if (!GetTarget()->IsAlive())
            return false;
        return ProcDamage(eventInfo) > 0 || ProcHeal(eventInfo, true) > 0;
    }

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();

        Unit* holder = GetTarget();
        uint32 const done = ProcDamage(eventInfo) + ProcHeal(eventInfo, true);
        uint32 const gain = CalculatePct(done, uint32(GetStackAmount()));
        uint32 const cap = holder->CountPctFromMaxHealth(DIVINITY_SHIELD_CAP_PCT);
        if (!gain || !cap)
            return;

        if (AuraEffect* shield = holder->GetAuraEffect(SPELL_DIVINITY_SHIELD, EFFECT_0, holder->GetGUID()))
        {
            int64 const now = shield->GetAmount();
            int64 const next = std::min<int64>(now + gain, int64(cap));
            if (next > now)
                shield->ChangeAmount(int32(next));
            shield->GetBase()->RefreshDuration();
            return;
        }

        CastSpellExtraArgs args(TRIGGERED_FULL_MASK);
        args.AddSpellBP0(int32(std::min(gain, cap)));
        holder->CastSpell(holder, SPELL_DIVINITY_SHIELD, args);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_vhr_boon_divinity::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_vhr_boon_divinity::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// 90414 Boon of Clearcasting: stacks% chance on damage or healing done to put
// the class's own Clearcasting (90401-90411, a -100% cost spellmod with one
// charge) on the holder. It is separate from Omen of Clarity and Arcane
// Concentration - its own roll and its own buff. The buff's spell_proc row
// spends the charge on the next costed spell the mod actually changed.
class spell_vhr_boon_clearcasting : public AuraScript
{
    PrepareAuraScript(spell_vhr_boon_clearcasting);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        if (IsBoonTriggeredSpell(eventInfo.GetSpellInfo()))
            return false;
        if (!GetTarget()->IsAlive())
            return false;
        if (!ProcDamage(eventInfo) && !ProcHeal(eventInfo, false))
            return false;
        return roll_chance_i(GetStackAmount());
    }

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& /*eventInfo*/)
    {
        PreventDefaultAction();

        Unit* holder = GetTarget();
        if (uint32 const buff = GetClearcastingBuffFor(holder->GetClass()))
            holder->CastSpell(holder, buff, TRIGGERED_FULL_MASK);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_vhr_boon_clearcasting::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_vhr_boon_clearcasting::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// 90415 Boon of the Viper: stacks% of the damage dealt comes back to the
// holder as mana, from any target. It is an energize logged under Viper
// Drain (90395), so the exact gain shows in the combat log and as "+N Mana"
// combat text. It used to drain the target's mana instead, which gave
// nothing against targets without mana and was cut by resilience and the
// target's damage-taken modifiers - far less than the boon promised.
class spell_vhr_boon_viper : public AuraScript
{
    PrepareAuraScript(spell_vhr_boon_viper);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        if (IsBoonTriggeredSpell(eventInfo.GetSpellInfo()))
            return false;
        Unit* holder = GetTarget();
        return holder->IsAlive() && holder->GetMaxPower(POWER_MANA) > 0 && ProcDamage(eventInfo) > 0;
    }

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();

        uint32 const gain = CalculatePct(ProcDamage(eventInfo), uint32(GetStackAmount()));
        SpellInfo const* logAs = sSpellMgr->GetSpellInfo(SPELL_VIPER_DRAIN);
        if (!gain || !logAs)
            return;

        Unit* holder = GetTarget();
        holder->EnergizeBySpell(holder, logAs, int32(gain), POWER_MANA);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_vhr_boon_viper::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_vhr_boon_viper::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// 90418 Boon of Aggression: every damaging hit leaves Mortal Wound (94304,
// 10 sec) on the target, stacked to the holder's own Aggression stacks at 1%
// healing received each. The stack count is what lets the client tooltip and
// icon show the real cut (a 3.3.5 client never gets aura amounts). One wound
// per holder; a hit only refreshes it and moves its stacks to the holder's.
class spell_vhr_boon_aggression : public AuraScript
{
    PrepareAuraScript(spell_vhr_boon_aggression);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        if (IsBoonTriggeredSpell(eventInfo.GetSpellInfo()))
            return false;
        Unit* victim = eventInfo.GetProcTarget();
        return GetTarget()->IsAlive() && victim && victim != GetTarget() && victim->IsAlive() && ProcDamage(eventInfo) > 0;
    }

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();

        Unit* holder = GetTarget();
        Unit* victim = eventInfo.GetProcTarget();
        uint8 const stacks = GetStackAmount();
        if (!victim || !stacks)
            return;

        Aura* wound = victim->GetAura(SPELL_AGGRESSION_WOUND, holder->GetGUID());
        if (wound)
            wound->RefreshDuration();
        else
        {
            // Instant and triggered, so the wound is on the victim (one stack)
            // when this returns - unless the victim was immune.
            holder->CastSpell(victim, SPELL_AGGRESSION_WOUND, CastSpellExtraArgs(TRIGGERED_FULL_MASK));
            wound = victim->GetAura(SPELL_AGGRESSION_WOUND, holder->GetGUID());
            if (!wound)
                return;
        }

        // SetStackAmount recomputes the amount (per-stack value x stacks) and
        // sends the new count. It does not clamp; Aggression's own cap (50)
        // matches the wound's StackAmount.
        if (wound->GetStackAmount() != stacks)
            wound->SetStackAmount(stacks);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_vhr_boon_aggression::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_vhr_boon_aggression::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

namespace
{
// What Boon of Deliverance breaks: every loss-of-control and movement
// impairing mechanic, plus silence and disarms.
constexpr uint32 DELIVERANCE_MECHANIC_MASK = IMMUNE_TO_MOVEMENT_IMPAIRMENT_AND_LOSS_CONTROL_MASK
    | (1 << MECHANIC_SILENCE) | (1 << MECHANIC_DISARM) | (1 << MECHANIC_GRIP);

// A harmful aura that is crowd control: by mechanic, or - for the many that
// carry none - by what one of its applied effects does.
bool IsDeliveranceCrowdControl(AuraApplication const* app)
{
    if (!app || app->IsPositive())
        return false;

    SpellInfo const* info = app->GetBase()->GetSpellInfo();
    if (info->GetSpellMechanicMaskByEffectMask(app->GetEffectMask()) & DELIVERANCE_MECHANIC_MASK)
        return true;

    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        if (!(app->GetEffectMask() & (1 << i)))
            continue;
        switch (info->GetEffect(SpellEffIndex(i)).ApplyAuraName)
        {
            case SPELL_AURA_MOD_STUN:
            case SPELL_AURA_MOD_ROOT:
            case SPELL_AURA_MOD_FEAR:
            case SPELL_AURA_MOD_CONFUSE:
            case SPELL_AURA_MOD_CHARM:
            case SPELL_AURA_MOD_SILENCE:
            case SPELL_AURA_MOD_PACIFY:
            case SPELL_AURA_MOD_PACIFY_SILENCE:
            case SPELL_AURA_MOD_DECREASE_SPEED:
            case SPELL_AURA_MOD_DISARM:
            case SPELL_AURA_MOD_DISARM_OFFHAND:
            case SPELL_AURA_MOD_DISARM_RANGED:
                return true;
            default:
                break;
        }
    }
    return false;
}

bool HasDeliveranceCrowdControl(Unit const* unit)
{
    for (auto const& [spellId, app] : unit->GetAppliedAuras())
        if (IsDeliveranceCrowdControl(app))
            return true;
    return false;
}

// A random living ally of `holder` in its battleground - a member of the same
// team, on the same map, never the holder. One under crowd control is picked
// first, so the boon spends itself where it matters; with nobody held, any
// ally gets the speed. Clones carry the boons they copied, so on a clone this
// frees one of the other clones - the same mirror every other boon follows.
Player* PickDeliveranceAlly(Player* holder)
{
    Battleground* bg = holder->GetBattleground();
    if (!bg)
        return nullptr;

    uint32 const team = holder->GetBGTeam();
    std::vector<Player*> held, loose;
    for (auto const& [guid, bgPlayer] : bg->GetPlayers())
    {
        if (guid == holder->GetGUID() || bgPlayer.Team != team || bgPlayer.OfflineRemoveTime)
            continue;
        Player* ally = ObjectAccessor::GetPlayer(*holder, guid);
        if (!ally || !ally->IsInWorld() || !ally->IsAlive())
            continue;
        (HasDeliveranceCrowdControl(ally) ? held : loose).push_back(ally);
    }

    std::vector<Player*> const& pool = held.empty() ? loose : held;
    if (pool.empty())
        return nullptr;
    return Trinity::Containers::SelectRandomContainerElement(pool);
}
}

// 94320 Boon of Deliverance: a critical hit or heal by the holder frees a
// random ally from all crowd control and puts 94321 (+50% run speed, 3 sec) on
// them. The spell_proc row limits it to crits (HitMask) and to once every
// 3 sec (Cooldown); a proc with no ally to help is refused here, so it does not
// spend the cooldown.
class spell_vhr_boon_deliverance : public AuraScript
{
    PrepareAuraScript(spell_vhr_boon_deliverance);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        if (!(eventInfo.GetHitMask() & PROC_HIT_CRITICAL))
            return false;
        if (IsBoonTriggeredSpell(eventInfo.GetSpellInfo()))
            return false;
        Player* holder = GetTarget()->ToPlayer();
        return holder && holder->IsAlive() && PickDeliveranceAlly(holder);
    }

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& /*eventInfo*/)
    {
        PreventDefaultAction();

        Player* holder = GetTarget()->ToPlayer();
        Player* ally = holder ? PickDeliveranceAlly(holder) : nullptr;
        if (!ally)
            return;

        ally->RemoveAppliedAuras([](AuraApplication const* app) { return IsDeliveranceCrowdControl(app); });

        // Cast by the ally on themselves, so neither range nor line of sight
        // to the holder matters; the holder is named as its source.
        CastSpellExtraArgs args(TRIGGERED_FULL_MASK);
        args.SetOriginalCaster(holder->GetGUID());
        ally->CastSpell(ally, SPELL_DELIVERANCE_SPEED, args);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_vhr_boon_deliverance::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_vhr_boon_deliverance::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Boons of Kinship and Focus Fire (the damage the holder deals) and Focus
// Fire's record of who hit what. The bonus itself is decided by
// VioletHoldBoons::GetPartyDamageBonusPct; this only applies it.
class vhr_party_boon_damage : public UnitScript
{
public:
    vhr_party_boon_damage() : UnitScript("vhr_party_boon_damage") { }

    static uint32 Boosted(Unit* attacker, Unit* target, uint32 damage)
    {
        if (!damage)
            return damage;
        int32 const pct = GetPartyDamageBonusPct(attacker, target);
        if (pct > 0)
            damage += CalculatePct(damage, pct);
        damage = VioletHoldClassBoons::ModifyMarkedPreyDamage(attacker, target, damage);
        return VioletHoldClassBoons::ModifyWarBannerDamage(attacker, target, damage);
    }

    void ModifyMeleeDamage(Unit* target, Unit* attacker, uint32& damage) override
    {
        damage = Boosted(attacker, target, damage);
        // Boon of the Ice Fortress: a melee hit on a holder in Ice Armor.
        if (damage)
            VioletHoldClassBoons::OnMeleeStruck(attacker, target);
    }

    void ModifySpellDamageTaken(Unit* target, Unit* attacker, int32& damage) override
    {
        if (damage > 0)
            damage = int32(Boosted(attacker, target, uint32(damage)));
    }

    // Periodic damage ticks.
    void ModifyPeriodicDamageAurasTick(Unit* target, Unit* attacker, uint32& damage, SpellInfo const* /*spellInfo*/) override
    {
        damage = Boosted(attacker, target, damage);
    }

    // Drain ticks - and, from the periodic HEAL handler, heal-over-time ticks
    // too, which must be left alone: only a tick on something the attacker
    // may fight is damage.
    void ModifyPeriodicDamageAurasTick(Unit* target, Unit* attacker, uint32& damage) override
    {
        if (attacker && target && attacker->IsValidAttackTarget(target))
            damage = Boosted(attacker, target, damage);
    }

    void OnDamage(Unit* attacker, Unit* victim, uint32& damage) override
    {
        if (damage)
            NoteHit(attacker, victim);
        // Boon of the Body Double: a party member about to drop below 25%.
        VioletHoldClassBoons::OnAllyDamaged(victim, damage);
        // Boon of the Guardian Beast: one about to drop below 50%.
        VioletHoldClassBoons::OnAllyStruck(attacker, victim, damage);
    }
};

void AddSC_violet_hold_boons()
{
    new vhr_party_boon_damage();
    new npc_violet_hold_boon_broker();
    new violet_hold_boon_commands();
    AddonRequestRoutes::Register("VHBOON", &HandleBoonRequest);
    RegisterSpellScript(spell_vhr_boon_flurry);
    RegisterSpellScript(spell_vhr_boon_echoes);
    RegisterSpellScript(spell_vhr_boon_ascension);
    RegisterSpellScript(spell_vhr_boon_cache);
    RegisterSpellScript(spell_vhr_boon_ricochet);
    RegisterSpellScript(spell_vhr_boon_reflection);
    RegisterSpellScript(spell_vhr_boon_vampire);
    RegisterSpellScript(spell_vhr_boon_divinity);
    RegisterSpellScript(spell_vhr_boon_clearcasting);
    RegisterSpellScript(spell_vhr_boon_viper);
    RegisterSpellScript(spell_vhr_boon_aggression);
    RegisterSpellScript(spell_vhr_boon_deliverance);
}
