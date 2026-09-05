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

 // Barracks+ hardcore ruleset (config-gated; entirely inert on Legionnaire+):
 //  - Loot drop on death: WORN GREEN-AND-BETTER EQUIPMENT is at stake - a
 //    configurable share drops into a chest at the corpse (Dire Maul beads
 //    style), the rest is destroyed as a deflationary sink. White gear is the
 //    floor and never drops; bags, inventory and money are safe. Whatever the
 //    death took is replaced with plain white field kit on resurrection, so
 //    nobody is ever left unable to fight. Open world only - battlegrounds,
 //    arenas, dungeons and raids all exempt, so a wipe costs a group nothing.
 //  - Opt-in free-for-all PvP: a flagger NPC in the capitals toggles it. The
 //    flag only ARMS in zones of a configurable minimum level - never in
 //    starter zones, capitals or sanctuaries - and while armed the player
 //    earns double experience and loot gold.
 //  - Playerbots are ALWAYS armed in eligible zones and render red to
 //    everyone.
 // Item bindings are removed by the core-side Centurion.Hardcore.NoBinding
 // switch (Item::IsSoulBound), which also opens the auction house to
 // everything.

#include "Bag.h"
#include "custom_barracks_hardcore.h"
#include "ChallengeModes.h"
#include "custom_bounty.h"
#include "Playerbot/Pve/PlayerbotPveManager.h"   // IsPvpOnlyBot
#include "ScriptMgr.h"
#include "Configuration/Config.h"
#include "Formulas.h"
#include "GameTime.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "Chat.h"
#include "CharacterCache.h"   // offline party members are still party members
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "Duration.h"
#include "GameObject.h"
#include "Log.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Map.h"
#include "Group.h"
#include "ObjectMgr.h"
#include "Optional.h"
#include "RBAC.h"
#include "Player.h"
#include "ScriptedCreature.h"
#include "ScriptedGossip.h"
#include "SharedDefines.h"
#include "Util.h"
#include "World.h"
#include "WorldSession.h"
#include "custom_loot_chest_helper.h"
#include <algorithm>
#include <cctype>
#include <array>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace BarracksHardcore
{
    bool s_enabled = false;
    uint32 s_chestEntry = 0;
    uint32 s_chestDespawnSeconds = 600;
    uint32 s_dropChancePercent = 50;
    uint32 s_minZoneLevel = 20;
    uint32 s_rewardMultiplier = 2;
    // How close a real person must be for a BOT's death to pay out gear.
    // 0 means always, which is what this was before.
    float s_botLootWitnessYards = 200.0f;
    // Visible badge for the war-mode opt-in, so a player can tell at a glance
    // whether their flag is armed. 0 disables it. Configurable because no
    // stock 3.3.5 spell is both named for this and free of side effects -
    // 32609/32610 ("Alliance/Horde Flag Visual Only") are genuinely
    // effect-free and already proven on this realm, and a bespoke "War Mode"
    // spell added through the usual spell_lplus -> dbcgen -> client patch
    // route drops straight in here without a code change.
    uint32 s_warModeAuraSpell = 0;
    // Levels below this are issued GREY field kit instead of white.
    uint32 s_greyKitMaxLevel = 15;
    // The kit dresses the wearer as though they were this many levels lower.
    // Issued gear was landing close enough to what the level could actually
    // earn that there was nothing to look forward to: the kit is meant to stop
    // a corpse run being naked, not to be the gear you keep. Subtracted from
    // the level the item search is run at, so the wearer is still handed the
    // BEST piece for that reduced level - the whole set moves down together
    // rather than becoming patchy. 0 restores the old behaviour.
    uint32 s_kitLevelOffset = 10;
    // Bots sit closer to their own level than people do. A person who finds the
    // kit thin can go and earn better; a bot cannot - it wears what the kit
    // gives it for good, and ten levels down it stops being able to kill the
    // things its own zone band is built around.
    uint32 s_kitBotLevelOffset = 5;
    // How far a worn kit piece may fall behind the level the kit now dresses
    // for before it is replaced. Without this a piece issued at level 15 is
    // still worn at 50, because the kit only fills empty slots.
    uint32 s_kitStaleLevels = 5;
    // Yards of drift between a bot's real position and the one its packets
    // carry to observers before it is reported. 0 disables the watch.
    float s_movementDivergenceYards = 6.0f;
    // Experience paid for killing a playerbot, counted in BUBBLES - the twenty
    // segments the experience bar is divided into, so one bubble is 5% of a
    // level and twenty is a full bar. Fractions are allowed. 0 disables.
    float s_playerKillXpBubbles = 2.0f;
    // How long a victim is remembered for the decay below.
    uint32 s_playerKillDiminishSeconds = 2 * HOUR;
    // How much of a repeat kill's value is LOST each time, inside the window
    // above. 0.25 leaves three quarters standing per repeat, which is the
    // gentle end; 0.5 is the old hardcoded halving; 0 turns it off.
    float s_playerKillXpDecayPerKill = 0.25f;
    std::unordered_set<uint32> s_botAccountIds;

    std::shared_mutex s_optInLock;
    std::unordered_set<uint32> s_optInGuids; // low guids of opted-in players

    void LoadHardcoreConfig()
    {
        s_enabled = sConfigMgr->GetBoolDefault("Centurion.Hardcore.Enable", false);
        s_chestEntry = uint32(std::max(0, sConfigMgr->GetIntDefault("Centurion.Hardcore.FullLoot.ChestGameObjectId", 0)));
        s_chestDespawnSeconds = uint32(std::max(30, sConfigMgr->GetIntDefault("Centurion.Hardcore.FullLoot.ChestDespawnSeconds", 600)));
        s_dropChancePercent = uint32(std::clamp(sConfigMgr->GetIntDefault("Centurion.Hardcore.FullLoot.DropChancePercent", 50), 0, 100));
        s_minZoneLevel = uint32(std::max(1, sConfigMgr->GetIntDefault("Centurion.Hardcore.FfaPvp.MinZoneLevel", 20)));
        s_rewardMultiplier = uint32(std::clamp(sConfigMgr->GetIntDefault("Centurion.Hardcore.FfaPvp.RewardMultiplier", 2), 1, 10));
        s_warModeAuraSpell = uint32(std::max(0, sConfigMgr->GetIntDefault("Centurion.Hardcore.FfaPvp.WarModeAuraSpell", 0)));
        s_greyKitMaxLevel = uint32(std::max(0, sConfigMgr->GetIntDefault("Centurion.Hardcore.FieldKit.GreyUntilLevel", 15)));
        s_kitLevelOffset = uint32(std::clamp(sConfigMgr->GetIntDefault("Centurion.Hardcore.FieldKit.LevelOffset", 10), 0, 60));
        s_kitBotLevelOffset = uint32(std::clamp(sConfigMgr->GetIntDefault("Centurion.Hardcore.FieldKit.BotLevelOffset", 5), 0, 60));
        s_kitStaleLevels = uint32(std::clamp(sConfigMgr->GetIntDefault("Centurion.Hardcore.FieldKit.StaleLevels", 5), 1, 60));
        s_movementDivergenceYards = std::max(0.0f, sConfigMgr->GetFloatDefault("Centurion.Hardcore.Diagnostics.MovementDivergenceYards", 6.0f));
        s_botLootWitnessYards = std::max(0.0f,
            sConfigMgr->GetFloatDefault("Centurion.Hardcore.BotLootWitnessYards", 200.0f));
        s_playerKillXpBubbles = std::clamp(
            sConfigMgr->GetFloatDefault("Centurion.Hardcore.PlayerKill.ExperienceBubbles", 2.0f), 0.0f, 20.0f);
        s_playerKillDiminishSeconds = uint32(std::max(0,
            sConfigMgr->GetIntDefault("Centurion.Hardcore.PlayerKill.DiminishSeconds", 2 * HOUR)));
        s_playerKillXpDecayPerKill = std::clamp(
            sConfigMgr->GetFloatDefault("Centurion.Hardcore.PlayerKill.XpDecayPerKill", 0.25f), 0.0f, 1.0f);

        s_botAccountIds.clear();
        std::stringstream stream(sConfigMgr->GetStringDefault("Playerbot.RandomPopulation.BotAccountIds", ""));
        std::string token;
        while (std::getline(stream, token, ','))
            if (!token.empty())
                s_botAccountIds.insert(uint32(std::strtoul(token.c_str(), nullptr, 10)));

        // The "scripts" logger has no configuration, so it falls back to root,
        // which is ERROR-only: anything logged there is invisible. Everything
        // diagnostic here goes to playerbots.* instead, which reaches
        // Playerbot.log.
        TC_LOG_INFO("playerbots.hardcore", "Hardcore config: enabled={} chest={} minZoneLevel={} botAccounts={}",
            uint32(s_enabled), s_chestEntry, s_minZoneLevel, uint32(s_botAccountIds.size()));
    }

    bool IsBotAccount(uint32 accountId)
    {
        return s_botAccountIds.contains(accountId);
    }

    bool IsPlayerbot(Player const* player)
    {
        WorldSession const* session = player ? player->GetSession() : nullptr;
        return session && IsBotAccount(session->GetAccountId());
    }

    // Is there a real person close enough for this death to mean anything?
    // Bots and companion/virtual sessions do not count - a hillside full of
    // other bots is still an empty hillside.
    bool AnyHumanPlayerWithin(Player const* victim, float radius)
    {
        Map const* map = victim ? victim->GetMap() : nullptr;
        if (!map)
            return false;

        for (auto const& reference : map->GetPlayers())
        {
            Player const* candidate = reference.GetSource();
            if (!candidate || candidate == victim || !candidate->IsInWorld())
                continue;

            WorldSession const* session = candidate->GetSession();
            if (!session || session->IsVirtualSession() || session->IsTransientPlayerSession() ||
                IsBotAccount(session->GetAccountId()))
                continue;

            if (candidate->IsWithinDist(victim, radius, false))
                return true;
        }

        return false;
    }

    bool IsOptedIn(uint32 guidLow)
    {
        std::shared_lock<std::shared_mutex> guard(s_optInLock);
        return s_optInGuids.contains(guidLow);
    }

    void SetOptedIn(uint32 guidLow, bool optedIn)
    {
        {
            std::unique_lock<std::shared_mutex> guard(s_optInLock);
            if (optedIn)
                s_optInGuids.insert(guidLow);
            else
                s_optInGuids.erase(guidLow);
        }
        if (optedIn)
            CharacterDatabase.PExecute("REPLACE INTO character_ffa_optin (guid) VALUES ({})", guidLow);
        else
            CharacterDatabase.PExecute("DELETE FROM character_ffa_optin WHERE guid = {}", guidLow);
    }

    // "The world": everything hardcore is inert inside battlegrounds/arenas.
    bool IsWorldContext(Player* player)
    {
        return s_enabled && player && !player->InBattleground() && !player->InArena();
    }

    // Inside a dungeon or a raid. Deliberately NOT folded into IsWorldContext:
    // that one also gates the full-loot chest, and death in a dungeon is still
    // meant to cost you your gear. This is only about the War Mode REWARD - the
    // doubled experience and loot gold ride the risk of being jumped in the open
    // world, and there is no such risk behind an instance portal.
    //
    // Map::IsDungeon covers raids as well (instance types 1 and 2); a
    // battleground or arena is a different instance type and is handled by
    // IsWorldContext above.
    bool IsInstancedContent(Player const* player)
    {
        Map const* map = player ? player->FindMap() : nullptr;
        return map && map->IsDungeon();
    }

    // Classic zone level caps. AreaTableEntry::ExplorationLevel is ZERO for
    // every zone in this realm's rebuilt DBC, so reading it armed nothing,
    // anywhere - the whole FFA system (and every bot's red name) was inert.
    // The zone's top level is the gate: Durotar/Elwynn (10) stay peaceful,
    // Westfall/Silverpine (20) and everything above them arm.
    uint8 ZoneTopLevel(uint32 zoneId)
    {
        switch (zoneId)
        {
        case 1: case 12: case 14: case 85: case 141: case 215:  return 10; // starter zones
        case 3430: case 3524:                                   return 10; // Eversong, Azuremyst
        case 38: case 40: case 130: case 148:                   return 20;
        case 3433: case 3525:                                   return 20; // Ghostlands, Bloodmyst
        case 17: case 44:                                       return 25;
        case 406:                                               return 27;
        case 10: case 11: case 267: case 331:                   return 30;
        case 400:                                               return 35;
        case 36: case 45: case 405:                             return 40;
        case 3: case 8: case 15: case 33:                       return 45;
        case 47: case 51: case 357: case 440:                   return 50;
        case 16: case 361: case 490:                            return 55;
        case 4: case 28: case 46:                               return 58;
        case 139: case 618: case 1377:                          return 60;
        default:                                                return 60; // Outland, Northrend, dungeons
        }
    }

    // Exported for the playerbot manager, which needs the same answer when it
    // decides whether travelling somewhere to pick a fight makes any sense.
    // With hardcore off there is no FFA gate at all and the realm's ordinary
    // PvP rules apply, so every non-sanctuary zone qualifies.
    bool IsOpenWorldPvpZone(uint32 zoneId)
    {
        if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(zoneId))
            if (zone->Flags & (AREA_FLAG_CAPITAL | AREA_FLAG_SANCTUARY))
                return false;

        if (!s_enabled)
            return true;

        return ZoneTopLevel(zoneId) >= s_minZoneLevel;
    }

    // FFA arms only in real PvP-level zones - never in starter zones like
    // Durotar, never in capitals or sanctuaries.
    bool IsFfaEligibleZone(Player* player)
    {
        return IsOpenWorldPvpZone(player->GetZoneId());
    }

    bool IsFfaArmed(Player* player)
    {
        if (!IsWorldContext(player))
            return false;

        // War mode does not reach inside the ropes. Everyone the hourly event
        // drops into the Battle Ring is there to fight whether they opted in or
        // not, and stops being a combatant the moment they step out of it - so
        // this is checked before both the zone gate and the opt-in.
        if (player->IsInGurubashiBattleRing())
            return true;

        if (!IsFfaEligibleZone(player))
            return false;

        WorldSession const* session = player->GetSession();
        if (session && IsBotAccount(session->GetAccountId()))
            return true;

        return IsOptedIn(player->GetGUID().GetCounter());
    }

    // Called on login, zone change and every player tick: Player::UpdateArea
    // recomputes the FFA byte on every sub-area crossing and never dirties the
    // faction field when it does, so the state has to be re-asserted rather
    // than set once. MUST CONVERGE: it runs per player per world tick, and a
    // branch that cannot satisfy its own condition would re-dirty the faction
    // field forever, rebuilding a values block for every observer each tick.
    // This realm is always at war. The PvP flag stays lit for everyone and
    // /pvp cannot put it out: the toggle is simply overwritten on the next
    // tick, and the five-minute cool-down timer never gets to expire. GMs are
    // left alone so they can move about unflagged.
    void EnforceAlwaysPvP(Player* player)
    {
        if (!s_enabled || !player || !player->IsInWorld() || player->IsGameMaster())
            return;

        if (!player->IsPvP() || player->pvpInfo.EndTimer)
        {
            player->pvpInfo.EndTimer = 0;
            player->UpdatePvP(true, true);
            player->RemoveFlag(PLAYER_FLAGS, PLAYER_FLAGS_PVP_TIMER);
        }
    }

    bool IsWarModeOptedIn(Player const* player)
    {
        if (!s_enabled || !player)
            return false;

        // A bot has no War Mode setting to compare against.
        WorldSession const* session = player->GetSession();
        if (!session || IsBotAccount(session->GetAccountId()))
            return false;

        return IsOptedIn(player->GetGUID().GetCounter());
    }

    bool WarModeBlocksGrouping(Player const* left, Player const* right)
    {
        if (!s_enabled || !left || !right)
            return false;

        // Bots group with anybody. Enforcing this against them would break
        // every bot party on the realm for a rule that is about people.
        auto isBot = [](Player const* p)
        {
            WorldSession const* s = p->GetSession();
            return !s || IsBotAccount(s->GetAccountId());
        };
        if (isBot(left) || isBot(right))
            return false;

        return IsWarModeOptedIn(left) != IsWarModeOptedIn(right);
    }

    // Whether this party member's War Mode disagrees with the given player's.
    //
    // Answered from the GUID rather than a Player*, so an OFFLINE member counts:
    // they are still in the party and still coming back, and a rule that only
    // noticed whoever happened to be logged in could be dodged by waiting for a
    // friend to step away. IsOptedIn is a guid-keyed set loaded from
    // character_ffa_optin, so it answers for them either way.
    //
    // Bots are exempt, exactly as in WarModeBlocksGrouping: the companion system
    // puts them in people's parties, and splitting a party over a setting a bot
    // does not have would break every companion group on the realm.
    bool MemberDisagreesOnWarMode(Player const* player, ObjectGuid memberGuid)
    {
        uint32 const accountId = sCharacterCache->GetCharacterAccountIdByGuid(memberGuid);
        if (!accountId || IsBotAccount(accountId))
            return false;

        return IsOptedIn(memberGuid.GetCounter()) != IsWarModeOptedIn(player);
    }

    // Changing your flag takes you out of a party that does not share it.
    //
    // OnCanGroupInvite refuses to BUILD a split party, but it only guards the
    // moment of joining: a legitimate party becomes illegal the instant somebody
    // flips their flag at the flagger. Without this the flagger is a way AROUND
    // the grouping rule rather than the thing that enforces it.
    void LeavePartyIfWarModeSplit(Player* player)
    {
        if (!s_enabled || !player)
            return;

        Group const* group = player->GetGroup();
        if (!group)
            return;

        bool split = false;
        for (Group::MemberSlot const& slot : group->GetMemberSlots())
        {
            if (slot.guid == player->GetGUID())
                continue;

            if (MemberDisagreesOnWarMode(player, slot.guid))
            {
                split = true;
                break;
            }
        }

        if (!split)
            return;

        // Said before the removal, so it arrives with the party frame still up
        // and reads as a consequence rather than a mystery.
        ChatHandler(player->GetSession()).PSendSysMessage(
            "You have left your party: War Mode is now %s and they do not share it.",
            IsWarModeOptedIn(player) ? "on" : "off");

        player->RemoveFromGroup();
    }

    // The war-mode badge. Tracks the OPT-IN rather than the armed state on
    // purpose: armed depends on the zone, so a badge that tracked it would
    // wink out every time the player stepped into a sanctuary and read as the
    // setting having turned itself off. This says "your flag is on", and the
    // flag itself decides where that bites.
    //
    // MUST CONVERGE, for the same reason ApplyFfaState must: it runs per
    // player per world tick, so it compares before it touches anything.
    void ApplyWarModeAura(Player* player)
    {
        if (!s_enabled || !s_warModeAuraSpell || !player || !player->IsInWorld())
            return;

        // Bots need no badge, and a couple of hundred of them re-broadcasting
        // one would cost far more than it could ever tell anybody.
        WorldSession const* session = player->GetSession();
        if (!session || IsBotAccount(session->GetAccountId()))
            return;

        // The badge comes OFF inside a dungeon or raid, and this is the only way
        // to stop it paying there: the experience and gold bonuses are carried by
        // the aura's own effects and applied by the core (SPELL_AURA_MOD_XP_PCT
        // in KillRewarder, the DUMMY percentage at loot time), so no hook of ours
        // gets a say once it is on. It comes straight back on the way out.
        bool const wantsBadge = IsOptedIn(player->GetGUID().GetCounter()) &&
            !IsInstancedContent(player);
        if (wantsBadge == player->HasAura(s_warModeAuraSpell))
            return; // converged - the overwhelmingly common case

        if (wantsBadge)
            player->AddAura(s_warModeAuraSpell, player);
        else
            player->RemoveAurasDueToSpell(s_warModeAuraSpell);
    }

    void ApplyFfaState(Player* player)
    {
        if (!s_enabled || !player || !player->IsInWorld())
            return;

        // Sanctuaries, arena areas and GM mode own the flag outright: the core
        // REFUSES to set the FFA byte there (Player::UpdatePvPState), so a
        // re-assert could never take effect and would simply repeat forever.
        // Sanctuary sub-areas sit inside plenty of eligible zones - Blackrock
        // Mountain, the Stair of Destiny, Gurubashi Arena, Acherus.
        // The Battle Ring is the exception to the arena skip, and it has to be:
        // the ring carries AREA_FLAG_ARENA like every other arena area, so the
        // blanket skip made the one place on the realm that IS a free-for-all the
        // one place this never armed one. The core does not own the flag there
        // either - UpdatePvPState defers to sanctuary and GM and nothing else -
        // so asserting it here converges instead of fighting anyone for it.
        AreaTableEntry const* area = sAreaTableStore.LookupEntry(player->GetAreaId());
        bool const inBattleRing = player->IsInGurubashiBattleRing();
        if (player->pvpInfo.IsInNoPvPArea || player->IsGameMaster() ||
            (!inBattleRing && area && (area->Flags & AREA_FLAG_ARENA)))
            return;

        bool const shouldFfa = IsFfaArmed(player);
        bool const wasFfa = player->IsFFAPvP();
        if (wasFfa == shouldFfa && player->pvpInfo.IsInFFAPvPArea == shouldFfa)
            return; // nothing has drifted - the overwhelmingly common case

        player->pvpInfo.IsInFFAPvPArea = shouldFfa;
        player->UpdatePvPState(true);

        // An armed bot also carries the ordinary PvP flag: the enemy-faction
        // render only reads as attackable on the client when the unit is
        // flagged, exactly like a real enemy player standing in the open.
        if (player->GetSession() && IsBotAccount(player->GetSession()->GetAccountId()))
            player->UpdatePvP(shouldFfa, true);

        // Only when the byte actually moved: the bot pseudo-faction render
        // (Unit::BuildValuesUpdate) keys on it, and the faction field is never
        // dirtied by the core, but forcing it on a tick where nothing changed
        // is pure broadcast cost.
        if (player->IsFFAPvP() != wasFfa)
        {
            player->ForceValuesUpdateAtIndex(UNIT_FIELD_FACTIONTEMPLATE);

            // The bot render override branches on the OBSERVER's byte, but
            // dirtying this player only rebuilds THIS player's values block.
            // Bots already on screen keep the colour they were last sent, so
            // after arming they stay green - and the client refuses to swing at
            // a friendly unit, leaving a player hunted by bots they cannot hit
            // until each one happens to re-send. Re-dirty the bots this flip
            // just re-coloured. Dead ones too, or a corpse keeps a stale colour
            // through its whole rez. Transitions only: not a hot path.
            std::vector<Player*> nearby;
            player->GetPlayerListInGrid(nearby, player->GetVisibilityRange(), false);
            for (Player* other : nearby)
                if (other->GetSession() && IsBotAccount(other->GetSession()->GetAccountId()))
                    other->ForceValuesUpdateAtIndex(UNIT_FIELD_FACTIONTEMPLATE);

            // Nothing re-checks a fight already under way: the auto-attack swing
            // loop and periodic aura ticks never re-ask IsValidAttackTarget.
            // Without this a player engages a bot, steps into a sanctuary or
            // disarms, and keeps swinging at something that can no longer swing
            // back. This is the core's own tool for exactly that.
            player->ValidateAttackersAndOwnTarget();
        }

        // Transitions only - this cannot spam.
        TC_LOG_INFO("playerbots.hardcore", "FFA {} for {} (zone {} area {} account {}): armed={} byteNow={} wasByte={}",
            shouldFfa ? "ARM" : "disarm", player->GetName(), player->GetZoneId(), player->GetAreaId(),
            player->GetSession() ? player->GetSession()->GetAccountId() : 0,
            uint32(shouldFfa), uint32(player->IsFFAPvP()), uint32(wasFfa));
    }

    // ---------------------------------------------------------------------
    // The white field kit: the floor every fighter stands on. White gear is
    // never taken by death, so a corpse run always leaves the victim able to
    // swing back - no naked death spiral, for players or bots alike.
    // ---------------------------------------------------------------------

    // Main hand before off hand, so a two-hander is settled before anything
    // tries to fill the slot it occupies.
    constexpr std::array<uint8, 12> kKitSlots = { {
        EQUIPMENT_SLOT_HEAD, EQUIPMENT_SLOT_SHOULDERS, EQUIPMENT_SLOT_CHEST,
        EQUIPMENT_SLOT_WAIST, EQUIPMENT_SLOT_LEGS, EQUIPMENT_SLOT_FEET,
        EQUIPMENT_SLOT_WRISTS, EQUIPMENT_SLOT_HANDS, EQUIPMENT_SLOT_BACK,
        EQUIPMENT_SLOT_MAINHAND, EQUIPMENT_SLOT_OFFHAND, EQUIPMENT_SLOT_RANGED
    } };

    // The kit issues its own non-sellable DUPLICATES rather than the real
    // items - see sql/custom/world/2026_09_02_01_world_fieldkit_nosell_duplicates.sql.
    // Free gear must not be a gold faucet: the kit comes back on every death, so
    // vendoring the replacements would print money. Each duplicate is a copy of a
    // real item with SellPrice and BuyPrice zeroed, which the client shows as a
    // sell price of zero and merchants refuse - ordinary behaviour for a worthless
    // item, with nothing new to explain to whoever is holding it. The identical
    // item looted or bought normally is untouched and still sells.
    bool IsFieldKitDuplicate(uint32 itemId)
    {
        // One definition of the range, shared with the item handler and Player,
        // which destroy a displaced kit piece rather than bagging it.
        return IsFieldKitDuplicateEntry(itemId);
    }

    // When each character's bags were last swept for stray kit pieces.
    //
    // thread_local, not shared-plus-mutex. A player is updated by exactly one
    // map thread at a time, so there is nothing to share - and at a 10ms map
    // interval with a few hundred characters this is reached tens of thousands
    // of times a second, which is no place for a global lock. A player that
    // migrates maps leaves a stale entry behind and gets a fresh one, costing at
    // most one early check.
    thread_local std::unordered_map<uint64, uint32> s_nextKitSweepMs;
    constexpr uint32 KIT_SWEEP_INTERVAL_MS = 2000;

    std::mutex s_whiteKitLock;
    bool s_whiteKitBuilt = false;
    std::unordered_map<uint32, std::vector<uint32>> s_whiteKitByInvType;
    // Greys for the earliest levels. A brand new character in plain white kit
    // is already better equipped than the world it is walking into; grey is
    // the honest floor for the first few levels, and white becomes something
    // the player reaches rather than starts with.
    std::unordered_map<uint32, std::vector<uint32>> s_greyKitByInvType;

    // Duplicate id -> the real item it was copied from.
    //
    // Every duplicate reads artifact quality now, so it is plainly a loaner
    // on the character sheet and not mistaken for earned gear. That costs us
    // the one signal that separated the grey tier from the white one: the two
    // are interleaved across the whole id range, so no range check can stand
    // in for it. The SOURCE item still carries its own quality, which is what
    // this map is for - without it every character below GreyUntilLevel would
    // be quietly promoted from grey kit to white.
    std::unordered_map<uint32, uint32> s_fieldKitSourceItem;

    // Placeholder and developer scaffolding wearing an item's clothes. These
    // sit in item_template at quality white, item level 1 and required level
    // 0, which is exactly the shape of a real starter item - so a level 1 bot
    // kitted out from the template store ends up in "CRobinson Plate Shoulders"
    // and a "[PH]" cap. Anything the world cannot actually hand out is not
    // field kit.
    bool LooksLikeScaffolding(std::string const& rawName)
    {
        // Lowercased once, so the markers need not enumerate every casing a
        // designer happened to use. The live data carries "(Test)", "(test)"
        // and "(TEST)" on sibling items.
        std::string name = rawName;
        std::transform(name.begin(), name.end(), name.begin(),
            [](unsigned char c) { return char(std::tolower(c)); });

        static constexpr std::array<char const*, 7> markers = { {
            "[ph]", "crobinson", "monster ", "old ", "deprecated", "unused", "placeholder"
        } };
        for (char const* marker : markers)
            if (name.find(marker) != std::string::npos)
                return true;

        // "test" as a WORD, however it happens to be punctuated - "Test ",
        // "(Test)", "[TEST]", "- test". The old list matched "Test " with a
        // trailing space, so "Pirates Patch (Test)" sailed straight through it
        // and ended up on a bot's head. Requiring a boundary on both sides
        // keeps real words like "Testament" out.
        for (size_t at = name.find("test"); at != std::string::npos; at = name.find("test", at + 4))
        {
            bool const beforeOk = at == 0 || !std::isalpha(static_cast<unsigned char>(name[at - 1]));
            size_t const after = at + 4;
            bool const afterOk = after >= name.size() || !std::isalpha(static_cast<unsigned char>(name[after]));
            if (beforeOk && afterOk)
                return true;
        }

        return false;
    }

    // Item ids the world can actually produce: sold, dropped, or handed to a
    // new character. Built once alongside the kit cache.
    std::unordered_set<uint32> s_obtainableItems;

    // Item ids run in release order, so the realm's level cap tells us where
    // its content stops: classic ends around 24000, Burning Crusade around
    // 35000.
    uint32 s_kitItemIdCeiling = 0xFFFFFFFF;

    void ComputeKitItemIdCeiling()
    {
        uint32 const maxLevel = sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL);
        if (maxLevel <= 60)
            s_kitItemIdCeiling = 24000;
        else if (maxLevel <= 70)
            s_kitItemIdCeiling = 35000;
        else
            s_kitItemIdCeiling = 0xFFFFFFFF;
    }

    void LoadObtainableItemsOnce()
    {
        char const* sources[] = {
            "SELECT DISTINCT item FROM npc_vendor",
            "SELECT DISTINCT Item FROM creature_loot_template",
            "SELECT DISTINCT Item FROM gameobject_loot_template",
            "SELECT DISTINCT Item FROM reference_loot_template",
            "SELECT DISTINCT Item FROM item_loot_template",
            "SELECT DISTINCT itemid FROM playercreateinfo_item",
        };

        for (char const* sql : sources)
        {
            QueryResult result = WorldDatabase.Query(sql);
            if (!result)
                continue;
            do
            {
                s_obtainableItems.insert((*result)[0].GetUInt32());
            } while (result->NextRow());
        }

        TC_LOG_INFO("playerbots.hardcore", "White field kit: {} item ids are obtainable in the world.",
            uint32(s_obtainableItems.size()));
    }

    // The duplicate -> source pairing the field-kit migration recorded.
    //
    // Fails OPEN: with no table, or a row whose source has gone, the
    // duplicate is judged on its own quality as before. That is the old
    // behaviour rather than an outage - a realm that never ran the migration
    // has no duplicates to classify in the first place.
    void LoadFieldKitSourcesOnce()
    {
        QueryResult result = WorldDatabase.Query(
            "SELECT kit_entry, source_entry FROM zz_fieldkit_map");
        if (!result)
        {
            TC_LOG_INFO("playerbots.hardcore", "Field kit: no zz_fieldkit_map; duplicates will be judged on their own quality.");
            return;
        }

        do
        {
            s_fieldKitSourceItem[(*result)[0].GetUInt32()] = (*result)[1].GetUInt32();
        } while (result->NextRow());

        TC_LOG_INFO("playerbots.hardcore", "Field kit: {} duplicates mapped back to their source item.",
            uint32(s_fieldKitSourceItem.size()));
    }

    // The quality that decides which tier a kit piece belongs to. Read
    // through to the original item, because the duplicate itself deliberately
    // reads artifact quality and no longer says anything about its tier.
    uint32 KitTierQuality(ItemTemplate const& proto)
    {
        auto itr = s_fieldKitSourceItem.find(proto.ItemId);
        if (itr != s_fieldKitSourceItem.end())
            if (ItemTemplate const* source = sObjectMgr->GetItemTemplate(itr->second))
                return source->Quality;

        return proto.Quality;
    }

    void BuildWhiteKitCacheOnce();

    // Built once, then read-only.
    void BuildWhiteKitCacheOnce()
    {
        std::lock_guard<std::mutex> guard(s_whiteKitLock);
        if (s_whiteKitBuilt)
            return;
        s_whiteKitBuilt = true;

        ComputeKitItemIdCeiling();
        LoadObtainableItemsOnce();
        LoadFieldKitSourcesOnce();

        // Prefer the non-sellable duplicates. They were filtered by exactly the
        // rules below at the moment they were created, so nothing here has to
        // judge them again - taking them wholesale is both correct and cheaper.
        for (auto const& itemPair : sObjectMgr->GetItemTemplateStore())
        {
            ItemTemplate const& proto = itemPair.second;
            if (!IsFieldKitDuplicate(proto.ItemId))
                continue;
            if (KitTierQuality(proto) == ITEM_QUALITY_POOR)
                s_greyKitByInvType[proto.InventoryType].push_back(proto.ItemId);
            else
                s_whiteKitByInvType[proto.InventoryType].push_back(proto.ItemId);
        }

        // Fall back to the real items when the duplicates are absent, so a realm
        // that has not run the migration still gets a field kit rather than none.
        bool const haveDuplicates = !s_whiteKitByInvType.empty() || !s_greyKitByInvType.empty();
        if (haveDuplicates)
            TC_LOG_INFO("scripts", "BarracksHardcore: field kit is using the non-sellable duplicates.");
        else
        {
            for (auto const& itemPair : sObjectMgr->GetItemTemplateStore())
            {
                ItemTemplate const& proto = itemPair.second;
                if (proto.Quality != ITEM_QUALITY_NORMAL && proto.Quality != ITEM_QUALITY_POOR)
                    continue;

                // Never hand out scaffolding, and never anything the world has no
                // way of producing on its own.
                if (LooksLikeScaffolding(proto.Name1))
                    continue;
                if (!s_obtainableItems.empty() && !s_obtainableItems.count(proto.ItemId))
                    continue;
                // Nothing from an expansion this realm does not run. Item ids are
                // laid down in release order, so the realm's level cap picks the
                // ceiling: a level 60 realm has no business issuing a Totem of the
                // Earthen Ring, which is perfectly legal data and pure anachronism
                // in a classic world.
                if (proto.ItemId > s_kitItemIdCeiling)
                    continue;
                if (proto.Class != ITEM_CLASS_ARMOR && proto.Class != ITEM_CLASS_WEAPON)
                    continue;
                if (proto.InventoryType == INVTYPE_NON_EQUIP)
                    continue;
                // Plain field gear only: nothing gated, zone-locked or bound.
                if (proto.RequiredLevel > 60 || proto.ItemLevel > 70)
                    continue;
                if (proto.RequiredSkill || proto.RequiredSpell || proto.RequiredReputationFaction ||
                    proto.RequiredHonorRank || proto.Area || proto.Map)
                    continue;
                if (proto.Bonding == BIND_QUEST_ITEM)
                    continue;
                // Nothing that expires: holiday masks and their kin carry a
                // duration and would rot off the wearer days later.
                if (proto.Duration)
                    continue;

                if (proto.Quality == ITEM_QUALITY_POOR)
                    s_greyKitByInvType[proto.InventoryType].push_back(proto.ItemId);
                else
                    s_whiteKitByInvType[proto.InventoryType].push_back(proto.ItemId);
            }
        }

        // Highest required level first, so a search can stop at its first
        // usable hit instead of walking thousands of items per slot for
        // every one of a few hundred logins.
        uint32 total = 0;
        uint32 greys = 0;
        for (auto* cache : { &s_whiteKitByInvType, &s_greyKitByInvType })
        {
            for (auto& kitPair : *cache)
            {
                std::sort(kitPair.second.begin(), kitPair.second.end(), [](uint32 left, uint32 right)
                {
                    ItemTemplate const* leftProto = sObjectMgr->GetItemTemplate(left);
                    ItemTemplate const* rightProto = sObjectMgr->GetItemTemplate(right);
                    if (leftProto->RequiredLevel != rightProto->RequiredLevel)
                        return leftProto->RequiredLevel > rightProto->RequiredLevel;
                    return leftProto->ItemLevel > rightProto->ItemLevel;
                });
                if (cache == &s_greyKitByInvType)
                    greys += uint32(kitPair.second.size());
                else
                    total += uint32(kitPair.second.size());
            }
        }
        TC_LOG_INFO("scripts", "BarracksHardcore: field kit cache built ({} white, {} grey items).", total, greys);
    }

    // Whether the world can produce this item at all - sold by a vendor,
    // dropped by something, or handed to a new character. Exported so the
    // playerbot manager can strip gear the world has no way of granting
    // without keeping a second copy of the same set.
    //
    // Fails OPEN: if the set has not been built yet, everything is considered
    // obtainable. The alternative is a pass that destroys the fleet's gear
    // because a query had not run.
    bool IsObtainableInWorld(uint32 itemId)
    {
        // A field-kit duplicate is legitimate by construction: it is a copy of an
        // item that IS obtainable. Judging the copy against the loot tables, which
        // will never mention it, would have the playerbot manager strip the kit
        // straight back off as scaffolding.
        if (IsFieldKitDuplicate(itemId))
            return true;

        BuildWhiteKitCacheOnce();
        return s_obtainableItems.empty() || s_obtainableItems.count(itemId) != 0;
    }

    // Classic armor proficiency: the heavy classes step up at 40.
    uint32 DesiredArmorSubclass(Player const* player)
    {
        switch (player->GetClass())
        {
        case CLASS_ROGUE:
        case CLASS_DRUID:
            return ITEM_SUBCLASS_ARMOR_LEATHER;
        case CLASS_HUNTER:
        case CLASS_SHAMAN:
            return player->GetLevel() >= 40 ? uint32(ITEM_SUBCLASS_ARMOR_MAIL) : uint32(ITEM_SUBCLASS_ARMOR_LEATHER);
        case CLASS_WARRIOR:
        case CLASS_PALADIN:
        case CLASS_DEATH_KNIGHT:
            return player->GetLevel() >= 40 ? uint32(ITEM_SUBCLASS_ARMOR_PLATE) : uint32(ITEM_SUBCLASS_ARMOR_MAIL);
        default:
            return ITEM_SUBCLASS_ARMOR_CLOTH;
        }
    }

    // The ammo this character actually shoots, as an ITEM_SUBCLASS_ARROW /
    // ITEM_SUBCLASS_BULLET value, or 0 when they have none.
    //
    // The equipped ammo field first, then the bags: a hunter who has arrows but
    // has not set them as current ammo still plainly shoots arrows, and issuing
    // them a gun on that technicality is the bug this exists to stop.
    uint32 CurrentAmmoSubclass(Player const* player)
    {
        if (uint32 const ammoId = player->GetUInt32Value(PLAYER_AMMO_ID))
            if (ItemTemplate const* ammo = sObjectMgr->GetItemTemplate(ammoId))
                if (ammo->Class == ITEM_CLASS_PROJECTILE)
                    return ammo->SubClass;

        auto fromItem = [](Item const* item) -> uint32
        {
            ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
            return proto && proto->Class == ITEM_CLASS_PROJECTILE ? proto->SubClass : 0u;
        };

        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (uint32 const found = fromItem(player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot)))
                return found;

        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (Bag* bag = const_cast<Player*>(player)->GetBagByPos(bagSlot))
                for (uint32 bagIndex = 0; bagIndex < bag->GetBagSize(); ++bagIndex)
                    if (uint32 const found = fromItem(bag->GetItemByPos(uint8(bagIndex))))
                        return found;

        return 0;
    }

    // Whether a ranged weapon suits the ammo the character is carrying.
    // Anything that is not ammo-fed - thrown, wands, relics - is unaffected.
    bool RangedWeaponSuitsAmmo(ItemTemplate const* proto, uint32 ammoSubclass)
    {
        if (!ammoSubclass || proto->Class != ITEM_CLASS_WEAPON)
            return true;

        switch (proto->SubClass)
        {
        case ITEM_SUBCLASS_WEAPON_BOW:
        case ITEM_SUBCLASS_WEAPON_CROSSBOW:
            return ammoSubclass == ITEM_SUBCLASS_ARROW;
        case ITEM_SUBCLASS_WEAPON_GUN:
            return ammoSubclass == ITEM_SUBCLASS_BULLET;
        default:
            return true;
        }
    }

    std::vector<uint32> InventoryTypesForSlot(uint8 slot, Player const* player)
    {
        switch (slot)
        {
        case EQUIPMENT_SLOT_HEAD:      return { INVTYPE_HEAD };
        case EQUIPMENT_SLOT_SHOULDERS: return { INVTYPE_SHOULDERS };
        case EQUIPMENT_SLOT_CHEST:     return { INVTYPE_CHEST, INVTYPE_ROBE };
        case EQUIPMENT_SLOT_WAIST:     return { INVTYPE_WAIST };
        case EQUIPMENT_SLOT_LEGS:      return { INVTYPE_LEGS };
        case EQUIPMENT_SLOT_FEET:      return { INVTYPE_FEET };
        case EQUIPMENT_SLOT_WRISTS:    return { INVTYPE_WRISTS };
        case EQUIPMENT_SLOT_HANDS:     return { INVTYPE_HANDS };
        case EQUIPMENT_SLOT_BACK:      return { INVTYPE_CLOAK };
        case EQUIPMENT_SLOT_MAINHAND:  return { INVTYPE_WEAPON, INVTYPE_WEAPONMAINHAND, INVTYPE_2HWEAPON };
        case EQUIPMENT_SLOT_OFFHAND:
        {
            // Dedicated off-hand weapons barely exist: the pool holds three of
            // them against forty-six ordinary one-handers, which is why a rogue
            // or hunter with Dual Wield came away with an empty off-hand almost
            // every time. A dual-wielder can hold INVTYPE_WEAPON in either
            // hand, so offer those too.
            //
            // INVTYPE_WEAPONMAINHAND stays out deliberately - it cannot go in an
            // off-hand at all, and offering it would just fail CanEquipNewItem
            // after the scan had already stopped on it.
            std::vector<uint32> types = { INVTYPE_SHIELD, INVTYPE_WEAPONOFFHAND, INVTYPE_HOLDABLE };
            if (player && player->CanDualWield())
                types.push_back(INVTYPE_WEAPON);
            return types;
        }
                                   // A hunter without a bow is not a hunter; casters get their wand
                                   // and the hybrids their relic out of the same slot.
        case EQUIPMENT_SLOT_RANGED:    return { INVTYPE_RANGED, INVTYPE_RANGEDRIGHT, INVTYPE_THROWN, INVTYPE_RELIC };
        default:                       return {};
        }
    }

    // Kit gear belongs in a slot or nowhere. It is issued straight into an
    // empty slot, a replaced piece is destroyed rather than bagged, and a
    // duplicate sells for nothing - so a copy sitting in a bag is either debris
    // from the spell where the kit was briefly lootable out of a death chest,
    // or a piece somebody moved out of a slot to dodge destroy-on-replace.
    // Either way it is swept.
    //
    // Positions are collected before anything is destroyed. DestroyItem shifts
    // what is in a slot, so deciding and acting in a single pass over live
    // containers is how items get missed.
    void DestroyLooseFieldKit(Player* player)
    {
        std::vector<std::pair<uint8, uint8>> loose;

        auto consider = [&loose](Item* item, uint8 bag, uint8 slot)
        {
            ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
            if (proto && IsFieldKitDuplicate(proto->ItemId))
                loose.push_back({ bag, slot });
        };

        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            consider(player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot), INVENTORY_SLOT_BAG_0, slot);

        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (Bag* bag = player->GetBagByPos(bagSlot))
                for (uint32 bagIndex = 0; bagIndex < bag->GetBagSize(); ++bagIndex)
                    consider(bag->GetItemByPos(uint8(bagIndex)), bagSlot, uint8(bagIndex));

        for (auto const& [bag, slot] : loose)
            player->DestroyItem(bag, slot, true);

        if (!loose.empty())
            TC_LOG_INFO("playerbots.hardcore", "Swept {} loose field kit pieces from {}'s bags.",
                uint32(loose.size()), player->GetName());
    }

    // Bot position desync watch.
    //
    // m_movementInfo.pos is what player-style packets tell observers; the unit's
    // own position is where the server actually has it. For a socketless bot
    // those are kept in step on every authoritative relocation, so a large gap
    // means something moved the bot without refreshing what observers are told -
    // which is exactly what a client draws as a bot standing somewhere it is not.
    //
    // Silent until it happens, and rate limited per bot: the point is to catch a
    // sporadic fault during ordinary play, not to add noise while it behaves.
    // thread_local for the same reason as the kit sweep above: this is on the
    // per-player tick, and a lock there would cost far more than the check.
    thread_local std::unordered_map<uint64, uint32> s_nextDivergenceCheckMs;
    thread_local std::unordered_map<uint64, uint32> s_nextDivergenceReportMs;
    constexpr uint32 DIVERGENCE_CHECK_INTERVAL_MS = 2000;
    constexpr uint32 DIVERGENCE_REPORT_INTERVAL_MS = 30000;

    void WatchMovementDivergence(Player* player)
    {
        if (!s_enabled || s_movementDivergenceYards <= 0.0f || !player || !IsPlayerbot(player))
            return;

        uint64 const rawGuid = player->GetGUID().GetRawValue();
        uint32 const nowMs = GameTime::GetGameTimeMS();
        uint32& nextCheckMs = s_nextDivergenceCheckMs[rawGuid];
        if (nextCheckMs && nowMs < nextCheckMs)
            return;
        nextCheckMs = nowMs + DIVERGENCE_CHECK_INTERVAL_MS;

        Position const& told = player->m_movementInfo.pos;
        float const drift = player->GetExactDist(told.GetPositionX(), told.GetPositionY(), told.GetPositionZ());
        if (drift < s_movementDivergenceYards)
            return;

        uint32& nextReportMs = s_nextDivergenceReportMs[rawGuid];
        if (nextReportMs && nowMs < nextReportMs)
            return;
        nextReportMs = nowMs + DIVERGENCE_REPORT_INTERVAL_MS;

        MotionMaster* motion = player->GetMotionMaster();
        TC_LOG_ERROR("playerbots.hardcore",
            "BOT DESYNC: {} (class {}) drifted {:.1f}y - server ({:.1f}, {:.1f}, {:.1f}) vs told "
            "({:.1f}, {:.1f}, {:.1f}). moving={} generator={} combat={} casting={} "
            "speed={:.2f} rooted={} zone={} map={}",
            player->GetName(), uint32(player->GetClass()), drift,
            player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(),
            told.GetPositionX(), told.GetPositionY(), told.GetPositionZ(),
            player->isMoving() ? "yes" : "no",
            motion ? uint32(motion->GetCurrentMovementGeneratorType()) : 999u,
            player->IsInCombat() ? "yes" : "no",
            player->HasUnitState(UNIT_STATE_CASTING) ? "yes" : "no",
            player->GetSpeed(MOVE_RUN),
            player->HasUnitState(UNIT_STATE_ROOT) ? "yes" : "no",
            player->GetZoneId(), player->GetMapId());
    }

    // Kit gear belongs in a slot or nowhere, so a copy that turns up in a bag
    // is swept whatever put it there. Throttled: the scan walks the backpack
    // and every bag, which is far too much to repeat on every tick of every
    // bot.
    void SweepLooseFieldKitThrottled(Player* player)
    {
        if (!s_enabled || !player)
            return;

        uint64 const rawGuid = player->GetGUID().GetRawValue();
        uint32 const nowMs = GameTime::GetGameTimeMS();
        uint32& nextSweepMs = s_nextKitSweepMs[rawGuid];
        if (nextSweepMs && nowMs < nextSweepMs)
            return;
        nextSweepMs = nowMs + KIT_SWEEP_INTERVAL_MS;

        DestroyLooseFieldKit(player);
    }

    // Whether this character is ALLOWED to wear the kit at all.
    //
    // Two challenge modes refuse anything above common quality outright, and
    // the kit reads artifact - so for a player running either of them every kit
    // piece is rejected by their own rules. Burning their white gear would
    // leave them permanently unable to equip ANYTHING, which is not a hardcore
    // rule, it is a bricked character. One person on the realm is running Iron
    // Man today; that is one too many to find out the hard way.
    //
    // The refusal itself lives in ChallengeModes' OnPlayerCanEquipItem hook,
    // which needs a real Item to ask, so it cannot be probed from here. The
    // settings are asked directly instead.
    bool KitMayDress(Player const* player)
    {
        return player && !sChallengeModes->IsEnabledForPlayer(SETTING_IRON_MAN, player) &&
            !sChallengeModes->IsEnabledForPlayer(SETTING_ITEM_QUALITY_LEVEL, player);
    }

    // Everything WORN in a slot the kit dresses that is neither issued kit nor
    // worth putting in a chest.
    //
    // The kit is the floor, and it is now the ONLY floor. White and grey gear
    // used to survive a death on the grounds that it kept the victim able to
    // swing back - but the kit already does that, and letting earned white gear
    // stay produced two lasting problems. A slot the player filled with a white
    // drop was never revisited, because the kit only ever fills an EMPTY slot,
    // so a level 50 character could still be wearing the white bracers they
    // picked up at 12. And the result read as neither one thing nor the other:
    // half issued red, half scavenged white, with no way to tell at a glance
    // which half a death would take.
    //
    // Burned rather than dropped. A corpse's white gear is not a prize, and a
    // chest full of it would bury the one green piece that is.
    //
    // ONLY kKitSlots, which is the whole point: destroy nothing the kit cannot
    // put back. The death rule's own loop walks all nineteen worn slots because
    // green and better go to a chest and are meant to be lost - but neck,
    // rings, trinkets, shirt and tabard have no kit piece behind them, so
    // burning a white one there would empty the slot for good.
    //
    // Positions are collected before anything is destroyed, matching
    // DestroyLooseFieldKit. Equipment slots are fixed and do not shift the way
    // a bag's do, but DestroyItem fans out into the set-bonus evaluators, and
    // those read the very slots being walked.
    uint32 BurnWornFloorGear(Player* player)
    {
        if (!s_enabled || !player || !KitMayDress(player))
            return 0;

        std::vector<uint8> burning;
        for (uint8 slot : kKitSlots)
        {
            Item const* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
            if (!proto)
                continue;
            // The kit's own duplicates are asked about directly rather than by
            // colour: they read artifact quality, so a quality test alone would
            // put the floor itself on the fire.
            if (IsFieldKitDuplicate(proto->ItemId) || proto->Quality >= ITEM_QUALITY_UNCOMMON)
                continue;
            burning.push_back(slot);
        }

        for (uint8 slot : burning)
            player->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);

        return uint32(burning.size());
    }

    // Fills every empty kit slot with the best plain white piece the wearer's
    // level and proficiency allow. Runs on resurrection (dead players cannot
    // equip anything) and at login, so nobody stays bare.
    void IssueWhiteFieldKit(Player* player)
    {
        if (!s_enabled || !player || !player->IsAlive() || player->IsGameMaster())
            return;

        BuildWhiteKitCacheOnce();
        DestroyLooseFieldKit(player);

        uint32 const wantedArmorSubclass = DesiredArmorSubclass(player);
        uint8 const level = player->GetLevel();

        // The level the ITEM SEARCH runs at, deliberately below the wearer's.
        // Floored at 1 rather than 0 so the bottom of the game still finds the
        // level-1 pieces instead of coming up empty and leaving a slot bare.
        uint32 const levelOffset = IsPlayerbot(player) ? s_kitBotLevelOffset : s_kitLevelOffset;
        uint8 const kitLevel = uint8(std::max(1, int32(level) - int32(levelOffset)));

        // Read once: it cannot change while the kit is being handed out, and
        // the bag scan behind it is not worth repeating per candidate.
        uint32 const ammoSubclass = CurrentAmmoSubclass(player);
        uint32 granted = 0;

        for (uint8 slot : kKitSlots)
        {
            if (Item* worn = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            {
                // A kit piece that outclasses the level the kit now dresses
                // for is retired here rather than left on. Otherwise lowering
                // the offset would only ever reach an EMPTY slot, so anyone
                // already wearing a full kit would keep the old set for good.
                //
                // Strictly limited to the kit's own duplicates. Gear the
                // player actually earned is never touched, however far above
                // the kit level it sits - that gear is the entire point.
                // Retired in BOTH directions. Only checking for a piece that was
                // too good left the opposite case running forever: a bot reborn at
                // the bottom of its band is kitted for that level and then climbs
                // thirty levels still wearing it, because the kit otherwise only
                // fills an EMPTY slot. Measured on the fleet, worn kit averaged
                // 18 levels behind its wearer and reached 41.
                //
                // The tolerance stops a piece being rebought every level: within a
                // few levels the pool rarely holds anything different anyway.
                ItemTemplate const* wornProto = worn->GetTemplate();
                if (!wornProto || !IsFieldKitDuplicate(wornProto->ItemId))
                    continue;

                bool const tooGood = wornProto->RequiredLevel > kitLevel;
                bool const fallenBehind = int32(kitLevel) - int32(wornProto->RequiredLevel) >
                    int32(s_kitStaleLevels);
                if (!tooGood && !fallenBehind)
                    continue;

                player->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);
            }

            uint32 bestItemId = 0;
            uint32 bestRequiredLevel = 0;
            uint32 bestItemLevel = 0;

            for (uint32 invType : InventoryTypesForSlot(slot, player))
            {
                // Greys below the threshold, whites from there on, and white
                // as the safety net underneath both.
                //
                // The fallback used to fire only when the grey cache held NO
                // list at all for this inventory type. That is a different
                // question from the one it meant to ask, and the gap is where
                // a whole character falls through: the grey pool holds nine
                // daggers and not one of them below required level 2, so a
                // brand new character asked for a weapon, was handed the grey
                // list, found nothing in it it could use, and walked out of the
                // starting zone bare-handed. Grey helms start at 25 and grey
                // shoulders at 15, so those two slots were empty for everybody
                // below level 15 as well.
                //
                // Falling through when the grey list yields nothing AT THIS
                // LEVEL is what "no grey to offer" was always meant to mean.
                std::vector<std::vector<uint32> const*> tiers;
                {
                    std::lock_guard<std::mutex> guard(s_whiteKitLock);
                    auto offer = [&tiers](std::unordered_map<uint32, std::vector<uint32>> const& cache,
                        uint32 wanted)
                    {
                        auto itr = cache.find(wanted);
                        if (itr != cache.end() && !itr->second.empty())
                            tiers.push_back(&itr->second); // immutable once built
                    };
                    if (level < s_greyKitMaxLevel)
                        offer(s_greyKitByInvType, invType);
                    offer(s_whiteKitByInvType, invType);
                }

                for (std::vector<uint32> const* ids : tiers)
                {
                    // Whether this tier had anything wearable at all. A tier that
                    // produced a candidate settles the inventory type even when
                    // that candidate lost to one from another type - only a tier
                    // with nothing to give falls through to the next.
                    bool tierOffered = false;

                    // The list is sorted by required level descending: the first
                    // usable entry is already the best this slot can offer, so
                    // the scan stops there.
                    for (uint32 itemId : *ids)
                    {
                        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
                        if (!proto || proto->RequiredLevel > kitLevel)
                            continue;

                        // A bow is no use to somebody carrying bullets. Skipping here
                        // rather than after the scan matters: the list is sorted and
                        // the loop stops on its first usable entry, so a mismatched
                        // weapon that got that far would take the slot outright.
                        if (!RangedWeaponSuitsAmmo(proto, ammoSubclass))
                            continue;

                        // Armor proficiency, decided explicitly. Cloaks are cloth
                        // for every class; shields and relics (libram, idol,
                        // totem) are their own subclasses and are gated by the
                        // proficiency check below instead.
                        if (proto->Class == ITEM_CLASS_ARMOR &&
                            proto->InventoryType != INVTYPE_CLOAK &&
                            proto->InventoryType != INVTYPE_SHIELD &&
                            proto->InventoryType != INVTYPE_RELIC &&
                            proto->SubClass != wantedArmorSubclass)
                            continue;

                        if (player->CanUseItem(proto) != EQUIP_ERR_OK)
                            continue;

                        // PROFICIENCY. CanUseItem(ItemTemplate const*) does NOT
                        // check it - the gate lives in the CanUseItem(Item*)
                        // overload, reached only through CanEquipItem. Without
                        // this a priest is handed the highest-level two-handed
                        // axe in the game: StoreNewItemInBestSlots quietly drops
                        // it in the bags, reports success, and the empty slot
                        // earns another copy on every single resurrection.
                        if (uint32 const itemSkill = proto->GetSkill())
                            if (!player->GetSkillValue(itemSkill))
                                continue;

                        // The authority on whether this can actually be worn.
                        uint16 equipDest = 0;
                        if (player->CanEquipNewItem(NULL_SLOT, equipDest, itemId, false) != EQUIP_ERR_OK)
                            continue;

                        // Closest to the wearer's own level, then the better item.
                        if (proto->RequiredLevel > bestRequiredLevel ||
                            (proto->RequiredLevel == bestRequiredLevel && proto->ItemLevel > bestItemLevel))
                        {
                            bestItemId = itemId;
                            bestRequiredLevel = proto->RequiredLevel;
                            bestItemLevel = proto->ItemLevel;
                        }
                        tierOffered = true;
                        break;
                    }

                    if (tierOffered)
                        break;
                }
            }

            // Equip or nothing: StoreNewItemInBestSlots would silently bag a
            // piece it could not wear and still report success, which leaves
            // the slot empty and earns another copy at the next death.
            if (!bestItemId)
                continue;

            uint16 equipDest = 0;
            if (player->CanEquipNewItem(NULL_SLOT, equipDest, bestItemId, false) != EQUIP_ERR_OK)
                continue;
            if (player->EquipNewItem(equipDest, bestItemId, true))
                ++granted;
        }

        // A ranged weapon with nothing to fire is a stick: whoever ends up
        // holding a bow, crossbow or gun gets a basic quiver's worth loaded.
        // Thrown weapons are their own ammunition and wands need none.
        if (Item const* ranged = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED))
        {
            if (ItemTemplate const* rangedProto = ranged->GetTemplate())
            {
                uint32 ammoId = 0;
                if (rangedProto->SubClass == ITEM_SUBCLASS_WEAPON_BOW ||
                    rangedProto->SubClass == ITEM_SUBCLASS_WEAPON_CROSSBOW)
                    ammoId = 2512; // Rough Arrow
                else if (rangedProto->SubClass == ITEM_SUBCLASS_WEAPON_GUN)
                    ammoId = 2516; // Light Shot

                if (ammoId && !player->GetItemCount(ammoId) && !player->GetUInt32Value(PLAYER_AMMO_ID))
                {
                    ItemPosCountVec ammoDest;
                    if (player->CanStoreNewItem(NULL_BAG, NULL_SLOT, ammoDest, ammoId, 200) == EQUIP_ERR_OK)
                    {
                        player->StoreNewItem(ammoDest, ammoId, true);
                        ++granted;
                    }
                }
                if (ammoId && player->GetItemCount(ammoId) && !player->GetUInt32Value(PLAYER_AMMO_ID))
                    player->SetAmmo(ammoId);
            }
        }

        if (granted)
        {
            player->SaveToDB(false);
            TC_LOG_INFO("playerbots.hardcore", "Issued {} pieces of white field kit to {}.",
                granted, player->GetName());
        }
    }

    // Full loot: worn GREEN AND BETTER equipment is at stake. White and grey
    // gear is the floor and never drops, and money is always safe. A person's
    // bags are safe too; a bot's carried gear is not - see below.
    void DropFullLootChest(Player* victim)
    {
        if (!s_enabled || !s_chestEntry || !victim || victim->IsGameMaster())
            return;

        if (!IsWorldContext(victim))
            return;

        // Nothing is taken inside a dungeon or a raid. A wipe is a group failing
        // at PvE content, not somebody losing a fight in the open world, and
        // stripping everyone's gear on a wipe would end instance running on this
        // realm outright.
        //
        // This one gate covers all three ways gear leaves you, because they all
        // funnel through here: the chest that drops green-and-better worn items,
        // the white/grey burn (BurnWornFloorGear runs from this function and
        // nowhere else on the death path), and a bot's carried gear below.
        if (IsInstancedContent(victim))
            return;

        // A bot only pays out where somebody could actually collect. The fleet
        // dies to the world about a hundred times more often than to a person -
        // 8197 creature deaths against 85 player kills in one log window - and
        // every one of those was quietly deleting a bot's gear into a chest
        // nobody was ever going to walk to. The corpse run gives up after the
        // second death, so a bot that gets in over its head never gets it back:
        // that is how a level 60 ends up grinding Eastern Plaguelands in white
        // kit nobody took off him.
        //
        // Same witness test the remote chest looting uses, so "is anyone
        // actually here" has one answer on this realm rather than two. Ghosts
        // count - somebody running their corpse back is still a person who came
        // for this - and bots do not, or the fleet would witness itself.
        // 0 restores the old behaviour: a bot always pays out.
        if (IsPlayerbot(victim) && s_botLootWitnessYards > 0.0f &&
            !playerbot::PveManager::AnyPersonWithin(victim, s_botLootWitnessYards))
            return;

        // The Battle Ring pays out through its own chest and nothing else. The
        // hourly event is a scrap over one prize, not a gear sink: a cache for
        // every death in there would bury the prize under a field of loot and
        // make the ring the cheapest place on the realm to farm gear.
        if (victim->IsInGurubashiBattleRing())
            return;

        CustomLootChests::PlayerChestBuilder chest(victim, s_chestEntry, Seconds(s_chestDespawnSeconds));
        std::vector<CustomLootChests::ItemLocation> droppedItems;

        // ONLY worn equipment is at stake: bags, their contents and money
        // are safe. Deflation: each worn piece leaves the corpse, but only
        // a configurable share reaches the chest - the rest simply burns.
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            Item* item = victim->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!item)
                continue;

            // The floor survives every death; only green and above is ever
            // lost, so nobody is ever left unable to fight back.
            //
            // The kit's own duplicates are asked about directly rather than by
            // colour. They read artifact quality now, so the quality test alone
            // put the floor itself at stake - and a kit piece is exempt whatever
            // colour it reads, which makes it the stronger claim of the two.
            ItemTemplate const* proto = item->GetTemplate();
            if (!proto || IsFieldKitDuplicate(proto->ItemId) ||
                proto->Quality < ITEM_QUALITY_UNCOMMON)
                continue;

            if (urand(0, 99) < s_dropChancePercent)
                chest.AddItem(item);
            droppedItems.push_back({ INVENTORY_SLOT_BAG_0, slot });
        }

        // Bots put their CARRIED gear at stake as well. A person keeps their
        // bags - losing what you were wearing is the rule, being emptied out is
        // a different game - but a bot is not playing for keeps, and a bot's
        // bags are where gear goes to stop existing: it hoards upgrades it has
        // outlevelled, and a zone-band rebirth moves everything it was wearing
        // straight into them. Left safe, that gear leaves the economy for good.
        //
        // Only weapons and armour, and only green and above, matching the worn
        // rule exactly. Bags themselves, trade goods, consumables and reagents
        // are untouched - a bot stripped of its containers could not carry the
        // field kit it is about to be issued.
        if (IsPlayerbot(victim))
        {
            auto stakeCarriedGear = [&](Item* item, uint8 bag, uint8 slot)
            {
                ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
                if (!proto || IsFieldKitDuplicate(proto->ItemId) ||
                    proto->Quality < ITEM_QUALITY_UNCOMMON)
                    return;
                if (proto->Class != ITEM_CLASS_WEAPON && proto->Class != ITEM_CLASS_ARMOR)
                    return;

                if (urand(0, 99) < s_dropChancePercent)
                    chest.AddItem(item);
                droppedItems.push_back({ bag, slot });
            };

            for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
                stakeCarriedGear(victim->GetItemByPos(INVENTORY_SLOT_BAG_0, slot), INVENTORY_SLOT_BAG_0, slot);

            for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
                if (Bag* bag = victim->GetBagByPos(bagSlot))
                    for (uint32 bagIndex = 0; bagIndex < bag->GetBagSize(); ++bagIndex)
                        stakeCarriedGear(bag->GetItemByPos(uint8(bagIndex)), bagSlot, uint8(bagIndex));
        }

        // Money is SAFE from the GEAR rule - nothing here ever costs a player
        // a copper for what they were wearing. A bounty is a different debt
        // entirely (custom_bounty.h) and settles into this same chest, so a
        // corpse leaves one object behind rather than two stacked on a spot.
        //
        // Taking it here is also what deducts it: the debt is recorded at
        // death and collected only once something exists to hold the coin.
        uint32 const bountyGold = Bounty::TakePendingChestGold(victim);
        chest.AddMoney(bountyGold);

        // The chest is only summoned when something could actually be IN it -
        // an empty one would be a cruel joke - but the loss happens either way.
        // Bailing out early on an empty chest meant a victim wearing a single
        // green piece kept it half the time, and the deflationary half of the
        // rule quietly never ran.
        //
        // White and grey never reach the chest, so they cannot be part of this
        // decision: a victim wearing nothing but the floor still loses it, and
        // still leaves no chest behind.
        bool const chestSpawned = (!droppedItems.empty() || bountyGold) && chest.Summon() != nullptr;

        // The PvP-only fleet pays out but is never stripped.
        //
        // Those bots ARE the battleground population, and taking their gear
        // every time somebody farms one degrades the thing they exist for: a
        // stripped bot is re-kitted in white, and after a few deaths the
        // battlegrounds are being fought by naked people. Their gear is what
        // makes them worth queueing against.
        //
        // This does make them a FAUCET rather than a transfer - the chest is a
        // copy, conjured out of nothing. That is the trade, and it is the only
        // place on the realm where the deflationary half of this rule is skipped.
        bool const keepsGear = playerbot::PveManager::IsPvpOnlyBot(victim);
        if (!keepsGear)
            for (CustomLootChests::ItemLocation const& dropped : droppedItems)
                victim->DestroyItem(dropped.Bag, dropped.Slot, true);

        // ...and the floor burns with it, so what stands back up is wearing
        // issued kit and nothing else. Same exemption: a PvP-only bot keeps
        // everything, for the same reason it keeps its greens.
        //
        // After the chest, not before, so nothing here can affect what reached
        // it. The kit itself is untouched - it is re-issued on resurrection
        // rather than rebought, and a bare corpse run is the same one a death
        // has always cost.
        uint32 const burned = keepsGear ? 0u : BurnWornFloorGear(victim);

        if (droppedItems.empty() && !burned && !bountyGold)
            return;

        TC_LOG_INFO("playerbots.hardcore", "{} {} {} worn items, burned {} of floor gear and {}c bounty at death; chest spawned: {} ({}% of gear reaches it).",
            victim->GetName(), keepsGear ? "copied" : "lost", uint32(droppedItems.size()),
            burned, bountyGold, uint32(chestSpawned), s_dropChancePercent);
    }
}

