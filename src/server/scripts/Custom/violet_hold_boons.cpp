/*
 * Violet Hold boons - the broker NPC and the aura scripts behind the two
 * chance-based boons and the level marker. See
 * game/Miscellaneous/VioletHoldBoons.h for the design and the boon table.
 */

#include "Battleground.h"
#include "BattlegroundVHR.h"
#include "CellImpl.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Map.h"
#include "Player.h"
#include "Random.h"
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
#include "Util.h"
#include "VioletHoldBoons.h"
#include "WorldSession.h"
#include "WrappedSpells.h"

#include <algorithm>
#include <iterator>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace VioletHoldBoons;

namespace
{
constexpr uint32 ACTION_PICK_BASE = GOSSIP_ACTION_INFO_DEF + 100;   // + uint8(Boon)

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
bool MayTrade(Player const* player, BattlegroundVHR const* run)
{
    if (!player || !run)
        return false;

    WorldSession const* session = player->GetSession();
    if (!session || session->IsTransientPlayerSession())
        return false;

    if (player->GetBattleground() != run)
        return false;

    return player->GetBGTeam() == run->GetHumanTeam();
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
//   VHBOON:DEAL:<broker>^<wave>^<final wave>^<greed>^<abundance>^<hoarder>[^<name>^<class id>]...
//   VHBOON:CARD:<broker>^<code>^<kind B|S|I>^<tier 1-5>^<run-wide>^<greed card>^<icon S<spell>|I<item>>
//               ^<name>^<headline>^<text>^<held>^<after>^<cap>^<unit>^<PickResult for you>[^<who can take it>]...
//   VHBOON:DECK:<broker>[^<tier>^<name>^<value>]...      (several lines when long)
//   VHBOON:SHOW:<broker>                                 the hand is complete
//   VHBOON:LOOK:<broker>^<name>^<code>                   a party member is looking at a card
//   VHBOON:TAKEN:<broker>^<name>^<code>                  the deal is over
//   VHBOON:GONE:<broker>                                 the broker left with nothing taken
// Addon -> server, CCGAMEREQ whispers to self:
//   VHBOON:PICK:<broker>:<code>
//   VHBOON:LOOK:<broker>:<code>
// Codes are Offer::Encode(); CODE_NONE is "looking at nothing".
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

bool IsRunWideBoon(Boon boon)
{
    return boon == Boon::Greed || boon == Boon::Hoarder || boon == Boon::Fellowship || boon == Boon::Abundance;
}

// The card's frame colour, read off the roll weight so a card that looks rare
// is rare in the pool: 1 common, 2 uncommon, 3 rare, 4 class spell, 5 legendary.
uint8 RarityTier(Offer offer)
{
    if (offer.kind == Offer::Kind::Item)
        return 5;
    if (offer.kind == Offer::Kind::ClassSpell)
        return 4;
    uint8 const weight = GetBoon(Boon(offer.index)).weight;
    return weight >= 8 ? 1 : weight >= 5 ? 2 : 3;
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
            text = "Yours for this run, soulbound. Destroyed when you leave the Hold.";
            break;
        }
        case Offer::Kind::ClassSpell:
        {
            ClassSpellInfo const& info = GetClassSpell(offer.index);
            kind = 'S';
            icon = "S" + std::to_string(ResolveRank(info.spellId, viewer->GetLevel()));
            name = info.name;
            headline = "Learn";
            char const* className = "Class";
            if (ChrClassesEntry const* entry = sChrClassesStore.LookupEntry(info.classId))
                className = entry->Name[LOCALE_enUS];
            text = Trinity::StringFormat("{} spell, taught at the best rank your level allows. Forgotten when you leave the Hold.", className);
            break;
        }
        default:
        {
            BoonInfo const& info = GetBoon(Boon(offer.index));
            icon = "S" + std::to_string(GetBoonSpellFor(info.boon, viewer->GetClass()));
            name = info.name;
            runWide = IsRunWideBoon(info.boon);
            held = GetStacks(viewer, info.boon);
            cap = info.maxStacks;
            after = std::min<uint32>(held + info.grantStacks, cap);
            unit = info.unit;
            text = info.summary;
            if (cap > 1)
                headline = Trinity::StringFormat("+{}{}", uint32(info.grantStacks), info.unit);
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
        if (IsRunWideBoon(info.boon))
            continue;
        uint8 const stacks = GetStacks(viewer, info.boon);
        if (!stacks)
            continue;

        Offer offer;
        offer.kind = Offer::Kind::Boon;
        offer.index = i;
        std::string value;
        if (info.maxStacks > 1)
            value = *info.unit ? Trinity::StringFormat("{}{}", uint32(stacks), info.unit) : Trinity::StringFormat("x{}", uint32(stacks));
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

    bool OnGossipHello(Player* player) override
    {
        BattlegroundVHR* run = GetRun(me);
        if (!MayTrade(player, run) || _consumed)
        {
            CloseGossipMenuFor(player);
            return true;
        }

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
            SendToParty(run, "VHBOON:GONE:" + BrokerTag(me->GetGUID()));
            // Frees the slot and removes the creature at the end of the
            // update. Nothing below may touch `me` after this.
            run->ConsumeBoonBroker(me->GetGUID());
            return true;
        }

        // The cards first, so the addon has them when the menu arrives.
        SendDeal(player, run, offers);

        ClearGossipMenuFor(player);
        for (Offer const& offer : offers)
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, DescribeOffer(player, offer),
                GOSSIP_SENDER_MAIN, ACTION_PICK_BASE + offer.Encode());

        SendGossipMenuFor(player, NPC_TEXT_BOON_BROKER, me->GetGUID());
        return true;
    }

