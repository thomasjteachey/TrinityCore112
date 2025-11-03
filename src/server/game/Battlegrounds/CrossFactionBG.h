#ifndef TRINITY_CROSSFACTIONBG_H
#define TRINITY_CROSSFACTIONBG_H

#include "SharedDefines.h"

class Player;

namespace CrossFactionBG
{
    bool IsMinimapColorFixEnabled();
    void ApplyTeamAndFaction(Player* player, TeamId teamId);
    void RestoreTeamAndFaction(Player* player);
}

#endif // TRINITY_CROSSFACTIONBG_H