using namespace BarracksHardcore;
using namespace Trinity::ChatCommands;

class barracks_hardcore_player : public PlayerScript
{
public:
    barracks_hardcore_player() : PlayerScript("barracks_hardcore_player") {}

    void OnLogin(Player* player, bool firstLogin) override
    {
        if (!s_enabled)
            return;

        uint32 const guidLow = player->GetGUID().GetCounter();
        if (QueryResult result = CharacterDatabase.PQuery("SELECT 1 FROM character_ffa_optin WHERE guid = {}", guidLow))
        {
            std::unique_lock<std::shared_mutex> guard(s_optInLock);
            s_optInGuids.insert(guidLow);
        }
        ApplyFfaState(player);
        ApplyWarModeAura(player);

        // A new character starts in issued kit rather than in the outfit
        // character creation handed it.
        //
        // Done here and not in OnPlayerCreate: that hook fires from inside the
        // create transaction's completion callback, on a Player that is not in
        // the world and is about to be destroyed, so equipping through it would
        // mean a second save racing the first. AT_LOGIN_FIRST is consumed by
        // HandlePlayerLogin before this runs and OnPlayerLogin is its last
        // statement, so by here the character is fully in the world and this is
        // the ordinary equip path.
        //
        // The starting outfit is white and grey by definition, so the same rule
        // that runs at every death does the whole job: burn the floor, then let
        // the kit dress the slots it emptied. A character therefore meets the
        // death rule for the first time before it has ever been in danger, and
        // sees exactly the gear it will keep seeing.
        if (firstLogin)
            BurnWornFloorGear(player);

        // Anyone already stripped bare (deaths taken before the kit existed)
        // is dressed on the way in.
        IssueWhiteFieldKit(player);
    }

