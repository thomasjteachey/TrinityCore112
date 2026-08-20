/*
 * Legionnaire Plus T2 set bonuses - priest and mage, the scripted half.
 *
 * The carriers themselves are Spell.dbc rows 90300-90394 (see
 * sql/custom/dbc/2026_08_17_00_dbc_t2_set_bonuses.sql, which is the contract
 * for every id in this file) plus the 2026_08_19_06 follow-up rows 90500/90501
 * and the updates to 90340/90383/90385 in
 * sql/custom/dbc/2026_08_19_06_dbc_t2_priest_mage.sql. Everything a plain aura
 * type could express - cost, range, global cooldown, the flat +300% physical on
 * Wraithblade - is already done in data; only the bonuses that need to look at
 * state, at a different spell, or at the player's hands live here.
 *
 * Same shape as custom_t1_set_bonuses.cpp: the passive is an inert dummy the
 * ItemSet grants, and each script either sits on that dummy or on the ability
 * it modifies and tests HasAura() on the caster.
 *
 * Wiring lives in ItemSet.dbc (SetSpellID/SetThreshold), written by the forge
 * tooling; sql/custom/world/2026_08_19_06_world_t2_priest_mage.sql binds these
 * classes and carries the spell_proc rows the proc carriers need.
 *
 * DEFERRED WORK RULE. Several bonuses here have to touch the player's inventory
 * or cast an area spell at the exact moment an aura is being removed (Ice Block
 * expiring, Confiscated Arms ending, the mage dying). None of that is done
 * inside the aura machinery: the aura hook only flags the player, and the
 * PlayerScript t2_priest_mage_update does the real work on the next
 * Player::Update tick, outside every aura container walk. That is the "defer
 * via a flag" half of the heap-corruption rule and it costs one atomic load per
 * player tick for everyone who has nothing pending.
 *
 * UMBRAL MERCY (90340) IS SPLIT WITH CORE. Keeping Shadowform up through a Holy
 * heal lives in src/server/game/Entities/Object/T2SpellHooks.cpp
 * (OnCancelAuraRequest swallows the client's autoUnshift CMSG_CANCEL_AURA(15473)
 * for 90340 holders, OnCastSpellRequest + the CheckShapeshift waiver let the
 * heal through with the form still up). Nothing in this file touches the form
 * any more - an earlier script-side re-apply was removed so the two cannot
 * fight. Only the PRICE is here: half of the effective healing, paid as shadow
 * self-damage. Since 2026-08-19 that is ONE mechanism for every heal - the
 * carrier's own proc (spell_proc row on 90340) queues the price and
 * t2_priest_mage_update pays it next tick. The eleven per-heal
 * spell_script_names bindings it replaced are gone.
 */

