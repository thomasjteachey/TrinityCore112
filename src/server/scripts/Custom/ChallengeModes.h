#ifndef CUSTOM_CHALLENGE_MODES_H
#define CUSTOM_CHALLENGE_MODES_H

#include "ObjectGuid.h"
#include "Define.h"

#include <string>
#include <unordered_map>

class Item;
class ItemTemplate;
class Player;
class Group;

enum ChallengeModeSettings : uint8
{
    SETTING_HARDCORE           = 0,
    SETTING_SEMI_HARDCORE      = 1,
    SETTING_SELF_CRAFTED       = 2,
    SETTING_ITEM_QUALITY_LEVEL = 3,
    SETTING_SLOW_XP_GAIN       = 4,
    SETTING_VERY_SLOW_XP_GAIN  = 5,
    SETTING_QUEST_XP_ONLY      = 6,
    SETTING_IRON_MAN           = 7,
    HARDCORE_DEAD              = 8,
    CHALLENGE_MODE_SETTING_MAX
};

class ChallengeModes
{
public:
    static ChallengeModes* instance();

    void LoadConfig();
    void LoadPlayer(Player* player);
    void UnloadPlayer(Player* player);
    void DeletePlayer(ObjectGuid guid);

    bool Enabled() const { return _enabled; }
    bool ChallengeEnabled(ChallengeModeSettings setting) const;
    bool IsEnabledForPlayer(ChallengeModeSettings setting, Player const* player) const;
    bool SetEnabledForPlayer(ChallengeModeSettings setting, Player* player, bool enabled);

    bool CanActivate(Player const* player) const;
    bool CanActivateMode(Player const* player, ChallengeModeSettings setting, std::string* error = nullptr) const;

    uint32 GetDisableLevel(ChallengeModeSettings setting) const;
    float GetXpMultiplier(ChallengeModeSettings setting) const;
    uint32 GetItemRewardAmount(ChallengeModeSettings setting) const;

    using RewardMap = std::unordered_map<uint8, uint32>;
    RewardMap const& GetTitleRewards(ChallengeModeSettings setting) const;
    RewardMap const& GetTalentRewards(ChallengeModeSettings setting) const;
    RewardMap const& GetItemRewards(ChallengeModeSettings setting) const;
    RewardMap const& GetAchievementRewards(ChallengeModeSettings setting) const;

private:
    ChallengeModes() = default;

    uint32 GetPlayerMask(Player const* player) const;
    void SetPlayerMask(ObjectGuid::LowType guid, uint32 mask);

    static uint32 Bit(ChallengeModeSettings setting) { return 1u << uint32(setting); }
    static void LoadStringToMap(RewardMap& mapToLoad, std::string const& configString);

    bool _enabled = false;
    bool _modeEnabled[CHALLENGE_MODE_SETTING_MAX] = { };
    uint32 _disableLevel[CHALLENGE_MODE_SETTING_MAX] = { };
    float _xpMultiplier[CHALLENGE_MODE_SETTING_MAX] = { };
    uint32 _itemRewardAmount[CHALLENGE_MODE_SETTING_MAX] = { };

    RewardMap _titleRewards[CHALLENGE_MODE_SETTING_MAX];
    RewardMap _talentRewards[CHALLENGE_MODE_SETTING_MAX];
    RewardMap _itemRewards[CHALLENGE_MODE_SETTING_MAX];
    RewardMap _achievementRewards[CHALLENGE_MODE_SETTING_MAX];

    std::unordered_map<ObjectGuid::LowType, uint32> _playerModes;
};

#define sChallengeModes ChallengeModes::instance()

#endif // CUSTOM_CHALLENGE_MODES_H
