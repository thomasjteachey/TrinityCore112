#include "CrossFactionBG.h"
#include "DBCStores.h"
#include "ObjectGuid.h"
#include "Pet.h"
#include "Player.h"
#include "World.h"

#include <unordered_map>

namespace
{
struct SavedFactionState
{
    uint32 OriginalFactionTemplate = 0;
    TeamId OriginalTeam = TEAM_NEUTRAL;
};

using SavedStateMap = std::unordered_map<ObjectGuid, SavedFactionState, ObjectGuid::Hash>;

SavedStateMap& GetSavedStates()
{
    static SavedStateMap savedStates;
    return savedStates;
}

uint32 GetFactionTemplateForTeam(TeamId teamId)
{
    uint8 const race = (teamId == TEAM_HORDE) ? RACE_ORC : RACE_HUMAN;
    if (ChrRacesEntry const* entry = sChrRacesStore.LookupEntry(race))
        return entry->FactionTemplateID;

    return 0;
}
}

namespace CrossFactionBG
{
bool IsMinimapColorFixEnabled()
{
    return sWorld->getBoolConfig(CONFIG_CROSSFACTION_BG_MINIMAP_COLOR_FIX);
}

void ApplyTeamAndFaction(Player* player, TeamId teamId)
{
    if (!player || teamId == TEAM_NEUTRAL || !IsMinimapColorFixEnabled())
        return;

    SavedStateMap& savedStates = GetSavedStates();
    SavedFactionState& state = savedStates[player->GetGUID()];

    if (!state.OriginalFactionTemplate)
    {
        state.OriginalFactionTemplate = player->GetFaction();
        state.OriginalTeam = player->GetTeamId();
    }

    player->SetTeamId(teamId);

    if (uint32 const factionTemplate = GetFactionTemplateForTeam(teamId))
        player->SetFaction(factionTemplate);

    if (Pet* pet = player->GetPet())
        pet->SetFaction(player->GetFaction());

    player->UpdateObjectVisibility();
}

void RestoreTeamAndFaction(Player* player)
{
    if (!player || !IsMinimapColorFixEnabled())
        return;

    SavedStateMap& savedStates = GetSavedStates();
    SavedStateMap::iterator const itr = savedStates.find(player->GetGUID());

    if (itr != savedStates.end())
    {
        if (itr->second.OriginalFactionTemplate)
            player->SetFaction(itr->second.OriginalFactionTemplate);

        if (itr->second.OriginalTeam != TEAM_NEUTRAL)
            player->SetTeamId(itr->second.OriginalTeam);

        if (Pet* pet = player->GetPet())
            pet->SetFaction(player->GetFaction());

        player->UpdateObjectVisibility();
        savedStates.erase(itr);
        return;
    }

    player->SetFactionForRace(player->GetRace());
    player->UpdateObjectVisibility();
}
}
