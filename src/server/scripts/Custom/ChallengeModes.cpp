#include "ChallengeModes.h"

#include "AchievementMgr.h"
#include "Chat.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "GameObjectAI.h"
#include "GossipDef.h"
#include "Group.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Pet.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "ScriptedGossip.h"
#include "SharedDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "WorldSession.h"

#include <sstream>

namespace
{
constexpr char const* ConfigNames[CHALLENGE_MODE_SETTING_MAX] =
{
    "Hardcore",
    "SemiHardcore",
    "SelfCrafted",
    "ItemQualityLevel",
    "SlowXpGain",
    "VerySlowXpGain",
    "QuestXpOnly",
    "IronMan",
    "HardcoreDead"
};

constexpr uint32 ACTION_ENABLE_BASE = GOSSIP_ACTION_INFO_DEF + 100;

bool IsRealChallengeMode(ChallengeModeSettings setting)
{
    return setting >= SETTING_HARDCORE && setting <= SETTING_IRON_MAN;
}

char const* GetChallengeDisplayName(ChallengeModeSettings setting)
{
    switch (setting)
    {
        case SETTING_HARDCORE:           return "Hardcore";
        case SETTING_SEMI_HARDCORE:      return "Semi-Hardcore";
        case SETTING_SELF_CRAFTED:       return "Self Crafted";
        case SETTING_ITEM_QUALITY_LEVEL: return "Poor/Normal Gear Only";
        case SETTING_SLOW_XP_GAIN:       return "Slow XP";
        case SETTING_VERY_SLOW_XP_GAIN:  return "Very Slow XP";
        case SETTING_QUEST_XP_ONLY:      return "Quest XP Only";
        case SETTING_IRON_MAN:           return "Iron Man";
        default:                         return "Unknown";
    }
}

bool IsIronManForbiddenFood(ItemTemplate const* proto)
{
    if (!proto || proto->Class != ITEM_CLASS_CONSUMABLE || proto->SubClass != ITEM_SUBCLASS_FOOD)
        return false;

    for (_Spell const& spell : proto->Spells)
    {
        if (spell.SpellId <= 0)
            continue;

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(uint32(spell.SpellId));
        if (!spellInfo)
            continue;

        for (uint8 i = EFFECT_0; i < MAX_SPELL_EFFECTS; ++i)
            if (spellInfo->GetEffect(SpellEffIndex(i)).ApplyAuraName == SPELL_AURA_PERIODIC_TRIGGER_SPELL)
                return true;
    }

    return false;
}

bool IsTradeSkillSpell(uint32 spellId)
{
    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
    if (!spellInfo)
        return false;

    for (uint8 i = EFFECT_0; i < MAX_SPELL_EFFECTS; ++i)
        if (spellInfo->GetEffect(SpellEffIndex(i)).Effect == SPELL_EFFECT_TRADE_SKILL)
            return true;

    return false;
}

bool IsAllowedIronManProfessionSpell(uint32 spellId)
{
    switch (spellId)
    {
        case 53428: // Runeforging
        case 2842:  // Poisons
        case 5149:  // Beast Training
            return true;
        default:
            return false;
    }
}
}

ChallengeModes* ChallengeModes::instance()
{
    static ChallengeModes instance;
    return &instance;
}

void ChallengeModes::LoadStringToMap(RewardMap& mapToLoad, std::string const& configString)
{
    mapToLoad.clear();

    std::stringstream configIdStream(configString);
    std::string delimitedValue;
    while (std::getline(configIdStream, delimitedValue, ','))
    {
        std::stringstream configPairStream(delimitedValue);
        uint32 configLevel = 0;
        uint32 rewardValue = 0;
        configPairStream >> configLevel >> rewardValue;
        if (!configPairStream || !configLevel || configLevel > DEFAULT_MAX_LEVEL || !rewardValue)
            continue;

        mapToLoad[uint8(configLevel)] = rewardValue;
    }
}

