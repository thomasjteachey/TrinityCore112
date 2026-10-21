-- Summoner's Rift: League's shop in place of the vendor window.
--
-- At a Rift shopkeeper the merchant window is shrunk out of sight (the merchant
-- session stays open: that is what buying and selling go through) and this
-- frame comes up instead, laid out as League's: categories on the left, the
-- items by tier in the middle, and on the right the selected item's build
-- tree - what it builds from, what you already hold of it, what it builds into
-- - with its price for you and a Purchase button. Right-click an item to buy
-- it at once; sell from the inventory (right-click an item there). The prices
-- are League's build-path prices (RiftShop.lua, the server's ShopPrice).

local SHOP_FIRST, SHOP_LAST = 991000, 991099;
local RIFT_ZONE = "Summoner's Rift";
local COPPER = 10000;
local ITEMS = CENTURION_RIFT_ITEMS or {};
local EDGE = { 0.78, 0.66, 0.43 };
local NAVY = { 0.02, 0.06, 0.10 };
local W, H = 800, 520;
local CAT_W, DETAIL_W = 120, 270;
local CELL, CELL_GAP = 40, 12;

local CATEGORIES = {
	{ key = nil, label = "All items" },
	{ key = "attack", label = "Attack" },
	{ key = "magic", label = "Magic" },
	{ key = "defense", label = "Defense" },
	{ key = "movement", label = "Movement" },
};
local TIERS = { { "basic", "Basic" }, { "epic", "Epic" }, { "legendary", "Legendary" } };

local function Shop() return CENTURION_RiftShop; end
local function YourCost(id) return Shop() and Shop().Cost(id) or (ITEMS[id] and ITEMS[id].gold or 0); end
local function Held() return Shop() and Shop().Held() or {}; end

local merchantIndex = {};      -- item id -> merchant slot
local selected = nil;
local category = nil;
local search = "";

-- The Rift's shopkeepers (item links may not be cached yet when the window opens).
local function IsRiftShop()
	if ( GetRealZoneText() ~= RIFT_ZONE ) then
		return false;
	end
	local npc = UnitName("npc") or "";
	return npc == "Order Shopkeeper" or npc == "Chaos Shopkeeper";
end

local idByName = {};
for id, item in pairs(ITEMS) do
	idByName[item.name] = id;
end

-- ------------------------------------------------------------------ frame

local shop = CreateFrame("Frame", "CENTURION_RiftShopFrame", UIParent);
shop:SetWidth(W);
shop:SetHeight(H);
shop:SetPoint("CENTER", UIParent, "CENTER", -80, 40);
shop:SetFrameStrata("HIGH");
shop:SetBackdrop({
	bgFile = "Interface\\Tooltips\\UI-Tooltip-Background",
	edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
	tile = true, tileSize = 16, edgeSize = 14,
	insets = { left = 3, right = 3, top = 3, bottom = 3 },
});
shop:SetBackdropColor(NAVY[1], NAVY[2], NAVY[3], 0.97);
shop:SetBackdropBorderColor(EDGE[1], EDGE[2], EDGE[3], 1);
shop:SetMovable(true);
shop:SetClampedToScreen(true);
shop:EnableMouse(true);
shop:RegisterForDrag("LeftButton");
shop:SetScript("OnDragStart", shop.StartMoving);
shop:SetScript("OnDragStop", shop.StopMovingOrSizing);
shop:Hide();
table.insert(UISpecialFrames, "CENTURION_RiftShopFrame");

local title = shop:CreateFontString(nil, "OVERLAY");
title:SetFont("Fonts\\MORPHEUS.TTF", 22, "OUTLINE");
title:SetPoint("TOPLEFT", shop, "TOPLEFT", 16, -12);
title:SetTextColor(EDGE[1], EDGE[2], EDGE[3]);
title:SetText("Shop");

local goldText = shop:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge");
goldText:SetPoint("BOTTOMRIGHT", shop, "BOTTOMRIGHT", -18, 14);
goldText:SetTextColor(1, 0.82, 0.25);

local close = CreateFrame("Button", nil, shop, "UIPanelCloseButton");
close:SetPoint("TOPRIGHT", shop, "TOPRIGHT", 2, 2);
close:SetScript("OnClick", function() CloseMerchant(); end);

