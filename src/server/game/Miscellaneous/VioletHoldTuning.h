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

#ifndef TRINITYCORE_VIOLET_HOLD_TUNING_H
#define TRINITYCORE_VIOLET_HOLD_TUNING_H

#include "Define.h"
#include "Optional.h"

#include <string>
#include <vector>

// Every Violet Hold balance number in one place (owner 2026-11-20: "make this
// as easy to balance as possible ... server commands and configs for
// everything").
//
// A knob is read, in this order, from: a live override (`.vhr tune <knob>
// <value>`, gone at restart), then `Centurion.VioletHold.<knob>` in
// worldserver.conf (`.reload config`), then its default. Every read happens
// at use, so a change applies from the next card, wave or run it touches.
//
// Per-boon overrides work the same way: `.vhr boon <name> weight|price <n>`,
// or Centurion.VioletHold.BoonWeights / Centurion.VioletHold.Shop.BoonPrices
// as "phoenix:0.5 greed:0" lists. Weight is in the table's units (8-10 is a
// plain common boon); 0 takes a boon out of every roll.
//
// A few knobs mirror existing World config values (PrepSeconds and friends):
// setting one changes the live World value until the next `.reload config`.
namespace VioletHoldTuning
{
    TC_GAME_API int32 Int(char const* knob);
    // Percent knobs as a fraction of 100.
    inline float Pct(char const* knob) { return float(Int(knob)) / 100.0f; }

    // `.vhr tune`: set ("reset" clears the override), list, clear all.
    // Returns the line to show the GM.
    TC_GAME_API std::string Set(std::string const& knob, std::string const& value);
    TC_GAME_API void List(std::string const& filter, std::vector<std::string>& lines);
    TC_GAME_API void ResetAll();

    // Per-boon: the roll weight in quarters (0 = never offered) and the shop
    // price, when overridden; nothing when the table and tiers decide.
    TC_GAME_API Optional<uint32> GetBoonQuarters(uint8 boonIndex);
    TC_GAME_API Optional<uint32> GetBoonPrice(uint8 boonIndex);
    // `.vhr boon <name> [weight <w> | price <n> | reset]`.
    TC_GAME_API std::string BoonCommand(std::string const& name, std::string const& what, std::string const& value);
}

#endif