    // Fires for real players and for bots alike: the bot manager revives its
    // dead through Player::ResurrectPlayer, which drives this same hook.
    void OnPlayerResurrect(Player* player) override
    {
        IssueWhiteFieldKit(player);
    }

    void OnUpdateZone(Player* player, uint32 /*newZone*/, uint32 /*newArea*/) override
    {
        ApplyFfaState(player);
        ApplyWarModeAura(player);
    }

    // Player::UpdateArea strips the FFA byte on every sub-area crossing and
    // there is no area-change script hook, so the state is re-asserted here.
    // No throttle: a PlayerScript is a singleton shared by every player, so a
    // member timer would only ever service one of them. ApplyFfaState costs a
    // couple of hash lookups and does nothing when nothing has drifted.
    // War Mode is a decision about whether you can be attacked, so a party
    // split down the middle on it cannot work: half of it is fair game in the
    // open world and half is not.
    //
    // The invite is the one choke point both directions pass through, so it
    // does not matter who invited whom. The core refuses with
    // ERR_INVITE_RESTRICTED when this returns false, which renders as a
    // generic "cannot invite" - so the reason is spelled out first.
    bool OnCanGroupInvite(Player* player, std::string& memberName) override
    {
        if (!s_enabled || !player)
            return true;

        Player* target = ObjectAccessor::FindPlayerByName(memberName);
        if (!target || !WarModeBlocksGrouping(player, target))
            return true;

        ChatHandler(player->GetSession()).PSendSysMessage(
            "%s has War Mode %s and you have it %s. You cannot group until you both "
            "set it the same way.",
            target->GetName().c_str(),
            IsWarModeOptedIn(target) ? "on" : "off",
            IsWarModeOptedIn(player) ? "on" : "off");
        return false;
    }