void ChallengeModes::LoadConfig()
{
    _enabled = sConfigMgr->GetBoolDefault("ChallengeModes.Enable", false);

    for (uint8 i = 0; i < CHALLENGE_MODE_SETTING_MAX; ++i)
    {
        _modeEnabled[i] = false;
        _disableLevel[i] = 0;
        _xpMultiplier[i] = 1.0f;
        _itemRewardAmount[i] = 1;
        _titleRewards[i].clear();
        _talentRewards[i].clear();
        _itemRewards[i].clear();
        _achievementRewards[i].clear();
    }

    if (!_enabled)
        return;

    for (uint8 i = SETTING_HARDCORE; i <= SETTING_IRON_MAN; ++i)
    {
        std::string const base = ConfigNames[i];
        _modeEnabled[i] = sConfigMgr->GetBoolDefault(base + ".Enable", true);
        _disableLevel[i] = uint32(sConfigMgr->GetIntDefault(base + ".DisableLevel", 0));
        _xpMultiplier[i] = sConfigMgr->GetFloatDefault(base + ".XPMultiplier", 1.0f);
        _itemRewardAmount[i] = uint32(sConfigMgr->GetIntDefault(base + ".ItemRewardAmount", 1));

        LoadStringToMap(_titleRewards[i], sConfigMgr->GetStringDefault(base + ".TitleRewards", ""));
        LoadStringToMap(_talentRewards[i], sConfigMgr->GetStringDefault(base + ".TalentRewards", ""));
        LoadStringToMap(_itemRewards[i], sConfigMgr->GetStringDefault(base + ".ItemRewards", ""));
        LoadStringToMap(_achievementRewards[i], sConfigMgr->GetStringDefault(base + ".AchievementReward", ""));
    }

    _xpMultiplier[SETTING_SLOW_XP_GAIN] = sConfigMgr->GetFloatDefault("SlowXpGain.XPMultiplier", 0.50f);
    _xpMultiplier[SETTING_VERY_SLOW_XP_GAIN] = sConfigMgr->GetFloatDefault("VerySlowXpGain.XPMultiplier", 0.25f);
}

void ChallengeModes::LoadPlayer(Player* player)
{
    if (!player)
        return;

    ObjectGuid::LowType const guid = player->GetGUID().GetCounter();
    uint32 mask = 0;

    if (QueryResult result = CharacterDatabase.PQuery("SELECT `mode`, `value` FROM `character_challenge_modes` WHERE `guid` = {}", guid))
    {
        do
        {
            Field* fields = result->Fetch();
            uint8 mode = fields[0].GetUInt8();
            bool value = fields[1].GetUInt8() != 0;
            if (mode < CHALLENGE_MODE_SETTING_MAX && value)
                mask |= Bit(ChallengeModeSettings(mode));
        }
        while (result->NextRow());
    }

    SetPlayerMask(guid, mask);
}

void ChallengeModes::UnloadPlayer(Player* player)
{
    if (player)
        _playerModes.erase(player->GetGUID().GetCounter());
}

void ChallengeModes::DeletePlayer(ObjectGuid guid)
{
    ObjectGuid::LowType const low = guid.GetCounter();
    _playerModes.erase(low);
    CharacterDatabase.Execute("DELETE FROM `character_challenge_modes` WHERE `guid` = {}", low);
}

bool ChallengeModes::ChallengeEnabled(ChallengeModeSettings setting) const
{
    if (setting == HARDCORE_DEAD)
        return true;

    if (!IsRealChallengeMode(setting))
        return false;

    return _modeEnabled[setting];
}

uint32 ChallengeModes::GetPlayerMask(Player const* player) const
{
    if (!player)
        return 0;

    auto itr = _playerModes.find(player->GetGUID().GetCounter());
    if (itr == _playerModes.end())
        return 0;

    return itr->second;
}

void ChallengeModes::SetPlayerMask(ObjectGuid::LowType guid, uint32 mask)
{
    if (!guid)
        return;

    _playerModes[guid] = mask;
}

bool ChallengeModes::IsEnabledForPlayer(ChallengeModeSettings setting, Player const* player) const
{
    if (!_enabled || !ChallengeEnabled(setting) || !player)
        return false;

    return (GetPlayerMask(player) & Bit(setting)) != 0;
}

bool ChallengeModes::SetEnabledForPlayer(ChallengeModeSettings setting, Player* player, bool enabled)
{
    if (!player || setting >= CHALLENGE_MODE_SETTING_MAX)
        return false;

    ObjectGuid::LowType const guid = player->GetGUID().GetCounter();
    uint32 mask = GetPlayerMask(player);

    if (enabled)
        mask |= Bit(setting);
    else
        mask &= ~Bit(setting);

    SetPlayerMask(guid, mask);

    if (enabled)
        CharacterDatabase.Execute("REPLACE INTO `character_challenge_modes` (`guid`, `mode`, `value`) VALUES ({}, {}, 1)", guid, uint8(setting));
    else
        CharacterDatabase.Execute("DELETE FROM `character_challenge_modes` WHERE `guid` = {} AND `mode` = {}", guid, uint8(setting));

    return true;
}

