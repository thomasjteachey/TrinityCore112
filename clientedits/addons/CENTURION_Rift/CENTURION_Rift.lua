-- Centurion Summoner's Rift: the skill pick panel.
--
-- The server (RiftProgression) runs the match: you start at level 10 and climb
-- to your real level, and every level hands out picks. A pick learns a spell you
-- know but cannot use yet, or ranks one up.
--
-- The panel only comes up when there is something to do - a pick in hand AND a
-- spell to spend it on - and goes away again once the picks are spent. A pick
-- with nothing to learn yet waits quietly for the next level. /rift opens it
-- any time.
--
--   receives  CCGAME     RIFT:<match level>:<real level>:<league level>:<picks>
--                        RIFTPICKS:<spell>,<spell>,...      (what can be picked now)
--                        RIFTGOLD:<gold>                    (gold earned: last hit, kill, ...)
--                        RIFT:END                           (the match is over)
--   sends     CCGAMEREQ  RIFTPICK:<spell> | RIFTSTATE:

local REQUEST_PREFIX = "CCGAME" .. "REQ";
local ANSWER_PREFIX = "CCGAME";

local BUTTON_SIZE = 40;
local BUTTON_GAP = 14;
local PER_ROW = 6;
local PAD = 16;
local HEADER = 52;          -- title band
local LABEL = 16;           -- the "New" / "Rank 4" line under each button
local GOLD = { 1.00, 0.82, 0.25 };
local EDGE = { 0.78, 0.62, 0.28 };

local state = { match = 0, real = 0, league = 0, picks = 0, offers = {}, atBase = false };
local dismissedAt = nil;    -- picks in hand when the panel was closed by hand
local forced = false;       -- opened with /rift

local function Send(payload)
	local me = UnitName("player");
	if ( SendAddonMessage and me ) then
		SendAddonMessage(REQUEST_PREFIX, payload, "WHISPER", me);
	end
end

-- ------------------------------------------------------------------ frame

local frame = CreateFrame("Frame", "CENTURION_RiftFrame", UIParent);
frame:SetFrameStrata("MEDIUM");
frame:SetPoint("BOTTOM", UIParent, "BOTTOM", 0, 190);
frame:SetBackdrop({
	bgFile = "Interface\\Tooltips\\UI-Tooltip-Background",
	edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
	tile = true, tileSize = 16, edgeSize = 14,
	insets = { left = 3, right = 3, top = 3, bottom = 3 },
});
frame:SetBackdropColor(0.04, 0.05, 0.08, 0.92);
frame:SetBackdropBorderColor(EDGE[1], EDGE[2], EDGE[3], 1);
frame:SetClampedToScreen(true);
frame:SetMovable(true);
frame:EnableMouse(true);
frame:RegisterForDrag("LeftButton");
frame:SetScript("OnDragStart", frame.StartMoving);
frame:SetScript("OnDragStop", function(self)
	self:StopMovingOrSizing();
	local point, _, relativePoint, x, y = self:GetPoint();
	CENTURION_Rift_Position = { point, relativePoint, x, y };
end);
frame:Hide();

-- A warm wash behind the title, fading down into the panel.
local wash = frame:CreateTexture(nil, "BORDER");
wash:SetTexture(1, 1, 1, 1);
wash:SetPoint("TOPLEFT", frame, "TOPLEFT", 4, -4);
wash:SetPoint("TOPRIGHT", frame, "TOPRIGHT", -4, -4);
wash:SetHeight(HEADER - 6);
wash:SetGradientAlpha("VERTICAL", 0.55, 0.38, 0.08, 0.00, 0.55, 0.38, 0.08, 0.45);

-- A thin gold rule under the title.
local rule = frame:CreateTexture(nil, "ARTWORK");
rule:SetTexture(EDGE[1], EDGE[2], EDGE[3], 0.6);
rule:SetHeight(1);
rule:SetPoint("TOPLEFT", frame, "TOPLEFT", PAD, -HEADER + 2);
rule:SetPoint("TOPRIGHT", frame, "TOPRIGHT", -PAD, -HEADER + 2);