local searchBox = CreateFrame("EditBox", "CENTURION_RiftShopSearch", shop, "InputBoxTemplate");
searchBox:SetWidth(180);
searchBox:SetHeight(20);
searchBox:SetPoint("TOPLEFT", shop, "TOPLEFT", CAT_W + 30, -14);
searchBox:SetAutoFocus(false);
local searchHint = searchBox:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall");
searchHint:SetPoint("LEFT", searchBox, "LEFT", 2, 0);
searchHint:SetText("Search items");

local rule = shop:CreateTexture(nil, "ARTWORK");
rule:SetTexture(EDGE[1], EDGE[2], EDGE[3], 0.5);
rule:SetHeight(1);
rule:SetPoint("TOPLEFT", shop, "TOPLEFT", 14, -44);
rule:SetPoint("TOPRIGHT", shop, "TOPRIGHT", -14, -44);

local function VRule(x)
	local t = shop:CreateTexture(nil, "ARTWORK");
	t:SetTexture(EDGE[1], EDGE[2], EDGE[3], 0.3);
	t:SetWidth(1);
	t:SetPoint("TOPLEFT", shop, "TOPLEFT", x, -50);
	t:SetPoint("BOTTOMLEFT", shop, "BOTTOMLEFT", x, 40);
end
VRule(CAT_W + 14);
VRule(W - DETAIL_W - 14);

-- ------------------------------------------------------------------ helpers

local function Price(fs, id)
	local cost = YourCost(id);
	local afford = GetMoney() >= cost * COPPER;
	fs:SetText(cost);
	if ( afford ) then
		fs:SetTextColor(1, 0.82, 0.25);
	else
		fs:SetTextColor(0.75, 0.30, 0.25);
	end
end

local function Buy(id)
	local index = merchantIndex[id];
	if ( not index ) then
		return;
	end
	BuyMerchantItem(index, 1);
end

local function ItemButton(parent, size)
	local b = CreateFrame("Button", nil, parent);
	b:SetWidth(size);
	b:SetHeight(size);
	b:RegisterForClicks("LeftButtonUp", "RightButtonUp");
	b.edge = b:CreateTexture(nil, "BACKGROUND");
	b.edge:SetPoint("TOPLEFT", b, "TOPLEFT", -2, 2);
	b.edge:SetPoint("BOTTOMRIGHT", b, "BOTTOMRIGHT", 2, -2);
	b.edge:SetTexture(EDGE[1], EDGE[2], EDGE[3], 0.55);
	b.tex = b:CreateTexture(nil, "ARTWORK");
	b.tex:SetAllPoints(b);
	b.owned = b:CreateTexture(nil, "OVERLAY");
	b.owned:SetTexture("Interface\\Buttons\\UI-CheckBox-Check");
	b.owned:SetWidth(18);
	b.owned:SetHeight(18);
	b.owned:SetPoint("TOPRIGHT", b, "TOPRIGHT", 4, 4);
	b.owned:Hide();
	b.price = b:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall");
	b.price:SetPoint("TOP", b, "BOTTOM", 0, -1);
	b:SetScript("OnEnter", function(self)
		if ( not self.item ) then return; end
		GameTooltip:SetOwner(self, "ANCHOR_RIGHT");
		if ( merchantIndex[self.item] ) then
			GameTooltip:SetMerchantItem(merchantIndex[self.item]);
		else
			GameTooltip:SetHyperlink("item:" .. self.item);
		end
		GameTooltip:Show();
	end);
	b:SetScript("OnLeave", function() GameTooltip:Hide(); end);
	return b;
end

local function Fill(b, id, held)
	b.item = id;
	b.tex:SetTexture(GetItemIcon(id) or "Interface\\Icons\\INV_Misc_QuestionMark");
	Price(b.price, id);
	if ( held and (held[id] or 0) > 0 ) then
		b.owned:Show();
	else
		b.owned:Hide();
	end
	b:Show();
end

-- ------------------------------------------------------------------ categories