    void OnUpdate(Player* player, uint32 /*diff*/) override
    {
        EnforceAlwaysPvP(player);
        ApplyFfaState(player);
        ApplyWarModeAura(player);
        SweepLooseFieldKitThrottled(player);
        WatchMovementDivergence(player);
    }

    void OnMapChanged(Player* player) override
    {
        ApplyFfaState(player);
        ApplyWarModeAura(player);
    }

    void OnGiveXP(Player* player, uint32& amount, Unit* victim) override
    {
        if (!s_enabled || s_rewardMultiplier < 2)
            return;

        // Experience for killing a PERSON is not a War Mode reward and must not
        // be multiplied here. That award is already derived from the victim's own
        // level and split between everyone who shared the kill, so scaling it
        // again would pay a flagged player several times over for one corpse - on
        // top of the honor bonus the aura carries in its own right.
        //
        // The aura's SPELL_AURA_MOD_XP_PCT never reaches it either: KillRewarder
        // applies that in _RewardXP, which is the PvE path, and the PvP award
        // calls GiveXP directly.
        if (victim && victim->GetTypeId() == TYPEID_PLAYER)
            return;

        // No War Mode reward behind an instance portal. The badge itself is
        // pulled in there so the aura cannot pay either; this closes the config
        // fallback for a realm running without the aura.
        if (IsInstancedContent(player))
            return;

        // The aura's own SPELL_AURA_MOD_XP_PCT has already been applied by
        // KillRewarder before this runs, so the config path is the fallback for
        // a realm with no War Mode aura, exactly as for gold.
        if (WarModeAuraProvidesXpBonus(player))
            return;

        // The reward rides the RISK: only while the flag is actually armed,
        // and only for real players - bots level at the normal pace.
        WorldSession const* session = player->GetSession();
        if (session && IsBotAccount(session->GetAccountId()))
            return;

        if (IsFfaArmed(player))
            amount *= s_rewardMultiplier;
    }

