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

#include "AccountBankMgr.h"

#include "Chat.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "Item.h"
#include "Bag.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Optional.h"
#include "Player.h"
#include "SharedDefines.h"
#include "WorldSession.h"
#include <bitset>
#include <unordered_map>
#include <string>
#include <string_view>
#include <vector>

namespace AccountBank
{
namespace
{
    void CleanupRemovedItem(Player* player, Item* item)
    {
        if (!player || !item)
            return;

        RemoveItemFromUpdateQueueOf(item, player);
        if (item->IsInWorld())
        {
            item->RemoveFromWorld();
            item->DestroyForPlayer(player);
        }
    }

    struct StoredBankItem
    {
        uint8 Bag = 0;
        uint8 Slot = 0;
        Item* ItemData = nullptr;
    };

    struct SessionState
    {
        ObjectGuid BankerGuid;
        std::vector<StoredBankItem> StoredItems;
    };

    using SessionMap = std::unordered_map<ObjectGuid::LowType, SessionState>;
    SessionMap AccountSessions;

    SessionState* GetSession(Player* player)
    {
        if (!player)
            return nullptr;

        auto const itr = AccountSessions.find(player->GetGUID().GetCounter());
        if (itr == AccountSessions.end())
            return nullptr;

        return &itr->second;
    }

    SessionState const* GetSession(Player const* player)
    {
        if (!player)
            return nullptr;

        auto const itr = AccountSessions.find(player->GetGUID().GetCounter());
        if (itr == AccountSessions.end())
            return nullptr;

        return &itr->second;
    }

    Optional<std::string> ValidateDepositableItem(Item const* item)
    {
        if (!item)
            return std::string("No item found.");

        if (item->IsNotEmptyBag())
            return std::string("Bags must be empty before depositing them.");

        if (item->IsSoulBound() && !item->IsBoundAccountWide())
            return std::string("Only account-bound or unbound items can be deposited.");

        ItemTemplate const* proto = item->GetTemplate();
        if (!proto)
            return std::string("Item template data is missing.");

        if (proto->HasFlag(ITEM_FLAG_CONJURED) || item->GetUInt32Value(ITEM_FIELD_DURATION))
            return std::string("Conjured or temporary items cannot be stored in the account bank.");

        return Optional<std::string>();
    }

    uint8 ToPlayerBankSlot(uint16 slot)
    {
        return BANK_SLOT_ITEM_START + slot;
    }

    uint16 ToItemPos(uint8 slot)
    {
        return (INVENTORY_SLOT_BAG_0 << 8) | slot;
    }

    std::bitset<MAX_SLOTS> GetUsedSlots(uint32 accountId)
    {
        std::bitset<MAX_SLOTS> used;
        CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_ACCOUNT_BANK_ITEMS);
        stmt->setUInt32(0, accountId);

        if (PreparedQueryResult result = CharacterDatabase.Query(stmt))
        {
            do
            {
                uint16 slot = (*result)[0].GetUInt16();
                if (slot < MAX_SLOTS)
                    used.set(slot);
            }
            while (result->NextRow());
        }

        return used;
    }

    Optional<uint16> GetFreeSlot(uint32 accountId)
    {
        std::bitset<MAX_SLOTS> used = GetUsedSlots(accountId);
        for (uint16 i = 0; i < MAX_SLOTS; ++i)
            if (!used.test(i))
                return i;

        return Optional<uint16>();
    }

