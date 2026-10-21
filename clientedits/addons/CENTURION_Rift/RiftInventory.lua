-- Summoner's Rift: League's inventory in place of the character pane and bags.
--
-- In the Rift a shop item is not WoW gear: it is held, not worn. The server
-- (RiftProgression::SyncItems) puts what the held items give on the champion,
-- and this frame is where they are seen - six item slots and a trinket slot,
-- the champion's stats beside them, the gold above. The character pane (C) and
-- the bags (B, the bag buttons, the shop opening them) bring this up instead
-- while you are in the Rift; the field kit you are wearing stays out of sight.
--
-- The items still sit in the bags underneath, so each slot is Blizzard's own
-- bag-slot button (ContainerFrameItemButtonTemplate) pointed at the bag slot
-- holding that item: right-click uses an item (in combat too) or sells it to the
-- shopkeeper, left-click or drag picks it up - onto your action bars, or onto
-- another slot to reorder. The slot order is the bag order.

local RIFT_ZONE = "Summoner's Rift";
local SHOP_FIRST, SHOP_LAST = 991000, 991099;
local ITEM_SLOTS = 6;

local SLOT = 44;
local GAP = 6;
local PAD = 14;
local HEADER = 34;
local STATS_W = 150;
local GOLD = { 1.00, 0.82, 0.25 };
local EDGE = { 0.78, 0.66, 0.43 };       -- League's gold trim
local NAVY = { 0.02, 0.06, 0.10 };

local function InRift()
	return GetRealZoneText() == RIFT_ZONE;
end

local function IsShopItem(id)
	return id and id >= SHOP_FIRST and id <= SHOP_LAST;
end

