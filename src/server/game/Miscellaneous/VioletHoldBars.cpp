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

// The bars a character walked into a Violet Hold run with, kept aside and given
// back. Miscellaneous/VioletHoldBars.h holds the rules; this is how they are kept.
//
// No crash can lose them. Every write to the table goes through the character
// database's asynchronous queue, the same queue every character save goes
// through, and that queue runs in order (CharacterDatabase.WorkerThreads = 1,
// which a logout's save followed by the next login relies on too). So what is
// on disk after a crash is always some leading part of what was queued:
//
//   - the row is written before any save of the run's bars can be;
//   - the bars put back and the row's deletion are ONE transaction, queued
//     behind every save made during the run - both land or neither does, and
//     if neither did the row is still there for the next login to give back.
//
// The rows are what survives a restart; the map below is what the battleground
// asks, so no leave reads the table and a quick way back in never races a write
// still waiting in the queue. The only reads are at login (and a fallback that
// should never be needed), and a login's own data is read off that same queue,
// behind everything queued before it.
//
// Every query is written out rather than prepared: a prepared statement for a
// table that does not exist is fatal at startup, and this branch is shared with
// realms that have never heard of this one (the CooldownStash reasoning).
//
// The table:
//
//   CREATE TABLE `custom_violet_hold_bars` (
//     `guid` int unsigned NOT NULL,
//     `spec` tinyint unsigned NOT NULL,
//     `actions` text NOT NULL,          -- "button:action:type,..." (RiftProgression's format)
//     PRIMARY KEY (`guid`)
//   );