#include "ScriptMgr.h"
#include "Log.h"
#include "Chat.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellHistory.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "T2SpellHooks.h"
#include "T2UnitHooks.h"
#include "UpdateFields.h"
#include "World.h"
#include "WorldSession.h"

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    enum T2PriestMageSpells
    {
        // priest - holy (90336 3pc, 90337 5pc, 90338 8pc)
        SPELL_T2_HOLY_CENSER_PASSIVE    = 90338,
        SPELL_T2_HOLY_CENSER_IMBUE      = 90384,
        // Holy Fire's chain tops out at rank 8, level 60. 15267 is rank 7 and
        // is the id that usually gets mistaken for the last one.
        SPELL_PRIEST_HOLY_FIRE          = 15261,
        // The BUFF, not the talent: 15270 is the passive that triggers it.
        SPELL_PRIEST_SPIRIT_TAP         = 15271,

        // priest - shadow (90339 3pc, 90340 5pc, 90341 8pc). Shadowform (15473)
        // itself is not named here: the form-keeping half of 90340 is core's
        // (T2SpellHooks), this file only prices the heals.
        SPELL_T2_UMBRAL_MERCY           = 90340,
        SPELL_T2_UMBRAL_MERCY_DAMAGE    = 90501,   // combat-log carrier for the self damage
        SPELL_T2_WRAITHBLADE            = 90341,
        SPELL_T2_WRAITHBLADE_ENERGIZE   = 90500,   // ENERGIZE helper, bp at runtime

        // mage - dusty / Ice Block (90342 3pc, 90343 5pc, 90344 8pc)
        SPELL_T2_GLACIAL_REPRIEVE       = 90342,
        SPELL_T2_HOARFROST_BLOOM        = 90343,
        SPELL_T2_ENCASED_IN_ICE         = 90385,
        SPELL_MAGE_ICE_BLOCK            = 45438,

        // mage - fiery payback (90345 3pc, 90346 5pc, 90347 8pc)
        SPELL_T2_SCORCHING_MOMENTUM     = 90346,
        SPELL_T2_ASHEN_CONFISCATION     = 90347,
        SPELL_T2_CONFISCATED_ARMS       = 90387,
        // Fiery Payback's own disarm: mechanic 3, 6 sec, and it takes the main
        // hand (aura 67) AND the ranged slot (aura 278) - which is why the
        // seizure takes slots 15 and 17 and not the off hand.
        SPELL_MAGE_FIERY_PAYBACK_DISARM = 64346,
    };

    // Fire Blast, family 3, word 0 bit 0x2 - identical on all eleven ranks.
    uint32 constexpr MAGE_FIRE_BLAST_FAMILY_MASK0 = 0x2;

    // .gm diagnostics customauras - broadcast to every opted-in GM session
    void SendCustomAuraDiag(std::string const& msg)
    {
        // Also to the log, not just to whoever happens to be watching. A
        // chat-only diagnostic cannot distinguish "the chain never ran" from
        // "no GM had the category enabled", which is precisely how three set
        // bonuses got mis-diagnosed twice. Logger `custom.auras`; free when off.
        TC_LOG_INFO("custom.auras", "{}", msg);

        for (auto const& sessionPair : sWorld->GetAllSessions())
            if (sessionPair.second && sessionPair.second->GetPlayer()
                && sessionPair.second->IsGmDiagnosticEnabled(GmDiagnosticCategory::CustomAuras))
                ChatHandler(sessionPair.second).SendSysMessage(msg.c_str());
    }

    // -----------------------------------------------------------------------
    // Shared deferred-work bookkeeping. Map updates run in parallel, so every
    // container below is guarded; the atomic is the fast path that lets
    // t2_priest_mage_update::OnUpdate return without taking the lock for the
    // (overwhelmingly common) player with nothing pending.
    // -----------------------------------------------------------------------
    std::mutex s_workMutex;
    std::unordered_set<ObjectGuid> s_playersWithWork;
    std::atomic<uint32> s_workCount{ 0 };

    void MarkWork(ObjectGuid guid)
    {
        std::lock_guard<std::mutex> guard(s_workMutex);
        s_playersWithWork.insert(guid);
        s_workCount.store(uint32(s_playersWithWork.size()), std::memory_order_release);
    }

    // Pops the flag; true if this player had work queued.
    bool TakeWork(ObjectGuid guid)
    {
        if (s_workCount.load(std::memory_order_acquire) == 0)
            return false;
        std::lock_guard<std::mutex> guard(s_workMutex);
        auto itr = s_playersWithWork.find(guid);
        if (itr == s_playersWithWork.end())
            return false;
        s_playersWithWork.erase(itr);
        s_workCount.store(uint32(s_playersWithWork.size()), std::memory_order_release);
        return true;
    }

    // Hoarfrost Bloom: mages whose Ice Block just ran out.
    std::unordered_set<ObjectGuid> s_pendingHoarfrost;

    // Umbral Mercy: self damage owed for healing done in Shadowform (direct
    // heals and Renew ticks alike), accumulated by the 90340 proc and paid on
    // the next Player::Update tick - a Renew tick on the priest himself fires
    // inside his own owned-aura walk, and DealDamage strips TAKE_DAMAGE auras.
    std::unordered_map<ObjectGuid, int32> s_pendingUmbralPrice;

    // Ashen Confiscation state, per mage.
    struct SeizedWeapon
    {
        uint8 slot = 0;                 // EQUIPMENT_SLOT_MAINHAND / RANGED
        uint32 entry = 0;
        ObjectGuid tempGuid;            // empty when visualOnly
        bool visualOnly = false;        // fell back to PLAYER_VISIBLE_ITEM_*
    };
    struct DisplacedWeapon
    {
        uint8 slot = 0;                 // where it was equipped
        ObjectGuid guid;
    };
    struct ConfiscationState
    {
        bool equipPending = false;
        bool cleanupPending = false;
        uint32 pendingMainHandEntry = 0;
        uint32 pendingRangedEntry = 0;
        std::vector<SeizedWeapon> seized;
        std::vector<DisplacedWeapon> displaced;
    };
    std::unordered_map<ObjectGuid, ConfiscationState> s_confiscations;

    // Equip / re-equip without the 1.5 s weapon-swap global cooldown that
    // Player::EquipItem starts for weapons changed in combat. Nothing the mage
    // pressed caused this swap, so the mage must not be locked out for it. The
    // timer is only read by EquipItem's "== 0" gate and by CanEquipItem (which we
    // call with not_loading = false, bypassing it), so parking it at 1 and
    // clearing it afterwards has no other effect.
    void EquipWithoutSwapCooldown(Player* player, uint16 pos, Item* item)
    {
        player->setWeaponChangeTimer(1);
        player->EquipItem(pos, item, true);
        player->setWeaponChangeTimer(0);
        // EquipItem skips _ApplyItemMods on a dead player (stats are normally
        // untouched by death and re-applied only through equip), so a re-equip
        // during the death cleanup would leave the weapon's stats missing
        // until the next manual re-equip. Apply them here to keep the
        // "equipped == modded" invariant.
        if (!player->IsAlive())
            player->_ApplyItemMods(item, uint8(pos & 255), true);
    }

    // Re-equip a displaced original. True on success.
    bool ReequipOriginal(Player* mage, DisplacedWeapon const& displaced)
    {
        Item* item = mage->GetItemByGuid(displaced.guid);
        if (!item)
            return false;
        if (item->IsEquipped())
            return true; // the mage already put it back by hand
        if (mage->GetItemByPos(INVENTORY_SLOT_BAG_0, displaced.slot))
            return false; // something else is there now, leave it in the bag
        uint16 pos = 0;
        if (mage->CanEquipItem(displaced.slot, pos, item, false, false) != EQUIP_ERR_OK)
            return false;
        mage->RemoveItem(item->GetBagSlot(), item->GetSlot(), true);
        EquipWithoutSwapCooldown(mage, pos, item);
        return true;
    }

    // Move an equipped item into the bags. True on success; on success the
    // item is recorded in `displaced` so the cleanup can put it back.
    bool DisplaceToBags(Player* mage, uint8 slot, std::vector<DisplacedWeapon>& displaced)
    {
        Item* item = mage->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item)
            return true; // nothing to move
        ItemPosCountVec dest;
        if (mage->CanStoreItem(NULL_BAG, NULL_SLOT, dest, item, false) != EQUIP_ERR_OK)
            return false;
        DisplacedWeapon record;
        record.slot = slot;
        record.guid = item->GetGUID();
        mage->RemoveItem(INVENTORY_SLOT_BAG_0, slot, true);
        mage->StoreItem(dest, item, true);
        displaced.push_back(record);
        return true;
    }

    // Put the victim's weapon entry into `slot` for real. Returns the seizure
    // record; visualOnly is set when the inventory had no room for the mage's
    // own weapon(s) or the equip was refused, in which case only the
    // appearance is swapped (the old behaviour) and the reason is logged.
    SeizedWeapon SeizeIntoSlot(Player* mage, uint8 slot, uint32 entry, std::vector<DisplacedWeapon>& displaced)
    {
        SeizedWeapon seized;
        seized.slot = slot;
        seized.entry = entry;

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
        std::string why;
        if (!proto)
            why = "no item_template for the entry";
        else
        {
            // Everything the mage is wearing in the way goes to the bags
            // first, one at a time, so each CanStoreItem sees the previous move.
            size_t const displacedBefore = displaced.size();
            bool roomy = DisplaceToBags(mage, slot, displaced);
            if (roomy && slot == EQUIPMENT_SLOT_MAINHAND && proto->InventoryType == INVTYPE_2HWEAPON)
                roomy = DisplaceToBags(mage, EQUIPMENT_SLOT_OFFHAND, displaced);

            if (!roomy)
                why = "no free bag slot for the mage's own weapon";
            else
            {
                Item* temp = Item::CreateItem(entry, 1, mage);
                if (!temp)
                    why = "Item::CreateItem failed";
                else
                {
                    // Registered BEFORE CanEquipItem: that is what lets the
                    // core waive class/proficiency/skill/level on exactly this
                    // item and report 300 weapon skill for it.
                    T2UnitHooks::RegisterTemporaryWeapon(temp->GetGUID());
                    // Soulbound: no trade, mail, auction or guild bank.
                    temp->SetBinding(true);
                    // Not vendorable either, independent of the core's own
                    // refusal for registered temporaries: HandleSellItemOpcode
                    // drops a sell request for a REFUNDABLE item on the floor
                    // (it assumes a CMSG_ITEM_REFUND is on its way). The
                    // recipient is set so SendRefundInfo / RefundItem do not
                    // read "traded" and strip the flag; with no extended cost
                    // paid both then bail before anything is refunded. No
                    // AddRefundReference: the item never reaches a save, and
                    // DestroyItem's SetNotRefundable is the only exit it needs.
                    temp->SetFlag(ITEM_FIELD_FLAGS, ITEM_FIELD_FLAG_REFUNDABLE);
                    temp->SetRefundRecipient(mage->GetGUID().GetCounter());

                    uint16 pos = 0;
                    // not_loading = false: the in-combat weapon-change timer,
                    // stun and casting gates are for the player's own swaps.
                    InventoryResult res = mage->CanEquipItem(slot, pos, temp, false, false);
                    if (res == EQUIP_ERR_OK)
                    {
                        EquipWithoutSwapCooldown(mage, pos, temp);
                        seized.tempGuid = temp->GetGUID();
                        return seized;
                    }

                    why = Trinity::StringFormat("CanEquipItem refused ({})", uint32(res));
                    T2UnitHooks::UnregisterTemporaryWeapon(temp->GetGUID());
                    delete temp; // never entered any container
                }
            }

            // Undo the displacement this slot caused, newest first.
            while (displaced.size() > displacedBefore)
            {
                DisplacedWeapon back = displaced.back();
                displaced.pop_back();
                ReequipOriginal(mage, back);
            }
        }

        // Appearance-only fallback.
        seized.visualOnly = true;
        if (slot < EQUIPMENT_SLOT_END)
            mage->SetUInt32Value(PLAYER_VISIBLE_ITEM_1_ENTRYID + (slot * 2), entry);

        SendCustomAuraDiag(Trinity::StringFormat(
            "[CustomAuras] {}: Ashen Confiscation - could not equip item {} in slot {} ({}); showing it only",
            mage->GetName(), entry, uint32(slot), why));
        return seized;
    }

    void CleanupConfiscation(Player* mage, char const* reason);

    // Drop a state entry that holds nothing (no seized weapons, nothing
    // pending) so OnSave/OnMapChanged never pay for a stale key.
    void ForgetConfiscationIfEmpty(ObjectGuid guid)
    {
        std::lock_guard<std::mutex> guard(s_workMutex);
        auto itr = s_confiscations.find(guid);
        if (itr != s_confiscations.end() && itr->second.seized.empty() && itr->second.displaced.empty()
            && !itr->second.equipPending && !itr->second.cleanupPending)
            s_confiscations.erase(itr);
    }

    void PerformConfiscationEquip(Player* mage, uint32 mainHandEntry, uint32 rangedEntry)
    {
        // The buff ended (or never landed) before this tick, or the mage is
        // dead / between maps: nothing to wield.
        if (!mage->IsInWorld() || !mage->IsAlive() || !mage->HasAura(SPELL_T2_CONFISCATED_ARMS)
            || (!mainHandEntry && !rangedEntry))
        {
            // All four of these were silent drops, and a silent drop here is
            // EXACTLY what "the stealing weapon didn't work" looks like from
            // the outside: the proc fired, the disarm landed, and then the
            // seizure evaporated one tick later with nothing said.
            SendCustomAuraDiag(Trinity::StringFormat(
                "[CustomAuras] {}: Ashen Confiscation - seizure dropped on the equip tick ({})",
                mage->GetName(),
                !mage->IsInWorld() ? "mage is no longer in world"
                    : !mage->IsAlive() ? "mage died before the equip tick"
                    : !mage->HasAura(SPELL_T2_CONFISCATED_ARMS) ? "Confiscated Arms (90387) is not on the mage - it never landed or was removed"
                    : "the victim had nothing in either seized slot"));
            ForgetConfiscationIfEmpty(mage->GetGUID());
            return;
        }

        std::vector<SeizedWeapon> seized;
        std::vector<DisplacedWeapon> displaced;
        if (mainHandEntry)
            seized.push_back(SeizeIntoSlot(mage, EQUIPMENT_SLOT_MAINHAND, mainHandEntry, displaced));
        if (rangedEntry)
            seized.push_back(SeizeIntoSlot(mage, EQUIPMENT_SLOT_RANGED, rangedEntry, displaced));

        {
            std::lock_guard<std::mutex> guard(s_workMutex);
            ConfiscationState& state = s_confiscations[mage->GetGUID()];
            state.seized.insert(state.seized.end(), seized.begin(), seized.end());
            state.displaced.insert(state.displaced.end(), displaced.begin(), displaced.end());
        }

        // The buff may have been stripped while we were moving items (an item
        // equip spell is not going to do that, but be exact): if it is gone,
        // undo right away rather than waiting for a removal hook that already
        // fired.
        if (!mage->HasAura(SPELL_T2_CONFISCATED_ARMS))
        {
            CleanupConfiscation(mage, "buff gone during equip");
            return;
        }

        for (SeizedWeapon const& weapon : seized)
            SendCustomAuraDiag(Trinity::StringFormat(
                "[CustomAuras] {}: Ashen Confiscation - {} item {} in slot {}",
                mage->GetName(), weapon.visualOnly ? "showing" : "wielding", weapon.entry, uint32(weapon.slot)));
    }

    // Tear the confiscation down: destroy every temporary weapon wherever it
    // ended up, put the mage's own weapons back, restore appearance. Safe to
    // call from any clean context (Player::Update, SaveToDB, logout); never
    // from inside an aura hook.
    void CleanupConfiscation(Player* mage, char const* reason)
    {
        ConfiscationState state;
        {
            std::lock_guard<std::mutex> guard(s_workMutex);
            auto itr = s_confiscations.find(mage->GetGUID());
            if (itr == s_confiscations.end())
                return;
            state = itr->second;
            s_confiscations.erase(itr);
        }

        uint32 destroyed = 0;
        for (SeizedWeapon const& weapon : state.seized)
        {
            if (weapon.visualOnly)
            {
                // SetVisibleItemSlot re-derives entry, enchants and the
                // transmog cache from whatever is really equipped.
                mage->SetVisibleItemSlot(weapon.slot, mage->GetItemByPos(INVENTORY_SLOT_BAG_0, weapon.slot));
                continue;
            }

            T2UnitHooks::UnregisterTemporaryWeapon(weapon.tempGuid);

            if (Item* item = mage->GetItemByGuid(weapon.tempGuid))
            {
                mage->DestroyItem(item->GetBagSlot(), item->GetSlot(), true);
                ++destroyed;
                continue;
            }
            // Sold in the window: it is sitting in a buyback slot. The
            // REFUNDABLE flag set at creation should make this unreachable;
            // the sweep stays so a temporary never survives its buff.
            for (uint32 i = BUYBACK_SLOT_START; i < BUYBACK_SLOT_END; ++i)
                if (Item* item = mage->GetItemFromBuyBackSlot(i))
                    if (item->GetGUID() == weapon.tempGuid)
                    {
                        mage->RemoveItemFromBuyBackSlot(i, true);
                        ++destroyed;
                        break;
                    }
        }

        // Originals back, newest displacement first (the off hand a 2H
        // pushed out goes back after the main hand is free again).
        uint32 restored = 0;
        for (auto itr = state.displaced.rbegin(); itr != state.displaced.rend(); ++itr)
            if (ReequipOriginal(mage, *itr))
                ++restored;

        if (!state.seized.empty())
            SendCustomAuraDiag(Trinity::StringFormat(
                "[CustomAuras] {}: Ashen Confiscation over ({}) - {} temporary weapon(s) destroyed, {} of {} own weapon(s) re-equipped",
                mage->GetName(), reason, destroyed, restored, uint32(state.displaced.size())));
    }

    // Umbral Mercy's price. Raw (no resist, no absorb, no done/taken
    // modifiers - Shadowform's own -15% physical would otherwise refund part of
    // it and a Power Word: Shield would eat it) but logged through 90501 so the
    // combat log shows "Umbral Mercy" hitting the priest for the amount.
    void DealUmbralMercyPrice(Player* priest, int32 amount)
    {
        if (amount <= 0)
            return;

        // Never lethal, and never from a reentrant death: a priest must not
        // one-shot himself with his own heal. The floor is TWO health, not
        // one: Unit::DealDamage treats any blow >= health-1 on a duelling
        // player as the duel-ending one (and since the attacker is the priest
        // himself, not his opponent, it is not even clamped - he forfeits to
        // his own set bonus). A price that big in a duel is waived outright
        // rather than paid to the floor, because a priest left at 2 health
        // by his own heal is the same forfeit one tick later.
        uint32 const health = priest->GetHealth();
        if (uint32(amount) + 1 >= health)
        {
            if (priest->duel)
            {
                SendCustomAuraDiag(Trinity::StringFormat(
                    "[CustomAuras] {}: Umbral Mercy - price {} waived in duel (health {}, would end it)",
                    priest->GetName(), amount, health));
                return;
            }
            amount = int32(health) - 2;
        }
        if (amount <= 0)
            return;

        if (sSpellMgr->GetSpellInfo(SPELL_T2_UMBRAL_MERCY_DAMAGE))
        {
            SpellNonMeleeDamage log(priest, priest, SPELL_T2_UMBRAL_MERCY_DAMAGE, SPELL_SCHOOL_MASK_SHADOW);
            log.damage = uint32(amount);
            priest->SendSpellNonMeleeDamageLog(&log);
            // DealSpellDamage -> DealDamage with attacker == victim: no
            // pushback, no combat entry, no procs; just the health.
            priest->DealSpellDamage(&log, false);
        }
        else
        {
            // Row not loaded yet (DBC not regenerated) - still charge the price.
            Unit::DealDamage(priest, priest, uint32(amount), nullptr,
                SELF_DAMAGE, SPELL_SCHOOL_MASK_SHADOW, nullptr, false);
        }
    }
}

