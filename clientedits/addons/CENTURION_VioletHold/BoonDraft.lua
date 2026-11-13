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
local BODY_FONT = "Fonts\\FRIZQT__.TTF";
local CODE_NONE = 999;

local CARD_W, CARD_H, CARD_GAP = 150, 200, 10;
local LIFT = 6;                -- how far the selected card rises
local PAD, HAND_TOP = 14, 44;  -- frame padding; where the cards start

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
	[1] = "Wrong class",
	[2] = "Owned",
	[3] = "Maxed",
	[4] = "Max level",
	[5] = "Level too low",
	[6] = "Bags full",
	[7] = "Weapon untrained",
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
-- Frame: a dark panel with a hairline border, a one-line header and the hand.

local frame = CreateFrame("Frame", "CENTURION_VioletHoldDraft", UIParent);
frame:Hide();
frame:SetFrameStrata("DIALOG");
frame:SetToplevel(true);
frame:SetPoint("CENTER", UIParent, "CENTER", 0, 40);
frame:SetBackdrop({ bgFile = WHITE, edgeFile = WHITE, edgeSize = 1 });
frame:SetBackdropColor(0.04, 0.04, 0.06, 0.94);
frame:SetBackdropBorderColor(1, 1, 1, 0.10);
frame:EnableMouse(true);
frame:SetMovable(true);
frame:SetClampedToScreen(true);
frame:RegisterForDrag("LeftButton");
frame:SetScript("OnDragStart", function(self) self:StartMoving(); end);
frame:SetScript("OnDragStop", function(self) self:StopMovingOrSizing(); end);
if ( UISpecialFrames ) then
	table.insert(UISpecialFrames, "CENTURION_VioletHoldDraft");   -- Escape closes it
end

local function Text(parent, size, layer)
	local fs = parent:CreateFontString(nil, layer or "OVERLAY");
	fs:SetFont(BODY_FONT, size);
	fs:SetShadowOffset(1, -1);
	fs:SetShadowColor(0, 0, 0, 0.8);
	return fs;
end

local title = Text(frame, 14);
title:SetPoint("TOPLEFT", frame, "TOPLEFT", PAD, -PAD);
title:SetTextColor(0.95, 0.95, 0.97);
title:SetText("Choose a boon");

local close = CreateFrame("Button", nil, frame, "UIPanelCloseButton");
close:SetScale(0.8);
close:SetPoint("TOPRIGHT", frame, "TOPRIGHT", 2, 2);

local waveText = Text(frame, 11);
waveText:SetPoint("RIGHT", close, "LEFT", 0, 0);
waveText:SetPoint("TOP", title, "TOP", 0, 0);
waveText:SetTextColor(0.55, 0.55, 0.62);

-- ---------------------------------------------------------------------------
-- Cards

local cards = {};
local Refresh;
local Take;

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