    // Gold bonus carried by the War Mode aura itself rather than by the engine,
    // so it is retuned by editing the spell instead of rebuilding. Effect 1 is
    // a DUMMY whose base points ARE the percentage.
    //
    // Honor deliberately has no counterpart here: effect 0 is a real
    // SPELL_AURA_MOD_HONOR_GAIN_PCT, which Player::RewardHonor already applies
    // on its own ("AddPct(honor_f, GetMaxPositiveAuraModifier(...))"), so the
    // honor half of this feature is pure data and needs no code at all.
    // True when the aura is carrying the experience bonus itself. If it is, the
    // XP hook below must keep its hands off: SPELL_AURA_MOD_XP_PCT is applied
    // natively by KillRewarder ("xp *= GetTotalAuraMultiplier(...)"), so adding
    // the config multiplier on top would double-count and pay quadruple.
    bool WarModeAuraProvidesXpBonus(Player const* player)
    {
        if (!s_warModeAuraSpell || !player)
            return false;

        Aura const* aura = player->GetAura(s_warModeAuraSpell);
        if (!aura)
            return false;

        AuraEffect const* effect = aura->GetEffect(EFFECT_0);
        return effect && effect->GetAuraType() == SPELL_AURA_MOD_XP_PCT && effect->GetAmount() > 0;
    }