    void RemoveStoredItem(uint32 accountId, uint16 slot, CharacterDatabaseTransaction const& trans)
    {
        CharacterDatabasePreparedStatement* deleteStmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_ACCOUNT_BANK_ITEM);
        deleteStmt->setUInt32(0, accountId);
        deleteStmt->setUInt16(1, slot);
        trans->Append(deleteStmt);
    }

    void StoreCharacterBankItems(Player* player, SessionState& session)
    {
        if (!player)
            return;

        session.StoredItems.clear();

        auto const storeItem = [&](uint8 bag, uint8 slot)
        {
            if (Item* item = player->GetItemByPos(bag, slot))
            {
                player->RemoveItem(bag, slot, true);
                CleanupRemovedItem(player, item);
                StoredBankItem stored;
                stored.Bag = bag;
                stored.Slot = slot;
                stored.ItemData = item;
                session.StoredItems.push_back(stored);
            }
        };

        for (uint8 slot = BANK_SLOT_ITEM_START; slot < BANK_SLOT_ITEM_END; ++slot)
            storeItem(INVENTORY_SLOT_BAG_0, slot);

        for (uint8 slot = BANK_SLOT_BAG_START; slot < BANK_SLOT_BAG_END; ++slot)
            storeItem(INVENTORY_SLOT_BAG_0, slot);
    }

    void RestoreCharacterBankItems(Player* player, SessionState& session)
    {
        if (!player)
            return;

        for (StoredBankItem& stored : session.StoredItems)
        {
            if (!stored.ItemData)
                continue;

            ItemPosCountVec dest;
            dest.emplace_back(ItemPosCount((stored.Bag << 8) | stored.Slot, stored.ItemData->GetCount()));
            player->StoreItem(dest, stored.ItemData, true);
        }

        session.StoredItems.clear();
    }

    bool LoadAccountBankView(Player* player)
    {
        if (!player)
            return false;

        WorldSession* session = player->GetSession();
        if (!session)
            return false;

        CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_ACCOUNT_BANK_ITEM_FULL);
        stmt->setUInt32(0, session->GetAccountId());

        PreparedQueryResult result = CharacterDatabase.Query(stmt);
        if (!result)
            return true;

        std::vector<Item*> insertedItems;

        do
        {
            Field* fields = result->Fetch();
            uint16 slot = fields[0].GetUInt16();
            if (slot >= MAX_SLOTS)
            {
                CharacterDatabasePreparedStatement* deleteStmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_ACCOUNT_BANK_ITEM);
                deleteStmt->setUInt32(0, session->GetAccountId());
                deleteStmt->setUInt16(1, slot);
                CharacterDatabase.Execute(deleteStmt);
                continue;
            }

            Field* itemFields = fields + 1;
            uint32 itemEntry = itemFields[11].GetUInt32();
            ObjectGuid::LowType itemGuid = itemFields[12].GetUInt32();
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemEntry);
            if (!proto)
            {
                CharacterDatabasePreparedStatement* deleteStmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_ACCOUNT_BANK_ITEM);
                deleteStmt->setUInt32(0, session->GetAccountId());
                deleteStmt->setUInt16(1, slot);
                CharacterDatabase.Execute(deleteStmt);
                continue;
            }

            Item* item = NewItemOrBag(proto);
            if (!item->LoadFromDB(itemGuid, ObjectGuid::Empty, itemFields, itemEntry))
            {
                CharacterDatabasePreparedStatement* deleteStmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_ACCOUNT_BANK_ITEM);
                deleteStmt->setUInt32(0, session->GetAccountId());
                deleteStmt->setUInt16(1, slot);
                CharacterDatabase.Execute(deleteStmt);
                delete item;
                continue;
            }

            item->SetOwnerGUID(player->GetGUID());

            ItemPosCountVec dest;
            dest.emplace_back(ItemPosCount(ToItemPos(ToPlayerBankSlot(slot)), item->GetCount()));
            Item* storedItem = player->StoreItem(dest, item, true);
            if (!storedItem)
            {
                delete item;
                for (Item* inserted : insertedItems)
                {
                    if (!inserted)
                        continue;

                    player->RemoveItem(inserted->GetBagSlot(), inserted->GetSlot(), true);
                    CleanupRemovedItem(player, inserted);
                    delete inserted;
                }
                return false;
            }

            insertedItems.push_back(storedItem);
        }
        while (result->NextRow());

        return true;
    }

    void SaveAccountBankView(Player* player)
    {
        if (!player)
            return;

        WorldSession* session = player->GetSession();
        if (!session)
            return;

        uint32 const accountId = session->GetAccountId();
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

        CharacterDatabasePreparedStatement* clearStmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_ACCOUNT_BANK_ITEMS_BY_ACCOUNT);
        clearStmt->setUInt32(0, accountId);
        trans->Append(clearStmt);

        for (uint16 slot = 0; slot < MAX_SLOTS; ++slot)
        {
            uint8 const playerSlot = ToPlayerBankSlot(slot);
            Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, playerSlot);
            if (!item)
                continue;

            player->RemoveItem(INVENTORY_SLOT_BAG_0, playerSlot, true);
            CleanupRemovedItem(player, item);
            item->DeleteFromInventoryDB(trans);
            item->SetGuidValue(ITEM_FIELD_CONTAINED, ObjectGuid::Empty);
            item->SetGuidValue(ITEM_FIELD_OWNER, ObjectGuid::Empty);
            item->SetNotRefundable(player);
            item->ClearSoulboundTradeable(player);
            item->FSetState(ITEM_NEW);
            item->SaveToDB(trans);

            CharacterDatabasePreparedStatement* insertStmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_ACCOUNT_BANK_ITEM);
            insertStmt->setUInt32(0, accountId);
            insertStmt->setUInt16(1, slot);
            insertStmt->setUInt32(2, item->GetGUID().GetCounter());
            trans->Append(insertStmt);

            delete item;
        }

        CharacterDatabase.CommitTransaction(trans);
    }

    bool EnsureCommandAllowed(Player const* player, ChatHandler* handler)
    {
        if (player && IsAccountBankOpen(player))
        {
            if (handler)
                handler->PSendSysMessage("Close the account bank NPC before using account bank chat commands.");
            return false;
        }

        return true;
    }
}

