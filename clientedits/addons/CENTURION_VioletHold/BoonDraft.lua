-- CENTURION_VioletHold / BoonDraft - the Ethereal Broker's offers as a hand of cards.
--
-- Talking to a broker in the Violet Hold, the server (violet_hold_boons.cpp,
-- npc_violet_hold_boon_brokerAI::SendDeal) sends the hand on the CCGAME addon
-- whisper before the usual gossip menu, and this file puts the cards up and
-- closes that menu. Without this addon the gossip list works as it always did.
-- Fields are split on '^'; <broker> is the broker's guid as UnitGUID writes it.
--
--    VHBOON:DEAL:<broker>^<wave>^<final wave>^<greed>^<abundance>^<hoarder>^<second opinions>^<common only>[^<name>^<class id>]...
--    VHBOON:CARD:<broker>^<code>^<B|S|I>^<tier 1-6>^<run-wide>^<greed card>^<S<spell>|I<item>>
--                ^<name>^<headline>^<text>^<held>^<after>^<cap>^<unit>^<status>[^<who can take it>]...
--    VHBOON:DECK:<broker>[^<tier>^<name>^<value>]...
--    VHBOON:SHOW:<broker>
--    VHBOON:LOOK:<broker>^<name>^<code>         (999 = looking at nothing)
--    VHBOON:TAKEN:<broker>^<name>^<code>
--    VHBOON:GONE:<broker>
--    VHBOON:REROLLED:<broker>^<name>
--
-- In a run with the shop (Centurion.VioletHold.Shop.Enable) the broker on the
-- entrance landing is a shop keeper. His DEAL ... SHOW block also carries
--
--    VHBOON:SHOP:<keeper>^<your shards>^<shards per wave>^<shards per killing blow>   (after DEAL)
--    VHBOON:PRICE:<keeper>^<code>^<price now>^<full price>^<waves discounted>          (after each CARD)
--
-- and arrives again whenever the board or your shards change while the window is
-- open. Buying keeps the window open on the restocked board.
--
-- and the addon answers on CCGAMEREQ, whispering itself:
--
--    VHBOON:PICK:<broker>:<code>     VHBOON:LOOK:<broker>:<code>     VHBOON:REROLL:<broker>:0
--    VHBOON:BUY:<keeper>:<code>
--
-- The hand is shared: the party sees the same cards, and the first one anybody
-- takes ends the deal. <status> is the server's PickResult for this character.
-- Closing the window sends LOOK 999, which at the shop also stops the updates.
--
-- /vhdraft demo  deals a made-up hand, to look at the frame outside the Hold.
-- /vhdraft shop  the same for the shop.

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
	{ name = "Artifact",  r = 0.90, g = 0.80, b = 0.50 },   -- class boons (server tier 6), WoW's artifact gold
};
local GOLD = { r = 0.95, g = 0.81, b = 0.42 };
local BARGAIN = { r = 0.45, g = 0.90, b = 0.50 };   -- a shop card that has got cheaper

-- The shop's look: violet glass, cards on display with hanging price tags.
local TEX = "Interface\\AddOns\\CENTURION_VioletHold\\Textures\\";
local SHARD_ICON = "Interface\\Icons\\INV_Enchant_ShardBrilliantLarge";
local TITLE_FONT = "Fonts\\MORPHEUS.TTF";
local SHOP_W, SHOP_H, SHOP_GAP, SHOP_PAD = 156, 214, 16, 22;
local SHOP_TOP = 96;           -- where the cards start, under the header
local SHOP_LIFT = 8;           -- how far the picked card rises
local SHOP_MIN_W = 640;
local SHOP_FRAME_H = 448;
local ARCANE = { r = 0.72, g = 0.61, b = 1.00 };     -- the frame's lilac trim
local LILAC = { r = 0.65, g = 0.60, b = 0.78 };      -- small print on violet
local VIOLET_DEEP = { r = 0.05, g = 0.035, b = 0.094 };
local VIOLET_TOP = { r = 0.106, g = 0.067, b = 0.192 };

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
-- The shop's dressing: everything here shows only while the hand is a shop
-- (ApplyLook). The broker's draft keeps the plain panel above.

local shop = { regions = {} };