// 90384 - Censer of the Faithful (holy 8pc), the imbue itself. The 8pc carrier
// (90338) is a plain PROC_TRIGGER_SPELL that hangs this 30 sec buff on the
// priest whenever Holy Fire crits; the payload is here, on the buff, so that
// it spends on the swing rather than on the crit that granted it.
class spell_t2_holy_censer : public AuraScript
{
    PrepareAuraScript(spell_t2_holy_censer);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        // "Imbue your MAIN HAND weapon". The spell_proc row already narrows
        // this to melee, but a dual-wielding priest's off-hand swing would
        // otherwise spend the single charge the main hand is holding.
        DamageInfo* damageInfo = eventInfo.GetDamageInfo();
        return damageInfo && damageInfo->GetAttackType() == BASE_ATTACK;
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        // Effect 0 is a DUMMY with no trigger spell: the core's default action
        // would only log "non-existent spell 0". The charge was already spent
        // in Aura::PrepareProcToTrigger, this does not keep the imbue.
        PreventDefaultAction();

        Unit* priest = GetTarget();
        Unit* victim = eventInfo.GetProcTarget();
        if (!priest || !victim)
            return;

        // Max rank, cast triggered: the imbue is not the priest casting Holy
        // Fire, so it must not pay mana, respect range or eat the global.
        priest->CastSpell(victim, SPELL_PRIEST_HOLY_FIRE, aurEff);
        priest->CastSpell(priest, SPELL_PRIEST_SPIRIT_TAP, aurEff);