bool IsDepositable(Item const* item)
{
    return !ValidateDepositableItem(item).has_value();
}

bool List(ChatHandler* handler)
{
    if (!handler)
        return false;

    WorldSession* session = handler->GetSession();
    if (!session)
        return false;

    if (!EnsureCommandAllowed(session->GetPlayer(), handler))
        return false;

    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_ACCOUNT_BANK_ITEMS);
    stmt->setUInt32(0, session->GetAccountId());

    PreparedQueryResult result = CharacterDatabase.Query(stmt);
    if (!result)
    {
        handler->PSendSysMessage("Account bank is empty (0/%u slots used).", MAX_SLOTS);
        return true;
    }

    handler->PSendSysMessage("Account bank contents:");
    uint32 used = 0;
    do
    {
        Field* fields = result->Fetch();
        uint16 slot = fields[0].GetUInt16();
        uint32 entry = fields[2].GetUInt32();
        uint32 count = fields[3].GetUInt32();
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
        std::string_view name = proto ? std::string_view(proto->Name1) : std::string_view("Unknown item");
        handler->PSendSysMessage("  Slot %u: %s (Entry: %u, Count: %u)", slot, name.data(), entry, count);
        ++used;
    }
    while (result->NextRow());

    handler->PSendSysMessage("%u/%u slots used.", used, MAX_SLOTS);
    return true;
}

bool Deposit(ChatHandler* handler, Player* player, Item* item)
{
    if (!handler || !player)
        return false;

    if (!EnsureCommandAllowed(player, handler))
        return false;

    Optional<std::string> error = ValidateDepositableItem(item);
    if (error)
    {
        handler->PSendSysMessage("%s", error->c_str());
        return false;
    }

    WorldSession* session = handler->GetSession();
    if (!session)
        return false;

    Optional<uint16> freeSlot = GetFreeSlot(session->GetAccountId());
    if (!freeSlot)
    {
        handler->PSendSysMessage("Account bank is full (%u slots).", MAX_SLOTS);
        return false;
    }

    ItemTemplate const* proto = item->GetTemplate();
    if (!proto)
        return false;

    std::string itemName = proto->Name1;
    ObjectGuid::LowType itemGuid = item->GetGUID().GetCounter();

    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

    player->MoveItemFromInventory(item->GetBagSlot(), item->GetSlot(), true);
    item->DeleteFromInventoryDB(trans);

    item->SetGuidValue(ITEM_FIELD_CONTAINED, ObjectGuid::Empty);
    item->SetGuidValue(ITEM_FIELD_OWNER, ObjectGuid::Empty);
    item->SetNotRefundable(player);
    item->ClearSoulboundTradeable(player);
    item->FSetState(ITEM_NEW);
    item->SaveToDB(trans);

    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_ACCOUNT_BANK_ITEM);
    stmt->setUInt32(0, session->GetAccountId());
    stmt->setUInt16(1, *freeSlot);
    stmt->setUInt32(2, itemGuid);
    trans->Append(stmt);

    player->SaveInventoryAndGoldToDB(trans);
    CharacterDatabase.CommitTransaction(trans);

    handler->PSendSysMessage("Deposited %s into account bank slot %u.", itemName.c_str(), *freeSlot);
    delete item;
    return true;
}