bool ChallengeModes::CanActivate(Player const* player) const
{
    if (!_enabled || !player)
        return false;

    if (player->getClass() == CLASS_DEATH_KNIGHT)
        return player->GetLevel() <= 55;

    return player->GetLevel() <= 1;
}

bool ChallengeModes::CanActivateMode(Player const* player, ChallengeModeSettings setting, std::string* error) const
{
    if (!CanActivate(player))
    {
        if (error)
            *error = "Challenge modes can only be enabled at level 1, or level 55 for Death Knights.";
        return false;
    }

    if (!IsRealChallengeMode(setting) || !ChallengeEnabled(setting))
    {
        if (error)
            *error = "That challenge mode is not enabled.";
        return false;
    }

    if (IsEnabledForPlayer(setting, player))
    {
        if (error)
            *error = "That challenge mode is already enabled.";
        return false;
    }

    if ((setting == SETTING_HARDCORE && IsEnabledForPlayer(SETTING_SEMI_HARDCORE, player)) ||
        (setting == SETTING_SEMI_HARDCORE && IsEnabledForPlayer(SETTING_HARDCORE, player)))
    {
        if (error)
            *error = "Hardcore and Semi-Hardcore cannot both be enabled.";
        return false;
    }

    if ((setting == SETTING_SLOW_XP_GAIN && IsEnabledForPlayer(SETTING_VERY_SLOW_XP_GAIN, player)) ||
        (setting == SETTING_VERY_SLOW_XP_GAIN && IsEnabledForPlayer(SETTING_SLOW_XP_GAIN, player)))
    {
        if (error)
            *error = "Slow XP and Very Slow XP cannot both be enabled.";
        return false;
    }

    if ((setting == SETTING_SELF_CRAFTED && IsEnabledForPlayer(SETTING_IRON_MAN, player)) ||
        (setting == SETTING_IRON_MAN && IsEnabledForPlayer(SETTING_SELF_CRAFTED, player)))
    {
        if (error)
            *error = "Self Crafted and Iron Man cannot both be enabled.";
        return false;
    }

    return true;
}

uint32 ChallengeModes::GetDisableLevel(ChallengeModeSettings setting) const
{
    return setting < CHALLENGE_MODE_SETTING_MAX ? _disableLevel[setting] : 0;
}

float ChallengeModes::GetXpMultiplier(ChallengeModeSettings setting) const
{
    return setting < CHALLENGE_MODE_SETTING_MAX ? _xpMultiplier[setting] : 1.0f;
}

uint32 ChallengeModes::GetItemRewardAmount(ChallengeModeSettings setting) const
{
    return setting < CHALLENGE_MODE_SETTING_MAX ? _itemRewardAmount[setting] : 1;
}

ChallengeModes::RewardMap const& ChallengeModes::GetTitleRewards(ChallengeModeSettings setting) const
{
    static RewardMap empty;
    return setting < CHALLENGE_MODE_SETTING_MAX ? _titleRewards[setting] : empty;
}

ChallengeModes::RewardMap const& ChallengeModes::GetTalentRewards(ChallengeModeSettings setting) const
{
    static RewardMap empty;
    return setting < CHALLENGE_MODE_SETTING_MAX ? _talentRewards[setting] : empty;
}

ChallengeModes::RewardMap const& ChallengeModes::GetItemRewards(ChallengeModeSettings setting) const
{
    static RewardMap empty;
    return setting < CHALLENGE_MODE_SETTING_MAX ? _itemRewards[setting] : empty;
}

ChallengeModes::RewardMap const& ChallengeModes::GetAchievementRewards(ChallengeModeSettings setting) const
{
    static RewardMap empty;
    return setting < CHALLENGE_MODE_SETTING_MAX ? _achievementRewards[setting] : empty;
}

class challenge_modes_world_script : public WorldScript
{
public:
    challenge_modes_world_script() : WorldScript("challenge_modes_world_script") { }