        SendCustomAuraDiag(Trinity::StringFormat(
            "[CustomAuras] {}: Censer of the Faithful discharged into {} - Holy Fire + Spirit Tap",
            priest->GetName(), victim->GetName()));
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_t2_holy_censer::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_t2_holy_censer::HandleProc, EFFECT_0, SPELL_AURA_ANY);
    }
};

// NOTE 90337 Kindled Zeal (holy 5pc) and its one-charge buff 90383 have NO
// script on purpose - both halves are data: the carrier is a PROC_TRIGGER_SPELL
// narrowed to Smite/Flash Heal crits by its spell_proc row, and 90383's two
// spellmods are consumed through the proc system (spell_proc row with
// PROC_ATTR_REQ_SPELLMOD, phase CAST, 1 charge: in this core a spellmod
// aura's charge is dropped by Aura::PrepareProcToTrigger on the cast-phase
// proc of the spell that took the mod, not by Spell::TakeMods). Both rows were
// missing from the 2026_08_17_01 world file; they are in 2026_08_19_06.

// RETIRED 2026-08-19: 'spell_t2_umbral_mercy', a SpellScript that sat on the
// nine heal chains (eleven spell_script_names rows of NEGATIVE first-rank ids)
// and charged the price from AfterHit. Everything it did is now done by the
// carrier's own proc below, for one reason: eleven rows that each depend on the
// spell_ranks chain still holding every rank of that heal is eleven ways for the
// bonus to go quietly missing, and "Umbral Mercy is not damaging me when I heal"
// is exactly what a dropped binding looks like. One aura, one spell_proc row and
// one script is the whole feature now. The DELETE in the world file still names
// the retired script, so replaying it drops the old rows.
//
// NOT covered (documented, not half-built): Prayer of Mending (its heals are
// cast by the bounce target, triggered, under the priest's guid) and Holy
// Nova's linked heal. Neither is in 90340's family mask, so both keep the stock
// behaviour - the same set the 5pc's own MOD_IGNORE_SHAPESHIFT effect covers.