bool Withdraw(ChatHandler* handler, Player* player, uint16 slot)
{
    if (!handler || !player)
        return false;

    if (!EnsureCommandAllowed(player, handler))
        return false;

    WorldSession* session = handler->GetSession();
    if (!session)
        return false;

    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_ACCOUNT_BANK_ITEM_DATA);
    stmt->setUInt32(0, session->GetAccountId());
    stmt->setUInt16(1, slot);

    PreparedQueryResult result = CharacterDatabase.Query(stmt);
    if (!result)
    {
        handler->PSendSysMessage("No item stored in slot %u.", slot);
        return true;
    }

    Field* fields = result->Fetch();
    uint32 itemEntry = fields[11].GetUInt32();
    ObjectGuid::LowType itemGuid = fields[12].GetUInt32();
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemEntry);
    if (!proto)
    {
        handler->PSendSysMessage("Item template %u is invalid, removing entry.", itemEntry);
        CharacterDatabasePreparedStatement* deleteStmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_ACCOUNT_BANK_ITEM);
        deleteStmt->setUInt32(0, session->GetAccountId());
        deleteStmt->setUInt16(1, slot);
        CharacterDatabase.Execute(deleteStmt);
        return true;
    }

    Item* item = NewItemOrBag(proto);
    if (!item->LoadFromDB(itemGuid, ObjectGuid::Empty, fields, itemEntry))
    {
        handler->PSendSysMessage("Stored item data is invalid, removing entry.");
        CharacterDatabasePreparedStatement* deleteStmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_ACCOUNT_BANK_ITEM);
        deleteStmt->setUInt32(0, session->GetAccountId());
        deleteStmt->setUInt16(1, slot);
        CharacterDatabase.Execute(deleteStmt);
        delete item;
        return true;
    }

    ItemPosCountVec dest;
    InventoryResult msg = player->CanStoreItem(NULL_BAG, NULL_SLOT, dest, item, false);
    if (msg != EQUIP_ERR_OK)
    {
        handler->PSendSysMessage("Not enough space in your inventory (error %u).", uint32(msg));
        delete item;
        return false;
    }

    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    RemoveStoredItem(session->GetAccountId(), slot, trans);

    item->SetOwnerGUID(player->GetGUID());
    player->MoveItemToInventory(dest, item, true);
    player->SaveInventoryAndGoldToDB(trans);
    CharacterDatabase.CommitTransaction(trans);

    handler->PSendSysMessage("Withdrew %s from account bank slot %u.", proto->Name1.c_str(), slot);
    return true;
}

PreparedQueryResult QueryAccountBankItems(uint32 accountId)
{
    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_ACCOUNT_BANK_ITEMS);
    stmt->setUInt32(0, accountId);
    return CharacterDatabase.Query(stmt);
}

bool OpenAccountBank(Player* player, ObjectGuid bankerGuid)
{
    if (!player)
        return false;

    if (SessionState* existing = GetSession(player))
    {
        if (existing->BankerGuid == bankerGuid)
            return true;

        CloseAccountBank(player);
    }

    if (!player->IsInWorld())
        return false;

    SessionState& session = AccountSessions[player->GetGUID().GetCounter()];
    session.BankerGuid = bankerGuid;
    StoreCharacterBankItems(player, session);

    if (!LoadAccountBankView(player))
    {
        RestoreCharacterBankItems(player, session);
        AccountSessions.erase(player->GetGUID().GetCounter());
        return false;
    }

    return true;
}

void CloseAccountBank(Player* player)
{
    if (!player)
        return;

    SessionState* session = GetSession(player);
    if (!session)
        return;

    SaveAccountBankView(player);
    RestoreCharacterBankItems(player, *session);
    AccountSessions.erase(player->GetGUID().GetCounter());
}

void UpdateAccountBankSession(Player* player)
{
    if (!player)
        return;

    SessionState* session = GetSession(player);
    if (!session)
        return;

    if (!player->IsInWorld())
    {
        CloseAccountBank(player);
        return;
    }

    Creature* banker = ObjectAccessor::GetCreature(*player, session->BankerGuid);
    if (!banker || !player->IsWithinDistInMap(banker, INTERACTION_DISTANCE, false))
        CloseAccountBank(player);
}

bool IsAccountBankOpen(Player const* player)
{
    return GetSession(player) != nullptr;
}

bool IsAccountBankAccessible(Player const* player)
{
    if (!player || !player->IsInWorld())
        return false;

    SessionState const* session = GetSession(player);
    if (!session)
        return false;

    Creature* banker = ObjectAccessor::GetCreature(*player, session->BankerGuid);
    return banker && player->IsWithinDistInMap(banker, INTERACTION_DISTANCE, false);
}

bool IsAccountBanker(Player const* player, ObjectGuid bankerGuid)
{
    if (!player)
        return false;

    SessionState const* session = GetSession(player);
    if (!session)
        return false;

    return session->BankerGuid == bankerGuid;
}

void UpdateAccountBankSessions()
{
    if (AccountSessions.empty())
        return;

    std::vector<ObjectGuid::LowType> sessionOwners;
    sessionOwners.reserve(AccountSessions.size());
    for (auto const& session : AccountSessions)
        sessionOwners.push_back(session.first);

    for (ObjectGuid::LowType guidLow : sessionOwners)
    {
        ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(guidLow);
        if (Player* player = ObjectAccessor::FindPlayer(guid))
            UpdateAccountBankSession(player);
    }
}

void HandleLogin(Player* player)
{
    if (!player)
        return;

    SessionState* session = GetSession(player);
    if (!session)
        return;

    SaveAccountBankView(player);
    RestoreCharacterBankItems(player, *session);
    AccountSessions.erase(player->GetGUID().GetCounter());
}
}