    void OnConfigLoad(bool /*reload*/) override
    {
        sChallengeModes->LoadConfig();
    }
};

class challenge_modes_player_script : public PlayerScript
{
public:
    challenge_modes_player_script() : PlayerScript("challenge_modes_player_script") { }

    void OnLogin(Player* player, bool /*firstLogin*/) override
    {
        sChallengeModes->LoadPlayer(player);

        if (sChallengeModes->IsEnabledForPlayer(SETTING_IRON_MAN, player))
            player->SetFreeTalentPoints(0);

        if (sChallengeModes->IsEnabledForPlayer(SETTING_HARDCORE, player) && sChallengeModes->IsEnabledForPlayer(HARDCORE_DEAD, player))
        {
            player->KillPlayer();
            player->GetSession()->KickPlayer("ChallengeModes: dead hardcore character login");
        }
    }

    void OnLogout(Player* player) override
    {
        sChallengeModes->UnloadPlayer(player);
    }

    void OnDelete(ObjectGuid guid, uint32 /*accountId*/) override
    {
        sChallengeModes->DeletePlayer(guid);
    }

    void OnPVPKill(Player* /*killer*/, Player* killed) override
    {
        MarkHardcoreDead(killed);
    }

    void OnPlayerKilledByCreature(Creature* /*killer*/, Player* killed) override
    {
        MarkHardcoreDead(killed);
        ApplySemiHardcorePenalty(killed);
    }

    void OnPlayerRepop(Player* player) override
    {
        if (!sChallengeModes->IsEnabledForPlayer(SETTING_HARDCORE, player))
            return;

        sChallengeModes->SetEnabledForPlayer(HARDCORE_DEAD, player, true);
        if (player->GetSession())
            player->GetSession()->KickPlayer("ChallengeModes: hardcore character died");
    }

    void OnPlayerResurrect(Player* player) override
    {
        if (sChallengeModes->IsEnabledForPlayer(SETTING_HARDCORE, player))
        {
            sChallengeModes->SetEnabledForPlayer(HARDCORE_DEAD, player, true);
            player->KillPlayer();
            if (player->GetSession())
                player->GetSession()->KickPlayer("ChallengeModes: hardcore resurrect blocked");
            return;
        }

        if (sChallengeModes->IsEnabledForPlayer(SETTING_IRON_MAN, player))
            player->KillPlayer();
    }

    void OnGiveXP(Player* player, uint32& amount, Unit* victim) override
    {
        if (!player || amount == 0)
            return;

        if (victim && sChallengeModes->IsEnabledForPlayer(SETTING_QUEST_XP_ONLY, player))
        {
            if (Pet* pet = player->GetPet())
                pet->GivePetXP(player->GetGroup() ? amount / 2 : amount);

            amount = 0;
            return;
        }

        float multiplier = 1.0f;
        for (uint8 i = SETTING_HARDCORE; i <= SETTING_IRON_MAN; ++i)
        {
            ChallengeModeSettings setting = ChallengeModeSettings(i);
            if (sChallengeModes->IsEnabledForPlayer(setting, player))
                multiplier *= sChallengeModes->GetXpMultiplier(setting);
        }

        amount = uint32(float(amount) * multiplier);
    }

    void OnLevelChanged(Player* player, uint8 /*oldLevel*/) override
    {
        if (!player)
            return;

        uint8 const level = player->GetLevel();
        for (uint8 i = SETTING_HARDCORE; i <= SETTING_IRON_MAN; ++i)
        {
            ChallengeModeSettings setting = ChallengeModeSettings(i);
            if (!sChallengeModes->IsEnabledForPlayer(setting, player))
                continue;

            GiveRewardsForLevel(player, setting, level);

            uint32 const disableLevel = sChallengeModes->GetDisableLevel(setting);
            if (disableLevel && disableLevel <= level)
                sChallengeModes->SetEnabledForPlayer(setting, player, false);
        }

        if (sChallengeModes->IsEnabledForPlayer(SETTING_IRON_MAN, player))
            player->SetFreeTalentPoints(0);
    }

    void OnTalentsReset(Player* player, bool /*noCost*/) override
    {
        if (sChallengeModes->IsEnabledForPlayer(SETTING_IRON_MAN, player))
            player->SetFreeTalentPoints(0);
    }

