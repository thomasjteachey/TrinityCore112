-- CENTURION_VioletHold / BoonDraft - the Ethereal Broker's offers as a hand of cards.
--
-- Talking to a broker in the Violet Hold, the server (violet_hold_boons.cpp,
-- npc_violet_hold_boon_brokerAI::SendDeal) sends the hand on the CCGAME addon
-- whisper before the usual gossip menu, and this file puts the cards up and
-- closes that menu. Without this addon the gossip list works as it always did.
-- Fields are split on '^'; <broker> is the broker's guid as UnitGUID writes it.
--
--    VHBOON:DEAL:<broker>^<wave>^<final wave>^<greed>^<abundance>^<hoarder>[^<name>^<class id>]...
--    VHBOON:CARD:<broker>^<code>^<B|S|I>^<tier 1-5>^<run-wide>^<greed card>^<S<spell>|I<item>>
--                ^<name>^<headline>^<text>^<held>^<after>^<cap>^<unit>^<status>[^<who can take it>]...
--    VHBOON:DECK:<broker>[^<tier>^<name>^<value>]...
--    VHBOON:SHOW:<broker>
--    VHBOON:LOOK:<broker>^<name>^<code>         (999 = looking at nothing)
--    VHBOON:TAKEN:<broker>^<name>^<code>
--    VHBOON:GONE:<broker>
--
-- and the addon answers on CCGAMEREQ, whispering itself:
--
--    VHBOON:PICK:<broker>:<code>     VHBOON:LOOK:<broker>:<code>
--
-- The hand is shared: the party sees the same cards, and the first one anybody
-- takes ends the deal. <status> is the server's PickResult for this character.
--
-- /vhdraft demo  deals a made-up hand, to look at the frame outside the Hold.

local PREFIX = "CCGAME";
local REQUEST_PREFIX = "CCGAME" .. "REQ";
local WHITE = "Interface\\Buttons\\WHITE8X8";
local TITLE_FONT = "Fonts\\MORPHEUS.TTF";
local BODY_FONT = "Fonts\\FRIZQT__.TTF";
local CODE_NONE = 999;

local CARD_W, CARD_H, CARD_GAP = 190, 300, 16;
local FAN_DROP, LIFT = 10, 18;
local TOP_PAD, HAND_TOP = 24, 112;

local RARITY = {
	{ name = "Common",    r = 0.81, g = 0.78, b = 0.86 },
	{ name = "Uncommon",  r = 0.26, g = 0.83, b = 0.44 },
	{ name = "Rare",      r = 0.35, g = 0.67, b = 1.00 },
	{ name = "Epic",      r = 0.73, g = 0.52, b = 1.00 },
	{ name = "Legendary", r = 1.00, g = 0.62, b = 0.26 },
};
local GOLD = { r = 0.95, g = 0.81, b = 0.42 };

local CLASS_TOKEN = {
	[1] = "WARRIOR", [2] = "PALADIN", [3] = "HUNTER", [4] = "ROGUE", [5] = "PRIEST",
	[6] = "DEATHKNIGHT", [7] = "SHAMAN", [8] = "MAGE", [9] = "WARLOCK", [11] = "DRUID",
};

-- PickResult (VioletHoldBoons.h) as the card and the Take button say it.
local STATUS_TEXT = {
	[1] = "Not for your class",
	[2] = "You already have this",
	[3] = "Already at your cap",
	[4] = "You can go no higher",
	[5] = "Your level is too low",
	[6] = "Your bags are full",
	[7] = "Weapon skill not trained",
	[8] = "Unavailable",
};

local draft;          -- the hand on screen
local pending;        -- the hand being received, until SHOW
local suppress;       -- { tag, untilTime }: close that broker's gossip menu
local demo = false;

local function Me()
	return UnitName("player");
end

