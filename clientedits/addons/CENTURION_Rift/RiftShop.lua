-- Summoner's Rift shop: League's build paths, shown the way League shows them.
--
-- The server prices a purchase League's way (RiftProgression::ShopPrice): an item
-- costs its price less every component the character already holds, all the way
-- down the tree, and those components go into it. The vendor window only knows
-- the full price, so this puts the real one on each button, and every shop
-- item's tooltip says what it builds from, what it builds into and what it costs
-- you. The build paths are RiftItems.lua, generated with the items by
-- import/rift/scripts/rift_shop.py.
local ITEMS = CENTURION_RIFT_ITEMS or {};
local COPPER = 10000;

local intoOf = {};
for id, item in pairs(ITEMS) do
	for _, part in ipairs(item.from) do
		intoOf[part] = intoOf[part] or {};
		table.insert(intoOf[part], id);
	end
end

local function ItemId(link)
	return link and tonumber(string.match(link, "item:(%d+)"));
end

-- What the character holds of each shop item, bags and equipped (the server
-- counts both).
local function Held()
	local held = {};
	for id in pairs(ITEMS) do
		local count = GetItemCount(id) or 0;
		if ( IsEquippedItem(id) ) then
			count = count + 1;
		end
		if ( count > 0 ) then
			held[id] = count;
		end
	end
	return held;
end

-- League gold: the item's price less its components', plus each component not
-- held, priced the same way. Held components leave the pool.
local function Cost(id, pool, depth)
	local item = ITEMS[id];
	if ( not item ) then
		return 0;
	end
	if ( #item.from == 0 or (depth or 0) > 4 ) then
		return item.gold;
	end
	local parts = 0;
	for _, part in ipairs(item.from) do
		parts = parts + (ITEMS[part] and ITEMS[part].gold or 0);
	end
	local cost = math.max(item.gold - parts, 0);
	for _, part in ipairs(item.from) do
		if ( (pool[part] or 0) > 0 ) then
			pool[part] = pool[part] - 1;
		else
			cost = cost + Cost(part, pool, (depth or 0) + 1);
		end
	end
	return cost;
end

local function YourCost(id)
	return Cost(id, Held());
end

local function NameOf(id)
	return (ITEMS[id] and ITEMS[id].name) or GetItemInfo(id) or ("item " .. id);
end

-- Tooltips: Builds from / into, and the price for you.
local function AddLines(tooltip)
	local _, link = tooltip:GetItem();
	local id = ItemId(link);
	local item = id and ITEMS[id];
	if ( not item ) then
		return;
	end
	tooltip:AddLine(" ");
	if ( #item.from > 0 ) then
		local names = {};
		local parts = 0;
		for _, part in ipairs(item.from) do
			table.insert(names, NameOf(part));
			parts = parts + (ITEMS[part] and ITEMS[part].gold or 0);
		end
		tooltip:AddLine("Builds from: " .. table.concat(names, " + ") .. string.format(" + %d gold", math.max(item.gold - parts, 0)), 1, 0.82, 0, true);
	end
	if ( intoOf[id] ) then
		local names = {};
		for _, into in ipairs(intoOf[id]) do
			table.insert(names, NameOf(into));
		end
		tooltip:AddLine("Builds into: " .. table.concat(names, ", "), 0.6, 0.8, 1, true);
	end
	local cost = YourCost(id);
	if ( cost < item.gold ) then
		tooltip:AddLine(string.format("Cost for you: %d gold (%d with nothing to build from)", cost, item.gold), 0.2, 1, 0.2, true);
	else
		tooltip:AddLine(string.format("Cost: %d gold", item.gold), 1, 1, 1, true);
	end
	tooltip:Show();
end

GameTooltip:HookScript("OnTooltipSetItem", AddLines);
if ( ItemRefTooltip ) then
	ItemRefTooltip:HookScript("OnTooltipSetItem", AddLines);
end

-- The vendor window: the price on each button is what this character pays.
local function UpdateMerchant()
	if ( not MerchantFrame or not MerchantFrame:IsShown() or MerchantFrame.selectedTab ~= 1 ) then
		return;
	end
	local perPage = MERCHANT_ITEMS_PER_PAGE or 10;
	local held = Held();
	for i = 1, perPage do
		local index = ((MerchantFrame.page or 1) - 1) * perPage + i;
		local id = ItemId(GetMerchantItemLink(index));
		local frameName = "MerchantItem" .. i .. "MoneyFrame";
		local money = _G[frameName];
		if ( id and ITEMS[id] and money ) then
			local pool = {};
			for k, v in pairs(held) do
				pool[k] = v;
			end
			local cost = Cost(id, pool) * COPPER;
			MoneyFrame_Update(frameName, cost);
			if ( SetMoneyFrameColor ) then
				pcall(SetMoneyFrameColor, frameName, GetMoney() < cost and "red" or "white");
			end
		end
	end
end

if ( MerchantFrame_UpdateMerchantInfo ) then
	hooksecurefunc("MerchantFrame_UpdateMerchantInfo", UpdateMerchant);
end
local watch = CreateFrame("Frame");
watch:RegisterEvent("BAG_UPDATE");
watch:RegisterEvent("PLAYER_EQUIPMENT_CHANGED");
watch:RegisterEvent("PLAYER_MONEY");
watch:SetScript("OnEvent", UpdateMerchant);