local function MakeCard(i)
	local c = CreateFrame("Button", nil, frame);
	c:SetWidth(CARD_W);
	c:SetHeight(CARD_H);
	c:SetBackdrop({ bgFile = WHITE, edgeFile = WHITE, edgeSize = 1 });
	c:SetBackdropColor(0.08, 0.08, 0.11, 1);
	c:RegisterForClicks("LeftButtonUp");

	-- rarity, as a strip along the top edge
	c.strip = c:CreateTexture(nil, "ARTWORK");
	c.strip:SetTexture(WHITE);
	c.strip:SetPoint("TOPLEFT", c, "TOPLEFT", 1, -1);
	c.strip:SetPoint("TOPRIGHT", c, "TOPRIGHT", -1, -1);
	c.strip:SetHeight(2);

	c.icon = c:CreateTexture(nil, "ARTWORK");
	c.icon:SetWidth(40);
	c.icon:SetHeight(40);
	c.icon:SetPoint("TOP", c, "TOP", 0, -18);
	c.icon:SetTexCoord(0.08, 0.92, 0.08, 0.92);

	c.kind = Text(c, 9);
	c.kind:SetPoint("TOPLEFT", c, "TOPLEFT", 8, -8);

	c.greed = Text(c, 9);
	c.greed:SetPoint("TOPRIGHT", c, "TOPRIGHT", -8, -8);
	c.greed:SetTextColor(GOLD.r, GOLD.g, GOLD.b);
	c.greed:SetText("GREED");

	-- who else in the party is looking at this card: their initial, class-coloured
	c.watch = {};
	for k = 1, 5 do
		local w = Text(c, 10);
		w:SetPoint("BOTTOMRIGHT", c, "TOPRIGHT", -8 - (k - 1) * 12, -54);
		c.watch[k] = w;
	end

	c.name = Text(c, 12);
	c.name:SetPoint("TOP", c, "TOP", 0, -68);
	c.name:SetWidth(CARD_W - 16);
	c.name:SetTextColor(0.95, 0.95, 0.97);

	c.headline = Text(c, 20);
	c.headline:SetPoint("TOP", c.name, "BOTTOM", 0, -4);

	c.text = Text(c, 10);
	c.text:SetPoint("TOP", c.headline, "BOTTOM", 0, -4);
	c.text:SetWidth(CARD_W - 20);
	c.text:SetJustifyV("TOP");
	c.text:SetTextColor(0.62, 0.62, 0.68);

	c.barBg = c:CreateTexture(nil, "ARTWORK");
	c.barBg:SetTexture(WHITE);
	c.barBg:SetVertexColor(1, 1, 1, 0.08);
	c.barBg:SetPoint("BOTTOMLEFT", c, "BOTTOMLEFT", 10, 24);
	c.barBg:SetWidth(CARD_W - 20);
	c.barBg:SetHeight(2);
	c.barHave = c:CreateTexture(nil, "OVERLAY");
	c.barHave:SetTexture(WHITE);
	c.barHave:SetVertexColor(0.55, 0.55, 0.62);
	c.barHave:SetPoint("TOPLEFT", c.barBg, "TOPLEFT", 0, 0);
	c.barHave:SetHeight(2);
	c.barGain = c:CreateTexture(nil, "OVERLAY");
	c.barGain:SetTexture(WHITE);
	c.barGain:SetPoint("TOPLEFT", c.barHave, "TOPRIGHT", 0, 0);
	c.barGain:SetHeight(2);
	c.stack = Text(c, 9);
	c.stack:SetPoint("TOPLEFT", c.barBg, "BOTTOMLEFT", 0, -4);
	c.stack:SetTextColor(0.62, 0.62, 0.68);
	c.cap = Text(c, 9);
	c.cap:SetPoint("TOPRIGHT", c.barBg, "BOTTOMRIGHT", 0, -4);
	c.cap:SetTextColor(0.45, 0.45, 0.52);

	-- why it is not yours, over the bottom of the card
	c.ribbon = c:CreateTexture(nil, "OVERLAY");
	c.ribbon:SetTexture(WHITE);
	c.ribbon:SetVertexColor(0, 0, 0, 0.75);
	c.ribbon:SetPoint("BOTTOMLEFT", c, "BOTTOMLEFT", 1, 1);
	c.ribbon:SetPoint("BOTTOMRIGHT", c, "BOTTOMRIGHT", -1, 1);
	c.ribbon:SetHeight(18);
	c.ribbonText = Text(c, 10);
	c.ribbonText:SetPoint("CENTER", c.ribbon, "CENTER", 0, 0);
	c.ribbonText:SetTextColor(0.85, 0.85, 0.9);

	-- kept for the deal-over state; the frame closes first, so it rarely shows
	c.stamp = Text(c, 14);
	c.stamp:SetPoint("CENTER", c, "CENTER", 0, 0);
	c.stamp:SetTextColor(GOLD.r, GOLD.g, GOLD.b);
	c.stamp:SetText("TAKEN");
	c.stamp:Hide();

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
	c.data = card;

	local x = PAD + (index - 1) * (CARD_W + CARD_GAP);
	c:ClearAllPoints();
	c:SetPoint("TOPLEFT", frame, "TOPLEFT", x, -HAND_TOP + (selected and LIFT or 0));
	c:SetFrameLevel(frame:GetFrameLevel() + (selected and 10 or 2));

	if ( selected ) then
		c:SetBackdropBorderColor(GOLD.r, GOLD.g, GOLD.b, 1);
	else
		c:SetBackdropBorderColor(rarity.r, rarity.g, rarity.b, 0.30);
	end
	c.strip:SetVertexColor(rarity.r, rarity.g, rarity.b, 0.9);
	c.icon:SetTexture(IconOf(card));

	-- Only what sets a card apart; a plain boon needs no label.
	local kinds = { S = "CLASS SPELL", I = "LEGENDARY" };
	c.kind:SetText(card.runWide and "RUN-WIDE" or kinds[card.kind] or "");
	c.kind:SetTextColor(rarity.r, rarity.g, rarity.b);
	if ( card.greedCard ) then c.greed:Show(); else c.greed:Hide(); end

	c.name:SetText(card.name);
	c.headline:SetText(card.headline ~= "" and card.headline or "Unique");
	c.headline:SetFont(BODY_FONT, card.headline ~= "" and 20 or 13);
	c.headline:SetTextColor(rarity.r, rarity.g, rarity.b);
	c.text:SetText(card.text);
	c.text:SetHeight(math.max(CARD_H - 68 - c.name:GetStringHeight() - c.headline:GetStringHeight() - 8 - 44, 12));

	if ( card.cap and card.cap > 1 ) then
		local width = CARD_W - 20;
		local have = math.min(card.held / card.cap, 1);
		local gain = math.min((card.after - card.held) / card.cap, 1 - have);
		c.barHave:SetWidth(math.max(have > 0 and 2 or 0.01, width * have));
		c.barGain:SetWidth(math.max(gain > 0 and 3 or 0.01, width * gain));
		c.barGain:SetVertexColor(rarity.r, rarity.g, rarity.b);
		c.stack:SetText(Unit(card, card.held) .. " > |cffffffff" .. Unit(card, card.after) .. "|r");
		c.cap:SetText("max " .. Unit(card, card.cap));
		c.barBg:Show(); c.barHave:Show(); c.barGain:Show(); c.stack:Show(); c.cap:Show();
	else
		c.barBg:Hide(); c.barHave:Hide(); c.barGain:Hide(); c.stack:Hide(); c.cap:Hide();
	end

	if ( card.status ~= 0 ) then
		local why = (card.status == 1 and #card.owners > 0) and (table.concat(card.owners, ", ") .. " only")
			or STATUS_TEXT[card.status] or STATUS_TEXT[8];
		c.ribbonText:SetText(why);
		c.ribbon:Show(); c.ribbonText:Show();
		c:SetAlpha(0.6);
	else
		c.ribbon:Hide(); c.ribbonText:Hide();
		c:SetAlpha(1);
	end
	c.stamp:Hide();

	local k = 0;
	for name, code in pairs(draft.looks) do
		if ( code == card.code and k < #c.watch ) then
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
-- The one button

local take = CreateFrame("Button", nil, frame, "UIPanelButtonTemplate");
take:SetWidth(120);
take:SetHeight(22);
take:SetPoint("BOTTOM", frame, "BOTTOM", 0, PAD);

-- Taking a card ends the deal: the hand goes the moment you pick.
Take = function()
	if ( not draft or draft.taken ) then return; end
	local card = CardByCode(draft.selected);
	if ( not card or card.status ~= 0 ) then return; end
	if ( not demo ) then
		Request("PICK", card.code);
	end
	draft.taken = { name = Me(), code = card.code };
	frame:Hide();
end
take:SetScript("OnClick", Take);

Refresh = function()
	if ( not draft ) then return; end
	local count = #draft.cards;
	local width = math.max(PAD * 2 + count * CARD_W + (count - 1) * CARD_GAP, 320);
	frame:SetWidth(width);
	frame:SetHeight(HAND_TOP + CARD_H + 16 + 22 + PAD);
	local room = (UIParent:GetWidth() or width) - 40;
	frame:SetScale(width > room and room / width or 1);

	waveText:SetText(string.format("Wave %d / %d", draft.wave, draft.final));

	for i, card in ipairs(draft.cards) do
		DrawCard(cards[i] or MakeCard(i), card, i, count);
	end
	for i = count + 1, #cards do cards[i]:Hide(); cards[i].data = nil; end

	local card = CardByCode(draft.selected);
	if ( card and card.status == 0 ) then
		take:SetText("Take");
		take:Enable();
	elseif ( card ) then
		take:SetText((card.status == 1 and #card.owners > 0) and (card.owners[1] .. " only")
			or STATUS_TEXT[card.status] or STATUS_TEXT[8]);
		take:Disable();
	end
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

-- Somebody took a card - you or a party member: the deal is over, the hand goes.
function handlers.TAKEN(f)
	if ( not draft or not SameGuid(draft.tag, f[1]) ) then return; end
	draft.taken = { name = f[2], code = tonumber(f[3]) };
	frame:Hide();
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
			  text = "Lost when you leave the Hold.",
			  held = 0, after = 0, cap = 0, unit = "", status = myClass == 4 and 0 or 1, owners = myClass == 4 and {} or { "Kaelen" } },
			{ code = 208, kind = "I", tier = 5, icon = "I160807", name = "Shard of the Defiler", headline = "Crusader", greedCard = true,
			  text = "Lost when you leave the Hold.",
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