local Refresh;
local catButtons = {};
for i, c in ipairs(CATEGORIES) do
	local b = CreateFrame("Button", nil, shop);
	b:SetWidth(CAT_W - 8);
	b:SetHeight(24);
	b:SetPoint("TOPLEFT", shop, "TOPLEFT", 16, -56 - (i - 1) * 28);
	b.bg = b:CreateTexture(nil, "BACKGROUND");
	b.bg:SetAllPoints(b);
	b.bg:SetTexture(1, 1, 1, 1);
	b.text = b:CreateFontString(nil, "OVERLAY", "GameFontHighlight");
	b.text:SetPoint("LEFT", b, "LEFT", 8, 0);
	b.text:SetText(c.label);
	b:SetScript("OnClick", function() category = c.key; Refresh(); end);
	catButtons[i] = b;
end

-- ------------------------------------------------------------------ grid

local GRID_X = CAT_W + 30;
local GRID_W = W - DETAIL_W - CAT_W - 60;
local PER_ROW = math.floor((GRID_W + CELL_GAP) / (CELL + CELL_GAP));
local tierLabels, grid = {}, {};
for i, t in ipairs(TIERS) do
	local fs = shop:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall");
	fs:SetTextColor(EDGE[1], EDGE[2], EDGE[3]);
	fs:SetText(string.upper(t[2]));
	tierLabels[i] = fs;
end

local function Matches(id, item)
	if ( category ) then
		local ok = false;
		for _, tag in ipairs(item.tags or {}) do
			if ( tag == category ) then ok = true; end
		end
		if ( not ok ) then return false; end
	end
	if ( search ~= "" and not string.find(string.lower(item.name), string.lower(search), 1, true) ) then
		return false;
	end
	return merchantIndex[id] ~= nil;
end

-- ------------------------------------------------------------------ detail

local DX = W - DETAIL_W;
local dIcon = ItemButton(shop, 44);
dIcon:SetPoint("TOPLEFT", shop, "TOPLEFT", DX, -58);
local dName = shop:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge");
dName:SetPoint("TOPLEFT", dIcon, "TOPRIGHT", 10, -2);
dName:SetWidth(DETAIL_W - 70);
dName:SetJustifyH("LEFT");
local dCost = shop:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall");
dCost:SetPoint("TOPLEFT", dName, "BOTTOMLEFT", 0, -4);
dCost:SetWidth(DETAIL_W - 70);
dCost:SetJustifyH("LEFT");

local treeLabel = shop:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall");
treeLabel:SetPoint("TOPLEFT", shop, "TOPLEFT", DX, -122);
treeLabel:SetTextColor(EDGE[1], EDGE[2], EDGE[3]);
treeLabel:SetText("BUILD PATH");

local treeNodes, treeLines = {}, {};
local function TreeNode(i)
	if ( not treeNodes[i] ) then
		treeNodes[i] = ItemButton(shop, 32);
		treeNodes[i]:SetScript("OnClick", function(self, button)
			if ( button == "RightButton" ) then Buy(self.item); else selected = self.item; Refresh(); end
		end);
	end
	return treeNodes[i];
end
local function TreeLine(i)
	if ( not treeLines[i] ) then
		local t = shop:CreateTexture(nil, "BORDER");
		t:SetTexture(EDGE[1], EDGE[2], EDGE[3], 0.7);
		treeLines[i] = t;
	end
	return treeLines[i];
end

local intoLabel = shop:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall");
intoLabel:SetTextColor(EDGE[1], EDGE[2], EDGE[3]);
intoLabel:SetText("BUILDS INTO");
local intoButtons = {};

local purchase = CreateFrame("Button", nil, shop, "UIPanelButtonTemplate");
purchase:SetWidth(DETAIL_W - 30);
purchase:SetHeight(26);
purchase:SetPoint("BOTTOMLEFT", shop, "BOTTOMLEFT", DX, 40);
purchase:SetScript("OnClick", function() if ( selected ) then Buy(selected); end end);

local hint = shop:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall");
hint:SetPoint("BOTTOMLEFT", shop, "BOTTOMLEFT", 16, 16);
hint:SetText("Right-click an item to buy it.  Sell from your inventory.");

