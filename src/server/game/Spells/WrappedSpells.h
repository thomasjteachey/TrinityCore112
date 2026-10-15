/*
 * Wrapped spells: abilities a player holds as a WRAPPER that casts the real
 * spell for them - the untouched stock rank, or a set-bonus alternate when the
 * set is worn (spell_warr_disarm_wrapper, spell_sha_*_totem_wrapper, the T2
 * wrappers in scripts/Custom, and the Purge TRIGGER_SPELL pair).
 *
 * Nothing a wrapper casts is ever held itself. Every path that would teach one
 * teaches its wrapper instead - Player::LearnSpell / AddSpell, the trainer
 * list (ObjectMgr::LoadTrainers) and talent ranks (LoadDBCStores) - and
 * characters saved before that are moved over at startup
 * (scripts/Custom/custom_wrapped_spells.cpp). A stock rank that stayed in a
 * spellbook skipped its wrapper, so the set bonus did nothing for it and the
 * paladin with both Consecrations drew two rings.
 *
 * Seal of Justice is not here: its wrapper 81472 exists in no Centurion
 * Spell.dbc, server or client. Consecration ranks 7-8 (48818/48819) have no
 * wrapper.
 */

#ifndef TRINITY_WRAPPED_SPELLS_H
#define TRINITY_WRAPPED_SPELLS_H

#include "Define.h"

namespace WrappedSpells
{
    struct Entry
    {
        uint32 Wrapped;     // cast by the wrapper, never held
        uint32 Wrapper;     // held instead
    };

    inline constexpr Entry Entries[] =
    {
        // Disarm, and Heimdall's Disarm
        {   676, 81492 }, { 81493, 81492 },

        // Tremor / Poison Cleansing / Grounding Totem, and their Autumn's Leaves versions
        {  8143, 81476 }, { 81479, 81476 },
        {  8166, 81477 }, { 81480, 81477 },
        {  8177, 81478 }, { 81481, 81478 },

        // Purge
        {   370, 81324 }, {  8012, 81325 },

        // Shadow Bolt ranks 1-13, and both of its clone sets
        {   686, 90420 }, { 90433, 90420 }, { 90446, 90420 },
        {   695, 90421 }, { 90434, 90421 }, { 90447, 90421 },
        {   705, 90422 }, { 90435, 90422 }, { 90448, 90422 },
        {  1088, 90423 }, { 90436, 90423 }, { 90449, 90423 },
        {  1106, 90424 }, { 90437, 90424 }, { 90450, 90424 },
        {  7641, 90425 }, { 90438, 90425 }, { 90451, 90425 },
        { 11659, 90426 }, { 90439, 90426 }, { 90452, 90426 },
        { 11660, 90427 }, { 90440, 90427 }, { 90453, 90427 },
        { 11661, 90428 }, { 90441, 90428 }, { 90454, 90428 },
        { 25307, 90429 }, { 90442, 90429 }, { 90455, 90429 },
        { 27209, 90430 }, { 90443, 90430 }, { 90456, 90430 },
        { 47808, 90431 }, { 90444, 90431 }, { 90457, 90431 },
        { 47809, 90432 }, { 90445, 90432 }, { 90458, 90432 },

        // Serpent Sting ranks 1-12, and their fire clones
        {  1978, 90460 }, { 90472, 90460 },
        { 13549, 90461 }, { 90473, 90461 },
        { 13550, 90462 }, { 90474, 90462 },
        { 13551, 90463 }, { 90475, 90463 },
        { 13552, 90464 }, { 90476, 90464 },
        { 13553, 90465 }, { 90477, 90465 },
        { 13554, 90466 }, { 90478, 90466 },
        { 13555, 90467 }, { 90479, 90467 },
        { 25295, 90468 }, { 90480, 90468 },
        { 27016, 90469 }, { 90481, 90469 },
        { 49000, 90470 }, { 90482, 90470 },
        { 49001, 90471 }, { 90483, 90471 },

        // Sprint ranks 1-3, and Ice Skate
        {  2983, 90514 }, {  8696, 90515 }, { 11305, 90516 }, { 90517, 90514 },

        // Consecration ranks 1-6, and their ring-less clones
        { 26573, 90526 }, { 90532, 90526 },
        { 20116, 90527 }, { 90533, 90527 },
        { 20922, 90528 }, { 90534, 90528 },
        { 20923, 90529 }, { 90535, 90529 },
        { 20924, 90530 }, { 90536, 90530 },
        { 27173, 90531 }, { 90537, 90531 },
    };

    // The wrapper a player holds instead of spellId, or 0 when spellId is not
    // wrapped. A linear walk: about a hundred entries, called on learn and at
    // load, never per tick.
    inline uint32 GetWrapper(uint32 spellId)
    {
        for (Entry const& entry : Entries)
            if (entry.Wrapped == spellId)
                return entry.Wrapper;
        return 0;
    }
}

#endif
