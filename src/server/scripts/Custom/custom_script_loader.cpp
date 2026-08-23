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
void AddSC_custom_pvpve_dungeon();
void AddSC_npc_scarlet_chapel_queue();
void AddSC_npc_account_banker();
void AddSC_go_lplus_jump_pad();
void AddSC_custom_game_lobby();
void AddSC_npc_transmogrifier();
void AddSC_custom_t1_set_bonuses();
void AddSC_custom_t2_warrior_paladin();
void AddSC_custom_t2_shaman_warlock();
void AddSC_custom_t2_druid_hunter();
void AddSC_custom_t2_priest_mage();
void AddSC_custom_t2_rogue_armor();
void AddSC_custom_southpark_nolife();
void AddSC_custom_sucky_demon();
void AddSC_GOMove_commandscript();
void AddSC_GOEditor();
void AddSC_rts_building();
void AddSC_custom_los_blocker();
void AddSC_custom_player_collision();
void AddSC_custom_global_collision();
void AddSC_custom_client_attest();
void AddSC_violet_hold_boons();
void AddSC_custom_spell_propagate();

void AddCustomScripts()
{
    LoadHiddenItemsetBonuses();
    AddSC_custom_hidden_itemset_bonus();
    AddSC_custom_los_blocker();
    AddSC_custom_player_collision();
    AddSC_custom_global_collision();
    AddSC_custom_client_attest();
    AddSC_custom_spell_propagate();
    AddBGReplayScripts();
    AddSC_custom_zone_group_rules();
    AddSC_mod_pvp_titles();
    AddSC_custom_diremaul_beads();
    AddSC_custom_gurubashi_arena();
    AddSC_custom_depleted_mark_exchange();
    AddSC_custom_pvpve_dungeon();
    AddSC_npc_scarlet_chapel_queue();
    AddSC_npc_account_banker();
    AddSC_go_lplus_jump_pad();
    AddSC_custom_game_lobby();
    AddSC_npc_transmogrifier();
    AddSC_custom_t1_set_bonuses();
    AddSC_custom_t2_warrior_paladin();
    AddSC_custom_t2_shaman_warlock();
    AddSC_custom_t2_druid_hunter();
    AddSC_custom_t2_priest_mage();
    AddSC_custom_t2_rogue_armor();
    AddSC_custom_southpark_nolife();
    AddSC_custom_sucky_demon();
    AddSC_GOMove_commandscript();
    AddSC_GOEditor();
    AddSC_rts_building();
    AddSC_violet_hold_boons();
}