local title = frame:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge");
title:SetPoint("TOPLEFT", frame, "TOPLEFT", PAD, -11);
title:SetTextColor(GOLD[1], GOLD[2], GOLD[3]);
title:SetShadowOffset(1, -1);

local subtitle = frame:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall");
subtitle:SetPoint("TOPLEFT", title, "BOTTOMLEFT", 0, -3);
subtitle:SetTextColor(0.80, 0.80, 0.80);

-- The picks in hand: a big number on the right of the title band.
local count = frame:CreateFontString(nil, "OVERLAY", "GameFontNormalHuge");
count:SetPoint("TOPRIGHT", frame, "TOPRIGHT", -PAD - 18, -9);
count:SetTextColor(0.45, 1.00, 0.45);
local countLabel = frame:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall");
countLabel:SetPoint("TOP", count, "BOTTOM", 0, 0);
countLabel:SetTextColor(0.70, 0.70, 0.70);

local close = CreateFrame("Button", nil, frame, "UIPanelCloseButton");
close:SetWidth(24);
close:SetHeight(24);
close:SetPoint("TOPRIGHT", frame, "TOPRIGHT", -1, -1);
close:SetScript("OnClick", function()
	dismissedAt = state.picks;
	forced = false;
	frame:Hide();
end);

-- Only seen when the panel is opened by hand with nothing to pick.
local idle = frame:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall");
idle:SetPoint("TOPLEFT", frame, "TOPLEFT", PAD, -HEADER - 10);
idle:SetJustifyH("LEFT");

-- ---------------------------------------------------------------- buttons

local buttons = {};

local function Shine(b, on)
	if ( not b.shine ) then
		return;
	end
	if ( on and AutoCastShine_AutoCastStart ) then
		AutoCastShine_AutoCastStart(b.shine, GOLD[1], GOLD[2], GOLD[3]);
	elseif ( not on and AutoCastShine_AutoCastStop ) then
		AutoCastShine_AutoCastStop(b.shine);
	end
end

local function GetButton(index)
	if ( buttons[index] ) then
		return buttons[index];
	end
	local name = "CENTURION_RiftPick" .. index;
	local b = CreateFrame("CheckButton", name, frame, "ActionButtonTemplate");
	b:SetWidth(BUTTON_SIZE);
	b:SetHeight(BUTTON_SIZE);
	b.icon = _G[name .. "Icon"];
	local normal = _G[name .. "NormalTexture"];
	if ( normal ) then
		-- The template's slot art is drawn for a 36 px button.
		normal:SetWidth(BUTTON_SIZE * 66 / 36);
		normal:SetHeight(BUTTON_SIZE * 66 / 36);
	end
	for _, part in ipairs({ "HotKey", "Count", "Name", "Border", "Flash" }) do
		local region = _G[name .. part];
		if ( region ) then
			region:Hide();
		end
	end

	local ok, shine = pcall(CreateFrame, "Frame", name .. "Shine", b, "AutoCastShineTemplate");
	if ( ok and shine ) then
		shine:SetPoint("CENTER", b, "CENTER", 0, 0);
		shine:SetWidth(BUTTON_SIZE - 2);
		shine:SetHeight(BUTTON_SIZE - 2);
		b.shine = shine;
	end

	b.label = b:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall");
	b.label:SetPoint("TOP", b, "BOTTOM", 0, -3);
	b.label:SetWidth(BUTTON_SIZE + BUTTON_GAP);

	b:SetScript("OnEnter", function(self)
		GameTooltip:SetOwner(self, "ANCHOR_TOP");
		GameTooltip:SetHyperlink("spell:" .. self.spell);
		if ( state.picks > 0 ) then
			GameTooltip:AddLine(self.isNew and "Click to learn this with a skill pick." or "Click to rank this up with a skill pick.", 0.4, 1, 0.4);
		end
		GameTooltip:Show();
	end);
	b:SetScript("OnLeave", function() GameTooltip:Hide(); end);
	b:SetScript("OnClick", function(self)
		self:SetChecked(nil);
		if ( state.picks > 0 ) then
			Send("RIFTPICK:" .. self.spell);
		end
	end);
	buttons[index] = b;
	return b;