#include "Miscellaneous/VioletHoldBars.h"
#include "Battleground.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "Player.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "Util.h"
#include "WorldSession.h"
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace VioletHoldBars
{
namespace
{
    bool StoragePresent = false;

    struct SavedButton
    {
        uint8 Button;
        uint32 Action;
        uint8 Type;
    };

    struct SavedBars
    {
        uint8 Spec = 0;
        std::vector<SavedButton> Buttons;
    };

    // Characters whose bars are being kept aside right now. Battleground maps
    // update on their own threads, logins on the world thread.
    std::mutex StashMutex;
    std::unordered_map<ObjectGuid::LowType, SavedBars> Stashed;

    // A real character's bars. A wave clone, a Fellowship ally or a bot owns
    // nothing that outlives the run, and wears a real character's name but not
    // its guid (CooldownStash::IsCandidate).
    bool IsCandidate(Player const* player)
    {
        if (!StoragePresent || !player)
            return false;

        WorldSession const* session = player->GetSession();
        return session && !session->IsVirtualSession() && !session->IsTransientPlayerSession();
    }

    std::vector<SavedButton> Snapshot(Player* player)
    {
        std::vector<SavedButton> out;
        for (uint8 button = 0; button < MAX_ACTION_BUTTONS; ++button)
            if (ActionButton const* ab = player->GetActionButton(button))
                out.push_back({ button, ab->GetAction(), uint8(ab->GetType()) });
        return out;
    }

    std::string ButtonsToString(std::vector<SavedButton> const& buttons)
    {
        std::string out;
        for (SavedButton const& b : buttons)
            out += Trinity::StringFormat("{}{}:{}:{}", out.empty() ? "" : ",", uint32(b.Button), b.Action, uint32(b.Type));
        return out;
    }

    std::vector<SavedButton> ButtonsFromString(std::string const& text)
    {
        std::vector<SavedButton> out;
        for (std::string_view part : Trinity::Tokenize(text, ',', false))
        {
            std::vector<std::string_view> const f = Trinity::Tokenize(part, ':', false);
            if (f.size() != 3)
                continue;
            Optional<uint32> const button = Trinity::StringTo<uint32>(f[0]);
            Optional<uint32> const action = Trinity::StringTo<uint32>(f[1]);
            Optional<uint32> const type = Trinity::StringTo<uint32>(f[2]);
            if (button && action && type && *button < MAX_ACTION_BUTTONS)
                out.push_back({ uint8(*button), *action, uint8(*type) });
        }
        return out;
    }

    // The row as it stands on disk. Only where the queue has nothing for this
    // character still waiting: a login, or a fallback.
    bool ReadRow(ObjectGuid::LowType guid, SavedBars& out)
    {
        QueryResult row = CharacterDatabase.PQuery("SELECT spec, actions FROM custom_violet_hold_bars WHERE guid = {}", guid);
        if (!row)
            return false;

        Field* fields = row->Fetch();
        out.Spec = fields[0].GetUInt8();
        out.Buttons = ButtonsFromString(fields[1].GetString());
        return true;
    }

    // What the character is owed, out of the map; read off the table when the
    // map has nothing (a stash from before a restart). False: nothing owed.
    bool Take(ObjectGuid::LowType guid, SavedBars& out)
    {
        {
            std::lock_guard<std::mutex> lock(StashMutex);
            auto const itr = Stashed.find(guid);
            if (itr != Stashed.end())
            {
                out = std::move(itr->second);
                Stashed.erase(itr);
                return true;
            }
        }
        return ReadRow(guid, out);
    }

    // The bars back on the character, and the row gone, in one transaction.
    // `sendNow` is false at login, where the bars ride out with the initial
    // packets a moment later.
    void Apply(Player* player, SavedBars const& saved, bool sendNow)
    {
        ObjectGuid::LowType const guid = player->GetGUID().GetCounter();
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

        if (saved.Spec == player->GetActiveSpec())
        {
            // Exactly as they were: whatever the run put on them comes off, and
            // every saved button goes back. A spell the character no longer
            // knows is refused by addActionButton and leaves its button empty.
            for (uint8 button = 0; button < MAX_ACTION_BUTTONS; ++button)
                player->removeActionButton(button);
            for (SavedButton const& b : saved.Buttons)
                player->addActionButton(b.Button, b.Action, b.Type);

            if (sendNow)
                player->SendActionButtons(1);

            // Written now rather than at the next save, inside the transaction
            // that deletes the row.
            player->SaveActionButtonsToDB(trans);
        }
        else if (saved.Spec < player->GetSpecsCount())
        {
            // It swapped specs inside. Player::ActivateSpec saved the spec it
            // walked in on to the database on the way past, so that is where its
            // bars go back; the spec it is on now is not touched.
            trans->PAppend("DELETE FROM character_action WHERE guid = {} AND spec = {}", guid, uint32(saved.Spec));
            std::string values;
            for (SavedButton const& b : saved.Buttons)
                values += Trinity::StringFormat("{}({},{},{},{},{})", values.empty() ? "" : ",", guid, uint32(saved.Spec), uint32(b.Button), b.Action, uint32(b.Type));
            if (!values.empty())
                trans->PAppend("INSERT INTO character_action (guid, spec, button, action, type) VALUES {}", values);
        }

        trans->PAppend("DELETE FROM custom_violet_hold_bars WHERE guid = {}", guid);
        CharacterDatabase.CommitTransaction(trans);

        TC_LOG_DEBUG("bg.battleground", "Violet Hold bars: {} got the {} button(s) of spec {} back (now on spec {}).",
            player->GetName(), saved.Buttons.size(), uint32(saved.Spec), uint32(player->GetActiveSpec()));
    }
}

void ProbeStorage()
{
    StoragePresent = CharacterDatabase.Query("SELECT TABLE_NAME FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'custom_violet_hold_bars'") != nullptr;

    if (!StoragePresent)
        TC_LOG_INFO("server.loading", "Violet Hold bars: `custom_violet_hold_bars` is not on this realm; a run's changes to the action bars stay after it.");
}

void Stash(Player* player)
{
    if (!IsCandidate(player))
        return;

    // A relog back into the run: these are the run's bars, and the stash made
    // at the door still stands.
    if (player->GetSession()->PlayerLoading())
        return;

    ObjectGuid::LowType const guid = player->GetGUID().GetCounter();
    SavedBars saved;
    saved.Spec = player->GetActiveSpec();
    saved.Buttons = Snapshot(player);
    std::string const actions = ButtonsToString(saved.Buttons);

    {
        std::lock_guard<std::mutex> lock(StashMutex);
        if (!Stashed.emplace(guid, std::move(saved)).second)
            return;   // already kept aside: that one is the real thing
    }

    // Queued ahead of any save that could carry the run's bars to disk.
    CharacterDatabase.PExecute("REPLACE INTO custom_violet_hold_bars (guid, spec, actions) VALUES ({}, {}, '{}')",
        guid, uint32(player->GetActiveSpec()), actions);
}

void Restore(Player* player)
{
    if (!IsCandidate(player))
        return;

    // Player::LoadFromDB carrying a character out of a run that ended without
    // it: its bars have not even been read yet. RestoreAtLogin, further into
    // the same load, is what gives them back.
    if (!player->IsInWorld() || player->GetSession()->PlayerLoading())
        return;

    SavedBars saved;
    if (Take(player->GetGUID().GetCounter(), saved))
        Apply(player, saved, true);
}

void RestoreAtLogin(Player* player)
{
    if (!IsCandidate(player))
        return;

    ObjectGuid::LowType const guid = player->GetGUID().GetCounter();

    // Going back into the run it left: it keeps fighting with the bars it had
    // there, and leaving will still give the real ones back - from the map,
    // which a restart may have emptied, so the row goes back into it.
    if (Battleground const* bg = player->GetBattleground(); bg && bg->GetTypeID() == BATTLEGROUND_VHR)
    {
        {
            std::lock_guard<std::mutex> lock(StashMutex);
            if (Stashed.count(guid))
                return;
        }
        SavedBars saved;
        if (ReadRow(guid, saved))
        {
            std::lock_guard<std::mutex> lock(StashMutex);
            Stashed.emplace(guid, std::move(saved));
        }
        return;
    }

    SavedBars saved;
    if (Take(guid, saved))
        Apply(player, saved, false);
}
}