    int32 WarModeGoldBonusPct(Player const* player)
    {
        if (!s_warModeAuraSpell || !player)
            return 0;

        Aura const* aura = player->GetAura(s_warModeAuraSpell);
        if (!aura)
            return 0;

        AuraEffect const* effect = aura->GetEffect(EFFECT_1);
        return effect ? effect->GetAmount() : 0;
    }

    void OnMoneyChanged(Player* player, int32& amount) override
    {
        if (!s_enabled || amount <= 0)
            return;

        // Loot pickups only: the loot window is open while money is looted,
        // which keeps vendor sales, mail and the auction house at face value.
        if (!player->GetLootGUID())
            return;

        WorldSession const* session = player->GetSession();
        if (session && IsBotAccount(session->GetAccountId()))
            return;

        if (!IsFfaArmed(player))
            return;

        // Same rule as the experience: the doubled gold is paid for carrying the
        // risk of open-world PvP, and an instance has none.
        if (IsInstancedContent(player))
            return;

        // Prefer the aura's own number; fall back to the config multiplier so a
        // realm with no War Mode aura configured behaves exactly as before.
        if (int32 const goldPct = WarModeGoldBonusPct(player); goldPct > 0)
            amount += CalculatePct(amount, goldPct);
        else if (s_rewardMultiplier >= 2)
            amount += amount * int32(s_rewardMultiplier - 1);
    }