end

-- -------------------------------------------------------------------- draw

local function Layout()
	local shown = 0;
	for _, spell in ipairs(state.offers) do
		local name, rank, icon = GetSpellInfo(spell);
		if ( name ) then
			shown = shown + 1;
			local b = GetButton(shown);
			b.spell = spell;
			b.icon:SetTexture(icon);
			local n = tonumber(rank and string.match(rank, "(%d+)") or "");
			b.isNew = not n or n <= 1;
			if ( b.isNew ) then
				b.label:SetText("New");
				b.label:SetTextColor(0.45, 1.00, 0.45);
			else
				b.label:SetText("Rank " .. n);
				b.label:SetTextColor(GOLD[1], GOLD[2], GOLD[3]);
			end
			b.icon:SetDesaturated(state.picks <= 0);
			Shine(b, state.picks > 0);
			b:Show();
		end
	end
	for i = shown + 1, #buttons do
		Shine(buttons[i], false);
		buttons[i]:Hide();
	end

	-- Buttons centred under the title band, in rows of PER_ROW.
	local cols = math.min(math.max(shown, 1), PER_ROW);
	local rows = math.max(1, math.ceil(shown / PER_ROW));
	local gridWidth = cols * BUTTON_SIZE + (cols - 1) * BUTTON_GAP;

	-- Wide enough for every line of text, so nothing runs out of the panel.
	local headerWidth = math.max(title:GetStringWidth(), subtitle:GetStringWidth()) + 70;
	local idleWidth = shown == 0 and idle:GetStringWidth() or 0;
	local width = math.ceil(math.max(gridWidth, headerWidth, idleWidth, 200) + 2 * PAD);
	frame:SetWidth(width);

	local left = (width - gridWidth) / 2;
	for i = 1, shown do
		local b = buttons[i];
		local row = math.floor((i - 1) / PER_ROW);
		local col = (i - 1) % PER_ROW;
		b:ClearAllPoints();
		b:SetPoint("TOPLEFT", frame, "TOPLEFT", left + col * (BUTTON_SIZE + BUTTON_GAP), -HEADER - 12 - row * (BUTTON_SIZE + LABEL + 10));
	end

	if ( shown > 0 ) then
		idle:Hide();
		frame:SetHeight(HEADER + 12 + rows * (BUTTON_SIZE + LABEL + 10) + 6);
	else
		idle:Show();
		frame:SetHeight(HEADER + 10 + idle:GetStringHeight() + 14);
	end
	return shown;
end

local function Refresh()
	if ( state.picks > 0 ) then
		title:SetText("Level up!");
	else
		title:SetText("Summoner's Rift");
	end
	subtitle:SetText(string.format("Level %d of %d", state.match, state.real));
	count:SetText(state.picks);
	countLabel:SetText(state.picks == 1 and "pick" or "picks");
	if ( state.picks > 0 ) then
		count:SetTextColor(0.45, 1.00, 0.45);
		idle:SetText("Nothing new to learn at this level.\nYour picks are kept for the next one.");
	else
		count:SetTextColor(0.50, 0.50, 0.50);
		idle:SetText("No picks right now - the next level brings one.");
	end

	local shown = Layout();

	-- Something to do: up it comes (again, if more picks arrived since it was
	-- closed). Nothing to do: out of the way, unless /rift asked for it. Picks
	-- are spent in the fountain only, so the panel lives there and nowhere else.
	if ( not state.atBase ) then
		forced = false;
		frame:Hide();
		return;
	end
	local actionable = state.picks > 0 and shown > 0;
	if ( actionable and (not dismissedAt or state.picks > dismissedAt) ) then
		dismissedAt = nil;
		if ( not frame:IsShown() ) then
			frame:SetAlpha(0);
			frame:Show();
			UIFrameFadeIn(frame, 0.25, 0, 1);
			PlaySoundFile("Sound\\Interface\\LevelUp.wav");
		end
	elseif ( not actionable and not forced ) then
		frame:Hide();
	end
	if ( state.picks == 0 ) then
		dismissedAt = nil;
	end
