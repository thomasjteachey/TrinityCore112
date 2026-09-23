/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "Mokgora.h"

#include "Chat.h"
#include "Config.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "GameObject.h"
#include "GameObjectData.h"
#include "GameTime.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SocialMgr.h"
#include "SpellAuras.h"
#include "StringFormat.h"
#include "TournamentMode.h"
#include "Util.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    // ---------------------------------------------------------------------
    // configuration
    // ---------------------------------------------------------------------
    struct MokgoraConfig
    {
        bool Enabled = false;

        // Face to face. Ten yards is a little under the range at which the
        // client lets you interact with someone, which is the distance the
        // gesture is worth: close enough that both of you chose to be there.
        float ChallengeRange = 10.0f;

        // How long the box stays up before the challenge lapses. Long enough to
        // read it twice, short enough that a forgotten offer does not fire an
        // hour later.
        uint32 OfferSeconds = 30;

        // "the large duel radius", in the wiki's words - a Duel to the Death is
        // fought on wider ground than an ordinary duel's 50 yards, and leaving
        // it is what fleeing means.
        float BoundsYards = 150.0f;
        uint32 BoundsGraceSeconds = 10;

        // Coward!, worn for three days by anyone who runs. -20% to attributes,
        // damage done, armour and resistances.
        //
        // THREE spell ids, not one. Five stats plus damage plus resistance is
        // seven effects and a 3.3.5 spell has three slots, so it is split: the
        // first id is the visible debuff and the rest are hidden companions
        // (SPELL_ATTR0_HIDDEN_CLIENTSIDE), which the client is never told about
        // and therefore needs no rows for. All of them are applied and expire
        // together, so the player sees one icon.
        std::vector<uint32> CowardSpells;
        uint32 CowardDays = 3;

        // Whether the area has to be one duels are allowed in. On by default:
        // sanctuaries and the other no-duel areas are no-duel for reasons that
        // have nothing to do with how lethal this particular duel is.
        bool RequireDuelArea = true;

        // Logging out mid-fight is fleeing, exactly as walking out of the ring
        // is, and carries the same Coward! debuff. Blizzard counts it; so do we.

        // Whether the realm hears about it.
        bool AnnounceStart = true;
        bool AnnounceEnd = true;

        // The duel flag. 21680 is the one the Duel spell (7266) itself plants
        // on this realm - read off its own EffectMiscValue rather than assumed.
        // 194 is stock TrinityCore's id and is NOT in this world's
        // gameobject_template, which is how the first attempt failed.
        uint32 FlagGameObjectId = 21680;
    };

    MokgoraConfig Config;

    // Set by the Mok'gora script once the Hardcore ruleset is up. See the
    // header: game/ cannot recognise a playerbot by itself.
    Mokgora::PlayerPredicate BotPredicate = nullptr;

    bool IsBot(Player const* player)
    {
        if (!player)
            return false;
        if (player->GetSession() && player->GetSession()->IsVirtualSession())
            return true;
        return BotPredicate && BotPredicate(player);
    }

    // ---------------------------------------------------------------------
    // outstanding challenges
    // ---------------------------------------------------------------------
    struct Offer
    {
        ObjectGuid Challenger;
        ObjectGuid Target;
        time_t Expires = 0;
    };

    // Keyed by the person who has to answer. One at a time in either
    // direction: two boxes on one screen is how somebody clicks the wrong one.
    std::unordered_map<ObjectGuid, Offer> Offers;

    Offer const* FindOfferTo(ObjectGuid target)
    {
        auto itr = Offers.find(target);
        return itr != Offers.end() ? &itr->second : nullptr;
    }

    Offer const* FindOfferFrom(ObjectGuid challenger)
    {
        for (auto const& [target, offer] : Offers)
            if (offer.Challenger == challenger)
                return &offer;
        return nullptr;
    }

    // ---------------------------------------------------------------------
    // the tally
    // ---------------------------------------------------------------------
    // A tally, and nothing else.
    //
    // There is no "finished" flag here: a Mok'gora produces a corpse and stops
    // caring. Whether that death is permanent is the CHARACTER's business,
    // settled by the rules it was already living under - which is exactly how
    // Blizzard words the warning, "death is permanent on Hardcore Realms",
    // rather than "this duel is permanent". A Hardcore character dies for good
    // through the ordinary PvP-death hooks in ChallengeModes, as it would to a
    // wolf; anybody else takes an ordinary death.
    //
    // NO PER-CHARACTER TALLY. Nobody is told, or reminded, what their record is.
    //
    // There was one, and the addon asked for it on PLAYER_ENTERING_WORLD - which
    // fires on every loading screen, so the realm greeted you with "Mok'gora: 0
    // won, 0 lost" every time you took a boat. A scoreboard nobody asked for,
    // read out on a loop. The whole thing is gone rather than merely quietened:
    // no counters, no `character_mokgora`, no STATS verb on the wire.
    //
    // `mokgora_log` below still records that a match happened, for whoever has
    // to answer "what did this corpse come from". It never speaks to a player.

    // Whether `mokgora_log` is actually there. A realm with the build but not
    // the schema still plays Mok'gora - it simply keeps no history - and above
    // all does not take the worldserver down with a query against a table that
    // does not exist.
    bool HistoryPersisted = false;

    void ProbeSchema()
    {
        HistoryPersisted = false;

        QueryResult result = CharacterDatabase.Query(
            "SELECT COUNT(*) FROM `information_schema`.`TABLES` "
            "WHERE `TABLE_SCHEMA` = DATABASE() AND `TABLE_NAME` = 'mokgora_log'");

        if (result && (*result)[0].GetUInt64() > 0)
        {
            HistoryPersisted = true;
            return;
        }

        TC_LOG_WARN("server.loading", "Mok'gora: `mokgora_log` is missing from the characters "
            "database - the duel works, but no history of one survives a restart.");
    }

    // ---------------------------------------------------------------------
    // talking
    // ---------------------------------------------------------------------
    void SendAddonLine(Player* player, std::string const& message)
    {
        if (!player || !player->GetSession() || player->GetSession()->IsVirtualSession())
            return;

        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player->GetGUID(), player->GetGUID(),
            message, 0, player->GetName(), player->GetName());
        player->SendDirectMessage(&data);
    }

    void Say(Player* player, std::string const& line)
    {
        if (player && player->GetSession())
            ChatHandler(player->GetSession()).SendSysMessage(line.c_str());
    }

    void Announce(std::string const& line)
    {
        sWorld->SendGlobalText(line.c_str(), nullptr);
    }

    // The realm's Mok'gora voice. Kept in one place so the colour of it is the
    // same wherever it speaks.
    std::string Banner(std::string const& text)
    {
        return "|cffff2020[Mok'gora]|r " + text;
    }

    void SendRules(Player* player)
    {
        SendAddonLine(player, Trinity::StringFormat("CCGAME\tMOKGORA:RULES:{}:{}:{}:{}:{}",
            Config.Enabled ? 1 : 0, uint32(Config.ChallengeRange), Config.OfferSeconds,
            uint32(Config.BoundsYards), Config.CowardDays));
    }

    void CloseBox(Player* player, char const* reason)
    {
        SendAddonLine(player, Trinity::StringFormat("CCGAME\tMOKGORA:CLOSE:{}", reason));
    }

    // ---------------------------------------------------------------------
    // the gates
    // ---------------------------------------------------------------------

    // Every reason a challenge cannot be issued, in the order they are asked.
    // Returns an empty string when it can. The text is written to be read by
    // the player who typed the command, so it says what THEY should do.
    std::string WhyNot(Player* challenger, Player* target)
    {
        if (!Config.Enabled)
            return "Mok'gora is not enabled on this realm.";

        if (!challenger || !target)
            return "There is nobody there.";

        if (challenger == target)
            return "You cannot challenge yourself.";

        // A person may throw it at a bot, and the bot takes it up - see
        // Challenge. A bot never throws one: nothing would be choosing to.
        if (IsBot(challenger))
            return "A bot cannot issue a Mok'gora.";

        // Nor a transient copy - a bounty hunter, a battleground fill, a lobby
        // mannequin. Nobody stands behind it, and it is retired seconds after
        // it dies, so there is no corpse to settle anything over.
        if (target->GetSession() && target->GetSession()->IsTransientPlayerSession())
            return Trinity::StringFormat("{} is not here to stay. There is nothing to settle with them.",
                target->GetName());

        if (challenger->IsGameMaster() || target->IsGameMaster())
            return "A Game Master may not take part in a Mok'gora.";

        if (!challenger->IsAlive())
            return "You are dead.";
        if (!target->IsAlive())
            return Trinity::StringFormat("{} is dead.", target->GetName());

        // No level gate on the FIGHT, and none anywhere else either. Blizzard
        // hangs its level-10 floor and its level-gap rule on the String of
        // Ears, and this realm does not grant one - so there is nothing left
        // for those limits to apply to. Anyone may challenge anyone.

        if (challenger->duel || target->duel)
            return "One of you is already in a duel.";

        if (FindOfferFrom(challenger->GetGUID()))
            return "You already have a challenge outstanding. Withdraw it first.";
        if (FindOfferTo(challenger->GetGUID()))
            return "Answer the challenge you have been given first.";
        if (FindOfferTo(target->GetGUID()) || FindOfferFrom(target->GetGUID()))
            return Trinity::StringFormat("{} is already settling a Mok'gora.", target->GetName());

        // Deliberately NOT gated on combat, and not on the Gurubashi ring.
        //
        // The rule is "anywhere you could have an ordinary duel", so the only
        // place checks allowed here are the ones Spell::EffectDuel itself
        // makes: the area's ALLOW_DUELS flag (below) and the custom-game lobby.
        // A combat check and a ring check were mine, and both made Mok'gora
        // narrower than the duel it is built on - the Gurubashi sand being
        // exactly where somebody would want one.
        if (challenger->InBattleground() || target->InBattleground() ||
            challenger->InArena() || target->InArena())
            return "Not in a battleground or an arena.";

        if (challenger->IsInCustomGameLobby() || target->IsInCustomGameLobby())
            return "Not in a custom-game lobby.";

        if (Tournament::AreSeparated(challenger, target))
            return "A tournament character and a world character cannot meet in a Mok'gora.";

        if (target->GetSocial() && target->GetSocial()->HasIgnore(challenger->GetGUID()))
            return Trinity::StringFormat("{} is not listening to you.", target->GetName());

        if (!challenger->IsWithinDistInMap(target, Config.ChallengeRange))
            return Trinity::StringFormat("You must be within {} yards of {} to say it to their face.",
                uint32(Config.ChallengeRange), target->GetName());

        if (Config.RequireDuelArea)
        {
            AreaTableEntry const* challengerArea = sAreaTableStore.LookupEntry(challenger->GetAreaId());
            AreaTableEntry const* targetArea = sAreaTableStore.LookupEntry(target->GetAreaId());
            if ((challengerArea && !(challengerArea->Flags & AREA_FLAG_ALLOW_DUELS)) ||
                (targetArea && !(targetArea->Flags & AREA_FLAG_ALLOW_DUELS)))
                return "Blood cannot be spilled here. Take it outside.";
        }

        return std::string();
    }

    // ---------------------------------------------------------------------
    // starting the fight
    // ---------------------------------------------------------------------

    // The duel the two of them are about to fight, built exactly the way
    // Spell::EffectDuel builds one - same flag object, same request packet,
    // same state machine - with the single difference that both DuelInfos are
    // marked. Everything downstream (the boundary, the countdown, the client's
    // own duel UI, DuelComplete) then works without knowing anything about
    // Mok'gora, and the one rule that does differ is read off that mark in
    // Unit::DealDamage.
    //
    // The request packet is sent even though consent has already been given in
    // the box: the client learns who it is fighting from it, and the addon
    // answers it for the player so the stock "duel?" popup never appears. A
    // player without the addon sees that popup and clicks Accept, which is a
    // second confirmation of something they have already agreed to in words.
    // `why` is filled with something the player can act on. One message for
    // three different failures sent the first bug report to the wrong place
    // entirely - a missing gameobject template reads exactly like standing
    // somewhere awkward, and "move and try again" is advice that could never
    // have worked.
    bool StartDuel(Player* challenger, Player* target, std::string& why)
    {
        Map* map = challenger->GetMap();
        if (!map || target->GetMap() != map)
        {
            why = "You are not on the same ground.";
            return false;
        }

        if (!sObjectMgr->GetGameObjectTemplate(Config.FlagGameObjectId))
        {
            TC_LOG_ERROR("misc", "Mok'gora: gameobject {} (Centurion.Mokgora.FlagGameObjectId) is not in "
                "gameobject_template - no Mok'gora can be started anywhere. The Duel spell 7266 names "
                "this realm's flag in its own EffectMiscValue; 21680 is the usual answer.",
                Config.FlagGameObjectId);
            why = "This realm has no duel flag to plant. A Game Master needs to set "
                  "Centurion.Mokgora.FlagGameObjectId - nothing you do will help.";
            return false;
        }

        Position const pos =
        {
            challenger->GetPositionX() + (target->GetPositionX() - challenger->GetPositionX()) / 2.0f,
            challenger->GetPositionY() + (target->GetPositionY() - challenger->GetPositionY()) / 2.0f,
            challenger->GetPositionZ(),
            challenger->GetOrientation()
        };

        GameObject* flag = new GameObject;
        QuaternionData const rot = QuaternionData::fromEulerAnglesZYX(pos.GetOrientation(), 0.f, 0.f);
        if (!flag->Create(map->GenerateLowGuid<HighGuid::GameObject>(), Config.FlagGameObjectId, map,
            challenger->GetPhaseMask(), pos, rot, 0, GO_STATE_READY))
        {
            delete flag;
            why = "The ground between you will not take the flag. Move and try again.";
            return false;
        }

        flag->SetFaction(challenger->GetFaction());
        flag->SetLevel(challenger->GetLevel() + 1);

        // Long, because the flag is what the boundary is measured from:
        // Player::CheckDuelDistance gives up silently once it cannot find the
        // object, so a flag that expires mid-fight would quietly stop anyone
        // being able to flee too far. DuelComplete deletes it the moment the
        // Mok'gora is settled either way, so this is only the backstop for a
        // fight that somehow never ends at all.
        flag->SetRespawnTime(30 * MINUTE);

        challenger->AddGameObject(flag);
        map->AddToMap(flag);

        // Before the request packet, not after: the addon answers the stock
        // duel popup on the player's behalf, and it can only do that if it has
        // already been told which request is the Mok'gora. Sent to both sides
        // because both are sent the request, even though the client suppresses
        // the popup for whoever initiated it.
        SendAddonLine(target, Trinity::StringFormat("CCGAME\tMOKGORA:INCOMING:{}", challenger->GetName()));
        SendAddonLine(challenger, Trinity::StringFormat("CCGAME\tMOKGORA:INCOMING:{}", target->GetName()));

        WorldPacket data(SMSG_DUEL_REQUESTED, 8 + 8);
        data << uint64(flag->GetGUID());
        data << uint64(challenger->GetGUID());
        challenger->SendDirectMessage(&data);
        target->SendDirectMessage(&data);

        challenger->duel = std::make_unique<DuelInfo>(target, challenger, false);
        target->duel = std::make_unique<DuelInfo>(challenger, challenger, false);
        challenger->duel->Mokgora = true;
        target->duel->Mokgora = true;

        challenger->SetGuidValue(PLAYER_DUEL_ARBITER, flag->GetGUID());
        target->SetGuidValue(PLAYER_DUEL_ARBITER, flag->GetGUID());

        sScriptMgr->OnPlayerDuelRequest(target, challenger);
        return true;
    }

    // ---------------------------------------------------------------------
    // ending it
    // ---------------------------------------------------------------------

    // Running away does NOT kill you.
    //
    // This was the one rule I had backwards. Leaving the ring, or logging out
    // of it, is punished with Coward! - three days of -20% to attributes,
    // damage done, armour and resistances - and the duel simply ends. Only the
    // blade kills here. A Duel to the Death is lethal because your opponent is
    // allowed to finish you, not because the system executes anyone who walks
    // away from it.
    //
    // (Some private Hardcore realms DO execute you for fleeing. This is not
    // that; this is Blizzard's.)
    // Coward! carries its duration from the config rather than from the DBC.
    //
    // Three days is 259,200,000 ms and SpellDuration.dbc has no such row; adding
    // one would mean another DBC to rebuild and ship to every client for a
    // number the server already knows. So the spell is authored with an
    // infinite duration and the real one is stamped on here, which also makes
    // Centurion.Mokgora.CowardDays mean something rather than being a label.
    //
    // It counts down only while the character is in the world, which is what
    // Blizzard specifies and what TrinityCore already does for free: remaining
    // aura duration is saved at logout and restored at login, so sitting the
    // three days out offline does not work.
    void WearCoward(ObjectGuid loserGuid)
    {
        Player* coward = ObjectAccessor::FindPlayer(loserGuid);
        if (!coward || Config.CowardSpells.empty())
            return;

        int32 const ms = int32(std::max<uint32>(Config.CowardDays, 1)) * DAY * IN_MILLISECONDS;

        // All of them, with the same clock, so the hidden halves cannot outlive
        // or predecease the icon the player can actually see.
        for (uint32 spellId : Config.CowardSpells)
        {
            coward->CastSpell(coward, spellId, true);

            if (Aura* brand = coward->GetAura(spellId))
            {
                brand->SetMaxDuration(ms);
                brand->SetDuration(ms);
            }
        }
    }

    void BrandTheCoward(Player* loser, Player const* winner)
    {
        if (winner)
        {
            // The line Blizzard writes when someone runs. Everyone in range
            // sees who it was.
            loser->TextEmote(Trinity::StringFormat("has fled from {} in a duel.",
                winner->GetName()), nullptr, true);
        }

        if (Config.CowardSpells.empty())
            return;

        // Deferred by a tick: this is reached from inside Player::DuelComplete,
        // which is still tearing the duel down behind it and is about to walk
        // both aura lists. Adding an aura underneath that is asking for the
        // iterator to be invalidated under its feet.
        ObjectGuid const loserGuid = loser->GetGUID();
        loser->m_Events.AddEventAtOffset([loserGuid]() { WearCoward(loserGuid); }, Milliseconds(1));
    }

    char const* HowItEnded(DuelCompleteType type)
    {
        switch (type)
        {
            case DUEL_WON:  return "blade";
            case DUEL_FLED: return "flight";
            default:        return "void";
        }
    }

    // History, not a scoreboard: one row per settled Mok'gora, for whoever has
    // to answer "where did this corpse come from". Nothing here is ever read
    // back to a player.
    void LogOutcome(Player* winner, Player* loser, DuelCompleteType type)
    {
        if (HistoryPersisted)
        {
            // Character names cannot hold a quote, but the names are written
            // into the statement rather than bound, so they are escaped anyway
            // rather than resting on that.
            std::string winnerName = winner->GetName();
            std::string loserName = loser->GetName();
            CharacterDatabase.EscapeString(winnerName);
            CharacterDatabase.EscapeString(loserName);

            CharacterDatabase.Execute(
                "INSERT INTO `mokgora_log` (`stamp`, `winner_guid`, `winner_name`, `loser_guid`, "
                "`loser_name`, `how`, `map`, `zone`) VALUES ({}, {}, '{}', {}, '{}', '{}', {}, {})",
                uint64(GameTime::GetGameTime()),
                winner->GetGUID().GetCounter(), winnerName,
                loser->GetGUID().GetCounter(), loserName,
                HowItEnded(type), loser->GetMapId(), loser->GetZoneId());
        }

        TC_LOG_INFO("misc", "Mok'gora: {} ({}) killed {} ({}) by {} on map {} zone {}.",
            winner->GetName(), winner->GetGUID().ToString(),
            loser->GetName(), loser->GetGUID().ToString(),
            HowItEnded(type), loser->GetMapId(), loser->GetZoneId());
    }
}