local function Split(text)
	local out = {};
	for field in string.gmatch(text .. "^", "([^%^]*)%^") do
		out[#out + 1] = field;
	end
	return out;
end

local function SameGuid(a, b)
	return a and b and string.upper(a) == string.upper(b);
end

local function Hex(r, g, b)
	return string.format("%02x%02x%02x", math.floor(r * 255 + 0.5), math.floor(g * 255 + 0.5), math.floor(b * 255 + 0.5));
end

local function ClassColor(classId)
	local token = CLASS_TOKEN[classId];
	local color = token and RAID_CLASS_COLORS and RAID_CLASS_COLORS[token];
	if ( color ) then
		return color.r, color.g, color.b;
	end
	return 0.8, 0.8, 0.8;
end

local function Request(verb, code)
	if ( demo or not draft or not SendAddonMessage ) then
		return;
	end
	SendAddonMessage(REQUEST_PREFIX, "VHBOON:" .. verb .. ":" .. draft.tag .. ":" .. code, "WHISPER", Me());
end

local function IconOf(card)
	local kind, id = string.match(card.icon or "", "^(%a)(%d+)$");
	id = tonumber(id);
	if ( kind == "S" and id and GetSpellInfo ) then
		local _, _, icon = GetSpellInfo(id);
		if ( icon ) then return icon; end
	elseif ( kind == "I" and id ) then
		local icon = GetItemIcon and GetItemIcon(id);
		if ( not icon and GetItemInfo ) then
			icon = select(10, GetItemInfo(id));
		end
		if ( icon ) then return icon; end
	end
	return "Interface\\Icons\\INV_Misc_QuestionMark";
end

-- ---------------------------------------------------------------------------
-- Frame

local frame = CreateFrame("Frame", "CENTURION_VioletHoldDraft", UIParent);
frame:Hide();
frame:SetFrameStrata("DIALOG");
frame:SetToplevel(true);
frame:SetPoint("CENTER", UIParent, "CENTER", 0, 20);
frame:SetBackdrop({
	bgFile = WHITE, edgeFile = "Interface\\DialogFrame\\UI-DialogBox-Border",
	tile = false, edgeSize = 24, insets = { left = 6, right = 6, top = 6, bottom = 6 },
});
frame:SetBackdropColor(0.07, 0.05, 0.11, 0.96);
frame:EnableMouse(true);
frame:SetMovable(true);
frame:SetClampedToScreen(true);
frame:RegisterForDrag("LeftButton");
frame:SetScript("OnDragStart", function(self) self:StartMoving(); end);
frame:SetScript("OnDragStop", function(self) self:StopMovingOrSizing(); end);
if ( UISpecialFrames ) then
	table.insert(UISpecialFrames, "CENTURION_VioletHoldDraft");
end

local function Text(parent, size, font, layer)
	local fs = parent:CreateFontString(nil, layer or "OVERLAY");
	fs:SetFont(font or BODY_FONT, size);
	fs:SetShadowOffset(1, -1);
	fs:SetShadowColor(0, 0, 0, 1);
	return fs;
end

local waveText = Text(frame, 11);
waveText:SetPoint("TOPLEFT", frame, "TOPLEFT", 28, -TOP_PAD);
waveText:SetTextColor(0.73, 0.66, 0.84);

local title = Text(frame, 26, TITLE_FONT);
title:SetPoint("TOPLEFT", waveText, "BOTTOMLEFT", 0, -6);
title:SetTextColor(0.96, 0.93, 1.0);
title:SetText("The Ethereal Broker deals");

local subtitle = Text(frame, 12);
subtitle:SetPoint("TOPLEFT", title, "BOTTOMLEFT", 0, -6);
subtitle:SetTextColor(0.77, 0.72, 0.85);
subtitle:SetText("Your party sees the same hand. The first card anyone takes ends the deal.");

local partyLabel = Text(frame, 10);
partyLabel:SetPoint("TOPRIGHT", frame, "TOPRIGHT", -28, -TOP_PAD);
partyLabel:SetTextColor(0.73, 0.66, 0.84);
partyLabel:SetText("PARTY AT THE TABLE");

local partyText = Text(frame, 13);
partyText:SetPoint("TOPRIGHT", partyLabel, "BOTTOMRIGHT", 0, -8);
partyText:SetJustifyH("RIGHT");

local close = CreateFrame("Button", nil, frame, "UIPanelCloseButton");
close:SetPoint("TOPRIGHT", frame, "TOPRIGHT", -4, -4);

-- ---------------------------------------------------------------------------
-- Cards

local cards = {};
local Refresh;

local function Select(code)
	if ( not draft or draft.taken or draft.selected == code ) then
		return;
	end
	draft.selected = code;
	if ( PlaySound ) then PlaySound("igMainMenuOptionCheckBoxOn"); end
	Request("LOOK", code);
	Refresh();
end

local function CardByCode(code)
	if ( not draft ) then return nil; end
	for _, card in ipairs(draft.cards) do
		if ( card.code == code ) then return card; end
	end
end

local Take;

local function MakeCard(i)
	local c = CreateFrame("Button", nil, frame);
	c:SetWidth(CARD_W);
	c:SetHeight(CARD_H);
	c:SetBackdrop({ bgFile = WHITE, edgeFile = WHITE, edgeSize = 2, insets = { left = 2, right = 2, top = 2, bottom = 2 } });
	c:SetBackdropColor(0.10, 0.075, 0.16, 1);
	c:RegisterForClicks("LeftButtonUp");

	c.art = c:CreateTexture(nil, "BACKGROUND");
	c.art:SetTexture(WHITE);
	c.art:SetPoint("TOPLEFT", c, "TOPLEFT", 2, -2);
	c.art:SetPoint("TOPRIGHT", c, "TOPRIGHT", -2, -2);
	c.art:SetHeight(108);

	c.glow = c:CreateTexture(nil, "ARTWORK");
	c.glow:SetTexture("Interface\\Buttons\\UI-ActionButton-Border");
	c.glow:SetBlendMode("ADD");
	c.glow:SetWidth(132);
	c.glow:SetHeight(132);
	c.glow:SetPoint("CENTER", c, "TOP", 0, -58);

	c.icon = c:CreateTexture(nil, "OVERLAY");
	c.icon:SetWidth(54);
	c.icon:SetHeight(54);
	c.icon:SetPoint("CENTER", c, "TOP", 0, -58);
	c.icon:SetTexCoord(0.07, 0.93, 0.07, 0.93);

	c.kind = Text(c, 9);
	c.kind:SetPoint("TOPLEFT", c, "TOPLEFT", 10, -9);

	c.greedBg = c:CreateTexture(nil, "OVERLAY");
	c.greedBg:SetTexture(WHITE);
	c.greedBg:SetVertexColor(GOLD.r, GOLD.g, GOLD.b);
	c.greedBg:SetWidth(46);
	c.greedBg:SetHeight(14);
	c.greedBg:SetPoint("TOPRIGHT", c, "TOPRIGHT", -8, -7);
	c.greed = Text(c, 9);
	c.greed:SetPoint("CENTER", c.greedBg, "CENTER", 0, 0);
	c.greed:SetText("GREED");
	c.greed:SetTextColor(0.1, 0.07, 0.02);
	c.greed:SetShadowOffset(0, 0);

	c.watch = {};
	for k = 1, 5 do
		local w = Text(c, 10);
		w:SetPoint("BOTTOMRIGHT", c, "TOPRIGHT", -8 - (k - 1) * 16, -104);
		c.watch[k] = w;
	end

	c.name = Text(c, 14);
	c.name:SetPoint("TOPLEFT", c, "TOPLEFT", 12, -118);
	c.name:SetWidth(CARD_W - 24);
	c.name:SetJustifyH("LEFT");
	c.name:SetTextColor(0.97, 0.94, 1.0);

	c.headline = Text(c, 22);
	c.headline:SetPoint("TOPLEFT", c.name, "BOTTOMLEFT", 0, -6);
	c.headline:SetJustifyH("LEFT");

	c.text = Text(c, 11);
	c.text:SetPoint("TOPLEFT", c.headline, "BOTTOMLEFT", 0, -6);
	c.text:SetWidth(CARD_W - 24);
	c.text:SetJustifyH("LEFT");
	c.text:SetJustifyV("TOP");
	c.text:SetTextColor(0.83, 0.79, 0.89);

	c.barBg = c:CreateTexture(nil, "ARTWORK");
	c.barBg:SetTexture(WHITE);
	c.barBg:SetVertexColor(1, 1, 1, 0.10);
	c.barBg:SetPoint("BOTTOMLEFT", c, "BOTTOMLEFT", 12, 40);
	c.barBg:SetWidth(CARD_W - 24);
	c.barBg:SetHeight(6);
	c.barHave = c:CreateTexture(nil, "OVERLAY");
	c.barHave:SetTexture(WHITE);
	c.barHave:SetVertexColor(0.56, 0.51, 0.65);
	c.barHave:SetPoint("TOPLEFT", c.barBg, "TOPLEFT", 0, 0);
	c.barHave:SetHeight(6);
	c.barGain = c:CreateTexture(nil, "OVERLAY");
	c.barGain:SetTexture(WHITE);
	c.barGain:SetPoint("TOPLEFT", c.barHave, "TOPRIGHT", 0, 0);
	c.barGain:SetHeight(6);
	c.stack = Text(c, 10);
	c.stack:SetPoint("BOTTOMLEFT", c.barBg, "TOPLEFT", 0, 4);
	c.stack:SetTextColor(0.77, 0.72, 0.85);
	c.cap = Text(c, 10);
	c.cap:SetPoint("BOTTOMRIGHT", c.barBg, "TOPRIGHT", 0, 4);
	c.cap:SetTextColor(0.77, 0.72, 0.85);

	c.rule = c:CreateTexture(nil, "ARTWORK");
	c.rule:SetTexture(WHITE);
	c.rule:SetVertexColor(1, 1, 1, 0.08);
	c.rule:SetPoint("BOTTOMLEFT", c, "BOTTOMLEFT", 12, 28);
	c.rule:SetWidth(CARD_W - 24);
	c.rule:SetHeight(1);
	c.rarity = Text(c, 10);
	c.rarity:SetPoint("BOTTOMLEFT", c, "BOTTOMLEFT", 12, 11);
	c.forWho = Text(c, 10);
	c.forWho:SetPoint("BOTTOMRIGHT", c, "BOTTOMRIGHT", -12, 11);
	c.forWho:SetTextColor(0.77, 0.72, 0.85);

	c.ribbon = c:CreateTexture(nil, "OVERLAY");
	c.ribbon:SetTexture(WHITE);
	c.ribbon:SetVertexColor(0.05, 0.04, 0.08, 0.92);
	c.ribbon:SetPoint("TOPLEFT", c, "TOPLEFT", 2, -96);
	c.ribbon:SetPoint("TOPRIGHT", c, "TOPRIGHT", -2, -96);
	c.ribbon:SetHeight(22);
	c.ribbonText = Text(c, 11);
	c.ribbonText:SetPoint("CENTER", c.ribbon, "CENTER", 0, 0);
	c.ribbonText:SetWidth(CARD_W - 16);
	c.ribbonText:SetTextColor(0.97, 0.94, 1.0);

	c.dim = c:CreateTexture(nil, "OVERLAY");
	c.dim:SetTexture(WHITE);
	c.dim:SetVertexColor(0.05, 0.04, 0.08, 0.6);
	c.dim:SetAllPoints(c);
	c.stamp = Text(c, 26, TITLE_FONT);
	c.stamp:SetPoint("CENTER", c, "CENTER", 0, 10);
	c.stamp:SetTextColor(GOLD.r, GOLD.g, GOLD.b);
	c.stamp:SetText("TAKEN");

	c:SetScript("OnClick", function(self) if ( self.data ) then Select(self.data.code); end end);
	c:SetScript("OnDoubleClick", function(self) if ( self.data ) then Select(self.data.code); Take(); end end);
	c:SetScript("OnEnter", function(self)
		local data = self.data;
		if ( not data or not GameTooltip ) then return; end
		local kind, id = string.match(data.icon or "", "^(%a)(%d+)$");
		GameTooltip:SetOwner(self, "ANCHOR_RIGHT");
		if ( kind == "S" ) then
			GameTooltip:SetHyperlink("spell:" .. id);
		elseif ( kind == "I" ) then
			GameTooltip:SetHyperlink("item:" .. id);
		else
			GameTooltip:SetText(data.name);
		end
		GameTooltip:Show();
	end);
	c:SetScript("OnLeave", function() if ( GameTooltip ) then GameTooltip:Hide(); end end);

	cards[i] = c;
	return c;
end

local function Unit(card, value)
	return value .. (card.unit or "");
end

local function DrawCard(c, card, index, count)
	local rarity = RARITY[card.tier] or RARITY[1];
	local selected = (draft.selected == card.code) and not draft.taken;
	local taken = draft.taken and draft.taken.code == card.code;
	c.data = card;

	local mid = (count + 1) / 2;
	local y = -HAND_TOP - math.abs(index - mid) * FAN_DROP + ((selected or taken) and LIFT or 0);
	local x = (index - 1) * (CARD_W + CARD_GAP) - ((count * CARD_W + (count - 1) * CARD_GAP) / 2) + CARD_W / 2;
	c:ClearAllPoints();
	c:SetPoint("TOP", frame, "TOP", x, y);
	c:SetFrameLevel(frame:GetFrameLevel() + ((selected or taken) and 10 or 2));

	if ( selected or taken ) then
		c:SetBackdropBorderColor(GOLD.r, GOLD.g, GOLD.b, 1);
	else
		c:SetBackdropBorderColor(rarity.r, rarity.g, rarity.b, 1);
	end
	c.art:SetVertexColor(rarity.r, rarity.g, rarity.b, 0.14);
	c.glow:SetVertexColor(rarity.r, rarity.g, rarity.b, selected and 0.9 or 0.5);
	c.icon:SetTexture(IconOf(card));

	local kinds = { B = "BOON", S = "CLASS SPELL", I = "LEGENDARY" };
	c.kind:SetText(card.runWide and "RUN-WIDE BOON" or kinds[card.kind] or "BOON");
	c.kind:SetTextColor(rarity.r, rarity.g, rarity.b);
	if ( card.greedCard ) then c.greedBg:Show(); c.greed:Show(); else c.greedBg:Hide(); c.greed:Hide(); end

	c.name:SetText(card.name);
	if ( card.headline ~= "" ) then
		c.headline:SetFont(BODY_FONT, 22);
		c.headline:SetText(card.headline);
	else
		c.headline:SetFont(BODY_FONT, 14);
		c.headline:SetText("Unique");
	end
	c.headline:SetTextColor(rarity.r, rarity.g, rarity.b);
	c.text:SetText(card.text);
	c.text:SetHeight(math.max(CARD_H - 118 - c.name:GetStringHeight() - c.headline:GetStringHeight() - 12 - 72, 24));

	if ( card.cap and card.cap > 1 ) then
		local width = CARD_W - 24;
		local have = math.min(card.held / card.cap, 1);
		local gain = math.min((card.after - card.held) / card.cap, 1 - have);
		c.barHave:SetWidth(math.max(have > 0 and 2 or 0.01, width * have));
		c.barGain:SetWidth(math.max(gain > 0 and 3 or 0.01, width * gain));
		c.barGain:SetVertexColor(rarity.r, rarity.g, rarity.b);
		c.stack:SetText(Unit(card, card.held) .. "  >  |cffffffff" .. Unit(card, card.after) .. "|r");
		c.cap:SetText("cap " .. Unit(card, card.cap));
		c.barBg:Show(); c.barHave:Show(); c.barGain:Show(); c.stack:Show(); c.cap:Show();
	else
		c.barBg:Hide(); c.barHave:Hide(); c.barGain:Hide(); c.stack:Hide(); c.cap:Hide();
	end

	c.rarity:SetText(rarity.name);
	c.rarity:SetTextColor(rarity.r, rarity.g, rarity.b);
	if ( card.status == 0 ) then
		c.forWho:SetText("For you");
	elseif ( #card.owners > 0 ) then
		c.forWho:SetText("For " .. table.concat(card.owners, ", "));
	else
		c.forWho:SetText("Nobody right now");
	end

	if ( card.status ~= 0 and not draft.taken ) then
		local why = (card.status == 1 and #card.owners > 0) and ("Meant for " .. table.concat(card.owners, ", "))
			or STATUS_TEXT[card.status] or STATUS_TEXT[8];
		c.ribbonText:SetText(why);
		c.ribbon:Show(); c.ribbonText:Show();
		c:SetAlpha(0.78);
	else
		c.ribbon:Hide(); c.ribbonText:Hide();
		c:SetAlpha(1);
	end

	if ( draft.taken ) then
		if ( taken ) then c.dim:Hide(); c.stamp:Show(); c:SetAlpha(1);
		else c.dim:Show(); c.stamp:Hide(); c:SetAlpha(0.45); end
	else
		c.dim:Hide(); c.stamp:Hide();
	end

	-- Who else is looking at this one.
	local k = 0;
	for name, code in pairs(draft.looks) do
		if ( code == card.code and k < #c.watch and not draft.taken ) then
			k = k + 1;
			local w = c.watch[k];
			w:SetText(string.sub(name, 1, 1));
			w:SetTextColor(ClassColor(draft.classOf[name]));
			w:Show();
		end
	end
	for j = k + 1, #c.watch do c.watch[j]:Hide(); end

	c:Show();
end

-- ---------------------------------------------------------------------------
-- Actions and the bottom panels

local leave = CreateFrame("Button", nil, frame, "UIPanelButtonTemplate");
leave:SetWidth(190);
leave:SetHeight(26);
leave:SetText("Leave it for the party");
leave:SetScript("OnClick", function() frame:Hide(); end);

local take = CreateFrame("Button", nil, frame, "UIPanelButtonTemplate");
take:SetWidth(270);
take:SetHeight(26);
take:SetPoint("LEFT", leave, "RIGHT", 12, 0);

local takenText = Text(frame, 13);
takenText:SetTextColor(0.9, 0.86, 0.96);

local hint = Text(frame, 11);
hint:SetTextColor(0.66, 0.61, 0.75);

local deckPanel = CreateFrame("Frame", nil, frame);
deckPanel:SetBackdrop({ bgFile = WHITE, edgeFile = WHITE, edgeSize = 1 });
deckPanel:SetBackdropColor(0.04, 0.03, 0.06, 0.7);
deckPanel:SetBackdropBorderColor(1, 1, 1, 0.10);
deckPanel:SetHeight(92);
local deckLabel = Text(deckPanel, 10);
deckLabel:SetPoint("TOPLEFT", deckPanel, "TOPLEFT", 12, -10);
deckLabel:SetTextColor(0.73, 0.66, 0.84);
local deckCount = Text(deckPanel, 10);
deckCount:SetPoint("TOPRIGHT", deckPanel, "TOPRIGHT", -12, -10);
deckCount:SetTextColor(0.66, 0.61, 0.75);

local runPanel = CreateFrame("Frame", nil, frame);
runPanel:SetBackdrop({ bgFile = WHITE, edgeFile = WHITE, edgeSize = 1 });
runPanel:SetBackdropColor(0.04, 0.03, 0.06, 0.7);
runPanel:SetBackdropBorderColor(1, 1, 1, 0.10);
runPanel:SetWidth(250);
runPanel:SetHeight(92);
local runLabel = Text(runPanel, 10);
runLabel:SetPoint("TOPLEFT", runPanel, "TOPLEFT", 12, -10);
runLabel:SetTextColor(0.73, 0.66, 0.84);
runLabel:SetText("RUN-WIDE  -  WHOLE PARTY");
local runLines = {};
for i = 1, 3 do
	local fs = Text(runPanel, 11);
	fs:SetPoint("TOPLEFT", runPanel, "TOPLEFT", 12, -12 - i * 18);
	fs:SetTextColor(0.9, 0.86, 0.96);
	runLines[i] = fs;
end

local chips = {};
local function Chip(i)
	local chip = chips[i];
	if ( not chip ) then
		chip = CreateFrame("Frame", nil, deckPanel);
		chip:SetBackdrop({ bgFile = WHITE, edgeFile = WHITE, edgeSize = 1 });
		chip:SetBackdropColor(1, 1, 1, 0.05);
		chip:SetBackdropBorderColor(1, 1, 1, 0.10);
		chip:SetHeight(22);
		chip.dot = chip:CreateTexture(nil, "OVERLAY");
		chip.dot:SetTexture(WHITE);
		chip.dot:SetWidth(6);
		chip.dot:SetHeight(6);
		chip.dot:SetPoint("LEFT", chip, "LEFT", 8, 0);
		chip.text = Text(chip, 11);
		chip.text:SetPoint("LEFT", chip.dot, "RIGHT", 6, 0);
		chips[i] = chip;
	end
	return chip;
end

local function DrawDeck(width)
	local x, y, row = 12, -28, 0;
	for i, entry in ipairs(draft.deck) do
		local rarity = RARITY[entry.tier] or RARITY[1];
		local chip = Chip(i);
		chip.dot:SetVertexColor(rarity.r, rarity.g, rarity.b);
		local value = entry.value ~= "" and string.format("  |cff%s%s|r", Hex(rarity.r, rarity.g, rarity.b), entry.value) or "";
		chip.text:SetText(entry.name .. value);
		local w = chip.text:GetStringWidth() + 28;
		chip:SetWidth(w);
		if ( x + w > width - 12 and x > 12 ) then
			x, row = 12, row + 1;
			y = -28 - row * 28;
		end
		chip:ClearAllPoints();
		if ( row < 2 ) then
			chip:SetPoint("TOPLEFT", deckPanel, "TOPLEFT", x, y);
			chip:Show();
		else
			chip:Hide();   -- a third row would leave the panel; the count still says how many
		end
		x = x + w + 6;
	end
	for i = #draft.deck + 1, #chips do chips[i]:Hide(); end
	deckLabel:SetText("YOUR DECK THIS RUN");
	deckCount:SetText(#draft.deck .. " held  -  all of it goes when you leave the Hold");
end

Take = function()
	if ( not draft or draft.taken ) then return; end
	local card = CardByCode(draft.selected);
	if ( not card or card.status ~= 0 ) then return; end
	if ( demo ) then
		draft.taken = { name = Me(), code = card.code };
		draft.closeAt = GetTime() + 5;
		Refresh();
		return;
	end
	Request("PICK", card.code);
end
take:SetScript("OnClick", Take);

Refresh = function()
	if ( not draft ) then return; end
	local count = #draft.cards;
	local handWidth = count * CARD_W + (count - 1) * CARD_GAP;
	local width = math.max(handWidth + 64, 820);
	local height = HAND_TOP + CARD_H + 2 * FAN_DROP + 128 + 92 + 20;
	frame:SetWidth(width);
	frame:SetHeight(height);
	local room = (UIParent:GetWidth() or width) - 40;
	frame:SetScale(width > room and room / width or 1);

	waveText:SetText(string.format("VIOLET HOLD  -  WAVE %d OF %d", draft.wave, draft.final));
	local names = {};
	for _, member in ipairs(draft.party) do
		local r, g, b = ClassColor(member.class);
		names[#names + 1] = string.format("|cff%s%s%s|r", Hex(r, g, b), member.name,
			member.name == Me() and " (you)" or "");
	end
	partyText:SetText(table.concat(names, "   "));

	for i, card in ipairs(draft.cards) do
		DrawCard(cards[i] or MakeCard(i), card, i, count);
	end
	for i = count + 1, #cards do cards[i]:Hide(); cards[i].data = nil; end

	local actionsY = -(HAND_TOP + CARD_H + 2 * FAN_DROP + 28);
	leave:ClearAllPoints();
	leave:SetPoint("TOPLEFT", frame, "TOP", -(190 + 12 + 270) / 2, actionsY);
	hint:ClearAllPoints();
	hint:SetPoint("TOP", frame, "TOP", 0, actionsY - 36);
	takenText:ClearAllPoints();
	takenText:SetPoint("TOP", frame, "TOP", 0, actionsY + 2);

	if ( draft.taken ) then
		leave:Hide(); take:Hide(); hint:SetText("");
		local card = CardByCode(draft.taken.code);
		local what = card and card.name or "a card";
		if ( draft.taken.name == Me() ) then
			takenText:SetText("You took " .. what .. ". The broker fades for the whole party.");
		else
			takenText:SetText(draft.taken.name .. " took " .. what .. ". The broker fades.");
		end
		takenText:Show();
	else
		takenText:Hide();
		leave:Show(); take:Show();
		local card = CardByCode(draft.selected);
		if ( card and card.status == 0 ) then
			take:SetText("Take " .. card.name);
			take:Enable();
		elseif ( card ) then
			take:SetText((card.status == 1 and #card.owners > 0) and ("Only " .. card.owners[1] .. " can take this")
				or STATUS_TEXT[card.status] or STATUS_TEXT[8]);
			take:Disable();
		end
		hint:SetText("Click a card to look at it. Taking one is final for the whole party.");
	end

	deckPanel:ClearAllPoints();
	deckPanel:SetPoint("BOTTOMLEFT", frame, "BOTTOMLEFT", 20, 20);
	deckPanel:SetPoint("BOTTOMRIGHT", frame, "BOTTOMRIGHT", -20 - 250 - 12, 20);
	runPanel:ClearAllPoints();
	runPanel:SetPoint("BOTTOMRIGHT", frame, "BOTTOMRIGHT", -20, 20);
	DrawDeck(width - 40 - 250 - 12);

	runLines[1]:SetText(string.format("Greed  %d / 3  |cffc4b8d8+%d card%s per broker|r", draft.greed, draft.greed, draft.greed == 1 and "" or "s"));
	runLines[2]:SetText(string.format("Abundance  %d / 3  |cffc4b8d8+%d rune%s per wave|r", draft.abundance, draft.abundance, draft.abundance == 1 and "" or "s"));
	runLines[3]:SetText(string.format("Hoarder  %d / 1  |cffc4b8d8+%d broker%s per wave|r", draft.hoarder, draft.hoarder, draft.hoarder == 1 and "" or "s"));
end

frame:SetScript("OnShow", function() if ( PlaySound ) then PlaySound("igCharacterInfoOpen"); end end);
frame:SetScript("OnHide", function()
	if ( PlaySound ) then PlaySound("igCharacterInfoClose"); end
	if ( GameTooltip ) then GameTooltip:Hide(); end
	if ( draft and not draft.taken ) then
		Request("LOOK", CODE_NONE);
	end
	draft = nil;
end);
frame:SetScript("OnUpdate", function()
	if ( draft and draft.closeAt and GetTime() >= draft.closeAt ) then
		frame:Hide();
	end
end);

-- ---------------------------------------------------------------------------
-- Messages

local function CloseBrokerGossip()
	if ( suppress and GetTime() < suppress.untilTime and SameGuid(UnitGUID("npc"), suppress.tag) and CloseGossip ) then
		CloseGossip();
	end
end

local function Show(hand)
	-- Default to the first card this character can take.
	for _, card in ipairs(hand.cards) do
		if ( card.status == 0 ) then hand.selected = card.code; break; end
	end
	hand.selected = hand.selected or (hand.cards[1] and hand.cards[1].code);
	-- Another broker's hand still up: put that one down first.
	if ( draft and draft.tag ~= hand.tag and not draft.taken ) then
		Request("LOOK", CODE_NONE);
	end
	draft = hand;
	Refresh();
	frame:Show();
	Request("LOOK", hand.selected or CODE_NONE);
end

local handlers = {};

function handlers.DEAL(f)
	pending = {
		tag = f[1], wave = tonumber(f[2]) or 0, final = tonumber(f[3]) or 0,
		greed = tonumber(f[4]) or 0, abundance = tonumber(f[5]) or 0, hoarder = tonumber(f[6]) or 0,
		party = {}, classOf = {}, cards = {}, deck = {}, looks = {},
	};
	for i = 7, #f - 1, 2 do
		local member = { name = f[i], class = tonumber(f[i + 1]) };
		pending.party[#pending.party + 1] = member;
		pending.classOf[member.name] = member.class;
	end
end

function handlers.CARD(f)
	if ( not pending or not SameGuid(pending.tag, f[1]) or #f < 15 ) then return; end
	local card = {
		code = tonumber(f[2]), kind = f[3], tier = tonumber(f[4]) or 1, runWide = f[5] == "1", greedCard = f[6] == "1",
		icon = f[7], name = f[8], headline = f[9], text = f[10],
		held = tonumber(f[11]) or 0, after = tonumber(f[12]) or 0, cap = tonumber(f[13]) or 0, unit = f[14],
		status = tonumber(f[15]) or 8, owners = {},
	};
	for i = 16, #f do
		if ( f[i] ~= "" ) then card.owners[#card.owners + 1] = f[i]; end
	end
	pending.cards[#pending.cards + 1] = card;
end

function handlers.DECK(f)
	if ( not pending or not SameGuid(pending.tag, f[1]) ) then return; end
	for i = 2, #f - 2, 3 do
		pending.deck[#pending.deck + 1] = { tier = tonumber(f[i]) or 1, name = f[i + 1], value = f[i + 2] };
	end
end

function handlers.SHOW(f)
	if ( not pending or not SameGuid(pending.tag, f[1]) or #pending.cards == 0 ) then return; end
	local hand = pending;
	pending = nil;
	demo = false;
	suppress = { tag = hand.tag, untilTime = GetTime() + 2 };
	Show(hand);
	CloseBrokerGossip();
end

function handlers.LOOK(f)
	if ( not draft or not SameGuid(draft.tag, f[1]) ) then return; end
	local code = tonumber(f[3]);
	draft.looks[f[2]] = (code and code ~= CODE_NONE) and code or nil;
	Refresh();
end

function handlers.TAKEN(f)
	if ( not draft or not SameGuid(draft.tag, f[1]) ) then return; end
	draft.taken = { name = f[2], code = tonumber(f[3]) };
	draft.closeAt = GetTime() + 5;
	Refresh();
end

function handlers.GONE(f)
	if ( draft and SameGuid(draft.tag, f[1]) ) then
		draft.taken = draft.taken or { name = "", code = -1 };
		frame:Hide();
	end
end

local events = CreateFrame("Frame");
events:RegisterEvent("CHAT_MSG_ADDON");
events:RegisterEvent("GOSSIP_SHOW");
events:RegisterEvent("PLAYER_ENTERING_WORLD");
events:SetScript("OnEvent", function(self, event, prefix, message)
	if ( event == "GOSSIP_SHOW" ) then
		CloseBrokerGossip();
		return;
	end
	if ( event == "PLAYER_ENTERING_WORLD" ) then
		pending = nil;
		if ( draft ) then draft.taken = draft.taken or { name = "", code = -1 }; end
		frame:Hide();
		return;
	end
	if ( prefix ~= PREFIX or type(message) ~= "string" ) then
		return;
	end
	local verb, rest = string.match(message, "^VHBOON:(%u+):(.*)$");
	local handler = verb and handlers[verb];
	if ( handler ) then
		handler(Split(rest));
	end
end);

-- ---------------------------------------------------------------------------
-- /vhdraft demo

local function Demo()
	local me = Me() or "You";
	local _, classToken = UnitClass("player");
	local myClass = 4;
	for id, token in pairs(CLASS_TOKEN) do if ( token == classToken ) then myClass = id; end end
	demo = true;
	Show({
		tag = "demo", wave = 7, final = 30, greed = 1, abundance = 1, hoarder = 0,
		party = { { name = me, class = myClass }, { name = "Kaelen", class = 7 }, { name = "Ysolde", class = 5 } },
		classOf = { [me] = myClass, Kaelen = 7, Ysolde = 5 },
		looks = { Ysolde = 1, Kaelen = 108 },
		cards = {
			{ code = 1, kind = "B", tier = 2, icon = "S90231", name = "Boon of Might", headline = "+3%",
			  text = "damage done", held = 6, after = 9, cap = 255, unit = "%", status = 0, owners = {} },
			{ code = 27, kind = "B", tier = 3, icon = "S90291", name = "Boon of the Vampire", headline = "+3%",
			  text = "of damage dealt returned as health", held = 0, after = 3, cap = 30, unit = "%", status = 0, owners = {} },
			{ code = 108, kind = "S", tier = 4, icon = "S51690", name = "Killing Spree", headline = "Learn",
			  text = "Rogue spell, taught at the best rank your level allows. Forgotten when you leave the Hold.",
			  held = 0, after = 0, cap = 0, unit = "", status = myClass == 4 and 0 or 1, owners = myClass == 4 and {} or { "Kaelen" } },
			{ code = 208, kind = "I", tier = 5, icon = "I160807", name = "Shard of the Defiler", headline = "Crusader", greedCard = true,
			  text = "Yours for this run, soulbound. Destroyed when you leave the Hold.",
			  held = 0, after = 0, cap = 0, unit = "", status = 1, owners = { "Ysolde" } },
		},
		deck = {
			{ tier = 1, name = "Swiftness", value = "16%" }, { tier = 2, name = "Might", value = "6%" },
			{ tier = 1, name = "Fortitude", value = "6%" }, { tier = 2, name = "Resolve", value = "15%" },
			{ tier = 3, name = "Ascension", value = "x1" }, { tier = 3, name = "Phoenix", value = "" },
			{ tier = 4, name = "Shadow Dance", value = "learned" },
		},
	});
end

SLASH_CENTURIONVHDRAFT1 = "/vhdraft";
SlashCmdList["CENTURIONVHDRAFT"] = function(arg)
	if ( string.lower(arg or "") == "demo" ) then
		Demo();
	else
		DEFAULT_CHAT_FRAME:AddMessage("|cffbb85ff/vhdraft demo|r deals a made-up hand of boon cards.");
	end
end