// 90340 - Umbral Mercy itself, and the whole price. The carrier is a dummy
// aura; its spell_proc row (family 6, mask0 0x11041E40 / mask1 0x4 - Lesser
// Heal, Heal, Greater Heal, Flash Heal, Prayer of Healing, Circle of Healing,
// Desperate Prayer, Renew and Binding Heal, byte for byte the mask on 90340's
// own MOD_IGNORE_SHAPESHIFT effect; ProcFlags DONE_SPELL_MAGIC_DMG_CLASS_POS |
// DONE_PERIODIC, SpellTypeMask HEAL, phase HIT) makes it see every heal the
// priest lands, direct or ticking, with that hit's HealInfo - effective heal,
// overheal excluded, which is what "healing done" means in the log.
//
// All nine heals are SPELL_DAMAGE_CLASS_MAGIC, so Spell::prepareDataForTriggerSystem
// leaves the proc flags empty and TargetInfo::DoDamageAndTriggers fills in
// DONE_SPELL_MAGIC_DMG_CLASS_POS for the positive hit; Renew's own cast heals
// nothing and is filtered out by SpellTypeMask (it reports NO_DMG_HEAL), its
// ticks come back through DONE_PERIODIC. Multi-target heals proc once per
// target, which is the same per-target pricing the old AfterHit did.
//
// Keeping the form up is NOT this script's job. The 3.3.5 client auto-unshifts
// (CMSG_CANCEL_AURA(15473) then CMSG_CAST_SPELL in one frame); that cancel is
// swallowed for 90340 holders in core (T2SpellHooks::OnCancelAuraRequest), and
// the cast is let through with the form still up (OnCastSpellRequest + the
// CheckShapeshift waiver, though for these nine heals 90340's own
// MOD_IGNORE_SHAPESHIFT effect already sets checkForm = false before the waiver
// is reached). The price must NOT assume that worked, though: see CheckProc.
//
// The price is only QUEUED here: a tick runs inside the owned-aura walk of
// whoever carries the Renew (the priest himself, often) and even a direct heal
// procs from inside the spell's hit frame, while DealDamage strips TAKE_DAMAGE
// auras from the priest - so the damage is dealt from
// t2_priest_mage_update::OnUpdate, one tick later, with nothing in flight.
class spell_t2_umbral_mercy_passive : public AuraScript
{
    PrepareAuraScript(spell_t2_umbral_mercy_passive);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        // Entry trace FIRST, before any gate. Every previous round could only
        // observe "no damage happened", which cannot distinguish "the proc never
        // fired" from "it fired and a gate rejected it" - and those have
        // completely different fixes. Now the log says which.
        HealInfo const* healInfo = eventInfo.GetHealInfo();
        SendCustomAuraDiag(Trinity::StringFormat(
            "[CustomAuras] Umbral Mercy CheckProc entered: spell={} healInfo={} effective={}",
            eventInfo.GetSpellInfo() ? eventInfo.GetSpellInfo()->Id : 0,
            healInfo != nullptr,
            healInfo ? healInfo->GetEffectiveHeal() : 0));

        if (!healInfo || !healInfo->GetEffectiveHeal())
        {
            SendCustomAuraDiag("[CustomAuras] Umbral Mercy: rejected - no effective healing (full-health target?)");
            return false;
        }
        Unit* target = GetTarget();
        Player* priest = target ? target->ToPlayer() : nullptr;
        if (!priest)
            return false;

        // "Doing so" is healing THIS BONUS made possible; a Renew ticking long
        // after the priest left the form is free, like the stock spell.
        //
        // Deliberately NOT a bare `GetShapeshiftForm() == FORM_SHADOW`. That was
        // the 2026-08-19 build's only silent gate and it races the very thing the
        // bonus exists for: the client auto-unshifts out of its own Spell.dbc,
        // which knows nothing about 90340, so every Holy heal pressed in
        // Shadowform is preceded by CMSG_CANCEL_AURA(15473). Any frame in which
        // that cancel wins - it arrived with no cast behind it, a second press
        // raced it, the form went down for an unrelated reason mid-cast - the
        // heal lands out of form, this returns false without a word, and the
        // priest heals for free. T2SpellHooks::CountsAsHealingInShadowform
        // anchors on the cancel instead, which is the same key press as the heal.
        return T2SpellHooks::CountsAsHealingInShadowform(priest);
    }

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        // The carrier's effect is a DUMMY with no trigger spell; the core's
        // default action would only log "non-existent spell 0".
        PreventDefaultAction();

        Unit* target = GetTarget();
        Player* priest = target ? target->ToPlayer() : nullptr;
        HealInfo const* healInfo = eventInfo.GetHealInfo();
        if (!priest || !healInfo)
            return;

        int32 const price = CalculatePct(int32(healInfo->GetEffectiveHeal()), 50);

        SpellInfo const* healSpell = eventInfo.GetSpellInfo();
        char const* healName = healSpell ? healSpell->SpellName[sWorld->GetDefaultDbcLocale()] : nullptr;
        SendCustomAuraDiag(Trinity::StringFormat(
            "[CustomAuras] {}: Umbral Mercy - {} healed {} for {} effective, price {}",
            priest->GetName(), healName ? healName : "?",
            healInfo->GetTarget() ? healInfo->GetTarget()->GetName() : std::string("?"),
            healInfo->GetEffectiveHeal(), price));

        if (price <= 0)
            return;

        {
            std::lock_guard<std::mutex> guard(s_workMutex);
            s_pendingUmbralPrice[priest->GetGUID()] += price;
        }
        MarkWork(priest->GetGUID());
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_t2_umbral_mercy_passive::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_t2_umbral_mercy_passive::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// 90341 - Wraithblade (shadow 8pc): the +300% melee damage is effect 2 and
// needs no code. This is the mana half - 10% of what the swing actually dealt,
// paid through the ENERGIZE helper 90500 so it is a real energize event in the
// combat log (and on the floating combat text), not a silent ModifyPower.
class spell_t2_wraithblade : public AuraScript
{
    PrepareAuraScript(spell_t2_wraithblade);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        DamageInfo* damageInfo = eventInfo.GetDamageInfo();
        return damageInfo && damageInfo->GetDamage() > 0;
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        // Effect 0 is a DUMMY with no trigger spell: the core's default action
        // would only log "non-existent spell 0" on every swing.
        PreventDefaultAction();

        DamageInfo* damageInfo = eventInfo.GetDamageInfo();
        Unit* priest = GetTarget();
        if (!damageInfo || !priest)
            return;

        int32 const mana = CalculatePct(int32(damageInfo->GetDamage()), 10);
        if (mana <= 0)
            return;

        // 90500 has DieSides 0 on its energize effect, so the override is the
        // exact amount. Triggered by this aura: free, instant, no global.
        // Until the server's Spell.dbc carries 90500 (CastSpell of an unknown id
        // is a silent no-op) the energize is logged under the carrier's own id
        // instead - same SMSG_SPELLENERGIZELOG, the client already knows 90341.
        if (sSpellMgr->GetSpellInfo(SPELL_T2_WRAITHBLADE_ENERGIZE))
        {
            CastSpellExtraArgs args(aurEff);
            args.AddSpellBP0(mana);
            priest->CastSpell(priest, SPELL_T2_WRAITHBLADE_ENERGIZE, args);
        }
        else
            priest->EnergizeBySpell(priest, GetSpellInfo(), mana, POWER_MANA);

        SendCustomAuraDiag(Trinity::StringFormat(
            "[CustomAuras] {}: Wraithblade - {} melee damage -> {} mana",
            priest->GetName(), damageInfo->GetDamage(), mana));
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_t2_wraithblade::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_t2_wraithblade::HandleProc, EFFECT_0, SPELL_AURA_ANY);
    }
};

// 120 and up - Cone of Cold, carrying the dusty 3pc: 3 sec off Ice Block per
// enemy struck. OnHit runs once per target that was actually hit, so the
// per-enemy accumulation is free - misses and immunes never reach here.
class spell_t2_dusty_glacial_reprieve : public SpellScript
{
    PrepareSpellScript(spell_t2_dusty_glacial_reprieve);

