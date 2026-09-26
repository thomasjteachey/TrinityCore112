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

// The realm treasury's lifecycle: load at startup, write the balance back once
// a minute, and log the ledger. The pool itself lives in the game library
// (RealmTreasury.h) because the core sinks - repairs, trainers, merchants -
// deposit into it directly.

#include "Config.h"
#include "RealmTreasury.h"
#include "ScriptMgr.h"

#include <algorithm>

namespace
{
    constexpr uint32 kSaveIntervalMs = 60 * IN_MILLISECONDS;
}

class custom_realm_treasury_world : public WorldScript
{
public:
    custom_realm_treasury_world() : WorldScript("custom_realm_treasury_world") { }

    void OnConfigLoad(bool /*reload*/) override
    {
        RealmTreasury::LoadConfig();
        _ledgerIntervalMs = uint32(std::clamp(sConfigMgr->GetIntDefault("Centurion.Treasury.LedgerMinutes", 10), 1, 1440))
            * MINUTE * IN_MILLISECONDS;
    }

    void OnUpdate(uint32 diff) override
    {
        _saveTimer += diff;
        if (_saveTimer >= kSaveIntervalMs)
        {
            _saveTimer = 0;
            if (RealmTreasury::IsDirty())
                RealmTreasury::SaveToDB(false);
        }

        _ledgerTimer += diff;
        if (_ledgerTimer >= _ledgerIntervalMs)
        {
            _ledgerTimer = 0;
            RealmTreasury::LogLedger();
        }
    }

    void OnShutdown() override
    {
        RealmTreasury::LogLedger();
        RealmTreasury::SaveToDB(true);
    }

private:
    uint32 _saveTimer = 0;
    uint32 _ledgerTimer = 0;
    uint32 _ledgerIntervalMs = 10 * MINUTE * IN_MILLISECONDS;
};

void AddSC_custom_realm_treasury()
{
    new custom_realm_treasury_world();
}