    bool OnGossipSelect(Player* player, uint32 /*menuId*/, uint32 gossipListId) override
    {
        uint32 const action = player->PlayerTalkClass->GetGossipOptionAction(gossipListId);
        CloseGossipMenuFor(player);

        Offer offer;
        if (action < ACTION_PICK_BASE || !Offer::Decode(action - ACTION_PICK_BASE, offer))
            return true;

        Pick(player, offer);
        return true;
    }

    // Both ways in - a gossip line and a card - end here.
    void Pick(Player* player, Offer offer)
    {
        BattlegroundVHR* run = GetRun(me);
        if (!MayTrade(player, run) || _consumed)
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
            return;
        }

        _consumed = true;

        char const* name = offer.kind == Offer::Kind::Boon ? GetBoon(Boon(offer.index)).name
                         : offer.kind == Offer::Kind::ClassSpell ? GetClassSpell(offer.index).name
                         : GetItemGrant(offer.index).name;
        if (WorldSession* session = player->GetSession())
            session->SendNotification("You receive %s.", name);
        run->PSendMessageToAll(STRING_BOON_TAKEN, CHAT_MSG_BG_SYSTEM_NEUTRAL, nullptr, player->GetName().c_str(), name);
        SendToParty(run, Trinity::StringFormat("VHBOON:TAKEN:{}^{}^{}", BrokerTag(me->GetGUID()), Field(player->GetName()), offer.Encode()));

        // Frees the slot and removes the creature at the end of the update.
        // Nothing below may touch `me` after this.
        run->ConsumeBoonBroker(me->GetGUID());
    }

    // A party member's cursor moved to another card (CODE_NONE: put down).
    // Told to the rest of the party so two people do not reach for the same one.
    void Look(Player* player, uint32 code)
    {
        BattlegroundVHR* run = GetRun(me);
        if (!MayTrade(player, run) || _consumed)
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

        SendToParty(run, Trinity::StringFormat("VHBOON:LOOK:{}^{}^{}", BrokerTag(me->GetGUID()), Field(player->GetName()), code), player);
    }

private:
    void SendDeal(Player* viewer, BattlegroundVHR* run, std::vector<Offer> const& live)
    {
        std::string const tag = BrokerTag(me->GetGUID());
        std::vector<Player const*> roster;
        run->CollectHumanPlayers(roster);

        std::string deal = Trinity::StringFormat("VHBOON:DEAL:{}^{}^{}^{}^{}^{}", tag, run->GetWaveNumber(), uint32(BG_VHR_FINAL_WAVE),
            uint32(run->GetBrokerBonusOffers()), uint32(run->GetBonusRunesPerWave()), uint32(run->GetBonusBrokersPerWave()));
        for (Player const* member : roster)
            if (member && deal.size() < MAX_LINE - 20)
                deal += Trinity::StringFormat("^{}^{}", Field(member->GetName()), uint32(member->GetClass()));
        Battleground::SendSquadFeedLine(viewer, deal);

        // Past the first OFFERS_PER_BROKER of the roll is what Greed added.
        std::vector<Offer> const& rolled = OffersFor(run);
        for (Offer const& offer : live)
        {
            size_t const position = size_t(std::find(rolled.begin(), rolled.end(), offer) - rolled.begin());
            Battleground::SendSquadFeedLine(viewer, CardLine(viewer, tag, offer, position >= OFFERS_PER_BROKER, roster));
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
        run->CollectHumanPlayers(roster);

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
            run->CollectHumanPlayers(roster);
            // Boon of Greed widens every broker rolled after it.
            _offers = RollOffers(roster, uint8(std::min<uint32>(OFFERS_PER_BROKER + run->GetBrokerBonusOffers(), 12)));
            _rolled = true;
        }
        return _offers;
    }

    std::vector<Offer> _offers;
    std::unordered_map<ObjectGuid, uint32> _looks;
    std::unordered_map<ObjectGuid, uint32> _lookTimes;
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
    if (!ai || GetRun(broker) != run)
    {
        // Gone already (taken by someone without the addon, or retired):
        // put the sender's cards away.
        if (parts[0] == "PICK")
            Battleground::SendSquadFeedLine(sender, "VHBOON:GONE:" + std::string(parts[1]));
        return;
    }

    if (parts[0] == "LOOK")
    {
        ai->Look(sender, *code);
        return;
    }

    if (parts[0] != "PICK")
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

void AddSC_violet_hold_boons()
{
    new npc_violet_hold_boon_broker();
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
}