    void HandleHit()
    {
        Unit* caster = GetCaster();
        if (!caster || !GetHitUnit())
            return;
        Player* mage = caster->ToPlayer();
        if (!mage || !mage->HasAura(SPELL_T2_GLACIAL_REPRIEVE))
            return;

        // ModifyCooldown is a no-op when Ice Block is not on cooldown, and it
        // walks the category cooldown down with the spell cooldown, so there
        // is nothing to guard here.
        mage->GetSpellHistory()->ModifyCooldown(SPELL_MAGE_ICE_BLOCK, -3 * IN_MILLISECONDS);
    }

    void Register() override
    {
        OnHit += SpellHitFn(spell_t2_dusty_glacial_reprieve::HandleHit);
    }
};

// 45438 - Ice Block, carrying the dusty 5pc: riding it out to the end freezes
// everything within 10 yards. Cancelling it early does not.
//
// This is a second script on 45438; the shipped spell_mage_ice_block (which
// triggers Hypothermia) is a SpellScript on the same id and must survive.
//
// The freeze is NOT cast from in here. Ice Block's expiry is processed inside
// Unit::_UpdateSpells' owned-aura walk on the mage, and at the moment effect 0
// (the stun) is unapplied, effects 1 and 2 (the immunities) are still on the
// mage. The hook only flags the mage; t2_priest_mage_update casts 90385 on the
// next Player::Update tick, with the block fully gone and no aura container in
// flight.
class spell_t2_dusty_hoarfrost : public AuraScript
{
    PrepareAuraScript(spell_t2_dusty_hoarfrost);

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* target = GetTarget();
        Player* mage = target ? target->ToPlayer() : nullptr;
        if (!mage)
            return;

        AuraApplication const* application = GetTargetApplication();
        AuraRemoveMode const mode = application ? application->GetRemoveMode() : AURA_REMOVE_NONE;

        if (!mage->HasAura(SPELL_T2_HOARFROST_BLOOM))
        {
            // Said out loud for the GM diag channel only: "Hoarfrost does
            // nothing" has looked exactly like this when the 5pc carrier was
            // never granted (the set-bonus delivery, not this script).
            SendCustomAuraDiag(Trinity::StringFormat(
                "[CustomAuras] {}: Ice Block ended (remove mode {}) without the Hoarfrost Bloom aura (90343) - no freeze",
                mage->GetName(), uint32(mode)));
            return;
        }
        // EXPIRE only: BY_CANCEL is the player clicking it off (or pressing
        // Ice Block again - the client sends a cancel for that too), BY_DEATH
        // is the block failing to save them, and neither should pay out.
        if (mode != AURA_REMOVE_BY_EXPIRE)
        {
            SendCustomAuraDiag(Trinity::StringFormat(
                "[CustomAuras] {}: Hoarfrost Bloom - Ice Block ended early (remove mode {}), no freeze",
                mage->GetName(), uint32(mode)));
            return;
        }

        {
            std::lock_guard<std::mutex> guard(s_workMutex);
            s_pendingHoarfrost.insert(mage->GetGUID());
        }
        MarkWork(mage->GetGUID());

        // Half the chain lives in t2_priest_mage_update; without this line the
        // aura hook and the deferred cast are indistinguishable when nothing
        // freezes.
        SendCustomAuraDiag(Trinity::StringFormat(
            "[CustomAuras] {}: Hoarfrost Bloom - Ice Block expired, freeze armed for the next tick",
            mage->GetName()));
    }

    void Register() override
    {
        // Effect 0 is the self stun; it is unapplied first and exactly once
        // per removal, so this fires once per block.
        AfterEffectRemove += AuraEffectRemoveFn(spell_t2_dusty_hoarfrost::HandleRemove, EFFECT_0, SPELL_AURA_MOD_STUN, AURA_EFFECT_HANDLE_REAL);
    }
};

// 90344 Reflective Carapace (dusty 8pc) lives in core: T2SpellHooks::
// OnImmuneSpellHit / OnImmuneMeleeHit (src/server/game/Entities/Object/
// T2SpellHooks.cpp), called from the immune branches the would-be damage
// never reaches a script from. Nothing to bind here.

// 31643 - Blazing Speed, carrying the fiery payback 5pc: the buff going up
// also resets Fire Blast. Bound to the triggered buff, not to the talent
// 31641/31642, because only the buff marks the moment it actually fired.
class spell_t2_fp_scorching_momentum : public AuraScript
{
    PrepareAuraScript(spell_t2_fp_scorching_momentum);

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* target = GetTarget();
        Player* mage = target ? target->ToPlayer() : nullptr;
        if (!mage || !mage->HasAura(SPELL_T2_SCORCHING_MOMENTUM))
            return;

        // Predicate rather than a rank list: Fire Blast is eleven ranks and
        // the mage only ever has the one they cast on cooldown. ResetCooldown
        // erases the category entry alongside the spell entry, so the shared
        // category 19 lockout goes with it.
        uint32 cleared = 0;
        mage->GetSpellHistory()->ResetCooldowns([&cleared](SpellHistory::CooldownStorageType::iterator itr)
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo(itr->first);
            if (!info || info->SpellFamilyName != SPELLFAMILY_MAGE)
                return false;
            if (!(info->SpellFamilyFlags[0] & MAGE_FIRE_BLAST_FAMILY_MASK0))
                return false;
            ++cleared;
            return true;
        }, true);

        if (cleared)
            SendCustomAuraDiag(Trinity::StringFormat(
                "[CustomAuras] {}: Scorching Momentum - Blazing Speed reset Fire Blast",
                mage->GetName()));
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_t2_fp_scorching_momentum::HandleApply, EFFECT_FIRST_FOUND, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL);
    }
};

// 90347 - Ashen Confiscation (fiery payback 8pc). Two jobs in one script,
// because no tier of this set grants a disarm: the proc IS the disarm (the
// spell_proc row narrows it to Fire Blast), and a disarm that actually lands
// is immediately followed by the seizure.
//
// The seizure is LITERAL since 2026-08-19: the victim's main hand (and ranged,
// both are what 64346 disarms) are created as temporary items, registered
// with T2UnitHooks so the core waives every requirement on them and reports
// 300 weapon skill, and equipped in the mage's own slots; the mage's weapons
// wait in the bags. The equip itself runs on the next Player::Update tick
// (PerformConfiscationEquip), never inside this proc.
//
// Scripting 64346 itself was the alternative and was rejected: it would run
// inside the VICTIM's aura machinery to hang a buff on the mage, and this set
// is the only disarm a mage without the Fiery Payback talent has anyway.
class spell_t2_ashen_confiscation : public AuraScript
{
    PrepareAuraScript(spell_t2_ashen_confiscation);

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        // Effect 0 is a DUMMY with no trigger spell: the core's default action
        // would only log "non-existent spell 0" on every Fire Blast.
        PreventDefaultAction();

        Unit* victim = eventInfo.GetProcTarget();
        Unit* target = GetTarget();
        Player* mage = target ? target->ToPlayer() : nullptr;
        if (!mage || !victim)
            return;