end

-- ----------------------------------------------------------------- server

-- "+21 gold" floating up over the character, League's last-hit popup.
local POPUP_TIME = 1.6;
local popups = {};

local function GoldPopup(amount)
	local f;
	for _, p in ipairs(popups) do
		if ( not p:IsShown() ) then f = p; break; end
	end
	if ( not f ) then
		f = CreateFrame("Frame", nil, UIParent);
		f:SetWidth(200);
		f:SetHeight(30);
		f.text = f:CreateFontString(nil, "OVERLAY", "NumberFontNormalHuge");
		f.text:SetPoint("CENTER", f, "CENTER", 0, 0);
		f.text:SetTextColor(1.0, 0.82, 0.0);
		f:SetScript("OnUpdate", function(self, elapsed)
			self.age = self.age + elapsed;
			local t = self.age / POPUP_TIME;
			if ( t >= 1 ) then
				self:Hide();
				return;
			end
			self:SetPoint("CENTER", UIParent, "CENTER", self.dx, 90 + 60 * t);
			self:SetAlpha(t < 0.7 and 1 or (1 - (t - 0.7) / 0.3));
		end);
		table.insert(popups, f);
	end
	f.age = 0;
	f.dx = math.random(-30, 30);
	f.text:SetText("+" .. amount .. " gold");
	f:SetPoint("CENTER", UIParent, "CENTER", f.dx, 90);
	f:SetAlpha(1);
	f:Show();
	-- no sound of its own: the client already chimes when money comes in
end

-- The match trims the spellbook to the ranks it has unlocked spell by spell
-- (the client keeps whatever spell list it was once sent), and every one of
-- those prints "You have unlearned ..." - a level-60 character walks in to a
-- wall of them, and out again to "You have learned ...". Hidden in the Rift.
local spellbookSpam = {};
for _, fmt in ipairs({ ERR_LEARN_SPELL_S, ERR_LEARN_ABILITY_S, ERR_LEARN_PASSIVE_S, ERR_SPELL_UNLEARNED_S }) do
	if ( fmt ) then
		local pattern = string.gsub(fmt, "([%(%)%.%%%+%-%*%?%[%]%^%$])", "%%%1");
		pattern = string.gsub(pattern, "%%%%s", ".+");
		table.insert(spellbookSpam, "^" .. pattern .. "$");
	end
end
local RIFT_ZONE = "Summoner's Rift";
ChatFrame_AddMessageEventFilter("CHAT_MSG_SYSTEM", function(self, event, message, ...)
	if ( GetRealZoneText() ~= RIFT_ZONE or type(message) ~= "string" ) then
		return false;
	end
	for _, pattern in ipairs(spellbookSpam) do
		if ( string.find(message, pattern) ) then
			return true;
		end
	end
	return false;
end);

local function Split(text, sep)
	local out = {};
	for part in string.gmatch(text, "([^" .. sep .. "]+)") do
		table.insert(out, part);
	end
	return out;
end