-- The trinket slot's items: the Rift's steeds (the server's IsTrinketItem).
local function IsTrinket(id)
	return id == 991038 or id == 991039;
end

-- ------------------------------------------------------------------ frame

local inv = CreateFrame("Frame", "CENTURION_RiftInventory", UIParent);
inv:SetFrameStrata("HIGH");
inv:SetWidth(PAD + STATS_W + PAD + 3 * SLOT + 2 * GAP + PAD + SLOT + PAD);
inv:SetHeight(HEADER + PAD + 2 * SLOT + GAP + PAD + 34);
inv:SetPoint("BOTTOMRIGHT", UIParent, "BOTTOMRIGHT", -90, 120);
inv:SetBackdrop({
	bgFile = "Interface\\Tooltips\\UI-Tooltip-Background",
	edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
	tile = true, tileSize = 16, edgeSize = 14,
	insets = { left = 3, right = 3, top = 3, bottom = 3 },
});
inv:SetBackdropColor(NAVY[1], NAVY[2], NAVY[3], 0.95);
inv:SetBackdropBorderColor(EDGE[1], EDGE[2], EDGE[3], 1);
inv:SetClampedToScreen(true);
inv:SetMovable(true);
inv:EnableMouse(true);
inv:RegisterForDrag("LeftButton");
inv:SetScript("OnDragStart", inv.StartMoving);
inv:SetScript("OnDragStop", inv.StopMovingOrSizing);
inv:Hide();
table.insert(UISpecialFrames, "CENTURION_RiftInventory");   -- Escape closes it

local wash = inv:CreateTexture(nil, "BORDER");
wash:SetTexture(1, 1, 1, 1);
wash:SetPoint("TOPLEFT", inv, "TOPLEFT", 4, -4);
wash:SetPoint("TOPRIGHT", inv, "TOPRIGHT", -4, -4);
wash:SetHeight(HEADER - 4);
wash:SetGradientAlpha("VERTICAL", 0.10, 0.25, 0.35, 0.00, 0.10, 0.25, 0.35, 0.55);

local rule = inv:CreateTexture(nil, "ARTWORK");
rule:SetTexture(EDGE[1], EDGE[2], EDGE[3], 0.6);
rule:SetHeight(1);
rule:SetPoint("TOPLEFT", inv, "TOPLEFT", PAD, -HEADER);
rule:SetPoint("TOPRIGHT", inv, "TOPRIGHT", -PAD, -HEADER);

local title = inv:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge");
title:SetPoint("TOPLEFT", inv, "TOPLEFT", PAD, -11);
title:SetTextColor(EDGE[1], EDGE[2], EDGE[3]);
title:SetText("Inventory");

local goldText = inv:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge");
goldText:SetPoint("TOPRIGHT", inv, "TOPRIGHT", -PAD - 18, -11);
goldText:SetTextColor(GOLD[1], GOLD[2], GOLD[3]);

local close = CreateFrame("Button", nil, inv, "UIPanelCloseButton");
close:SetPoint("TOPRIGHT", inv, "TOPRIGHT", 2, 2);
close:SetScript("OnClick", function() inv:Hide(); end);

local hint = inv:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall");
hint:SetPoint("BOTTOMRIGHT", inv, "BOTTOMRIGHT", -PAD, 8);
hint:SetText("Right-click: use, or sell at the shop");

-- ------------------------------------------------------------------ stats

local STATS = {
	{ "Attack Power", function()
		local base, pos, neg = UnitAttackPower("player");
		local rbase, rpos, rneg = UnitRangedAttackPower("player");
		return math.max(base + pos + neg, (rbase or 0) + (rpos or 0) + (rneg or 0));
	end },
	{ "Spell Power", function()
		local best = 0;
		for school = 2, 7 do
			best = math.max(best, GetSpellBonusDamage(school) or 0);
		end
		return best;
	end },
	{ "Armor", function() return select(2, UnitArmor("player")); end },
	{ "Magic Resist", function() return select(2, UnitResistance("player", 2)); end },
	{ "Attack Speed", function()
		local speed = UnitAttackSpeed("player");
		if ( (UnitRangedDamage("player") or 0) > 0 and select(2, UnitClass("player")) == "HUNTER" ) then
			speed = UnitRangedDamage("player");
		end
		return speed and speed > 0 and string.format("%.2f", 1 / speed) or "-";
	end },
	{ "Critical Strike", function()
		return string.format("%.1f%%", math.max(GetCritChance() or 0, GetSpellCritChance(2) or 0, GetRangedCritChance() or 0));
	end },
	{ "Health", function() return UnitHealthMax("player"); end },
	{ "Mana", function() return UnitPowerMax("player", 0); end },
};

local statLines = {};
for i, stat in ipairs(STATS) do
	local label = inv:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall");
	label:SetPoint("TOPLEFT", inv, "TOPLEFT", PAD, -HEADER - 8 - (i - 1) * 14);
	label:SetTextColor(0.65, 0.72, 0.78);
	label:SetText(stat[1]);
	local value = inv:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall");
	value:SetPoint("TOPRIGHT", inv, "TOPLEFT", PAD + STATS_W, -HEADER - 8 - (i - 1) * 14);
	value:SetJustifyH("RIGHT");
	statLines[i] = value;
end

local divider = inv:CreateTexture(nil, "ARTWORK");
divider:SetTexture(EDGE[1], EDGE[2], EDGE[3], 0.35);
divider:SetWidth(1);
divider:SetPoint("TOPLEFT", inv, "TOPLEFT", PAD + STATS_W + PAD / 2, -HEADER - 6);
divider:SetPoint("BOTTOMLEFT", inv, "BOTTOMLEFT", PAD + STATS_W + PAD / 2, PAD + 14);

-- ------------------------------------------------------------------ slots

local GRID_X = PAD + STATS_W + PAD;
local GRID_Y = -HEADER - PAD + 4;

local function SlotBackground(x, y, label)
	local bg = inv:CreateTexture(nil, "BACKGROUND");
	bg:SetTexture(0, 0, 0, 0.55);
	bg:SetWidth(SLOT);
	bg:SetHeight(SLOT);
	bg:SetPoint("TOPLEFT", inv, "TOPLEFT", x, y);
	local edge = inv:CreateTexture(nil, "BORDER");
	edge:SetTexture("Interface\\Buttons\\UI-Quickslot2");
	edge:SetWidth(SLOT * 1.6);
	edge:SetHeight(SLOT * 1.6);
	edge:SetPoint("CENTER", bg, "CENTER", 0, 0);
	edge:SetVertexColor(EDGE[1], EDGE[2], EDGE[3], 0.9);
	if ( label ) then
		local text = inv:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall");
		text:SetPoint("TOP", bg, "BOTTOM", 0, -2);
		text:SetText(label);
	end
	return bg;
end

-- One holder per slot: the template reads the bag from its parent's ID.
local slots = {};
local function MakeSlot(index, x, y, label)
	SlotBackground(x, y, label);
	local holder = CreateFrame("Frame", nil, inv);
	holder:SetAllPoints(inv);
	local button = CreateFrame("Button", "CENTURION_RiftItem" .. index, holder, "ContainerFrameItemButtonTemplate");
	button:SetWidth(SLOT);
	button:SetHeight(SLOT);
	button:ClearAllPoints();
	button:SetPoint("TOPLEFT", inv, "TOPLEFT", x, y);
	local normal = _G[button:GetName() .. "NormalTexture"];
	if ( normal ) then
		normal:SetWidth(SLOT * 1.6);
		normal:SetHeight(SLOT * 1.6);
	end
	button:Hide();
	slots[index] = { holder = holder, button = button };
end

for i = 1, ITEM_SLOTS do
	local col = (i - 1) % 3;
	local row = math.floor((i - 1) / 3);
	MakeSlot(i, GRID_X + col * (SLOT + GAP), GRID_Y - row * (SLOT + GAP));
end
MakeSlot(ITEM_SLOTS + 1, GRID_X + 3 * (SLOT + GAP) + 4, GRID_Y - (SLOT + GAP) / 2, "Trinket");

-- The held shop items in bag order, trinkets apart.
local function Scan()
	local items, trinket = {}, nil;
	for bag = 0, NUM_BAG_SLOTS do
		for slot = 1, GetContainerNumSlots(bag) do
			local link = GetContainerItemLink(bag, slot);
			local id = link and tonumber(string.match(link, "item:(%d+)"));
			if ( IsShopItem(id) ) then
				if ( IsTrinket(id) ) then
					trinket = trinket or { bag, slot };
				else
					table.insert(items, { bag, slot });
				end
			end
		end
	end
	return items, trinket;
end

local function Point(index, where)
	local s = slots[index];
	local button = s.button;
	if ( not where ) then
		button:Hide();
		return;
	end
	local bag, slot = where[1], where[2];
	s.holder:SetID(bag);
	button:SetID(slot);
	local texture, count, locked = GetContainerItemInfo(bag, slot);
	SetItemButtonTexture(button, texture);
	SetItemButtonCount(button, count);
	SetItemButtonDesaturated(button, locked);
	if ( ContainerFrame_UpdateCooldown ) then
		ContainerFrame_UpdateCooldown(bag, button);
	end
	button:Show();
	if ( GameTooltip:IsOwned(button) ) then
		ContainerFrameItemButton_OnEnter(button);
	end
end

local function Refresh()
	if ( not inv:IsShown() ) then
		return;
	end
	goldText:SetText(string.format("%d gold", math.floor(GetMoney() / 10000)));
	local items, trinket = Scan();
	for i = 1, ITEM_SLOTS do
		Point(i, items[i]);
	end
	Point(ITEM_SLOTS + 1, trinket);
	for i, stat in ipairs(STATS) do
		local ok, value = pcall(stat[2]);
		statLines[i]:SetText(ok and tostring(value or "-") or "-");
	end
end

inv:SetScript("OnShow", Refresh);

-- ------------------------------------------------------------------ the takeover

local function Toggle()
	if ( inv:IsShown() ) then
		inv:Hide();
	else
		inv:Show();
	end
end

-- The bag functions call each other (ToggleBackpack runs ToggleBag(0),
-- OpenAllBags runs OpenBackpack and OpenBag), so one key press fires several of
-- these hooks: acting on each toggled the frame twice and F12 did nothing. They
-- are collected instead and acted on once, the next frame: a toggle (F12, B, a
-- bag button) wins over an open (the shop opening the bags).
local busy = false;
local pending = nil;           -- "toggle" or "open"
local wasShown = false;        -- the frame before the first hook of this press
local resolver = CreateFrame("Frame");
resolver:Hide();
resolver:SetScript("OnUpdate", function(self)
	self:Hide();
	local mode = pending;
	pending = nil;
	if ( not mode or not InRift() ) then
		return;
	end
	busy = true;
	CloseAllBags();
	busy = false;
	if ( mode == "toggle" and wasShown ) then
		inv:Hide();
	else
		inv:Show();
	end
end);

local function BagsOpened(mode)
	if ( busy or not InRift() ) then
		return;
	end
	if ( not pending ) then
		wasShown = inv:IsShown();
	end
	if ( pending ~= "toggle" ) then
		pending = mode;
	end
	resolver:Show();
end

hooksecurefunc("ToggleBackpack", function() BagsOpened("toggle"); end);
hooksecurefunc("ToggleBag", function() BagsOpened("toggle"); end);
if ( OpenAllBags ) then
	hooksecurefunc("OpenAllBags", function(forceOpen) BagsOpened(forceOpen and "open" or "toggle"); end);
end
if ( OpenBackpack ) then hooksecurefunc("OpenBackpack", function() BagsOpened("open"); end); end
if ( OpenBag ) then hooksecurefunc("OpenBag", function() BagsOpened("open"); end); end
local function BagsClosed()
	if ( not busy and not pending and InRift() ) then
		inv:Hide();
	end
end
hooksecurefunc("CloseAllBags", BagsClosed);
if ( CloseBackpack ) then hooksecurefunc("CloseBackpack", BagsClosed); end

-- The real character pane is still there: /paperdoll (or /charpane), or the
-- Character button on the frame.
local bypass = false;
local function RealCharacterPane()
	bypass = true;
	ToggleCharacter("PaperDollFrame");
	bypass = false;
end
SLASH_CENTURIONPAPERDOLL1 = "/paperdoll";
SLASH_CENTURIONPAPERDOLL2 = "/charpane";
SlashCmdList["CENTURIONPAPERDOLL"] = RealCharacterPane;

local paperdoll = CreateFrame("Button", nil, inv, "UIPanelButtonTemplate");
paperdoll:SetWidth(84);
paperdoll:SetHeight(18);
paperdoll:SetPoint("BOTTOMLEFT", inv, "BOTTOMLEFT", PAD, 8);
paperdoll:SetText("Character");
paperdoll:SetScript("OnClick", RealCharacterPane);

hooksecurefunc("ToggleCharacter", function(tab)
	if ( bypass or tab ~= "PaperDollFrame" or not InRift() ) then
		return;
	end
	local wasShown = inv:IsShown();
	if ( CharacterFrame:IsShown() ) then
		HideUIPanel(CharacterFrame);
	end
	if ( wasShown ) then
		inv:Hide();
	else
		inv:Show();
	end
end);

-- League words on the shop items' tooltips: they are held, not equipped.
local function Relabel(tooltip)
	local _, link = tooltip:GetItem();
	local id = link and tonumber(string.match(link, "item:(%d+)"));
	if ( not IsShopItem(id) ) then
		return;
	end
	local name = tooltip:GetName();
	for i = 2, tooltip:NumLines() do
		local line = _G[name .. "TextLeft" .. i];
		local text = line and line:GetText();
		if ( text ) then
			local rest = string.match(text, "^" .. (ITEM_SPELL_TRIGGER_ONEQUIP or "Equip:") .. "%s*(.*)$");
			if ( rest ) then
				line:SetText("Passive: " .. rest);
			else
				rest = string.match(text, "^" .. (ITEM_SPELL_TRIGGER_ONUSE or "Use:") .. "%s*(.*)$");
				if ( rest ) then
					line:SetText("Active: " .. rest);
				end
			end
		end
	end
end
GameTooltip:HookScript("OnTooltipSetItem", Relabel);
if ( ItemRefTooltip ) then
	ItemRefTooltip:HookScript("OnTooltipSetItem", Relabel);
end

local events = CreateFrame("Frame");
for _, event in ipairs({ "BAG_UPDATE", "BAG_UPDATE_COOLDOWN", "ITEM_LOCK_CHANGED", "PLAYER_MONEY", "UNIT_STATS",
		"UNIT_RESISTANCES", "UNIT_ATTACK_POWER", "UNIT_RANGED_ATTACK_POWER", "UNIT_ATTACK_SPEED", "UNIT_MAXHEALTH",
		"UNIT_MAXMANA", "PLAYER_LEVEL_UP", "UNIT_AURA", "ZONE_CHANGED_NEW_AREA", "PLAYER_ENTERING_WORLD" }) do
	events:RegisterEvent(event);
end
events:SetScript("OnEvent", function(self, event, unit)
	if ( (event == "ZONE_CHANGED_NEW_AREA" or event == "PLAYER_ENTERING_WORLD") and not InRift() ) then
		inv:Hide();
		return;
	end
	if ( unit and type(unit) == "string" and string.match(event, "^UNIT_") and unit ~= "player" ) then
		return;
	end
	Refresh();
end);