    bool OnCanEquipItem(Player* player, uint8 /*slot*/, uint16& /*dest*/, Item* item, bool /*swap*/, bool /*notLoading*/) override
    {
        if (!player || !item || !item->GetTemplate())
            return true;

        ItemTemplate const* proto = item->GetTemplate();
        if (sChallengeModes->IsEnabledForPlayer(SETTING_SELF_CRAFTED, player))
        {
            if (!proto->HasSignature())
                return false;

            return item->GetGuidValue(ITEM_FIELD_CREATOR) == player->GetGUID();
        }

        if (sChallengeModes->IsEnabledForPlayer(SETTING_ITEM_QUALITY_LEVEL, player) && proto->Quality > ITEM_QUALITY_NORMAL)
            return false;

        if (sChallengeModes->IsEnabledForPlayer(SETTING_IRON_MAN, player) && proto->Quality > ITEM_QUALITY_NORMAL)
            return false;

        return true;
    }

    bool OnCanUseItem(Player* player, ItemTemplate const* proto, InventoryResult& result) override
    {
        if (!player || !proto || !sChallengeModes->IsEnabledForPlayer(SETTING_IRON_MAN, player))
            return true;

        if (proto->Class == ITEM_CLASS_CONSUMABLE &&
            (proto->SubClass == ITEM_SUBCLASS_POTION || proto->SubClass == ITEM_SUBCLASS_ELIXIR || proto->SubClass == ITEM_SUBCLASS_FLASK || IsIronManForbiddenFood(proto)))
        {
            result = EQUIP_ERR_YOU_CAN_NEVER_USE_THAT_ITEM;
            return false;
        }

        return true;
    }

    bool OnCanApplyEnchantment(Player* player, Item* /*item*/, EnchantmentSlot /*slot*/, bool apply, bool /*applyDur*/, bool /*ignoreCondition*/) override
    {
        if (!apply)
            return true;

        return !sChallengeModes->IsEnabledForPlayer(SETTING_IRON_MAN, player);
    }

    void OnLearnSpell(Player* player, uint32 spellId) override
    {
        if (!player || !sChallengeModes->IsEnabledForPlayer(SETTING_IRON_MAN, player))
            return;

        if (IsAllowedIronManProfessionSpell(spellId))
            return;

        if (IsTradeSkillSpell(spellId))
            player->RemoveSpell(spellId, false, false);
    }

    bool OnCanGroupInvite(Player* player, std::string& /*memberName*/) override
    {
        return !sChallengeModes->IsEnabledForPlayer(SETTING_IRON_MAN, player);
    }

    bool OnCanGroupAccept(Player* player, Group* /*group*/) override
    {
        return !sChallengeModes->IsEnabledForPlayer(SETTING_IRON_MAN, player);
    }

private:
    static void MarkHardcoreDead(Player* player)
    {
        if (sChallengeModes->IsEnabledForPlayer(SETTING_HARDCORE, player))
            sChallengeModes->SetEnabledForPlayer(HARDCORE_DEAD, player, true);
    }

    static void ApplySemiHardcorePenalty(Player* player)
    {
        if (!sChallengeModes->IsEnabledForPlayer(SETTING_SEMI_HARDCORE, player))
            return;

        for (uint8 i = 0; i < EQUIPMENT_SLOT_END; ++i)
        {
            if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
            {
                ChatHandler(player->GetSession()).PSendSysMessage("You have lost |Hitem:%u:0:0:0:0:0:0:0:0|h[%s]|h|r.", item->GetEntry(), item->GetTemplate()->Name1.c_str());
                player->DestroyItem(INVENTORY_SLOT_BAG_0, item->GetSlot(), true);
            }
        }

        player->SetMoney(0);
    }