namespace Mokgora
{

void LoadConfig()
{
    Config.Enabled = sConfigMgr->GetBoolDefault("Centurion.Mokgora.Enable", false);
    Config.ChallengeRange = std::clamp(
        sConfigMgr->GetFloatDefault("Centurion.Mokgora.ChallengeRange", 10.0f), 2.0f, 100.0f);
    Config.OfferSeconds = uint32(std::clamp(
        sConfigMgr->GetIntDefault("Centurion.Mokgora.OfferSeconds", 30), 5, 300));
    Config.BoundsYards = std::clamp(
        sConfigMgr->GetFloatDefault("Centurion.Mokgora.BoundsYards", 50.0f), 10.0f, 200.0f);
    Config.BoundsGraceSeconds = uint32(std::clamp(
        sConfigMgr->GetIntDefault("Centurion.Mokgora.BoundsGraceSeconds", 10), 1, 60));
    Config.RequireDuelArea = sConfigMgr->GetBoolDefault("Centurion.Mokgora.RequireDuelArea", true);
    Config.CowardSpells.clear();
    {
        std::string const raw = sConfigMgr->GetStringDefault("Centurion.Mokgora.CowardSpells", "");
        for (std::string_view piece : Trinity::Tokenize(raw, ' ', false))
            if (Optional<uint32> id = Trinity::StringTo<uint32>(piece))
                if (*id)
                    Config.CowardSpells.push_back(*id);
    }
    Config.CowardDays = uint32(std::clamp(
        sConfigMgr->GetIntDefault("Centurion.Mokgora.CowardDays", 3), 0, 30));
    Config.AnnounceStart = sConfigMgr->GetBoolDefault("Centurion.Mokgora.AnnounceStart", true);
    Config.AnnounceEnd = sConfigMgr->GetBoolDefault("Centurion.Mokgora.AnnounceEnd", true);
    Config.FlagGameObjectId = uint32(std::max(1,
        sConfigMgr->GetIntDefault("Centurion.Mokgora.FlagGameObjectId", 21680)));

    if (Config.Enabled && !HistoryPersisted)
        ProbeSchema();
}

bool IsEnabled()            { return Config.Enabled; }
float ChallengeRange()      { return Config.ChallengeRange; }
float BoundsYards()         { return Config.BoundsYards; }
uint32 BoundsGraceSeconds() { return Config.BoundsGraceSeconds; }
uint32 CowardSpell()        { return Config.CowardSpells.empty() ? 0 : Config.CowardSpells.front(); }

void SetBotPredicate(PlayerPredicate predicate)
{
    BotPredicate = predicate;
}

bool IsMokgoraDuel(Player const* player)
{
    return player && player->duel && player->duel->Mokgora;
}

bool IsFighting(Player const* player)
{
    return IsMokgoraDuel(player) && player->duel->State == DUEL_STATE_IN_PROGRESS;
}

bool HasPendingOffer(Player const* player)
{
    if (!player)
        return false;

    return FindOfferTo(player->GetGUID()) != nullptr || FindOfferFrom(player->GetGUID()) != nullptr;
}

std::string Explain(Player* challenger, Player* target)
{
    std::string const reason = WhyNot(challenger, target);
    return reason.empty() ? std::string("allowed") : reason;
}

// -------------------------------------------------------------------------
// the challenge
// -------------------------------------------------------------------------

bool Challenge(Player* challenger, std::string const& targetName)
{
    if (!challenger || !challenger->GetSession())
        return false;

    if (!Config.Enabled)
    {
        Say(challenger, Banner("Mok'gora is not enabled on this realm."));
        return true;
    }

    Player* target = nullptr;
    if (!targetName.empty())
        target = ObjectAccessor::FindPlayerByName(targetName);
    else
        target = challenger->GetSelectedPlayer();

    if (!target)
    {
        Say(challenger, Banner(targetName.empty()
            ? "Target the one you mean to challenge, or name them: /mokgora <name>."
            : Trinity::StringFormat("There is no {} here.", targetName)));
        return true;
    }

    if (std::string const reason = WhyNot(challenger, target); !reason.empty())
    {
        Say(challenger, Banner(reason));
        return true;
    }

    // A bot has no box to read and nothing to type into it, so there is no
    // offer: it takes the challenge up on the spot. The consent that matters
    // is the challenger's, already given in words before this was sent.
    if (IsBot(target))
    {
        std::string why;
        if (!StartDuel(challenger, target, why))
        {
            Say(challenger, Banner(why));
            return true;
        }

        // The fleet answers an ordinary duel request with a countdown of its
        // own (playerbot_loader's OnDuelRequest), which StartDuel's request
        // hook has already given it. Whatever that does not reach - a bot the
        // fleet does not manage - is started here instead, so the challenger
        // is never left holding a request nobody will ever answer.
        if (target->duel && challenger->duel && target->duel->State == DUEL_STATE_CHALLENGED)
        {
            time_t const start = GameTime::GetGameTime() + 3;
            target->duel->StartTime = start;
            challenger->duel->StartTime = start;
            target->duel->State = DUEL_STATE_COUNTDOWN;
            challenger->duel->State = DUEL_STATE_COUNTDOWN;
            target->SendDuelCountdown(3000);
            challenger->SendDuelCountdown(3000);
        }

        Say(challenger, Banner(Trinity::StringFormat("{} accepts. One of you will die.", target->GetName())));
        return true;
    }

    Offer offer;
    offer.Challenger = challenger->GetGUID();
    offer.Target = target->GetGUID();
    offer.Expires = GameTime::GetGameTime() + Config.OfferSeconds;
    Offers[target->GetGUID()] = offer;

    // The challenger's side: what they have done, and how to take it back.
    Say(challenger, Banner(Trinity::StringFormat(
        "You have challenged {} to Mok'gora. If they accept, one of you will die.",
        target->GetName())));
    Say(challenger, Banner("Type /mokgora withdraw to take it back."));
    SendAddonLine(challenger, Trinity::StringFormat("CCGAME\tMOKGORA:SENT:{}:{}",
        target->GetName(), Config.OfferSeconds));

    // The challenged side. The addon turns this into the box; the words below
    // it are what a player without the addon reads instead, and they have to
    // carry the whole warning on their own.
    SendAddonLine(target, Trinity::StringFormat("CCGAME\tMOKGORA:OFFER:{}:{}:{}:{}",
        challenger->GetName(), challenger->GetLevel(), uint32(challenger->GetClass()), Config.OfferSeconds));

    Say(target, Banner(Trinity::StringFormat(
        "{} (level {}) has challenged you to MOK'GORA - a duel to the death.",
        challenger->GetName(), challenger->GetLevel())));
    Say(target, Banner("Your opponent will be able to kill you. Fleeing, or logging out, "
        "earns you Coward! for three days."));
    Say(target, Banner(Trinity::StringFormat(
        "Type |cff20ff20/mokgora accept|r to fight, or |cffff2020/mokgora decline|r to refuse. {} seconds.",
        Config.OfferSeconds)));

    return true;
}

bool Accept(Player* target)
{
    if (!target || !target->GetSession())
        return false;

    Offer const* offer = FindOfferTo(target->GetGUID());
    if (!offer)
    {
        Say(target, Banner("Nobody has challenged you."));
        return true;
    }

    Player* challenger = ObjectAccessor::FindPlayer(offer->Challenger);
    Offers.erase(target->GetGUID());

    if (!challenger)
    {
        Say(target, Banner("They are gone."));
        CloseBox(target, "GONE");
        return true;
    }

    // Everything is asked again. Between the offer and the answer either of
    // them could have walked off, started a fight, stepped into a sanctuary or
    // died, and the box on the screen knows none of that.
    if (std::string const reason = WhyNot(challenger, target); !reason.empty())
    {
        Say(target, Banner(reason));
        Say(challenger, Banner(Trinity::StringFormat("The Mok'gora with {} cannot go ahead: {}",
            target->GetName(), reason)));
        CloseBox(target, "STALE");
        return true;
    }

    std::string why;
    if (!StartDuel(challenger, target, why))
    {
        Say(target, Banner(why));
        Say(challenger, Banner(why));
        CloseBox(target, "FAILED");
        return true;
    }

    CloseBox(target, "ACCEPTED");
    return true;
}

bool Decline(Player* target)
{
    if (!target)
        return false;

    Offer const* offer = FindOfferTo(target->GetGUID());
    if (!offer)
    {
        Say(target, Banner("Nobody has challenged you."));
        return true;
    }

    Player* challenger = ObjectAccessor::FindPlayer(offer->Challenger);
    Offers.erase(target->GetGUID());

    Say(target, Banner("You refuse the Mok'gora."));
    CloseBox(target, "DECLINED");

    if (challenger)
    {
        Say(challenger, Banner(Trinity::StringFormat("{} refuses your Mok'gora.", target->GetName())));
        SendAddonLine(challenger, "CCGAME\tMOKGORA:CLOSE:REFUSED");
    }

    return true;
}

bool Withdraw(Player* challenger)
{
    if (!challenger)
        return false;

    Offer const* offer = FindOfferFrom(challenger->GetGUID());
    if (!offer)
    {
        Say(challenger, Banner("You have no challenge outstanding."));
        return true;
    }

    ObjectGuid const targetGuid = offer->Target;
    Offers.erase(targetGuid);

    Say(challenger, Banner("You withdraw your challenge."));
    SendAddonLine(challenger, "CCGAME\tMOKGORA:CLOSE:WITHDRAWN");

    if (Player* target = ObjectAccessor::FindPlayer(targetGuid))
    {
        Say(target, Banner(Trinity::StringFormat("{} withdraws the Mok'gora.", challenger->GetName())));
        CloseBox(target, "WITHDRAWN");
    }

    return true;
}

// -------------------------------------------------------------------------
// hooks
// -------------------------------------------------------------------------

void OnDuelStart(Player* first, Player* second)
{
    if (!first || !second || !IsMokgoraDuel(first))
        return;

    for (Player* who : { first, second })
    {
        Player* other = (who == first) ? second : first;
        SendAddonLine(who, Trinity::StringFormat("CCGAME\tMOKGORA:BEGIN:{}", other->GetName()));
        Say(who, Banner(Trinity::StringFormat("MOK'GORA. {} or you. Run and you die anyway.",
            other->GetName())));
    }

    if (Config.AnnounceStart)
    {
        std::string zone = "the world";
        if (AreaTableEntry const* area = sAreaTableStore.LookupEntry(first->GetZoneId()))
            if (area->AreaName[sWorld->GetDefaultDbcLocale()])
                zone = area->AreaName[sWorld->GetDefaultDbcLocale()];

        Announce(Banner(Trinity::StringFormat(
            "{} and {} have entered Mok'gora in {}. Only one walks away.",
            first->GetName(), second->GetName(), zone)));
    }
}

void OnDuelEnd(Player* winner, Player* loser, DuelCompleteType type)
{
    // Read off the loser: DuelComplete calls this on the losing side, whose
    // duel is still standing.
    if (!winner || !loser || !IsMokgoraDuel(loser))
        return;

    // Interference, a teleport, a zone that swallowed the flag - somebody else
    // decided it, so nobody won it. The loser may well be dead all the same;
    // that death is an ordinary one and is scored as one.
    if (type == DUEL_INTERRUPTED)
    {
        for (Player* who : { winner, loser })
        {
            SendAddonLine(who, "CCGAME\tMOKGORA:END:::void");
            Say(who, Banner("The Mok'gora is void. It was not settled between you."));
        }
        return;
    }

    LogOutcome(winner, loser, type);

    for (Player* who : { winner, loser })
        SendAddonLine(who, Trinity::StringFormat("CCGAME\tMOKGORA:END:{}:{}:{}",
            winner->GetName(), loser->GetName(), HowItEnded(type)));

    if (type == DUEL_FLED)
    {
        // They ran, and they live. Three days of wearing what that was worth.
        Say(loser, Banner("You fled a Duel to the Death. Coward."));
        Say(winner, Banner(Trinity::StringFormat("{} fled. No ear for your string.", loser->GetName())));
        BrandTheCoward(loser, winner);
    }
    else
    {
        // A corpse, so an ear.
        Say(winner, Banner(Trinity::StringFormat("{} lies dead. The Mok'gora is yours.", loser->GetName())));
        Say(loser, Banner(Trinity::StringFormat("{} has killed you in Mok'gora.", winner->GetName())));
    }

    if (Config.AnnounceEnd)
    {
        // Two calls rather than one with a chosen format: StringFormat checks
        // its format string against the arguments at compile time, which a
        // string picked at run time cannot satisfy.
        Announce(Banner(type == DUEL_FLED
            ? Trinity::StringFormat("{} has fled from {} in a duel.",
                loser->GetName(), winner->GetName())
            : Trinity::StringFormat("{} has killed {} in Mok'gora.",
                winner->GetName(), loser->GetName())));
    }
}

void OnLogout(Player* player)
{
    if (!player)
        return;

    // Drop anything they had outstanding, in either direction.
    if (Offer const* offer = FindOfferTo(player->GetGUID()))
    {
        if (Player* challenger = ObjectAccessor::FindPlayer(offer->Challenger))
            Say(challenger, Banner(Trinity::StringFormat("{} has gone. The challenge lapses.", player->GetName())));
        Offers.erase(player->GetGUID());
    }
    if (Offer const* offer = FindOfferFrom(player->GetGUID()))
    {
        ObjectGuid const targetGuid = offer->Target;
        Offers.erase(targetGuid);
        if (Player* target = ObjectAccessor::FindPlayer(targetGuid))
        {
            Say(target, Banner(Trinity::StringFormat("{} has gone. The challenge lapses.", player->GetName())));
            CloseBox(target, "GONE");
        }
    }

    if (!IsFighting(player) || !player->IsAlive())
        return;

    // "Fleeing a duel to the death by leaving the large duel radius OR BY
    // LOGGING OUT". The exit door is a door out of the ring, not an escape
    // from the consequence - but the consequence is Coward!, not a corpse.
    Player* opponent = player->duel->Opponent;

    Say(player, Banner("You logged out of a Duel to the Death. Coward."));
    if (opponent)
    {
        Say(opponent, Banner(Trinity::StringFormat("{} logged out of the Mok'gora rather than face you.",
            player->GetName())));
    }

    // Resolving it runs OnDuelEnd, which brands them - but that brand is
    // queued for the next tick, and there is no next tick on the way out: the
    // save is a few lines further down this same call. So it is applied here,
    // directly, and the queued one finds the aura already on and stacks
    // nothing (Coward! does not stack).
    player->DuelComplete(DUEL_FLED);

    if (!Config.CowardSpells.empty() && !player->HasAura(Config.CowardSpells.front()))
        WearCoward(player->GetGUID());
}

void OnLogin(Player* player)
{
    if (!player || !Config.Enabled)
        return;

    // Only the rules, so the warning box can word itself. Nothing is said to
    // the player and nothing is read from the database: logging in is not an
    // occasion for being told how you have done.
    SendRules(player);
}

void Update()
{
    if (Offers.empty())
        return;

    time_t const now = GameTime::GetGameTime();
    for (auto itr = Offers.begin(); itr != Offers.end(); )
    {
        if (itr->second.Expires > now)
        {
            ++itr;
            continue;
        }

        if (Player* target = ObjectAccessor::FindPlayer(itr->second.Target))
        {
            Say(target, Banner("The Mok'gora challenge lapses unanswered."));
            CloseBox(target, "EXPIRED");
        }
        if (Player* challenger = ObjectAccessor::FindPlayer(itr->second.Challenger))
        {
            Say(challenger, Banner("Your Mok'gora challenge went unanswered."));
            SendAddonLine(challenger, "CCGAME\tMOKGORA:CLOSE:EXPIRED");
        }

        itr = Offers.erase(itr);
    }
}

// -------------------------------------------------------------------------
// the wire
// -------------------------------------------------------------------------

bool HandleChatMessage(Player* sender, uint32 type, uint32 lang, std::string const& msg)
{
    if (!sender || msg.empty())
        return false;

    std::string_view argument;

    if (lang == LANG_ADDON)
    {
        static constexpr std::string_view Request = "CCGAMEREQ\tMOKGORA:";
        if (!StringStartsWith(msg, Request))
            return false;

        argument = std::string_view(msg).substr(Request.size());
    }
    else
    {
        // The spoken fallback. It cannot be a real chat command: regular
        // accounts are refused by the command parser before it ever looks at
        // the name, so the gate has to sit above it - which is also why this
        // swallows the message either way rather than letting ".mokgora" reach
        // the chat frame as speech.
        if (type != CHAT_MSG_SAY && type != CHAT_MSG_PARTY && type != CHAT_MSG_PARTY_LEADER &&
            type != CHAT_MSG_YELL && type != CHAT_MSG_RAID && type != CHAT_MSG_RAID_LEADER &&
            type != CHAT_MSG_GUILD && type != CHAT_MSG_WHISPER)
            return false;

        std::string_view text(msg);
        if (text.empty() || text[0] != '.')
            return false;

        text.remove_prefix(1);

        // The whole word only. ".mokgorafoo" is not a Mok'gora command, and
        // swallowing it would eat somebody else's.
        auto takesWord = [&text](std::string_view word)
        {
            return StringStartsWithI(text, word) &&
                (text.size() == word.size() || text[word.size()] == ' ');
        };

        if (takesWord("mokgora"))
            text.remove_prefix(7);
        else if (takesWord("makgora"))
            text.remove_prefix(7);
        else if (takesWord("mak"))
            text.remove_prefix(3);
        else
            return false;

        while (!text.empty() && text.front() == ' ')
            text.remove_prefix(1);

        // ".mokgora why ..." is the GM diagnostic, which is a real chat command
        // registered in the script. Letting it fall through is what puts it in
        // front of the parser below - otherwise this gate would read "why Bob"
        // as a challenge to somebody called "why Bob".
        if (StringStartsWithI(text, "why"))
            return false;

        // The spoken form names its verb the same way the addon does, so one
        // set of words below serves both.
        if (text.empty())
            argument = "CHALLENGE:";
        else if (StringEqualI(text, "accept") || StringEqualI(text, "yes"))
            argument = "ACCEPT";
        else if (StringEqualI(text, "decline") || StringEqualI(text, "no"))
            argument = "DECLINE";
        else if (StringEqualI(text, "withdraw") || StringEqualI(text, "cancel"))
            argument = "WITHDRAW";
        else
        {
            // ".mokgora <name>"
            Challenge(sender, std::string(text));
            return true;
        }
    }

    static constexpr std::string_view ChallengeVerb = "CHALLENGE:";
    if (StringStartsWith(argument, ChallengeVerb))
    {
        Challenge(sender, std::string(argument.substr(ChallengeVerb.size())));
        return true;
    }
    if (argument == "CHALLENGE")
    {
        Challenge(sender, std::string());
        return true;
    }
    if (argument == "ACCEPT")
    {
        Accept(sender);
        return true;
    }
    if (argument == "DECLINE")
    {
        Decline(sender);
        return true;
    }
    if (argument == "WITHDRAW")
    {
        Withdraw(sender);
        return true;
    }
    if (argument == "RULES")
    {
        SendRules(sender);
        return true;
    }

    // A MOKGORA verb this build does not know. Swallowed rather than spoken -
    // an addon newer than the realm should not shout its protocol in /say.
    return lang == LANG_ADDON;
}

} // namespace Mokgora