-- Lay the tree out: the item, its components under it, theirs under them.
local function Tree(held)
	local used, nl = 0, 0;
	local top = -150;
	local cx = DX + (DETAIL_W - 30) / 2;
	local function Node(id, x, y)
		used = used + 1;
		local b = TreeNode(used);
		b:ClearAllPoints();
		b:SetPoint("TOP", shop, "TOPLEFT", x, y);
		Fill(b, id, held);
		return b;
	end
	local function Link(x1, y1, x2, y2)
		-- an elbow: down from the parent, across, down to the child
		local mid = (y1 + y2) / 2;
		nl = nl + 1;
		local v1 = TreeLine(nl);
		v1:ClearAllPoints();
		v1:SetPoint("TOPLEFT", shop, "TOPLEFT", x1, y1);
		v1:SetWidth(1); v1:SetHeight(math.max(1, y1 - mid)); v1:Show();
		nl = nl + 1;
		local hz = TreeLine(nl);
		hz:ClearAllPoints();
		hz:SetPoint("TOPLEFT", shop, "TOPLEFT", math.min(x1, x2), mid);
		hz:SetWidth(math.max(1, math.abs(x2 - x1))); hz:SetHeight(1); hz:Show();
		nl = nl + 1;
		local v2 = TreeLine(nl);
		v2:ClearAllPoints();
		v2:SetPoint("TOPLEFT", shop, "TOPLEFT", x2, mid);
		v2:SetWidth(1); v2:SetHeight(math.max(1, mid - y2)); v2:Show();
	end
	local pool = {};
	for k, v in pairs(held) do pool[k] = v; end
	local function Walk(id, x, y, width, depth)
		local b = Node(id, x, y);
		-- a held component counts once (the server takes it into the build)
		if ( depth > 0 and (pool[id] or 0) > 0 ) then
			pool[id] = pool[id] - 1;
			b.owned:Show();
		else
			b.owned:Hide();
		end
		local parts = ITEMS[id] and ITEMS[id].from or {};
		if ( depth >= 2 or #parts == 0 ) then
			return;
		end
		local step = width / #parts;
		for k, part in ipairs(parts) do
			local px = x - width / 2 + step * (k - 0.5);
			local py = y - 62;
			Link(x, y - 32, px, py);
			Walk(part, px, py, step, depth + 1);
		end
	end
	if ( selected ) then
		Walk(selected, cx, top, DETAIL_W - 30, 0);
	end
	for i = used + 1, #treeNodes do treeNodes[i]:Hide(); end
	for i = nl + 1, #treeLines do treeLines[i]:Hide(); end
end

-- ------------------------------------------------------------------ refresh

Refresh = function()
	if ( not shop:IsShown() ) then
		return;
	end
	wipe(merchantIndex);
	for i = 1, GetMerchantNumItems() do
		local id = tonumber(string.match(GetMerchantItemLink(i) or "", "item:(%d+)")) or idByName[GetMerchantItemInfo(i) or ""];
		if ( id ) then
			merchantIndex[id] = i;
		end
	end
	local held = Held();
	goldText:SetText(string.format("%d gold", math.floor(GetMoney() / COPPER)));
	for i, c in ipairs(CATEGORIES) do
		local on = c.key == category;
		catButtons[i].bg:SetVertexColor(EDGE[1], EDGE[2], EDGE[3], on and 0.35 or 0.06);
	end
	if ( search == "" ) then searchHint:Show(); else searchHint:Hide(); end

	-- the grid, tier by tier, in League's price order
	local used = 0;
	local y = -56;
	for i, t in ipairs(TIERS) do
		local ids = {};
		for id, item in pairs(ITEMS) do
			if ( item.tier == t[1] and Matches(id, item) ) then
				table.insert(ids, id);
			end
		end
		table.sort(ids, function(a, b) return ITEMS[a].gold < ITEMS[b].gold or (ITEMS[a].gold == ITEMS[b].gold and a < b); end);
		local label = tierLabels[i];
		if ( #ids > 0 ) then
			label:ClearAllPoints();
			label:SetPoint("TOPLEFT", shop, "TOPLEFT", GRID_X, y);
			label:Show();
			y = y - 16;
			for k, id in ipairs(ids) do
				used = used + 1;
				local b = grid[used];
				if ( not b ) then
					b = ItemButton(shop, CELL);
					b:SetScript("OnClick", function(self, button)
						if ( button == "RightButton" ) then Buy(self.item); else selected = self.item; Refresh(); end
					end);
					grid[used] = b;
				end
				local col = (k - 1) % PER_ROW;
				local row = math.floor((k - 1) / PER_ROW);
				b:ClearAllPoints();
				b:SetPoint("TOPLEFT", shop, "TOPLEFT", GRID_X + col * (CELL + CELL_GAP), y - row * (CELL + 18));
				Fill(b, id, held);
				if ( id == selected ) then
					b.edge:SetTexture(1, 0.9, 0.6, 1);
				else
					b.edge:SetTexture(EDGE[1], EDGE[2], EDGE[3], 0.55);
				end
			end
			y = y - (math.floor((#ids - 1) / PER_ROW) + 1) * (CELL + 18) - 8;
		else
			label:Hide();
		end
	end
	for i = used + 1, #grid do grid[i]:Hide(); end

	-- the detail panel
	if ( selected and ITEMS[selected] ) then
		Fill(dIcon, selected, nil);
		dIcon.price:SetText("");
		local name, _, quality = GetItemInfo(selected);
		dName:SetText(name or ITEMS[selected].name);
		local r, g, b = GetItemQualityColor(quality or 1);
		dName:SetTextColor(r, g, b);
		local cost = YourCost(selected);
		if ( cost < ITEMS[selected].gold ) then
			dCost:SetText(string.format("|cffffd100%d gold|r for you  (%d)", cost, ITEMS[selected].gold));
		else
			dCost:SetText(string.format("|cffffd100%d gold|r", cost));
		end
		treeLabel:Show();
		Tree(held);
		-- builds into
		local into = Shop() and Shop().IntoOf and Shop().IntoOf[selected] or {};
		local iy = -150 - 3 * 62 - 6;
		intoLabel:ClearAllPoints();
		intoLabel:SetPoint("TOPLEFT", shop, "TOPLEFT", DX, iy);
		if ( #into > 0 ) then intoLabel:Show(); else intoLabel:Hide(); end
		for k, id in ipairs(into) do
			local b = intoButtons[k];
			if ( not b ) then
				b = ItemButton(shop, 30);
				b:SetScript("OnClick", function(self, button)
					if ( button == "RightButton" ) then Buy(self.item); else selected = self.item; Refresh(); end
				end);
				intoButtons[k] = b;
			end
			b:ClearAllPoints();
			b:SetPoint("TOPLEFT", shop, "TOPLEFT", DX + (k - 1) * 40, iy - 16);
			Fill(b, id, held);
		end
		for k = #into + 1, #intoButtons do intoButtons[k]:Hide(); end
		purchase:SetText(string.format("Purchase  (%d gold)", cost));
		if ( GetMoney() >= cost * COPPER and merchantIndex[selected] ) then purchase:Enable(); else purchase:Disable(); end
		purchase:Show();
	else
		dIcon:Hide();
		dName:SetText("Pick an item");
		dName:SetTextColor(EDGE[1], EDGE[2], EDGE[3]);
		dCost:SetText("Its build path and price show here.");
		treeLabel:Hide();
		intoLabel:Hide();
		for _, b in ipairs(treeNodes) do b:Hide(); end
		for _, t in ipairs(treeLines) do t:Hide(); end
		for _, b in ipairs(intoButtons) do b:Hide(); end
		purchase:Hide();
	end
end

searchBox:SetScript("OnTextChanged", function(self) search = self:GetText() or ""; Refresh(); end);
searchBox:SetScript("OnEscapePressed", function(self) self:ClearFocus(); end);
shop:SetScript("OnShow", Refresh);

-- ------------------------------------------------------------------ the takeover

local hidden = false;
local function HideMerchant()
	if ( not hidden and MerchantFrame ) then
		MerchantFrame:SetAlpha(0);
		MerchantFrame:SetScale(0.001);
		hidden = true;
	end
end
local function RestoreMerchant()
	if ( hidden and MerchantFrame ) then
		MerchantFrame:SetScale(1);
		MerchantFrame:SetAlpha(1);
		hidden = false;
	end
end

local events = CreateFrame("Frame");
for _, e in ipairs({ "MERCHANT_SHOW", "MERCHANT_CLOSED", "MERCHANT_UPDATE", "BAG_UPDATE", "PLAYER_MONEY" }) do
	events:RegisterEvent(e);
end
events:SetScript("OnEvent", function(self, event)
	if ( event == "MERCHANT_SHOW" ) then
		if ( IsRiftShop() ) then
			HideMerchant();
			shop:Show();
		else
			RestoreMerchant();
		end
	elseif ( event == "MERCHANT_CLOSED" ) then
		shop:Hide();
		RestoreMerchant();
	else
		Refresh();
	end
end);