local function ShopRegion(region)
	shop.regions[#shop.regions + 1] = region;
	region:Hide();
	return region;
end

local function Tex(parent, layer, file)
	local t = parent:CreateTexture(nil, layer or "ARTWORK");
	t:SetTexture(file or WHITE);
	return t;
end

-- Violet glass: dark at the foot, a little lighter at the head.
shop.bg = ShopRegion(Tex(frame, "BORDER"));
shop.bg:SetPoint("TOPLEFT", frame, "TOPLEFT", 1, -1);
shop.bg:SetPoint("BOTTOMRIGHT", frame, "BOTTOMRIGHT", -1, 1);
shop.bg:SetGradientAlpha("VERTICAL", VIOLET_DEEP.r, VIOLET_DEEP.g, VIOLET_DEEP.b, 0.97, VIOLET_TOP.r, VIOLET_TOP.g, VIOLET_TOP.b, 0.97);

-- A soft lilac glow round the frame: two faint outlines outside the border.
local function Outline(inset, alpha)
	local edges = {
		{ "TOPLEFT", "TOPRIGHT", -inset, inset, inset, inset },
		{ "BOTTOMLEFT", "BOTTOMRIGHT", -inset, -inset, inset, -inset },
	};
	for _, e in ipairs(edges) do
		local t = ShopRegion(Tex(frame, "BACKGROUND"));
		t:SetPoint(e[1], frame, e[1], e[3], e[4]);
		t:SetPoint(e[2], frame, e[2], e[5], e[6]);
		t:SetHeight(1);
		t:SetVertexColor(ARCANE.r, ARCANE.g, ARCANE.b, alpha);
	end
	for _, side in ipairs({ { "LEFT", -inset }, { "RIGHT", inset } }) do
		local t = ShopRegion(Tex(frame, "BACKGROUND"));
		t:SetPoint("TOP" .. side[1], frame, "TOP" .. side[1], side[2], inset);
		t:SetPoint("BOTTOM" .. side[1], frame, "BOTTOM" .. side[1], side[2], -inset);
		t:SetWidth(1);
		t:SetVertexColor(ARCANE.r, ARCANE.g, ARCANE.b, alpha);
	end
end
Outline(2, 0.22);
Outline(4, 0.08);

-- A lilac jewel on every corner, over the border.
local jewels = CreateFrame("Frame", nil, frame);
jewels:SetAllPoints(frame);
jewels:SetFrameLevel(frame:GetFrameLevel() + 20);
for _, corner in ipairs({ "TOPLEFT", "TOPRIGHT", "BOTTOMLEFT", "BOTTOMRIGHT" }) do
	for _, layer in ipairs({ { 18, ARCANE, 1 }, { 15, VIOLET_DEEP, 1 }, { 8, ARCANE, 1 } }) do
		local t = ShopRegion(Tex(jewels, "OVERLAY", TEX .. "diamond"));
		t:SetWidth(layer[1]);
		t:SetHeight(layer[1]);
		t:SetPoint("CENTER", frame, corner, 0, 0);
		t:SetVertexColor(layer[2].r, layer[2].g, layer[2].b, layer[3]);
	end
end

-- The heading: a gold title between two lilac rules, the wave under it.
shop.title = ShopRegion(frame:CreateFontString(nil, "OVERLAY"));
shop.title:SetFont(TITLE_FONT, 24);
shop.title:SetShadowOffset(1, -1);
shop.title:SetShadowColor(0, 0, 0, 0.9);
shop.title:SetTextColor(GOLD.r, GOLD.g, GOLD.b);
shop.title:SetPoint("TOP", frame, "TOP", 0, -20);
shop.title:SetText("The Broker's Wares");

shop.ruleLeft = ShopRegion(Tex(frame, "ARTWORK"));
shop.ruleLeft:SetHeight(1);
shop.ruleLeft:SetWidth(90);
shop.ruleLeft:SetPoint("RIGHT", shop.title, "LEFT", -14, -1);
shop.ruleLeft:SetGradientAlpha("HORIZONTAL", ARCANE.r, ARCANE.g, ARCANE.b, 0, ARCANE.r, ARCANE.g, ARCANE.b, 0.8);
shop.ruleRight = ShopRegion(Tex(frame, "ARTWORK"));
shop.ruleRight:SetHeight(1);
shop.ruleRight:SetWidth(90);
shop.ruleRight:SetPoint("LEFT", shop.title, "RIGHT", 14, -1);
shop.ruleRight:SetGradientAlpha("HORIZONTAL", ARCANE.r, ARCANE.g, ARCANE.b, 0.8, ARCANE.r, ARCANE.g, ARCANE.b, 0);

shop.sub = ShopRegion(Text(frame, 10));
shop.sub:SetPoint("TOP", shop.title, "BOTTOM", 0, -5);
shop.sub:SetTextColor(LILAC.r, LILAC.g, LILAC.b);

-- Your shards: a crystal medallion top right, its label beside it.
shop.purse = CreateFrame("Frame", nil, frame);
shop.purse:SetWidth(58);
shop.purse:SetHeight(58);
shop.purse:SetPoint("TOPRIGHT", frame, "TOPRIGHT", -34, -12);
ShopRegion(shop.purse);
local purseGlow = Tex(shop.purse, "BACKGROUND", TEX .. "glow");
purseGlow:SetBlendMode("ADD");
purseGlow:SetWidth(96);
purseGlow:SetHeight(96);
purseGlow:SetPoint("CENTER", shop.purse, "CENTER", 0, 0);
purseGlow:SetVertexColor(0.55, 0.40, 1.00, 0.55);
local purseRim = Tex(shop.purse, "BORDER", TEX .. "circle");
purseRim:SetAllPoints(shop.purse);
purseRim:SetVertexColor(ARCANE.r, ARCANE.g, ARCANE.b, 1);
local purseFace = Tex(shop.purse, "ARTWORK", TEX .. "circle");
purseFace:SetPoint("TOPLEFT", shop.purse, "TOPLEFT", 1.5, -1.5);
purseFace:SetPoint("BOTTOMRIGHT", shop.purse, "BOTTOMRIGHT", -1.5, 1.5);
purseFace:SetGradient("VERTICAL", 0.086, 0.051, 0.173, 0.227, 0.137, 0.439);
local purseIcon = Tex(shop.purse, "OVERLAY", SHARD_ICON);
purseIcon:SetWidth(18);
purseIcon:SetHeight(18);
purseIcon:SetPoint("TOP", shop.purse, "TOP", 0, -8);
purseIcon:SetTexCoord(0.08, 0.92, 0.08, 0.92);
shop.count = shop.purse:CreateFontString(nil, "OVERLAY");
shop.count:SetFont(TITLE_FONT, 19);
shop.count:SetShadowOffset(1, -1);
shop.count:SetShadowColor(0, 0, 0, 0.9);
shop.count:SetTextColor(GOLD.r, GOLD.g, GOLD.b);
shop.count:SetPoint("BOTTOM", shop.purse, "BOTTOM", 0, 9);

shop.purseLabel = ShopRegion(Text(frame, 10));
shop.purseLabel:SetPoint("TOPRIGHT", shop.purse, "TOPLEFT", -10, -9);
shop.purseLabel:SetJustifyH("RIGHT");
shop.purseLabel:SetTextColor(LILAC.r, LILAC.g, LILAC.b);
shop.purseLabel:SetText("YOUR SHARDS");
shop.income = ShopRegion(Text(frame, 10));
shop.income:SetPoint("TOPRIGHT", shop.purseLabel, "BOTTOMRIGHT", 0, -3);
shop.income:SetJustifyH("RIGHT");
shop.income:SetTextColor(0.49, 0.45, 0.62);
-- Boon of the Usurer's interest and Boon of Safekeeping's locks, when held
shop.extras = ShopRegion(Text(frame, 10));
shop.extras:SetPoint("TOPRIGHT", shop.income, "BOTTOMRIGHT", 0, -3);
shop.extras:SetJustifyH("RIGHT");
shop.extras:SetTextColor(0.49, 0.45, 0.62);

-- The rule of the board, or what waiting does to the picked card.
shop.footer = ShopRegion(Text(frame, 11));
shop.footer:SetPoint("BOTTOM", frame, "BOTTOM", 0, 16);
shop.footer:SetTextColor(0.55, 0.51, 0.69);
shop.footRuleLeft = ShopRegion(Tex(frame, "ARTWORK"));
shop.footRuleLeft:SetHeight(1);
shop.footRuleLeft:SetWidth(60);
shop.footRuleLeft:SetPoint("RIGHT", shop.footer, "LEFT", -12, 0);
shop.footRuleLeft:SetGradientAlpha("HORIZONTAL", 0.29, 0.23, 0.48, 0, 0.29, 0.23, 0.48, 1);
shop.footRuleRight = ShopRegion(Tex(frame, "ARTWORK"));
shop.footRuleRight:SetHeight(1);
shop.footRuleRight:SetWidth(60);
shop.footRuleRight:SetPoint("LEFT", shop.footer, "RIGHT", 12, 0);
shop.footRuleRight:SetGradientAlpha("HORIZONTAL", 0.29, 0.23, 0.48, 1, 0.29, 0.23, 0.48, 0);

shop.soldOut = ShopRegion(Text(frame, 13));
shop.soldOut:SetPoint("TOP", frame, "TOP", 0, -SHOP_TOP - 90);
shop.soldOut:SetTextColor(LILAC.r, LILAC.g, LILAC.b);
shop.soldOut:SetText("Sold out. The next cleared wave restocks the board.");

local look;   -- "shop" or "draft": what the frame is dressed as now
local function ApplyLook(isShop)
	local want = isShop and "shop" or "draft";
	if ( look == want ) then return; end
	look = want;
	if ( isShop ) then
		frame:SetBackdropColor(VIOLET_DEEP.r, VIOLET_DEEP.g, VIOLET_DEEP.b, 0.97);
		frame:SetBackdropBorderColor(0.42, 0.31, 0.69, 1);
		title:Hide();
		waveText:Hide();
	else
		frame:SetBackdropColor(0.04, 0.04, 0.06, 0.94);
		frame:SetBackdropBorderColor(1, 1, 1, 0.10);
		title:Show();
		waveText:Show();
	end
	for _, region in ipairs(shop.regions) do
		if ( isShop ) then region:Show(); else region:Hide(); end
	end
end

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

-- The real spell or item tooltip for a card (the broker's and the shop's).
local function CardTooltip(self)
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
	c.stack:SetWidth(80);
	c.stack:SetJustifyH("LEFT");
	c.stack:SetTextColor(0.62, 0.62, 0.68);
	c.cap = Text(c, 9);
	c.cap:SetPoint("TOPRIGHT", c.barBg, "BOTTOMRIGHT", 0, -4);
	c.cap:SetWidth(50);
	c.cap:SetJustifyH("RIGHT");
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
	c:SetScript("OnEnter", CardTooltip);
	c:SetScript("OnLeave", function() if ( GameTooltip ) then GameTooltip:Hide(); end end);

	cards[i] = c;
	return c;
end

local function Unit(card, value)
	return value .. (card.unit or "");
end

-- Shards still missing for a shop card (0 when you can pay, or no shop).
local function ShortOf(card)
	if ( not draft or not draft.shop or not card.price ) then
		return 0;
	end
	return math.max(card.price - (draft.shop.shards or 0), 0);
end

local function Shards(n)
	return n .. (n == 1 and " shard" or " shards");
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
	c.kind:SetText(card.runWide and "RUN-WIDE" or (card.tier == 6 and "ARTIFACT") or kinds[card.kind] or "");
	c.kind:SetTextColor(rarity.r, rarity.g, rarity.b);

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
		c.stack:SetText(card.held .. " > |cffffffff" .. Unit(card, card.after) .. "|r");
		c.cap:SetText(Unit(card, card.cap) .. " max");
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
-- Shop cards: a glass case with a round icon, and under it a hanging price tag
-- and a line saying how short you are or how much it has come down.

local shopCols = {};

local function MakeShopCard(i)
	local col = CreateFrame("Frame", nil, frame);
	col:SetWidth(SHOP_W);
	col:SetHeight(SHOP_H + 64);

	local c = CreateFrame("Button", nil, col);
	c:SetWidth(SHOP_W);
	c:SetHeight(SHOP_H);
	c:SetPoint("TOP", col, "TOP", 0, 0);
	c:SetBackdrop({ bgFile = WHITE, edgeFile = WHITE, edgeSize = 1 });
	c:SetBackdropColor(0.07, 0.047, 0.133, 1);
	c:RegisterForClicks("LeftButtonUp");
	col.card = c;
	c.col = col;

	-- picked: a halo of the card's colour behind it
	col.halo = Tex(col, "BACKGROUND", TEX .. "glow");
	col.halo:SetBlendMode("ADD");
	col.halo:SetPoint("TOPLEFT", c, "TOPLEFT", -30, 30);
	col.halo:SetPoint("BOTTOMRIGHT", c, "BOTTOMRIGHT", 30, -30);

	-- the glass: the card's colour lit from the top
	c.shine = Tex(c, "BORDER");
	c.shine:SetPoint("TOPLEFT", c, "TOPLEFT", 1, -1);
	c.shine:SetPoint("TOPRIGHT", c, "TOPRIGHT", -1, -1);
	c.shine:SetHeight(110);

	c.kind = Text(c, 9);
	c.kind:SetPoint("TOPLEFT", c, "TOPLEFT", 8, -8);

	-- NEW / CHEAPER, a tab on the right edge
	c.flagBg = Tex(c, "ARTWORK");
	c.flagBg:SetPoint("TOPRIGHT", c, "TOPRIGHT", 1, -10);
	c.flagBg:SetHeight(13);
	c.flag = Text(c, 9);
	c.flag:SetShadowOffset(0, 0);
	c.flag:SetPoint("RIGHT", c.flagBg, "RIGHT", -6, 0);
	c.flag:SetTextColor(0.04, 0.04, 0.06);

	c.watch = {};
	for k = 1, 5 do
		local w = Text(c, 10);
		w:SetPoint("TOPRIGHT", c, "TOPRIGHT", -8 - (k - 1) * 12, -30);
		c.watch[k] = w;
	end

	-- the icon, round, in a ring of the card's colour
	c.glowDisc = Tex(c, "ARTWORK", TEX .. "circle");
	c.glowDisc:SetWidth(62);
	c.glowDisc:SetHeight(62);
	c.glowDisc:SetPoint("TOP", c, "TOP", 0, -28);
	c.icon = Tex(c, "ARTWORK");
	c.icon:SetWidth(52);
	c.icon:SetHeight(52);
	c.icon:SetPoint("CENTER", c.glowDisc, "CENTER", 0, 0);
	c.ring = Tex(c, "OVERLAY", TEX .. "ring");
	c.ring:SetAllPoints(c.glowDisc);

	c.name = Text(c, 12);
	c.name:SetPoint("TOP", c, "TOP", 0, -98);
	c.name:SetWidth(SHOP_W - 16);
	c.name:SetTextColor(0.96, 0.94, 1.0);

	c.headline = Text(c, 20);
	c.headline:SetPoint("TOP", c.name, "BOTTOM", 0, -3);

	c.text = Text(c, 10);
	c.text:SetPoint("TOP", c.headline, "BOTTOM", 0, -3);
	c.text:SetWidth(SHOP_W - 20);
	c.text:SetJustifyV("TOP");
	c.text:SetTextColor(LILAC.r, LILAC.g, LILAC.b);

	c.barBg = Tex(c, "ARTWORK");
	c.barBg:SetVertexColor(1, 1, 1, 0.08);
	c.barBg:SetPoint("BOTTOMLEFT", c, "BOTTOMLEFT", 10, 22);
	c.barBg:SetWidth(SHOP_W - 20);
	c.barBg:SetHeight(2);
	c.barHave = Tex(c, "OVERLAY");
	c.barHave:SetVertexColor(0.55, 0.52, 0.66);
	c.barHave:SetPoint("TOPLEFT", c.barBg, "TOPLEFT", 0, 0);
	c.barHave:SetHeight(2);
	c.barGain = Tex(c, "OVERLAY");
	c.barGain:SetPoint("TOPLEFT", c.barHave, "TOPRIGHT", 0, 0);
	c.barGain:SetHeight(2);
	c.stack = Text(c, 9);
	c.stack:SetPoint("TOPLEFT", c.barBg, "BOTTOMLEFT", 0, -4);
	c.stack:SetWidth(84);
	c.stack:SetJustifyH("LEFT");
	c.stack:SetTextColor(LILAC.r, LILAC.g, LILAC.b);
	c.cap = Text(c, 9);
	c.cap:SetPoint("TOPRIGHT", c.barBg, "BOTTOMRIGHT", 0, -4);
	c.cap:SetWidth(50);
	c.cap:SetJustifyH("RIGHT");
	c.cap:SetTextColor(0.45, 0.41, 0.58);

	-- why it is not yours (wrong class, maxed...), over the foot of the card
	c.ribbon = Tex(c, "OVERLAY");
	c.ribbon:SetVertexColor(0, 0, 0, 0.75);
	c.ribbon:SetPoint("BOTTOMLEFT", c, "BOTTOMLEFT", 1, 1);
	c.ribbon:SetPoint("BOTTOMRIGHT", c, "BOTTOMRIGHT", -1, 1);
	c.ribbon:SetHeight(18);
	c.ribbonText = Text(c, 10);
	c.ribbonText:SetPoint("CENTER", c.ribbon, "CENTER", 0, 0);
	c.ribbonText:SetTextColor(0.85, 0.85, 0.9);

	-- under the card: a thread, the price tag, and the note
	col.thread = Tex(col, "ARTWORK");
	col.thread:SetWidth(1);
	col.thread:SetHeight(10);
	col.thread:SetPoint("TOP", c, "BOTTOM", 0, 0);

	col.tag = CreateFrame("Frame", nil, col);
	col.tag:SetHeight(30);
	col.tag:SetPoint("TOP", col.thread, "BOTTOM", 0, 0);
	col.tagLeft = Tex(col.tag, "ARTWORK", TEX .. "tagcap");
	col.tagLeft:SetWidth(10);
	col.tagLeft:SetPoint("TOPLEFT", col.tag, "TOPLEFT", 0, 0);
	col.tagLeft:SetPoint("BOTTOMLEFT", col.tag, "BOTTOMLEFT", 0, 0);
	col.tagRight = Tex(col.tag, "ARTWORK", TEX .. "tagcap");
	col.tagRight:SetTexCoord(1, 0, 0, 1);
	col.tagRight:SetWidth(10);
	col.tagRight:SetPoint("TOPRIGHT", col.tag, "TOPRIGHT", 0, 0);
	col.tagRight:SetPoint("BOTTOMRIGHT", col.tag, "BOTTOMRIGHT", 0, 0);
	col.tagMid = Tex(col.tag, "ARTWORK");
	col.tagMid:SetPoint("TOPLEFT", col.tagLeft, "TOPRIGHT", 0, 0);
	col.tagMid:SetPoint("BOTTOMRIGHT", col.tagRight, "BOTTOMLEFT", 0, 0);
	col.tagText = col.tag:CreateFontString(nil, "OVERLAY");
	col.tagText:SetFont(TITLE_FONT, 17);
	col.tagText:SetPoint("CENTER", col.tag, "CENTER", 8, 0);
	col.tagIcon = Tex(col.tag, "OVERLAY", SHARD_ICON);
	col.tagIcon:SetWidth(13);
	col.tagIcon:SetHeight(13);
	col.tagIcon:SetTexCoord(0.08, 0.92, 0.08, 0.92);
	col.tagIcon:SetPoint("RIGHT", col.tagText, "LEFT", -5, 0);

	col.note = Text(col, 10);
	col.note:SetPoint("TOP", col.tag, "BOTTOM", 0, -4);

	-- Boon of Safekeeping: the padlock beside the tag
	col.lock = CreateFrame("Button", nil, col);
	col.lock:SetWidth(18);
	col.lock:SetHeight(18);
	col.lock:SetPoint("RIGHT", col.tag, "LEFT", -4, 0);
	col.lock:RegisterForClicks("LeftButtonUp");
	col.lock.icon = Tex(col.lock, "ARTWORK", TEX .. "lock");
	col.lock.icon:SetAllPoints(col.lock);
	col.lock:SetScript("OnClick", function(self)
		local card = col.card.data;
		if ( card and self.clickable ) then Request("LOCK", card.code); end
	end);
	col.lock:SetScript("OnEnter", function(self)
		if ( not GameTooltip or not self.tip ) then return; end
		GameTooltip:SetOwner(self, "ANCHOR_RIGHT");
		GameTooltip:SetText(self.tip);
		GameTooltip:Show();
	end);
	col.lock:SetScript("OnLeave", function() if ( GameTooltip ) then GameTooltip:Hide(); end end);

	c:SetScript("OnClick", function(self) if ( self.data ) then Select(self.data.code); end end);
	c:SetScript("OnDoubleClick", function(self) if ( self.data ) then Select(self.data.code); Take(); end end);
	c:SetScript("OnEnter", CardTooltip);
	c:SetScript("OnLeave", function() if ( GameTooltip ) then GameTooltip:Hide(); end end);

	shopCols[i] = col;
	return col;
end

local function TagColors(t, top, bottom)
	for _, piece in ipairs({ t.tagLeft, t.tagMid, t.tagRight }) do
		piece:SetGradient("VERTICAL", bottom[1], bottom[2], bottom[3], top[1], top[2], top[3]);
	end
end

local function DrawShopCard(col, card, index, left)
	local c = col.card;
	local rarity = RARITY[card.tier] or RARITY[1];
	local selected = draft.selected == card.code;
	local short = ShortOf(card);
	local dim = (short > 0 or card.status ~= 0) and not selected;
	c.data = card;

	col:ClearAllPoints();
	col:SetPoint("TOPLEFT", frame, "TOPLEFT", left + (index - 1) * (SHOP_W + SHOP_GAP), -SHOP_TOP + (selected and SHOP_LIFT or 0));
	col:SetFrameLevel(frame:GetFrameLevel() + (selected and 10 or 2));
	c:SetFrameLevel(col:GetFrameLevel() + 1);
	col.tag:SetFrameLevel(col:GetFrameLevel() + 1);
	col:SetAlpha(dim and 0.6 or 1);

	if ( selected ) then
		c:SetBackdropBorderColor(GOLD.r, GOLD.g, GOLD.b, 1);
		col.halo:SetVertexColor(rarity.r, rarity.g, rarity.b, 0.5);
		col.halo:Show();
	else
		c:SetBackdropBorderColor(rarity.r, rarity.g, rarity.b, 0.45);
		col.halo:Hide();
	end
	c.shine:SetGradientAlpha("VERTICAL", rarity.r, rarity.g, rarity.b, 0, rarity.r, rarity.g, rarity.b, 0.20);

	local kinds = { S = "CLASS SPELL", I = "LEGENDARY" };
	c.kind:SetText(card.runWide and "RUN-WIDE" or (card.tier == 6 and "ARTIFACT") or kinds[card.kind] or "");
	c.kind:SetTextColor(rarity.r, rarity.g, rarity.b);

	if ( card.flag ) then
		c.flag:SetText(card.flag);
		c.flagBg:SetWidth((c.flag:GetStringWidth() or 30) + 12);
		local tint = card.flag == "NEW" and { 0.62, 0.82, 1.0 } or { BARGAIN.r, BARGAIN.g, BARGAIN.b };
		c.flagBg:SetVertexColor(tint[1], tint[2], tint[3], 1);
		c.flag:Show(); c.flagBg:Show();
	else
		c.flag:Hide(); c.flagBg:Hide();
	end

	c.glowDisc:SetVertexColor(rarity.r * 0.35, rarity.g * 0.35, rarity.b * 0.35, 1);
	c.ring:SetVertexColor(rarity.r, rarity.g, rarity.b, 1);
	if ( SetPortraitToTexture ) then
		SetPortraitToTexture(c.icon, IconOf(card));
	else
		c.icon:SetTexture(IconOf(card));
	end

	c.name:SetText(card.name);
	c.headline:SetText(card.headline ~= "" and card.headline or "Unique");
	c.headline:SetFont(BODY_FONT, (card.headline ~= "" and string.len(card.headline) <= 6) and 20 or 14);
	c.headline:SetTextColor(rarity.r, rarity.g, rarity.b);
	c.text:SetText(card.text);
	c.text:SetHeight(math.max(SHOP_H - 98 - c.name:GetStringHeight() - c.headline:GetStringHeight() - 6 - 44, 12));

	if ( card.cap and card.cap > 1 ) then
		local width = SHOP_W - 20;
		local have = math.min(card.held / card.cap, 1);
		local gain = math.min((card.after - card.held) / card.cap, 1 - have);
		c.barHave:SetWidth(math.max(have > 0 and 2 or 0.01, width * have));
		c.barGain:SetWidth(math.max(gain > 0 and 3 or 0.01, width * gain));
		c.barGain:SetVertexColor(rarity.r, rarity.g, rarity.b);
		c.stack:SetText(card.held .. " > |cffffffff" .. Unit(card, card.after) .. "|r");
		c.cap:SetText(Unit(card, card.cap) .. " max");
		c.barBg:Show(); c.barHave:Show(); c.barGain:Show(); c.stack:Show(); c.cap:Show();
	else
		c.barBg:Hide(); c.barHave:Hide(); c.barGain:Hide(); c.stack:Hide(); c.cap:Hide();
	end

	if ( card.status ~= 0 ) then
		c.ribbonText:SetText((card.status == 1 and #card.owners > 0) and (table.concat(card.owners, ", ") .. " only")
			or STATUS_TEXT[card.status] or STATUS_TEXT[8]);
		c.ribbon:Show(); c.ribbonText:Show();
	else
		c.ribbon:Hide(); c.ribbonText:Hide();
	end

	local k = 0;
	for name, code in pairs(draft.looks) do
		if ( code == card.code and k < #c.watch ) then
			k = k + 1;
			c.watch[k]:SetText(string.sub(name, 1, 1));
			c.watch[k]:SetTextColor(ClassColor(draft.classOf[name]));
			c.watch[k]:Show();
		end
	end
	for j = k + 1, #c.watch do c.watch[j]:Hide(); end

	-- the tag: gold, green once it has come down, dark red while you are short
	local price = card.price or 0;
	local discounted = card.basePrice and price < card.basePrice;
	col.tagText:SetText(price > 0 and tostring(price) or "Free");
	if ( short > 0 ) then
		TagColors(col, { 0.23, 0.11, 0.14 }, { 0.17, 0.08, 0.10 });
		col.tagText:SetTextColor(0.85, 0.51, 0.51);
		col.thread:SetVertexColor(0.35, 0.19, 0.25, 1);
		col.note:SetText(short .. " SHORT");
		col.note:SetTextColor(0.79, 0.48, 0.48);
	elseif ( discounted ) then
		TagColors(col, { 0.18, 0.48, 0.27 }, { 0.11, 0.29, 0.16 });
		col.tagText:SetTextColor(0.85, 1.0, 0.89);
		col.thread:SetVertexColor(rarity.r, rarity.g, rarity.b, 0.6);
		col.note:SetText(math.floor(100 - price * 100 / card.basePrice + 0.5) .. "% OFF  ·  WAS " .. card.basePrice);
		col.note:SetTextColor(BARGAIN.r, BARGAIN.g, BARGAIN.b);
	else
		TagColors(col, { 0.79, 0.64, 0.29 }, { 0.54, 0.42, 0.14 });
		col.tagText:SetTextColor(0.10, 0.07, 0.02);
		col.thread:SetVertexColor(rarity.r, rarity.g, rarity.b, 0.6);
		col.note:SetText("");
	end
	col.tag:SetWidth(math.max(76, (col.tagText:GetStringWidth() or 12) + 13 + 5 + 26));

	-- the padlock: yours (gold, click to take it off), somebody else's (lilac),
	-- or an empty one you could put on (faint, click to lock)
	local s = draft.shop;
	local lock = col.lock;
	lock.clickable = false;
	if ( card.lockedByMe ) then
		lock.icon:SetVertexColor(GOLD.r, GOLD.g, GOLD.b, 1);
		lock.tip = "Locked by you: it won't reroll, and it still gets cheaper. Click to unlock.";
		lock.clickable = true;
		lock:Show();
	elseif ( card.lockedBy ) then
		lock.icon:SetVertexColor(ARCANE.r, ARCANE.g, ARCANE.b, 1);
		lock.tip = "Locked by " .. card.lockedBy .. ": it won't reroll.";
		lock:Show();
	elseif ( (s.locksMax or 0) > 0 ) then
		lock.icon:SetVertexColor(0.85, 0.82, 0.95, 0.35);
		lock.tip = string.format("Lock this card so it won't reroll (%d of %d locks in use).", s.locksUsed or 0, s.locksMax);
		lock.clickable = (s.locksUsed or 0) < s.locksMax;
		lock:Show();
	else
		lock.tip = nil;
		lock:Hide();
	end

	c:Show();
	col:Show();
end

-- What the next cleared wave does to the picked card, or the board's rule.
local function Forecast(card)
	local s = draft.shop;
	local reroll, cut = s.reroll or 50, s.discount or 25;
	if ( not card or not card.price or card.price == 0 ) then
		return string.format("Every wave, each card has a %d%% chance to vanish. If it stays, it gets %d%% cheaper.", reroll, cut);
	end
	local base = card.basePrice or card.price;
	local nextPrice = math.max(1, math.floor(base * ((100 - cut) / 100) ^ ((card.discounts or 0) + 1) + 0.5));
	-- your price comes off the board's by as much as Boon of the Haggler takes
	local off = (card.basePrice and card.price) and math.max(0, (math.max(1, math.floor(base * ((100 - cut) / 100) ^ (card.discounts or 0) + 0.5))) - card.price) or 0;
	nextPrice = math.max(1, nextPrice - off);
	if ( card.lockedBy ) then
		if ( nextPrice >= card.price ) then
			return "Locked: it stays on the board at this price.";
		end
		return string.format("Locked: it stays on the board and drops to %s next wave.", Shards(nextPrice));
	end
	if ( reroll >= 100 ) then
		return "If you wait, it leaves the board when the next wave is cleared.";
	end
	if ( nextPrice >= card.price ) then
		-- already as cheap as it gets
		if ( reroll <= 0 ) then
			return "If you wait, it stays at this price.";
		end
		return string.format("If you wait: a %d%% chance it leaves the board next wave.", reroll);
	end
	if ( reroll <= 0 ) then
		return string.format("If you wait, it drops to %s next wave.", Shards(nextPrice));
	end
	return string.format("If you wait: a %d%% chance it leaves the board next wave, otherwise it drops to %s.", reroll, Shards(nextPrice));
end

-- ---------------------------------------------------------------------------
-- The one button

local take = CreateFrame("Button", nil, frame, "UIPanelButtonTemplate");
take:SetWidth(120);
take:SetHeight(22);
take:SetPoint("BOTTOMLEFT", frame, "BOTTOMLEFT", PAD, PAD);

local reroll = CreateFrame("Button", nil, frame, "UIPanelButtonTemplate");
reroll:SetWidth(150);
reroll:SetHeight(22);
reroll:SetPoint("BOTTOMRIGHT", frame, "BOTTOMRIGHT", -PAD, PAD);
reroll:SetScript("OnClick", function()
	if ( not draft or draft.taken ) then return; end
	-- At the shop: redraw every unlocked card, for shards or a free reroll.
	if ( draft.shop ) then
		local s = draft.shop;
		local cost = (s.freeRerolls or 0) > 0 and 0 or (s.rerollCost or 0);
		if ( draft.rerolling or cost > (s.shards or 0) ) then return; end
		if ( demo ) then return; end
		draft.rerolling = true;
		draft.rerollingUntil = GetTime() + 2;
		Request("REROLL", 0);
		Refresh();
		return;
	end
	if ( (draft.secondOpinions or 0) < 1 ) then return; end
	reroll:Disable();
	Request("REROLL", 0);
end);

-- At the shop: any one player starts the next wave - between waves the one
-- waiting behind the doors, mid-fight the next one called in on top.
local readyButton = CreateFrame("Button", nil, frame, "UIPanelButtonTemplate");
readyButton:SetWidth(132);
readyButton:SetHeight(22);
readyButton:Hide();
readyButton:SetScript("OnClick", function()
	local r = draft and draft.shop and draft.ready;
	if ( not r or not r.waiting or draft.readying or demo ) then return; end
	draft.readying = true;
	draft.readyingUntil = GetTime() + 2;
	Request("READY", 0);
	Refresh();
end);

-- Taking a card ends the deal: the hand goes the moment you pick.
-- Buying one from the shop does not: the window stays up, and the server sends
-- the restocked board back. Until it does, the button waits.
Take = function()
	if ( not draft or draft.taken ) then return; end
	local card = CardByCode(draft.selected);
	if ( not card or card.status ~= 0 ) then return; end
	if ( draft.shop ) then
		if ( draft.buying or ShortOf(card) > 0 ) then return; end
		if ( demo ) then
			draft.shop.shards = draft.shop.shards - (card.price or 0);
			if ( PlaySound ) then PlaySound("LOOTWINDOWCOINSOUND"); end
			Refresh();
			return;
		end
		draft.buying = card.code;
		draft.buyingUntil = GetTime() + 2;   -- a refused buy (too far away) sends no board back
		Request("BUY", card.code);
		Refresh();
		return;
	end
	if ( not demo ) then
		Request("PICK", card.code);
	end
	draft.taken = { name = Me(), code = card.code };
	frame:Hide();
end
take:SetScript("OnClick", Take);

local function StatusText(card)
	return (card.status == 1 and #card.owners > 0) and (card.owners[1] .. " only")
		or STATUS_TEXT[card.status] or STATUS_TEXT[8];
end

local function FitToScreen(width)
	local room = (UIParent:GetWidth() or width) - 40;
	frame:SetScale(width > room and room / width or 1);
end

-- The shop: the cards in their cases, Buy under the one you picked.
local function RefreshShop()
	ApplyLook(true);
	local count = #draft.cards;
	local needed = SHOP_PAD * 2 + count * SHOP_W + math.max(count - 1, 0) * SHOP_GAP;
	local width = math.max(needed, SHOP_MIN_W);
	frame:SetWidth(width);
	frame:SetHeight(SHOP_FRAME_H);
	FitToScreen(width);

	local s = draft.shop;
	shop.sub:SetText(string.format("WAVE %d OF %d%s", draft.wave, draft.final,
		#draft.party > 1 and "  ·  SHARED WITH YOUR PARTY" or ""));
	shop.count:SetText(tostring(s.shards or 0));
	shop.income:SetText(string.format("+%d a wave  ·  +%d a kill", s.perWave or 0, s.perKill or 0));
	local extras = {};
	if ( (s.interest or 0) > 0 ) then extras[#extras + 1] = string.format("+%d interest", s.interest); end
	if ( (s.locksMax or 0) > 0 ) then extras[#extras + 1] = string.format("locks %d/%d", s.locksUsed or 0, s.locksMax); end
	shop.extras:SetText(table.concat(extras, "  ·  "));

	local left = SHOP_PAD + (width - needed) / 2;
	for i, card in ipairs(draft.cards) do
		DrawShopCard(shopCols[i] or MakeShopCard(i), card, i, left);
	end
	for i = count + 1, #shopCols do
		shopCols[i]:Hide(); shopCols[i].card:Hide(); shopCols[i].card.data = nil;
	end
	for i = 1, #cards do cards[i]:Hide(); cards[i].data = nil; end
	if ( count == 0 ) then shop.soldOut:Show(); else shop.soldOut:Hide(); end

	local card, index = nil, nil;
	for i, c in ipairs(draft.cards) do
		if ( c.code == draft.selected ) then card, index = c, i; end
	end
	shop.footer:SetText(Forecast(card));

	-- Reroll, top left: free while a Gambler reroll is left this wave
	local cost = (s.freeRerolls or 0) > 0 and 0 or (s.rerollCost or 0);
	reroll:ClearAllPoints();
	reroll:SetPoint("TOPLEFT", frame, "TOPLEFT", 18, -18);
	reroll:SetWidth(132);
	reroll:SetFrameLevel(frame:GetFrameLevel() + 12);
	if ( draft.rerolling ) then
		reroll:SetText("Rerolling...");
		reroll:Disable();
	elseif ( cost == 0 ) then
		reroll:SetText((s.freeRerolls or 0) > 1 and string.format("Reroll (%d free)", s.freeRerolls) or "Reroll (free)");
		reroll:Enable();
	else
		reroll:SetText("Reroll  ·  " .. Shards(cost));
		if ( cost > (s.shards or 0) ) then reroll:Disable(); else reroll:Enable(); end
	end
	reroll:Show();

	-- The next-wave button, under Reroll
	local r = draft.ready;
	readyButton:ClearAllPoints();
	readyButton:SetPoint("TOPLEFT", reroll, "BOTTOMLEFT", 0, -6);
	readyButton:SetFrameLevel(frame:GetFrameLevel() + 12);
	if ( r and r.waiting ) then
		if ( draft.readying ) then
			readyButton:SetText("Sending...");
			readyButton:Disable();
		elseif ( r.call == 2 ) then
			readyButton:SetText(r.wave and string.format("Call wave %d now", r.wave) or "Call next wave now");
			readyButton:Enable();
		else
			readyButton:SetText(r.wave and string.format("Start wave %d", r.wave) or "Start next wave");
			readyButton:Enable();
		end
		readyButton:Show();
	else
		readyButton:Hide();
	end

	take:ClearAllPoints();
	if ( not card ) then
		take:Hide();
		return;
	end
	take:SetWidth(120);
	take:SetPoint("TOP", shopCols[index].note, "BOTTOM", 0, -3);
	take:SetFrameLevel(shopCols[index]:GetFrameLevel() + 3);
	take:Show();
	if ( card.status ~= 0 ) then
		take:SetText(StatusText(card));
		take:Disable();
	elseif ( draft.buying ) then
		take:SetText("Buying...");
		take:Disable();
	elseif ( ShortOf(card) > 0 ) then
		take:SetText("Not enough");
		take:Disable();
	else
		take:SetText("Buy");
		take:Enable();
	end
end

Refresh = function()
	if ( not draft ) then return; end
	if ( draft.shop ) then
		RefreshShop();
		return;
	end

	ApplyLook(false);
	readyButton:Hide();
	for i = 1, #shopCols do
		shopCols[i]:Hide(); shopCols[i].card:Hide(); shopCols[i].card.data = nil;
	end
	local count = #draft.cards;
	local width = math.max(PAD * 2 + count * CARD_W + (count - 1) * CARD_GAP, 320);
	frame:SetWidth(width);
	frame:SetHeight(HAND_TOP + CARD_H + 16 + 22 + PAD);
	FitToScreen(width);

	title:SetText("Choose a boon");
	waveText:SetText(string.format("Wave %d / %d%s", draft.wave, draft.final, draft.commonOnly and "  |cffb9b6c4Common only|r" or ""));

	for i, card in ipairs(draft.cards) do
		DrawCard(cards[i] or MakeCard(i), card, i, count);
	end
	for i = count + 1, #cards do cards[i]:Hide(); cards[i].data = nil; end

	take:ClearAllPoints();
	take:SetPoint("BOTTOMLEFT", frame, "BOTTOMLEFT", PAD, PAD);
	take:SetWidth(120);
	take:SetFrameLevel(frame:GetFrameLevel() + 3);
	take:Show();
	local card = CardByCode(draft.selected);
	if ( not card ) then
		take:SetText("Take");
		take:Disable();
	elseif ( card.status ~= 0 ) then
		take:SetText(StatusText(card));
		take:Disable();
	else
		take:SetText("Take");
		take:Enable();
	end

	reroll:ClearAllPoints();
	reroll:SetPoint("BOTTOMRIGHT", frame, "BOTTOMRIGHT", -PAD, PAD);
	reroll:SetWidth(150);
	reroll:SetFrameLevel(frame:GetFrameLevel() + 3);
	if ( (draft.secondOpinions or 0) > 0 ) then
		reroll:SetText(string.format("Second opinion (%d)", draft.secondOpinions));
		reroll:Enable();
		reroll:Show();
	else
		reroll:Hide();
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
	if ( draft and draft.buying and GetTime() >= (draft.buyingUntil or 0) ) then
		draft.buying = nil;
		Refresh();
	end
	if ( draft and draft.rerolling and GetTime() >= (draft.rerollingUntil or 0) ) then
		draft.rerolling = nil;
		Refresh();
	end
	if ( draft and draft.readying and GetTime() >= (draft.readyingUntil or 0) ) then
		draft.readying = nil;
		Refresh();
	end
end);

-- ---------------------------------------------------------------------------
-- Messages

local function CloseBrokerGossip()
	if ( suppress and GetTime() < suppress.untilTime and SameGuid(UnitGUID("npc"), suppress.tag) and CloseGossip ) then
		CloseGossip();
	end
end

-- The shop board as last shown (code -> waves discounted), so the next one can
-- tag what is NEW since then and what got CHEAPER. Per shop keeper: a new run
-- has a new keeper and starts with no tags.
local seenShop = { tag = nil, board = {} };

local function TagChanges(hand)
	if ( SameGuid(seenShop.tag, hand.tag) ) then
		for _, card in ipairs(hand.cards) do
			local before = seenShop.board[card.code];
			if ( before == nil ) then
				card.flag = "NEW";
			elseif ( (card.discounts or 0) > before ) then
				card.flag = "CHEAPER";
			end
		end
	end
	seenShop.tag = hand.tag;
	seenShop.board = {};
	for _, card in ipairs(hand.cards) do
		seenShop.board[card.code] = card.discounts or 0;
	end
end

local function Show(hand)
	if ( hand.shop and not demo ) then
		TagChanges(hand);
	end
	-- The shop sending its board again (a purchase, a cleared wave): keep the
	-- card you had picked out if it is still there, and say nothing new.
	local again = draft and hand.shop and SameGuid(draft.tag, hand.tag) and not draft.taken and frame:IsShown();
	-- What you were buying is off the board: the sale went through.
	if ( again and draft.buying and PlaySound ) then
		local stillThere = false;
		for _, card in ipairs(hand.cards) do
			if ( card.code == draft.buying ) then stillThere = true; end
		end
		if ( not stillThere ) then PlaySound("LOOTWINDOWCOINSOUND"); end
	end
	if ( again ) then
		for _, card in ipairs(hand.cards) do
			if ( card.code == draft.selected ) then hand.selected = card.code; break; end
		end
	end
	-- Default to the first card this character can take (and, at the shop, pay for).
	if ( not hand.selected ) then
		for _, card in ipairs(hand.cards) do
			local short = hand.shop and card.price and (card.price > (hand.shop.shards or 0));
			if ( card.status == 0 and not short ) then hand.selected = card.code; break; end
		end
	end
	hand.selected = hand.selected or (hand.cards[1] and hand.cards[1].code);
	-- Another broker's hand still up: put that one down first.
	if ( draft and not SameGuid(draft.tag, hand.tag) and not draft.taken ) then
		Request("LOOK", CODE_NONE);
	end
	local looked = again and draft.selected;
	draft = hand;
	Refresh();
	frame:Show();
	-- (Never LOOK 999 here: at the shop that would mean the window closed.)
	if ( hand.selected and hand.selected ~= looked ) then
		Request("LOOK", hand.selected);
	end
end

local handlers = {};

function handlers.DEAL(f)
	pending = {
		tag = f[1], wave = tonumber(f[2]) or 0, final = tonumber(f[3]) or 0,
		greed = tonumber(f[4]) or 0, abundance = tonumber(f[5]) or 0, hoarder = tonumber(f[6]) or 0,
		secondOpinions = tonumber(f[7]) or 0, commonOnly = f[8] == "1",
		party = {}, classOf = {}, cards = {}, deck = {}, looks = {},
	};
	for i = 9, #f - 1, 2 do
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

-- The broker is the shop keeper: this hand is for sale.
function handlers.SHOP(f)
	if ( not pending or not SameGuid(pending.tag, f[1]) ) then return; end
	pending.shop = {
		shards = tonumber(f[2]) or 0, perWave = tonumber(f[3]) or 0, perKill = tonumber(f[4]) or 0,
		reroll = tonumber(f[5]) or 50, discount = tonumber(f[6]) or 25,
		rerollCost = tonumber(f[7]) or 2, freeRerolls = tonumber(f[8]) or 0,
		locksUsed = tonumber(f[9]) or 0, locksMax = tonumber(f[10]) or 0, interest = tonumber(f[11]) or 0,
	};
end

-- The next-wave button: WAVE:<tag>^<shown>^0^1^0^<call>^<wave>. call 1 =
-- send the waiting wave in, 2 = call the next one in on top of the fight.
-- (Fields 3-5 are the retired party vote, kept for older addons.)
function handlers.WAVE(f)
	if ( not pending or not pending.shop or not SameGuid(pending.tag, f[1]) ) then return; end
	pending.ready = { waiting = f[2] == "1", call = tonumber(f[6]) or 1, wave = tonumber(f[7]) };
end

function handlers.PRICE(f)
	if ( not pending or not pending.shop or not SameGuid(pending.tag, f[1]) ) then return; end
	local code = tonumber(f[2]);
	for _, card in ipairs(pending.cards) do
		if ( card.code == code ) then
			card.price = tonumber(f[3]) or 0;
			card.basePrice = tonumber(f[4]) or card.price;
			card.discounts = tonumber(f[5]) or 0;
			card.lockedBy = (f[6] and f[6] ~= "") and f[6] or nil;
			card.lockedByMe = f[7] == "1";
			return;
		end
	end
end

function handlers.DECK(f)
	if ( not pending or not SameGuid(pending.tag, f[1]) ) then return; end
	for i = 2, #f - 2, 3 do
		pending.deck[#pending.deck + 1] = { tier = tonumber(f[i]) or 1, name = f[i + 1], value = f[i + 2] };
	end
end

function handlers.SHOW(f)
	-- A sold-out shop still opens (it shows your shards); a broker with no
	-- cards has nothing to show.
	if ( not pending or not SameGuid(pending.tag, f[1]) or (#pending.cards == 0 and not pending.shop) ) then return; end
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

function handlers.REROLLED(f)
	if ( draft and SameGuid(draft.tag, f[1]) ) then
		draft.taken = { name = f[2] or "", code = -1 };
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
		tag = "demo", wave = 7, final = 30, greed = 1, abundance = 1, hoarder = 0, secondOpinions = 2, commonOnly = false,
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

local function ShopDemo()
	local me = Me() or "You";
	demo = true;
	Show({
		tag = "demo-shop", wave = 9, final = 25, greed = 0, abundance = 0, hoarder = 0, secondOpinions = 0, commonOnly = false,
		shop = { shards = 11, perWave = 6, perKill = 1, reroll = 50, discount = 25,
		         rerollCost = 2, freeRerolls = 1, locksUsed = 1, locksMax = 3, interest = 1 },
		party = { { name = me, class = 4 }, { name = "Kaelen", class = 7 } }, classOf = { [me] = 4, Kaelen = 7 },
		looks = { Kaelen = 27 }, deck = {},
		cards = {
			{ code = 1, kind = "B", tier = 2, icon = "S90231", name = "Boon of Might", headline = "+5%",
			  text = "damage done", held = 10, after = 15, cap = 255, unit = "%", status = 0, owners = {}, price = 5, basePrice = 5 },
			{ code = 27, kind = "B", tier = 3, icon = "S90291", name = "Boon of the Vampire", headline = "+5%",
			  text = "of damage dealt returned as health", held = 0, after = 5, cap = 30, unit = "%", status = 0, owners = {},
			  price = 5, basePrice = 8, discounts = 2, flag = "CHEAPER" },
			{ code = 208, kind = "S", tier = 4, icon = "S31224", name = "Cloak of Shadows", headline = "Learn",
			  text = "Lost when you leave the Hold.", held = 0, after = 0, cap = 0, unit = "", status = 0, owners = {},
			  price = 10, basePrice = 10, flag = "NEW" },
			{ code = 160, kind = "B", tier = 6, icon = "S94349", name = "Boon of the Assassin", headline = "",
			  text = "Backstab works from the front and is never dodged, parried or blocked; Sinister Strike also hits a second enemy",
			  held = 0, after = 1, cap = 1, unit = "", status = 0, owners = {}, price = 15, basePrice = 15 },
			{ code = 302, kind = "I", tier = 5, icon = "I19019", name = "Thunderfury, Blessed Blade of the Windseeker", headline = "Crusader",
			  text = "Lost when you leave the Hold.", held = 0, after = 0, cap = 0, unit = "", status = 0, owners = {},
			  price = 20, basePrice = 20, lockedBy = me, lockedByMe = true },
		},
	});
end

SLASH_CENTURIONVHDRAFT1 = "/vhdraft";
SlashCmdList["CENTURIONVHDRAFT"] = function(arg)
	arg = string.lower(arg or "");
	if ( arg == "demo" ) then
		Demo();
	elseif ( arg == "shop" ) then
		ShopDemo();
	else
		DEFAULT_CHAT_FRAME:AddMessage("|cffbb85ff/vhdraft demo|r deals a made-up hand of boon cards; |cffbb85ff/vhdraft shop|r shows a made-up shop.");
	end
end
