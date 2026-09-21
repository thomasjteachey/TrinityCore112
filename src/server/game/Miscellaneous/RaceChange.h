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

#ifndef TRINITYCORE_RACE_CHANGE_H
#define TRINITYCORE_RACE_CHANGE_H

#include "DatabaseEnvFwd.h"
#include "Define.h"
#include "ObjectGuid.h"

// What a race change has to do to the spellbook, on top of the stock handler
// (WorldSession::HandleCharFactionOrRaceChangeCallback), which only swaps the
// languages.
//
// The stock server leaves the old race's abilities in `character_spell`: the
// new racial skill line is learned at the next login, but nothing ever takes
// the old one's spells away, so a Tauren turned Human keeps War Stomp and
// Plainsrunning next to Every Man for Himself. Three sources are covered:
//
//  - SkillLineAbility rows gated on the OLD race and not the new one: the
//    racial skill lines, Plainsrunning, the class-flavoured Every Man for
//    Himself, and the engineering mounts, which trade for their counterpart
//    through player_factionchange_spells rather than simply vanishing.
//  - The start-spell list the character is taught at every login
//    (playercreateinfo_spell_custom, or the tournament kit), where it differs
//    between the two races.
//  - The priest racials. On this realm they are quest rewards on skill lines
//    open to every race, so nothing in the DBC says whose they are; the table
//    in RaceChange.cpp does. The new race's pair is handed over at every rank
//    the character's level reaches, and the quests that award them are marked
//    done so they are not offered again.
//
// The character is offline (this runs from the character screen), so all of
// it is SQL appended to the race change's own transaction.
namespace RaceChange
{
    void AppendSpellSwap(CharacterDatabaseTransaction trans, ObjectGuid guid, uint8 oldRace, uint8 newRace,
        uint8 playerClass, uint8 level, bool tournament);
}

#endif // TRINITYCORE_RACE_CHANGE_H