    static void GiveRewardsForLevel(Player* player, ChallengeModeSettings setting, uint8 level)
    {
        auto const& titleRewards = sChallengeModes->GetTitleRewards(setting);
        if (auto itr = titleRewards.find(level); itr != titleRewards.end())
        {
            if (CharTitlesEntry const* titleInfo = sCharTitlesStore.LookupEntry(itr->second))
                player->SetTitle(titleInfo);
            else
                TC_LOG_ERROR("server.custom", "ChallengeModes: invalid title reward {} for mode {}", itr->second, GetChallengeDisplayName(setting));
        }

        auto const& talentRewards = sChallengeModes->GetTalentRewards(setting);
        if (auto itr = talentRewards.find(level); itr != talentRewards.end())
            player->SetFreeTalentPoints(player->GetFreeTalentPoints() + itr->second);

        auto const& achievementRewards = sChallengeModes->GetAchievementRewards(setting);
        if (auto itr = achievementRewards.find(level); itr != achievementRewards.end())
        {
            if (AchievementEntry const* achievementInfo = sAchievementStore.LookupEntry(itr->second))
                player->CompletedAchievement(achievementInfo);
            else
                TC_LOG_ERROR("server.custom", "ChallengeModes: invalid achievement reward {} for mode {}", itr->second, GetChallengeDisplayName(setting));
        }

        auto const& itemRewards = sChallengeModes->GetItemRewards(setting);
        if (auto itr = itemRewards.find(level); itr != itemRewards.end())
            player->SendItemRetrievalMail(itr->second, sChallengeModes->GetItemRewardAmount(setting));
    }
};

class gobject_challenge_modes : public GameObjectScript
{
public:
    gobject_challenge_modes() : GameObjectScript("gobject_challenge_modes") { }

    struct gobject_challenge_modesAI : public GameObjectAI
    {
        explicit gobject_challenge_modesAI(GameObject* go) : GameObjectAI(go) { }

        bool OnGossipHello(Player* player) override
        {
            if (!player)
                return true;

            ClearGossipMenuFor(player);

            if (!sChallengeModes->CanActivate(player))
            {
                ChatHandler(player->GetSession()).SendSysMessage("Challenge modes can only be enabled at level 1, or level 55 for Death Knights.");
                CloseGossipMenuFor(player);
                return true;
            }

            AddChallengeOption(player, SETTING_HARDCORE, "Enable Hardcore");
            AddChallengeOption(player, SETTING_SEMI_HARDCORE, "Enable Semi-Hardcore");
            AddChallengeOption(player, SETTING_SELF_CRAFTED, "Enable Self Crafted");
            AddChallengeOption(player, SETTING_ITEM_QUALITY_LEVEL, "Enable Poor/Normal Gear Only");
            AddChallengeOption(player, SETTING_SLOW_XP_GAIN, "Enable Slow XP");
            AddChallengeOption(player, SETTING_VERY_SLOW_XP_GAIN, "Enable Very Slow XP");
            AddChallengeOption(player, SETTING_QUEST_XP_ONLY, "Enable Quest XP Only");
            AddChallengeOption(player, SETTING_IRON_MAN, "Enable Iron Man");

            SendGossipMenuFor(player, 12669, me);
            return true;
        }

        bool OnGossipSelect(Player* player, uint32 /*menuId*/, uint32 gossipListId) override
        {
            if (!player)
                return true;

            uint32 const action = player->PlayerTalkClass->GetGossipOptionAction(gossipListId);
            if (action < ACTION_ENABLE_BASE || action >= ACTION_ENABLE_BASE + SETTING_IRON_MAN + 1)
            {
                CloseGossipMenuFor(player);
                return true;
            }

            ChallengeModeSettings setting = ChallengeModeSettings(action - ACTION_ENABLE_BASE);
            std::string error;
            if (!sChallengeModes->CanActivateMode(player, setting, &error))
            {
                ChatHandler(player->GetSession()).PSendSysMessage("%s", error.c_str());
                CloseGossipMenuFor(player);
                return true;
            }

            sChallengeModes->SetEnabledForPlayer(setting, player, true);
            if (setting == SETTING_IRON_MAN)
                player->SetFreeTalentPoints(0);

            ChatHandler(player->GetSession()).PSendSysMessage("%s challenge mode enabled.", GetChallengeDisplayName(setting));
            CloseGossipMenuFor(player);
            return true;
        }

    private:
        static void AddChallengeOption(Player* player, ChallengeModeSettings setting, char const* text)
        {
            if (sChallengeModes->CanActivateMode(player, setting))
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, text, GOSSIP_SENDER_MAIN, ACTION_ENABLE_BASE + uint32(setting));
        }
    };

    GameObjectAI* GetAI(GameObject* go) const override
    {
        return new gobject_challenge_modesAI(go);
    }
};

void AddSC_mod_challenge_modes()
{
    sChallengeModes->LoadConfig();

    new challenge_modes_world_script();
    new challenge_modes_player_script();
    new gobject_challenge_modes();
}