    // Scale the kill by the level gap, mirroring the creature experience curve
    // in Formulas.h so the con colours mean exactly what a player already
    // expects from mobs:
    //
    //   grey   - worth nothing at all, same threshold the client uses to grey
    //            the name out
    //   green  - falls off linearly, reaching zero exactly at the grey line
    //   yellow - full value
    //   orange - +5% per level above, matching the mob curve
    //   red    - capped at +4 levels, so 1.2x, again as mobs are capped
    float PlayerKillConScale(uint8 killerLevel, uint8 victimLevel)
    {
        if (victimLevel >= killerLevel)
        {
            uint8 levelDiff = victimLevel - killerLevel;
            if (levelDiff > 4)
                levelDiff = 4;

            return float(20 + levelDiff) / 20.0f;
        }

        if (victimLevel <= Trinity::XP::GetGrayLevel(killerLevel))
            return 0.0f;

        uint8 const zeroDifference = Trinity::XP::GetZeroDifference(killerLevel);
        if (!zeroDifference)
            return 0.0f;

        return float(zeroDifference + victimLevel - killerLevel) / float(zeroDifference);
    }

    // Experience for an open-world PvP kill.
    //
    // Counted in BUBBLES: the experience bar is drawn as twenty segments, so a
    // bubble is 5% of a level and the whole bar is twenty of them. The reward
    // is therefore a share of the KILLER's own next-level requirement, which
    // keeps it worth the same at every level instead of becoming irrelevant.
    //
    // This custom reward is WORLD PvP only. Battlegrounds and arenas already
    // have their own reward systems and must never receive this extra XP.
    void AwardPlayerKillExperience(Player* killer, Player* victim)
    {
        if (!s_enabled || s_playerKillXpBubbles <= 0.0f)
            return;

        if (!killer || !victim || killer == victim)
            return;

        if (killer->InBattleground() || victim->InBattleground())
            return;

        // Everyone who shared the kill, the way honor divides it: the killer,
        // plus any group member close enough to have been part of the fight.
        std::vector<Player*> recipients;
        if (Group* group = killer->GetGroup())
        {
            for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
                if (Player* member = itr->GetSource())
                    if (member == killer || (member->IsAlive() && member->IsAtGroupRewardDistance(victim)))
                        recipients.push_back(member);
        }
        if (recipients.empty())
            recipients.push_back(killer);

        // Measured on the VICTIM's bar rather than the killer's: what a kill is
        // worth is decided by who died. A twentieth of that bar is one bubble,
        // so the same victim is worth the same to everybody who helped.
        uint32 const victimLevelXp = sObjectMgr->GetXPForLevel(victim->GetLevel());
        if (!victimLevelXp)
            return;

        // Scaled by how much of the victim's death people were responsible for.
        // A quarter of the damage from players pays a quarter of the experience,
        // so letting a mob do the work and last-hitting is worth what it should
        // be worth. Pets count as their owner.
        float const playerShare = victim->GetPvpDamageShare();

        float const perHead = float(victimLevelXp) * s_playerKillXpBubbles
            / 20.0f * playerShare / float(recipients.size());

        for (Player* member : recipients)
        {
            if (!member->IsAlive())
                continue;

            // Only real people are paid. Bots fight each other constantly on an
            // FFA realm - the zone guardians hunt each other by design - so
            // paying them would run the whole fleet to the cap in minutes. The
            // VICTIM may be either: this is the open-world PvP reward, not a
            // playerbot-only one.
            if (IsPlayerbot(member))
                continue;

            // Con is judged per recipient, because a group can be any spread of
            // levels and a grey victim is worth nothing to the person it is grey
            // to - even if it was worth something to whoever landed the blow.
            float const conScale = PlayerKillConScale(member->GetLevel(), victim->GetLevel());
            if (conScale <= 0.0f)
                continue;

            // And this member's OWN diminishing return against this victim.
            // Held per killer and per victim, so breaking the group up resets
            // nobody's count and somebody who has never killed this victim is
            // not punished for another player's farming.
            float const repeat = member->ConsumePvpXpDiminishing(victim->GetGUID(),
                s_playerKillDiminishSeconds, s_playerKillXpDecayPerKill);

            uint32 const amount = uint32(perHead * conScale * repeat);
            if (!amount)
                continue;

            // GiveXP walks multiple levels if the award overflows, and honours
            // the level cap and the XP-frozen flag on its own.
            member->GiveXP(amount, victim);

            TC_LOG_INFO("playerbots.hardcore",
                "{} (level {}) shared the kill of {} {} (level {}) for {} xp "
                "({} bubbles x{:.2f} con x{:.2f} repeat x{:.2f} player-damage, one of {} shares).",
                member->GetName(), member->GetLevel(), IsPlayerbot(victim) ? "playerbot" : "player",
                victim->GetName(), victim->GetLevel(), amount, s_playerKillXpBubbles,
                conScale, repeat, playerShare, uint32(recipients.size()));
        }
    }

