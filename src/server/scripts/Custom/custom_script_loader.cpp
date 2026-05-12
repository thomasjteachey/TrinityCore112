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

// This is where scripts' loading functions should be declared:

// The name of this function should match:
// void Add${NameOfDirectory}Scripts()
#include "BGReplay.cpp"

void LoadHiddenItemsetBonuses();
void AddSC_custom_hidden_itemset_bonus();
void AddSC_custom_zone_group_rules();
void AddSC_mod_pvp_titles();
void AddSC_custom_diremaul_beads();
void AddSC_custom_gurubashi_arena();
void AddSC_custom_depleted_mark_exchange();
void AddSC_custom_polearm_staff_inner_auras();
void AddSC_custom_pvpve_dungeon();
void AddSC_npc_scarlet_chapel_queue();
void AddSC_npc_account_banker();

void AddCustomScripts()
{
    LoadHiddenItemsetBonuses();
    AddSC_custom_hidden_itemset_bonus();
    AddBGReplayScripts();
    AddSC_custom_zone_group_rules();
    AddSC_mod_pvp_titles();
    AddSC_custom_diremaul_beads();
    AddSC_custom_gurubashi_arena();
    AddSC_custom_depleted_mark_exchange();
    AddSC_custom_polearm_staff_inner_auras();
    AddSC_custom_pvpve_dungeon();
    AddSC_npc_scarlet_chapel_queue();
    AddSC_npc_account_banker();
}