        // First rung of the chain, said out loud: if this never appears in the
        // diag channel during a live test the 8pc aura was never granted (or
        // its spell_proc row is missing) and everything below is moot.
        SendCustomAuraDiag(Trinity::StringFormat(
            "[CustomAuras] {}: Ashen Confiscation - Fire Blast proc on {}, casting the disarm",
            mage->GetName(), victim->GetName()));

        mage->CastSpell(victim, SPELL_MAGE_FIERY_PAYBACK_DISARM, aurEff);

        // Mechanic immunity, a PvP trinket or a resist all eat the disarm, and
        // "when you disarm an enemy" means the disarm has to be on them.
        if (!victim->HasAura(SPELL_MAGE_FIERY_PAYBACK_DISARM, mage->GetGUID()))
        {
            SendCustomAuraDiag(Trinity::StringFormat(
                "[CustomAuras] {}: Ashen Confiscation - disarm did not land on {}",
                mage->GetName(), victim->GetName()));
            return;
        }

        // What the victim is holding. Players: the real items in the two
        // disarmed slots. Creatures: their virtual item ids (which are item
        // entries; ones without an item_template are skipped at equip time).
        uint32 mainHandEntry = 0;
        uint32 rangedEntry = 0;
        if (Player* robbed = victim->ToPlayer())
        {
            if (Item* item = robbed->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND))
                mainHandEntry = item->GetEntry();
            if (Item* item = robbed->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED))
                rangedEntry = item->GetEntry();
        }
        else
        {
            mainHandEntry = victim->GetVirtualItemId(0);
            rangedEntry = victim->GetVirtualItemId(2);
        }

        // The attack power and the disarm immunity are 90387's two effects;
        // the amount is computed in its own script, at apply.
        mage->CastSpell(mage, SPELL_T2_CONFISCATED_ARMS, true);

        bool queued = false;
        {
            std::lock_guard<std::mutex> guard(s_workMutex);
            ConfiscationState& state = s_confiscations[mage->GetGUID()];
            // A seizure already in hand (the proc's internal cooldown is
            // longer than the buff, so this is belt and braces): refresh the
            // buff only, never stack a second set of weapons.
            if (state.seized.empty() && !state.equipPending)
            {
                state.equipPending = true;
                state.pendingMainHandEntry = mainHandEntry;
                state.pendingRangedEntry = rangedEntry;
                queued = true;
            }
        }
        if (queued)
            MarkWork(mage->GetGUID());

        // This line used to claim a seizure unconditionally, including when
        // nothing was queued at all - so a mage whose previous confiscation
        // never got torn down saw "seizing ..." for ever while no weapon ever
        // changed hands. Say which of the three actually happened.
        if (!queued)
            SendCustomAuraDiag(Trinity::StringFormat(
                "[CustomAuras] {}: Ashen Confiscation - disarmed {} but a seizure is still in hand; only the buff was refreshed, no weapons taken",
                mage->GetName(), victim->GetName()));
        else if (!mainHandEntry && !rangedEntry)
            SendCustomAuraDiag(Trinity::StringFormat(
                "[CustomAuras] {}: Ashen Confiscation - disarmed {}, but it has nothing in its main hand or ranged slot; nothing to seize",
                mage->GetName(), victim->GetName()));
        else
            SendCustomAuraDiag(Trinity::StringFormat(
                "[CustomAuras] {}: Ashen Confiscation - disarmed {}; seizing main hand {} / ranged {}",
                mage->GetName(), victim->GetName(), mainHandEntry, rangedEntry));
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_t2_ashen_confiscation::HandleProc, EFFECT_0, SPELL_AURA_ANY);
    }
};

// 90387 - Confiscated Arms, the buff 90347 hangs on the mage. Ships with a
// zero attack power amount on purpose; the real number is the mage's own spell
// power, which only exists at runtime. Its removal - expiry, death, dispel,
// anything - is what hands the weapons back.
class spell_t2_confiscated_arms : public AuraScript
{
    PrepareAuraScript(spell_t2_confiscated_arms);

    void CalcAttackPower(AuraEffect const* /*aurEff*/, int32& amount, bool& canBeRecalculated)
    {
        // Resolved once, at apply, and pinned: SpellBaseDamageBonusDone folds
        // attack power back into spell power for anything carrying
        // MOD_SPELL_DAMAGE_OF_ATTACK_POWER, and a recalculating amount would
        // chase its own tail on such a target.
        canBeRecalculated = false;
        amount = 0;

        // Fire, not the whole magic mask: this is the fiery payback set, and
        // all-school spell power (misc 126) is counted by a fire query anyway,
        // while a magic-wide query would also sweep in +frost / +arcane gear.
        if (Unit* caster = GetCaster())
            amount = std::max(0, caster->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_FIRE));
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* target = GetTarget();
        Player* mage = target ? target->ToPlayer() : nullptr;
        if (!mage)
            return;

        // Flag only. Destroying and re-equipping items applies and removes
        // item auras on the mage, and this hook can be running inside
        // RemoveAllAurasOnDeath's walk of exactly those containers.
        bool hasState = false;
        {
            std::lock_guard<std::mutex> guard(s_workMutex);
            auto itr = s_confiscations.find(mage->GetGUID());
            if (itr != s_confiscations.end())
            {
                itr->second.cleanupPending = true;
                itr->second.equipPending = false;   // too late to wield anything
                hasState = true;
            }
        }
        if (hasState)
            MarkWork(mage->GetGUID());
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_t2_confiscated_arms::CalcAttackPower, EFFECT_0, SPELL_AURA_MOD_ATTACK_POWER);
        AfterEffectRemove += AuraEffectRemoveFn(spell_t2_confiscated_arms::HandleRemove, EFFECT_0, SPELL_AURA_MOD_ATTACK_POWER, AURA_EFFECT_HANDLE_REAL);
    }
};

// The deferred-work runner plus the persistence guards for the confiscated
// weapons. OnUpdate is the "next tick" every hook above defers to; OnSave runs
// at the top of Player::SaveToDB, BEFORE _SaveInventory, so a temporary weapon
// can never reach item_instance/character_inventory - an autosave, a far
// teleport save or the logout save simply ends the confiscation first. OnLogout
// and OnMapChanged are the belt to that brace.
class t2_priest_mage_update : public PlayerScript
{
public:
    t2_priest_mage_update() : PlayerScript("t2_priest_mage_update") { }

