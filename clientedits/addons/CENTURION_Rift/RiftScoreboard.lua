-- Summoner's Rift: League's scoreboard. Hold ` (the key left of 1 - Tab stays
-- WoW's targeting; any key can be set under Key Bindings, "Scoreboard (hold)")
-- or open the battleground score window (its key, the minimap icon):
-- both teams, every champion's level, kills / deaths / assists, minions and the
-- items they hold.
--
-- The server (BattlegroundSRF::SendScores) sends every two seconds, per champion:
--   CCGAME  RIFTSCORE:<id>:<name>:<team>:<class>:<level>:<kills>:<deaths>:<assists>:<cs>:<item>,<item>,...
-- Team 0 is Order (Alliance), 1 Chaos (Horde).

local ANSWER_PREFIX = "CCGAME";
local RIFT_ZONE = "Summoner's Rift";
local STALE = 8;
local ROW_H, ICON, ITEM = 40, 32, 26;
local WIDTH = 760;
local TEAM_COLOUR = { [0] = { 0.30, 0.62, 1.00 }, [1] = { 1.00, 0.32, 0.27 } };
local TEAM_NAME = { [0] = "ORDER", [1] = "CHAOS" };
local EDGE = { 0.78, 0.66, 0.43 };
local CLASSES = { "WARRIOR", "PALADIN", "HUNTER", "ROGUE", "PRIEST", "DEATHKNIGHT", "SHAMAN", "MAGE", "WARLOCK", nil, "DRUID" };

local rows = {};          -- id -> { name, team, class, level, k, d, a, cs, items, seen }

local function InRift()
	return GetRealZoneText() == RIFT_ZONE;
end

-- ------------------------------------------------------------------ frame

local board = CreateFrame("Frame", "CENTURION_RiftScoreboard", UIParent);
board:SetWidth(WIDTH);
board:SetFrameStrata("DIALOG");
board:SetPoint("CENTER", UIParent, "CENTER", 0, 40);
board:SetBackdrop({
	bgFile = "Interface\\Tooltips\\UI-Tooltip-Background",
	edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
	tile = true, tileSize = 16, edgeSize = 14,
	insets = { left = 3, right = 3, top = 3, bottom = 3 },
});
board:SetBackdropColor(0.02, 0.05, 0.09, 0.94);
board:SetBackdropBorderColor(EDGE[1], EDGE[2], EDGE[3], 1);
board:Hide();
table.insert(UISpecialFrames, "CENTURION_RiftScoreboard");

local header = board:CreateFontString(nil, "OVERLAY");
header:SetFont("Fonts\\MORPHEUS.TTF", 24, "OUTLINE");
header:SetPoint("TOP", board, "TOP", 0, -10);

local leave = CreateFrame("Button", nil, board, "UIPanelButtonTemplate");
leave:SetWidth(150);
leave:SetHeight(22);
leave:SetPoint("BOTTOM", board, "BOTTOM", 0, 10);
leave:SetText("Leave Battleground");
leave:SetScript("OnClick", function() LeaveBattlefield(); end);
leave:Hide();

local columns = board:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall");
columns:SetPoint("TOPLEFT", board, "TOPLEFT", 0, -42);

local lines = {};
local function Line(i)
	if ( lines[i] ) then
		return lines[i];
	end
	local l = CreateFrame("Frame", nil, board);
	l:SetHeight(ROW_H);
	l:SetPoint("LEFT", board, "LEFT", 12, 0);
	l:SetPoint("RIGHT", board, "RIGHT", -12, 0);
	l.bg = l:CreateTexture(nil, "BACKGROUND");
	l.bg:SetAllPoints(l);
	l.bg:SetTexture(1, 1, 1, 1);
	l.icon = l:CreateTexture(nil, "ARTWORK");
	l.icon:SetWidth(ICON);
	l.icon:SetHeight(ICON);
	l.icon:SetPoint("LEFT", l, "LEFT", 6, 0);
	l.icon:SetTexture("Interface\\WorldStateFrame\\Icons-Classes");
	l.level = l:CreateFontString(nil, "OVERLAY", "NumberFontNormalSmall");
	l.level:SetPoint("BOTTOMRIGHT", l.icon, "BOTTOMRIGHT", 2, -2);
	l.name = l:CreateFontString(nil, "OVERLAY", "GameFontHighlight");
	l.name:SetPoint("LEFT", l.icon, "RIGHT", 10, 0);
	l.name:SetWidth(150);
	l.name:SetJustifyH("LEFT");
	l.items = {};
	for k = 1, 7 do
		local b = CreateFrame("Button", nil, l);
		b:SetWidth(ITEM);
		b:SetHeight(ITEM);
		b:SetPoint("LEFT", l, "LEFT", 220 + (k - 1) * (ITEM + 4) + (k == 7 and 8 or 0), 0);
		b.bg = b:CreateTexture(nil, "BACKGROUND");
		b.bg:SetAllPoints(b);
		b.bg:SetTexture(0, 0, 0, 0.6);
		b.tex = b:CreateTexture(nil, "ARTWORK");
		b.tex:SetAllPoints(b);
		b:SetScript("OnEnter", function(self)
			if ( self.item ) then
				GameTooltip:SetOwner(self, "ANCHOR_RIGHT");
				GameTooltip:SetHyperlink("item:" .. self.item);
				GameTooltip:Show();
			end
		end);
		b:SetScript("OnLeave", function() GameTooltip:Hide(); end);
		l.items[k] = b;
	end
	l.kda = l:CreateFontString(nil, "OVERLAY", "GameFontHighlight");
	l.kda:SetPoint("LEFT", l, "LEFT", 470, 0);
	l.kda:SetWidth(110);
	l.cs = l:CreateFontString(nil, "OVERLAY", "GameFontHighlight");
	l.cs:SetPoint("LEFT", l, "LEFT", 590, 0);
	l.cs:SetWidth(60);
	l.gold = l:CreateFontString(nil, "OVERLAY", "GameFontNormal");
	l.gold:SetPoint("LEFT", l, "LEFT", 660, 0);
	l.gold:SetWidth(70);
	lines[i] = l;
	return l;
end

local ITEMS = CENTURION_RIFT_ITEMS or {};

local function Paint()
	local now = GetTime();
	local teams = { [0] = {}, [1] = {} };
	local kills = { [0] = 0, [1] = 0 };
	for id, r in pairs(rows) do
		if ( now - r.seen > STALE ) then
			rows[id] = nil;
		elseif ( teams[r.team] ) then
			table.insert(teams[r.team], r);
			kills[r.team] = kills[r.team] + r.k;
		end
	end
	for t = 0, 1 do
		table.sort(teams[t], function(a, b) return a.name < b.name; end);
	end
	local c0, c1 = TEAM_COLOUR[0], TEAM_COLOUR[1];
	header:SetText(string.format("|cff%02x%02x%02xORDER  %d|r   |cffc8a96e-|r   |cff%02x%02x%02x%d  CHAOS|r",
		math.floor(c0[1] * 255), math.floor(c0[2] * 255), math.floor(c0[3] * 255), kills[0],
		math.floor(c1[1] * 255), math.floor(c1[2] * 255), math.floor(c1[3] * 255), kills[1]));
	columns:SetText("");
	local me = UnitName("player");
	local myGuid = UnitGUID("player");
	local myId = myGuid and tostring(tonumber(string.sub(myGuid, -8), 16));
	local y = -48;
	local n = 0;
	for t = 0, 1 do
		local c = TEAM_COLOUR[t];
		for _, r in ipairs(teams[t]) do
			n = n + 1;
			local l = Line(n);
			l:ClearAllPoints();
			l:SetPoint("TOPLEFT", board, "TOPLEFT", 12, y);
			l:SetPoint("TOPRIGHT", board, "TOPRIGHT", -12, y);
			l.bg:SetVertexColor(c[1], c[2], c[3], (r.id == myId or r.name == me) and 0.30 or 0.12);
			local token = CLASSES[r.class];
			local coords = token and CLASS_ICON_TCOORDS and CLASS_ICON_TCOORDS[token];
			if ( coords ) then
				l.icon:SetTexCoord(unpack(coords));
			end
			l.level:SetText(r.level);
			l.name:SetText(r.name);
			l.name:SetTextColor(c[1], c[2], c[3]);
			local worth = 0;
			local slot = 0;
			for k = 1, 7 do
				l.items[k].item = nil;
				l.items[k].tex:SetTexture(nil);
			end
			for _, item in ipairs(r.items) do
				if ( slot < 6 ) then
					slot = slot + 1;
					local b = l.items[slot];
					b.item = item;
					b.tex:SetTexture(GetItemIcon(item) or "Interface\\Icons\\INV_Misc_QuestionMark");
				end
				worth = worth + (ITEMS[item] and ITEMS[item].gold or 0);
			end
			l.kda:SetText(string.format("%d / %d / %d", r.k, r.d, r.a));
			l.cs:SetText(r.cs);
			l.gold:SetText(worth > 0 and (worth .. "g") or "");
			l:Show();
			y = y - ROW_H - 4;
		end
		y = y - 10;
	end
	for i = n + 1, #lines do
		lines[i]:Hide();
	end
	if ( n == 0 ) then
		y = y - 30;
	end
	local over = GetBattlefieldWinner and GetBattlefieldWinner();
	if ( over ) then
		leave:Show();
	else
		leave:Hide();
	end
	board:SetHeight(-y + (over and 40 or 8));
end

board:SetScript("OnShow", Paint);
local since = 0;
board:SetScript("OnUpdate", function(self, elapsed)
	since = since + elapsed;
	if ( since > 0.5 ) then
		since = 0;
		Paint();
	end
end);

-- Hold to look (the binding runs on key up too).
BINDING_HEADER_CENTURIONRIFT = "Centurion Summoner's Rift";
BINDING_NAME_CENTURIONRIFT_SCORE = "Scoreboard (hold)";
function CENTURION_RiftScore_Key(keystate)
	if ( keystate == "down" ) then
		board:Show();
	else
		board:Hide();
	end
end
SLASH_CENTURIONRIFTSCORE1 = "/riftscore";
SlashCmdList["CENTURIONRIFTSCORE"] = function()
	if ( board:IsShown() ) then board:Hide(); else board:Show(); end
end;

-- ` holds the scoreboard while in the Rift, if that key is free (League uses Tab,
-- but Tab is WoW's targeting). Given back outside the Rift. Bindings cannot change
-- in combat, so a change waits for it to end.
local SCORE_KEY = "`";
local owner = CreateFrame("Frame");
local bound = false;
local function Rebind()
	if ( InCombatLockdown() ) then
		owner:RegisterEvent("PLAYER_REGEN_ENABLED");
		return;
	end
	local want = InRift();
	if ( want and not bound ) then
		local current = GetBindingAction(SCORE_KEY);
		if ( current == nil or current == "" or current == "CENTURIONRIFT_SCORE" ) then
			SetOverrideBinding(owner, false, SCORE_KEY, "CENTURIONRIFT_SCORE");
		end
		bound = true;
	elseif ( not want and bound ) then
		ClearOverrideBindings(owner);
		bound = false;
		board:Hide();
	end
end

-- The battleground score window (the minimap icon, its key, the match's end) is
-- this one. It is not hidden but kept open out of sight while the board shows:
-- hiding it made every toggle an "open", and at the end of a match WoW reopens a
-- closed score window on every status update - the board could not be closed.
-- So the two open and close together, whichever way either is opened or closed.
local scoreHidden = false;
local function Invisible(on)
	if ( not WorldStateScoreFrame or on == scoreHidden ) then
		return;
	end
	WorldStateScoreFrame:SetAlpha(on and 0 or 1);
	WorldStateScoreFrame:SetScale(on and 0.001 or 1);
	scoreHidden = on;
end
if ( WorldStateScoreFrame ) then
	WorldStateScoreFrame:HookScript("OnShow", function(self)
		if ( InRift() ) then
			Invisible(true);
			board:Show();
		else
			Invisible(false);
		end
	end);
	WorldStateScoreFrame:HookScript("OnHide", function()
		if ( scoreHidden ) then
			board:Hide();
		end
	end);
end
board:HookScript("OnHide", function()
	if ( scoreHidden and WorldStateScoreFrame and WorldStateScoreFrame:IsShown() ) then
		HideUIPanel(WorldStateScoreFrame);
	end
end);

local close = CreateFrame("Button", nil, board, "UIPanelCloseButton");
close:SetPoint("TOPRIGHT", board, "TOPRIGHT", 2, 2);
close:SetScript("OnClick", function() board:Hide(); end);

owner:RegisterEvent("CHAT_MSG_ADDON");
owner:RegisterEvent("ZONE_CHANGED_NEW_AREA");
owner:RegisterEvent("PLAYER_ENTERING_WORLD");
owner:SetScript("OnEvent", function(self, event, prefix, message)
	if ( event == "CHAT_MSG_ADDON" ) then
		if ( prefix ~= ANSWER_PREFIX or type(message) ~= "string" ) then
			return;
		end
		local id, name, team, class, level, k, d, a, cs, items =
			string.match(message, "^RIFTSCORE:(%d+):([^:]*):(%d+):(%d+):(%d+):(%d+):(%d+):(%d+):(%d+):(.*)$");
		if ( not id ) then
			return;
		end
		local list = {};
		for item in string.gmatch(items, "(%d+)") do
			table.insert(list, tonumber(item));
		end
		rows[id] = { id = id, name = name, team = tonumber(team), class = tonumber(class), level = tonumber(level), k = tonumber(k),
			d = tonumber(d), a = tonumber(a), cs = tonumber(cs), items = list, seen = GetTime() };
		return;
	end
	if ( event == "PLAYER_REGEN_ENABLED" ) then
		self:UnregisterEvent("PLAYER_REGEN_ENABLED");
	end
	if ( event ~= "CHAT_MSG_ADDON" and not InRift() ) then
		for id in pairs(rows) do
			rows[id] = nil;
		end
	end
	Rebind();
end);

CENTURION_RiftScoreboardRows = rows;   -- for tests