local function OnAnswer(message)
	if ( message == "RIFT:END" ) then
		state.picks = 0;
		state.offers = {};
		state.atBase = false;
		forced = false;
		dismissedAt = nil;
		for _, b in ipairs(buttons) do
			Shine(b, false);
		end
		frame:Hide();
		return;
	end
	-- In the fountain or out of it (BattlegroundSRF, on every change). Walking in
	-- with talent points to spend opens the talent frame; walking out closes it.
	local base = string.match(message, "^RIFTBASE:(%d)$");
	if ( base ) then
		local was = state.atBase;
		state.atBase = base == "1";
		if ( state.atBase and not was and (GetUnspentTalentPoints() or 0) > 0 ) then
			if ( not PlayerTalentFrame and TalentFrame_LoadUI ) then
				TalentFrame_LoadUI();
			end
			if ( PlayerTalentFrame and not PlayerTalentFrame:IsShown() ) then
				ShowUIPanel(PlayerTalentFrame);
			end
		elseif ( not state.atBase and was and PlayerTalentFrame and PlayerTalentFrame:IsShown() ) then
			HideUIPanel(PlayerTalentFrame);
		end
		Refresh();
		return;
	end
	local gold = string.match(message, "^RIFTGOLD:(%d+)$");
	if ( gold ) then
		GoldPopup(tonumber(gold));
		return;
	end
	local picks = string.match(message, "^RIFTPICKS:(.*)$");
	if ( picks ) then
		state.offers = {};
		for _, id in ipairs(Split(picks, ",")) do
			table.insert(state.offers, tonumber(id));
		end
		Refresh();
		return;
	end
	local match, real, league, pending = string.match(message, "^RIFT:(%d+):(%d+):(%d+):(%d+)$");
	if ( match ) then
		state.match, state.real, state.league, state.picks = tonumber(match), tonumber(real), tonumber(league), tonumber(pending);
		Refresh();
	end
end

SLASH_CENTURIONRIFT1 = "/rift";
SlashCmdList["CENTURIONRIFT"] = function()
	if ( frame:IsShown() ) then
		forced = false;
		dismissedAt = state.picks;
		frame:Hide();
	elseif ( not state.atBase ) then
		DEFAULT_CHAT_FRAME:AddMessage("Skill picks are spent at your fountain: go back to base to open them.", 1.0, 0.82, 0.0);
	else
		forced = true;
		dismissedAt = nil;
		Send("RIFTSTATE:");
		frame:SetAlpha(1);
		frame:Show();
		Refresh();
	end
end

local driver = CreateFrame("Frame");
driver:RegisterEvent("CHAT_MSG_ADDON");
driver:RegisterEvent("PLAYER_ENTERING_WORLD");
driver:RegisterEvent("VARIABLES_LOADED");
driver:SetScript("OnEvent", function(self, event, arg1, arg2, arg3, arg4)
	if ( event == "VARIABLES_LOADED" ) then
		local p = CENTURION_Rift_Position;
		if ( p ) then
			frame:ClearAllPoints();
			frame:SetPoint(p[1], UIParent, p[2], p[3], p[4]);
		end
		return;
	end
	if ( event == "PLAYER_ENTERING_WORLD" ) then
		-- Back from a /reload or a loading screen mid-match: ask where we are.
		Send("RIFTSTATE:");
		return;
	end
	-- From the server: a whisper to ourselves (its sender is our own name, or none).
	if ( arg1 ~= ANSWER_PREFIX or type(arg2) ~= "string" or arg3 ~= "WHISPER" or (arg4 and arg4 ~= "" and arg4 ~= UnitName("player")) ) then
		return;
	end
	OnAnswer(arg2);
end);