    void OnUpdate(Player* player, uint32 /*diff*/) override
    {
        // Umbral Mercy's swallowed Shadowform cancel. Not gated on TakeWork: the
        // mark lives in core (T2SpellHooks), which has its own atomic fast path,
        // and this is the tick that guarantees a deliberate toggle always comes
        // off. Player::Update calls this hook AFTER Unit::Update has returned, so
        // removing the aura here is safe - see the comment above the call site in
        // Player::Update.
        T2SpellHooks::ResolvePendingShadowformCancel(player);

        if (!player || !TakeWork(player->GetGUID()))
            return;

        ObjectGuid const guid = player->GetGUID();

        // Hoarfrost Bloom - the freeze, one tick after Ice Block ran out.
        bool hoarfrost = false;
        bool equip = false;
        bool cleanup = false;
        int32 umbralPrice = 0;
        uint32 mainHandEntry = 0;
        uint32 rangedEntry = 0;
        {
            std::lock_guard<std::mutex> guard(s_workMutex);
            hoarfrost = s_pendingHoarfrost.erase(guid) > 0;
            auto priceItr = s_pendingUmbralPrice.find(guid);
            if (priceItr != s_pendingUmbralPrice.end())
            {
                umbralPrice = priceItr->second;
                s_pendingUmbralPrice.erase(priceItr);
            }
            auto itr = s_confiscations.find(guid);
            if (itr != s_confiscations.end())
            {
                cleanup = itr->second.cleanupPending;
                equip = itr->second.equipPending && !cleanup;
                mainHandEntry = itr->second.pendingMainHandEntry;
                rangedEntry = itr->second.pendingRangedEntry;
                itr->second.cleanupPending = false;
                itr->second.equipPending = false;
                itr->second.pendingMainHandEntry = 0;
                itr->second.pendingRangedEntry = 0;
            }
        }

        if (hoarfrost && player->IsInWorld() && player->IsAlive() && player->HasAura(SPELL_T2_HOARFROST_BLOOM))
        {
            // 90385 selects TARGET_UNIT_SRC_AREA_ENEMY (radius index 13 = 10
            // yd) around the mage, so this adds auras to nearby enemies and
            // never to the mage. Effect 0 is a 6 s MOD_STUN with mechanic
            // FREEZE and AuraInterruptFlags TAKE_DAMAGE, so damage breaks it.
            if (!sSpellMgr->GetSpellInfo(SPELL_T2_ENCASED_IN_ICE))
                SendCustomAuraDiag(Trinity::StringFormat(
                    "[CustomAuras] {}: Hoarfrost Bloom - helper spell {} is not in the loaded Spell.dbc, no freeze",
                    player->GetName(), uint32(SPELL_T2_ENCASED_IN_ICE)));
            else
            {
                // The cast RESULT is the only thing that separates "froze the
                // room" from "nothing hostile was standing inside 10 yd": an
                // area spell that selects no target fails with
                // SPELL_FAILED_NO_VALID_TARGETS (26) and is otherwise entirely
                // silent. Announcing success unconditionally is what made a
                // working freeze indistinguishable from a dead one.
                SpellCastResult const result = player->CastSpell(player, SPELL_T2_ENCASED_IN_ICE, true);
                if (result == SPELL_CAST_OK)
                    SendCustomAuraDiag(Trinity::StringFormat(
                        "[CustomAuras] {}: Hoarfrost Bloom - Ice Block ran out, freezing 10 yd",
                        player->GetName()));
                else
                    SendCustomAuraDiag(Trinity::StringFormat(
                        "[CustomAuras] {}: Hoarfrost Bloom - Ice Block ran out but the freeze ({}) did not go off, cast result {} (26 = no valid target, i.e. nothing hostile within 10 yd)",
                        player->GetName(), uint32(SPELL_T2_ENCASED_IN_ICE), uint32(result)));
            }
        }
        else if (hoarfrost)
        {
            // The aura hook armed the freeze and this tick threw it away. Was
            // dark; it is the only thing that distinguishes "the Ice Block
            // hook never ran" from "the deferred tick dropped it".
            SendCustomAuraDiag(Trinity::StringFormat(
                "[CustomAuras] {}: Hoarfrost Bloom - freeze was queued but dropped on the next tick ({})",
                player->GetName(),
                !player->IsInWorld() ? "no longer in world"
                    : !player->IsAlive() ? "mage is dead"
                    : "the Hoarfrost Bloom aura (90343) is gone"));
        }

        // Umbral Mercy - the Renew ticks' price, one tick after the heal.
        if (umbralPrice > 0 && player->IsInWorld() && player->IsAlive() && player->HasAura(SPELL_T2_UMBRAL_MERCY))
        {
            uint32 const before = player->GetHealth();
            DealUmbralMercyPrice(player, umbralPrice);
            SendCustomAuraDiag(Trinity::StringFormat(
                "[CustomAuras] {}: Umbral Mercy - healing done in Shadowform cost {} health ({} -> {})",
                player->GetName(), umbralPrice, before, player->GetHealth()));
        }

        if (cleanup)
            CleanupConfiscation(player, "buff ended");
        else if (equip)
            PerformConfiscationEquip(player, mainHandEntry, rangedEntry);
    }

    void OnSave(Player* player) override
    {
        if (!player)
            return;
        // Cheap pre-check so the common save costs one lock at most.
        {
            std::lock_guard<std::mutex> guard(s_workMutex);
            if (s_confiscations.find(player->GetGUID()) == s_confiscations.end())
                return;
        }
        // Weapons first (so the save never sees them), then the buff: its
        // removal flags a cleanup that will find nothing left to do.
        CleanupConfiscation(player, "save");
        player->RemoveAurasDueToSpell(SPELL_T2_CONFISCATED_ARMS);
    }

    void OnMapChanged(Player* player) override
    {
        if (!player)
            return;
        {
            std::lock_guard<std::mutex> guard(s_workMutex);
            if (s_confiscations.find(player->GetGUID()) == s_confiscations.end())
                return;
        }
        CleanupConfiscation(player, "map change");
        player->RemoveAurasDueToSpell(SPELL_T2_CONFISCATED_ARMS);
    }

    void OnLogout(Player* player) override
    {
        if (!player)
            return;
        ObjectGuid const guid = player->GetGUID();
        CleanupConfiscation(player, "logout");
        // Umbral Mercy's marks live in core; drop them with the session so a
        // pending cancel can never come back attached to the same GUID.
        T2SpellHooks::ForgetPlayer(player);
        std::lock_guard<std::mutex> guard(s_workMutex);
        s_pendingUmbralPrice.erase(guid);
        s_pendingHoarfrost.erase(guid);
        s_confiscations.erase(guid);
        if (s_playersWithWork.erase(guid))
            s_workCount.store(uint32(s_playersWithWork.size()), std::memory_order_release);
    }
};

void AddSC_custom_t2_priest_mage()
{
    RegisterSpellScript(spell_t2_holy_censer);
    RegisterSpellScript(spell_t2_umbral_mercy_passive);
    RegisterSpellScript(spell_t2_wraithblade);
    RegisterSpellScript(spell_t2_dusty_glacial_reprieve);
    RegisterSpellScript(spell_t2_dusty_hoarfrost);
    RegisterSpellScript(spell_t2_fp_scorching_momentum);
    RegisterSpellScript(spell_t2_ashen_confiscation);
    RegisterSpellScript(spell_t2_confiscated_arms);
    new t2_priest_mage_update();
}