    void OnPVPKill(Player* killer, Player* victim) override
    {
        AwardPlayerKillExperience(killer, victim);

        // Bots are players, so bot-on-bot kills land here rather than in the
        // creature hook - and on a full-loot FFA realm the guardians hunt each
        // other constantly. A bot stripping another bot on an empty hillside
        // has the same problem as a bot stripped by a mob: nobody can reach
        // the chest, and the fleet grinds itself out of its gear. A kill
        // involving a real person, or witnessed by one, still costs the gear.
        if (victim && IsPlayerbot(victim) && (!killer || IsPlayerbot(killer)) &&
            !AnyHumanPlayerWithin(victim, 200.0f))
            return;

        DropFullLootChest(victim);
    }

    void OnPlayerKilledByCreature(Creature* killer, Player* victim) override
    {
        // Unit::Kill routes a PET killing blow through OnPlayerKilledByCreature
        // because the literal attacker is a Creature. Resolve the hunter/warlock
        // pet back to its real-player owner and award the exact same WORLD-PvP
        // XP as a direct player killing blow. AwardPlayerKillExperience itself
        // rejects BGs/arenas, bots as killers, gray victims and level-cap XP.
        if (killer && killer->IsPet())
            if (Player* owner = killer->GetCharmerOrOwnerPlayerOrPlayerItself())
                AwardPlayerKillExperience(owner, victim);

        // A bot killed by a mob with nobody around has stripped itself onto an
        // empty hillside: no one can reach the chest before it despawns, so
        // the only lasting effect is the fleet grinding itself out of its own
        // gear. Deaths a player could actually witness or loot still count,
        // and so does every death at the hands of a player (OnPVPKill).
        if (IsPlayerbot(victim) && !AnyHumanPlayerWithin(victim, 200.0f))
            return;

        DropFullLootChest(victim);
    }
};

// The town flagger: one gossip NPC entry, spawned in the capitals. Gossip
// lives on the CreatureAI in this core, not on CreatureScript.
class npc_ffa_flagger : public CreatureScript
{
public:
    npc_ffa_flagger() : CreatureScript("npc_ffa_flagger") {}

    struct npc_ffa_flaggerAI : public ScriptedAI
    {
        explicit npc_ffa_flaggerAI(Creature* creature) : ScriptedAI(creature) {}

        bool OnGossipHello(Player* player) override
        {
            if (!s_enabled)
            {
                SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, me->GetGUID());
                return true;
            }

            // Greenhorns see the offer but cannot take it: the select
            // handler turns them away until level 10.
            if (player->GetLevel() < 10)
            {
                me->Whisper("Come back when you have seen your tenth season. The mark is not for greenhorns.", LANG_UNIVERSAL, player);
                AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Flag me for free-for-all PvP. (Requires level 10.)", GOSSIP_SENDER_MAIN, 1);
            }
            else if (IsOptedIn(player->GetGUID().GetCounter()))
                AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Remove my free-for-all flag.", GOSSIP_SENDER_MAIN, 2);
            else
                AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Flag me for free-for-all PvP. (Double experience and gold while armed!)", GOSSIP_SENDER_MAIN, 1);
            // A second option is load-bearing: the 3.3.5 client auto-selects
            // a one-option gossip menu, so right-clicking Grix toggled the
            // flag instantly with no window ever shown.
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Farewell.", GOSSIP_SENDER_MAIN, 3);
            // Custom flavor text row (bplusworld.npc_text 900001).
            SendGossipMenuFor(player, 900001, me->GetGUID());
            return true;
        }

        bool OnGossipSelect(Player* player, uint32 /*menuId*/, uint32 gossipListId) override
        {
            uint32 const action = player->PlayerTalkClass->GetGossipOptionAction(gossipListId);
            CloseGossipMenuFor(player);
            if (!s_enabled)
                return true;
            if (player->GetLevel() < 10)
            {
                me->Whisper("Not yet, greenhorn. Come back at level 10.", LANG_UNIVERSAL, player);
                return true;
            }

            uint32 const guidLow = player->GetGUID().GetCounter();
            if (action == 1)
            {
                SetOptedIn(guidLow, true);
                me->Whisper("You are marked for free-for-all combat. The flag arms outside the safe zones - fight well, and profit doubly.", LANG_UNIVERSAL, player);
            }
            else
            {
                // A bounty cannot be walked off. Otherwise the flagger is an
                // escape hatch: earn a price on your head, stroll back here,
                // disarm, and keep it - untouchable while the fleet is still
                // dispatched at you. The debt has to lapse on its own.
                if (Bounty::GetStacks(player))
                {
                    me->Whisper("Your head is bought and paid for. No mark I lift calls off hunters already paid - "
                        "outlive the price, then come back to me.", LANG_UNIVERSAL, player);
                    return true;
                }

                SetOptedIn(guidLow, false);
                me->Whisper("Your mark is lifted. The wilds are merely dangerous again.", LANG_UNIVERSAL, player);
            }
            ApplyFfaState(player);
            ApplyWarModeAura(player);

            // Last, so the badge and flag are already settled and the message
            // describes the state the player is actually in.
            LeavePartyIfWarModeSplit(player);
            return true;
        }
    };

    CreatureAI* GetAI(Creature* creature) const override
    {
        return new npc_ffa_flaggerAI(creature);
    }
};

class barracks_hardcore_world : public WorldScript
{
public:
    barracks_hardcore_world() : WorldScript("barracks_hardcore_world") {}

    void OnConfigLoad(bool /*reload*/) override
    {
        LoadHardcoreConfig();
        if (s_enabled)
            CharacterDatabase.DirectExecute("CREATE TABLE IF NOT EXISTS character_ffa_optin (guid INT UNSIGNED NOT NULL, PRIMARY KEY (guid)) ENGINE=InnoDB");
    }
};

// Killing the same person again and again pays less each time, halving on a
// two-hour memory (Centurion.Hardcore.PlayerKill.DiminishSeconds). That memory
// lives on the KILLER, keyed by whoever they killed, and bots are never paid
// so a bot never holds one: "every bot's factor" is really the set of records
// real players hold AGAINST the fleet. This forgets them.
class barracks_hardcore_commands : public CommandScript
{
public:
    barracks_hardcore_commands() : CommandScript("barracks_hardcore_commands") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable hardcoreTable =
        {
            { "resetkillxp", HandleResetKillXp, rbac::RBAC_PERM_COMMAND_GM, Console::Yes },
        };
        static ChatCommandTable commandTable =
        {
            { "hardcore", hardcoreTable },
        };
        return commandTable;
    }

    // .hardcore resetkillxp [me|all]
    //
    //   (nothing)  every online player forgets every online bot
    //   me         only the caller forgets them
    //   all        every online player forgets EVERY victim, bot or not - the
    //              only way to also drop a record held against a bot that has
    //              since logged out, since an offline victim cannot be listed
    //
    // Nothing persists the map, so forgetting is the whole of the reset: the
    // next kill of that bot pays full price again.
    static bool HandleResetKillXp(ChatHandler* handler, Optional<std::string> scope)
    {
        bool const everyVictim = scope && StringEqualI(*scope, "all");
        bool const callerOnly = scope && StringEqualI(*scope, "me");
        if (scope && !everyVictim && !callerOnly)
            return false;

        Player* caller = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (callerOnly && !caller)
        {
            handler->SendSysMessage("hardcore: \"me\" needs a player to be me.");
            return true;
        }

        // One pass to sort the world into the people who hold records and the
        // bots those records are about.
        std::vector<Player*> holders;
        std::vector<ObjectGuid> botGuids;
        for (auto const& pair : ObjectAccessor::GetPlayers())
        {
            Player* player = pair.second;
            if (!player || !player->IsInWorld())
                continue;

            if (IsPlayerbot(player))
                botGuids.push_back(player->GetGUID());
            else if (!callerOnly || player == caller)
                holders.push_back(player);
        }

        uint32 records = 0;
        uint32 touched = 0;
        for (Player* holder : holders)
        {
            uint32 cleared = 0;
            if (everyVictim)
                cleared = holder->ClearPvpXpDiminishing();
            else
                for (ObjectGuid const& botGuid : botGuids)
                    cleared += holder->ClearPvpXpDiminishing(botGuid);

            if (cleared)
            {
                ++touched;
                records += cleared;
                TC_LOG_INFO("playerbots.hardcore",
                    "PvP experience diminishing reset: {} forgot {} {}.",
                    holder->GetName(), cleared, everyVictim ? "victims" : "bots");
            }
        }

        if (everyVictim)
            handler->PSendSysMessage("hardcore: cleared %u kill record%s (every victim) held by %u player%s.",
                records, records == 1 ? "" : "s", touched, touched == 1 ? "" : "s");
        else
            handler->PSendSysMessage("hardcore: cleared %u kill record%s held by %u player%s against %u online bot%s.",
                records, records == 1 ? "" : "s", touched, touched == 1 ? "" : "s",
                uint32(botGuids.size()), botGuids.size() == 1 ? "" : "s");
        return true;
    }
};

void AddSC_custom_barracks_hardcore()
{
    LoadHardcoreConfig();
    new barracks_hardcore_player();
    new npc_ffa_flagger();
    new barracks_hardcore_world();
    new barracks_hardcore_commands();
}