-- Health bars over every unit's head, League style: a thin bar of our own over
-- each minion, monster and turret while a match runs.
--
-- WoW 3.3.5 gives an addon no way to put a frame over a unit in the world, so the
-- Centurion client DLL (unitbars.cpp) hands the positions over:
-- CenturionUnitBarsScan() walks the visible units and CenturionUnitBar(i) says
-- where unit i's head is on screen, its health and how it stands to the player.
-- The bars follow that every frame, nameplates or not.
--
-- A client without that DLL falls back to nameplates: they are switched on while
-- a match runs and stripped to nothing - border, name, level, glow, icons faded
-- out - with the same minimal bar drawn in their place.
do
	local BAR_WIDTH, BAR_HEIGHT = 46, 4;
	-- The DLL path's bar in yards at the unit, and the size it keeps to on screen.
	local BAR_YARDS_WIDE, BAR_YARDS_HIGH, BAR_MIN_WIDTH, BAR_MAX_WIDTH = 2.6, 0.22, 14, 90;
	-- Champions' bars are this much bigger. Opacity: a unit's bar at half, a
	-- champion's at three quarters, the target's solid.
	local PLAYER_BAR_SCALE, BAR_ALPHA, PLAYER_BAR_ALPHA = 1.6, 0.5, 0.75;
	local WHITE = "Interface\\Buttons\\WHITE8X8";
	local active = false;

	-- League's colours: allies blue, enemies red, the jungle's neutrals gold.
	local function ReactionColour(reaction)
		if ( reaction <= 3 ) then
			return 0.86, 0.18, 0.16;
		elseif ( reaction == 4 ) then
			return 0.92, 0.72, 0.18;
		end
		return 0.25, 0.55, 1.0;
	end

	local function NewBar(parent)
		local bar = CreateFrame("StatusBar", nil, parent);
		bar:SetStatusBarTexture(WHITE);
		bar:SetWidth(BAR_WIDTH);
		bar:SetHeight(BAR_HEIGHT);
		bar:SetMinMaxValues(0, 1);
		local bg = bar:CreateTexture(nil, "BACKGROUND");
		bg:SetTexture(0, 0, 0, 0.75);
		bg:SetPoint("TOPLEFT", bar, "TOPLEFT", -1, 1);
		bg:SetPoint("BOTTOMRIGHT", bar, "BOTTOMRIGHT", 1, -1);
		bar.bg = bg;
		return bar;
	end

	local SetActive;

	if ( CenturionUnitBarsScan and CenturionUnitBar ) then
		-- Every bar owns a click band at least CLICK_BAND pixels tall, the bar in
		-- its middle, and bands never overlap: the stacking keeps them apart and
		-- each bar's hit rectangle is exactly its band. Three bars on one spot
		-- used to share their click areas (the bar's own reached 6 px past it,
		-- the bars 1 px apart) and the middle one could hardly be clicked.
		local CLICK_BAND, CLICK_PAD_X = 12, 3;
		local pool, shown = {}, 0;
		local entries, order = {}, {};
		local driver = CreateFrame("Frame", nil, WorldFrame);

		local function HideFrom(first)
			for i = first, shown do
				pool[i]:Hide();
			end
			shown = first - 1;
		end

		-- Two bars' click bands (x = centre, y = the bar's top, pad above and below) touch.
		local function Overlaps(a, b)
			return math.abs(a.x - b.x) * 2 < a.w + b.w + 2 * CLICK_PAD_X
				and a.y + a.pad > b.y - b.h - b.pad and a.y - a.h - a.pad < b.y + b.pad;
		end

		local target, mouseover;
		local function Before(a, b)
			if ( a.guid == target ) ~= ( b.guid == target ) then
				return a.guid == target;
			end
			return a.y < b.y;
		end

		driver:SetScript("OnUpdate", function()
			local width, height = WorldFrame:GetWidth(), WorldFrame:GetHeight();
			target, mouseover = UnitGUID("target"), UnitGUID("mouseover");
			local n = CenturionUnitBarsScan();
			for i = 1, n do
				local fx, fy, health, reaction, scale, guid, isPlayer = CenturionUnitBar(i);
				local e = entries[i] or {};
				entries[i] = e;
				-- Sized in yards where the unit stands, so it shrinks with distance
				-- as a name would, and hung where the name would be. A champion's
				-- bar is bigger than a minion's, as in League.
				local big = isPlayer == 1 and PLAYER_BAR_SCALE or 1;
				local yard = (scale or 0) * height;
				if ( yard > 0 ) then
					e.w = math.min(math.max(yard * BAR_YARDS_WIDE, BAR_MIN_WIDTH), BAR_MAX_WIDTH) * big;
					e.h = math.min(math.max(yard * BAR_YARDS_HIGH, 2), 6) * big;
				else
					e.w, e.h = BAR_WIDTH * big, BAR_HEIGHT * big;
				end
				e.x, e.y = fx * width, fy * height - 1;
				e.pad = math.max((CLICK_BAND - e.h) / 2, 1);
				e.health, e.reaction, e.guid, e.player = health, reaction, guid, isPlayer == 1;
				order[i] = e;
			end
			for i = n + 1, #order do
				order[i] = nil;
			end

			-- No bar in front of another, as WoW stacks nameplates: the target's
			-- bar keeps its place, then from the bottom of the screen up each bar
			-- that touches one already placed moves up above it.
			table.sort(order, Before);
			for k = 2, n do
				local e = order[k];
				for _ = 1, 24 do
					local moved = false;
					for j = 1, k - 1 do
						local other = order[j];
						if ( Overlaps(e, other) ) then
							e.y = other.y + other.pad + e.h + e.pad;
							moved = true;
						end
					end
					if ( not moved ) then
						break;
					end
				end
			end

			for k = 1, n do
				local e = order[k];
				local bar = pool[k];
				if ( not bar ) then
					bar = NewBar(WorldFrame);
					bar:SetFrameStrata("LOW");
					-- A click on a bar targets its unit (the DLL's CenturionTargetGUID:
					-- minions share names, so TargetUnit cannot). The bar is a few
					-- pixels high; its click area reaches a little past it.
					if ( CenturionTargetGUID ) then
						bar:EnableMouse(true);
						bar:SetScript("OnMouseUp", function(self, button)
							if ( self.guid and button == "LeftButton" ) then
								CenturionTargetGUID(self.guid);
							end
						end);
						bar:SetScript("OnEnter", function(self) self.hover = true; end);
						bar:SetScript("OnLeave", function(self) self.hover = false; end);
					end
					pool[k] = bar;
				end
				bar.guid = e.guid;
				if ( CenturionTargetGUID ) then
					bar:SetHitRectInsets(-CLICK_PAD_X, -CLICK_PAD_X, -e.pad, -e.pad);
				end
				bar:SetWidth(e.w);
				bar:SetHeight(e.h);
				bar:ClearAllPoints();
				bar:SetPoint("TOP", WorldFrame, "BOTTOMLEFT", e.x, e.y);
				bar:SetValue(e.health);
				bar:SetStatusBarColor(ReactionColour(e.reaction));
				bar:SetAlpha(e.guid == target and 1 or (e.player and PLAYER_BAR_ALPHA or BAR_ALPHA));
				-- The target's bar is framed in white and drawn over the rest.
				if ( e.guid == target ) then
					bar.bg:SetTexture(1, 1, 1, 0.95);
					bar:SetFrameLevel(driver:GetFrameLevel() + 20);
				elseif ( e.guid == mouseover or bar.hover ) then
					bar.bg:SetTexture(0.65, 0.65, 0.65, 0.9);
					bar:SetFrameLevel(driver:GetFrameLevel() + 1);
				else
					bar.bg:SetTexture(0, 0, 0, 0.75);
					bar:SetFrameLevel(driver:GetFrameLevel() + 1);
				end
				bar:Show();
			end
			if ( n < shown ) then
				HideFrom(n + 1);
			end
			shown = n;
		end);
		driver:Hide();

		SetActive = function(on)
			if ( on == active ) then
				return;
			end
			active = on;
			if ( on ) then
				driver:Show();
			else
				driver:Hide();
				HideFrom(1);
			end
		end
	else
		local PLATE_CVARS = { "nameplateShowEnemies", "nameplateShowFriends" };
		local FLASH = "Interface\\TargetingFrame\\UI-TargetingFrame-Flash";
		local saved = nil;
		local skins = {};           -- nameplate frame -> { health, cast, regions, bar }
		local seen = 0;

		local function IsNamePlate(f)
			if ( f:GetName() ) then
				return false;
			end
			local region = f:GetRegions();
			return region and region.GetObjectType and region:GetObjectType() == "Texture" and region:GetTexture() == FLASH;
		end

		local function Apply(skin)
			local alpha = active and 0 or 1;
			for _, region in ipairs(skin.regions) do
				region:SetAlpha(alpha);
			end
			skin.health:SetAlpha(alpha);
			if ( skin.cast ) then
				skin.cast:SetAlpha(alpha);
			end
			if ( active ) then
				skin.bar:Show();
			else
				skin.bar:Hide();
			end
		end

		local function Skin(f)
			local health, cast = f:GetChildren();
			if ( not health ) then
				return;
			end
			local skin = { health = health, cast = cast, regions = { f:GetRegions() } };
			local bar = NewBar(f);
			bar:SetPoint("BOTTOM", f, "BOTTOM", 0, 8);
			bar:SetScript("OnUpdate", function(self)
				local lo, hi = health:GetMinMaxValues();
				self:SetMinMaxValues(lo, hi);
				self:SetValue(health:GetValue());
				self:SetStatusBarColor(health:GetStatusBarColor());
			end);
			skin.bar = bar;
			skins[f] = skin;
			Apply(skin);
		end

		local function Scan()
			local n = WorldFrame:GetNumChildren();
			if ( n == seen ) then
				return;
			end
			seen = n;
			for _, f in ipairs({ WorldFrame:GetChildren() }) do
				if ( not skins[f] and IsNamePlate(f) ) then
					Skin(f);
				end
			end
		end

		local plates = CreateFrame("Frame");
		local since = 0;
		plates:SetScript("OnUpdate", function(self, elapsed)
			since = since + elapsed;
			if ( since >= 0.25 ) then
				since = 0;
				Scan();
			end
		end);

		SetActive = function(on)
			if ( on == active ) then
				return;
			end
			active = on;
			if ( on ) then
				saved = {};
				for _, cvar in ipairs(PLATE_CVARS) do
					saved[cvar] = GetCVar(cvar);
					SetCVar(cvar, 1);
				end
			elseif ( saved ) then
				for cvar, value in pairs(saved) do
					SetCVar(cvar, value);
				end
				saved = nil;
			end
			for _, skin in pairs(skins) do
				Apply(skin);
			end
		end
	end

	-- In a match the bars stand in for the names: creature, pet, guardian and
	-- totem names are switched off while it runs and put back as they were.
	local NAME_CVARS = { "UnitNameNPC", "UnitNameFriendlyPetName", "UnitNameEnemyPetName",
		"UnitNameFriendlyGuardianName", "UnitNameEnemyGuardianName", "UnitNameFriendlyTotemName",
		"UnitNameEnemyTotemName" };
	local savedNames = nil;
	local function HideNames(on)
		if ( on and not savedNames ) then
			savedNames = {};
			for _, cvar in ipairs(NAME_CVARS) do
				local ok, value = pcall(GetCVar, cvar);
				if ( ok and value ) then
					savedNames[cvar] = value;
					SetCVar(cvar, 0);
				end
			end
		elseif ( not on and savedNames ) then
			for cvar, value in pairs(savedNames) do
				SetCVar(cvar, value);
			end
			savedNames = nil;
		end
	end
	local SetBars = SetActive;
	SetActive = function(on)
		HideNames(on);
		SetBars(on);
	end

	local events = CreateFrame("Frame");
	events:RegisterEvent("CHAT_MSG_ADDON");
	events:SetScript("OnEvent", function(self, event, prefix, message, channel, sender)
		if ( prefix ~= ANSWER_PREFIX or type(message) ~= "string" or channel ~= "WHISPER" ) then
			return;
		end
		if ( sender and sender ~= "" and sender ~= UnitName("player") ) then
			return;
		end
		if ( message == "RIFT:END" ) then
			SetActive(false);
		elseif ( string.match(message, "^RIFT:%d+:") ) then
			SetActive(true);
		end
	end);
end
