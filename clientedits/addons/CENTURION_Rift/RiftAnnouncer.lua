-- Summoner's Rift: League's announcer - the banner at the top of the screen and
-- the announcer's voice.
--
-- The server (BattlegroundSRF) sends one line per event to every champion:
--   CCGAME  RIFTANN:<kind>:<killer>:<victim>:<killer's team>:<arg>:<killer class>:<victim class>
-- kinds: KILL FIRSTBLOOD DOUBLE TRIPLE QUADRA PENTA SPREE (arg = streak) SHUTDOWN
-- (arg = gold) EXECUTED ACE TURRET INHIB INHIBRESPAWN DRAGON BARON MINIONS WELCOME
-- END. Teams are 0 = Order (Alliance), 1 = Chaos (Horde). The banner is worded
-- from where you stand - your kill, an ally's, an enemy's - as League words it,
-- and the line is the announcer's own (CENTURION_RIFT_VO, RiftAnnouncerVO.lua).

local ANSWER_PREFIX = "CCGAME";
local SHOW_TIME, FADE_TIME = 2.6, 0.35;
local ALLY = { 0.30, 0.62, 1.00 };
local ENEMY = { 1.00, 0.30, 0.25 };
local GOLD = { 1.00, 0.84, 0.40 };
local CLASSES = { "WARRIOR", "PALADIN", "HUNTER", "ROGUE", "PRIEST", "DEATHKNIGHT", "SHAMAN", "MAGE", "WARLOCK", nil, "DRUID" };

local SPREES = { [3] = "KILLING SPREE", [4] = "RAMPAGE", [5] = "UNSTOPPABLE", [6] = "DOMINATING", [7] = "GODLIKE" };
local SPREE_VO = { [3] = "killing_spree", [4] = "rampage", [5] = "unstoppable", [6] = "dominating", [7] = "godlike" };
local MULTI = { DOUBLE = "DOUBLE KILL", TRIPLE = "TRIPLE KILL", QUADRA = "QUADRA KILL", PENTA = "PENTAKILL" };

local function MyTeam()
	return UnitFactionGroup("player") == "Horde" and 1 or 0;
end

-- ------------------------------------------------------------------ banner

local banner = CreateFrame("Frame", "CENTURION_RiftBanner", UIParent);
banner:SetWidth(560);
banner:SetHeight(84);
banner:SetPoint("TOP", UIParent, "TOP", 0, -110);
banner:SetFrameStrata("HIGH");
banner:Hide();

local band = banner:CreateTexture(nil, "BACKGROUND");
band:SetTexture(1, 1, 1, 1);
band:SetAllPoints(banner);
band:SetGradientAlpha("HORIZONTAL", 0.02, 0.05, 0.09, 0, 0.02, 0.05, 0.09, 0.85);
local bandRight = banner:CreateTexture(nil, "BACKGROUND");
bandRight:SetTexture(1, 1, 1, 1);
bandRight:SetPoint("TOPLEFT", banner, "TOP", 0, 0);
bandRight:SetPoint("BOTTOMRIGHT", banner, "BOTTOMRIGHT", 0, 0);
bandRight:SetGradientAlpha("HORIZONTAL", 0.02, 0.05, 0.09, 0.85, 0.02, 0.05, 0.09, 0);
band:SetPoint("BOTTOMRIGHT", banner, "BOTTOM", 0, 0);

local function Rule(point)
	local t = banner:CreateTexture(nil, "BORDER");
	t:SetTexture(1, 1, 1, 1);
	t:SetHeight(1);
	t:SetPoint(point .. "LEFT", banner, point .. "LEFT", 60, 0);
	t:SetPoint(point .. "RIGHT", banner, point .. "RIGHT", -60, 0);
	t:SetGradientAlpha("HORIZONTAL", 0.78, 0.66, 0.43, 0, 0.78, 0.66, 0.43, 1);
	return t;
end
local ruleTop, ruleBottom = Rule("TOP"), Rule("BOTTOM");

local title = banner:CreateFontString(nil, "OVERLAY");
title:SetFont("Fonts\\MORPHEUS.TTF", 34, "OUTLINE");
title:SetPoint("TOP", banner, "TOP", 0, -8);
title:SetShadowOffset(2, -2);

local subline = banner:CreateFontString(nil, "OVERLAY");
subline:SetFont("Fonts\\FRIZQT__.TTF", 14, "OUTLINE");
subline:SetPoint("TOP", title, "BOTTOM", 0, -6);

local function ClassIcon(anchor, side)
	local t = banner:CreateTexture(nil, "OVERLAY");
	t:SetWidth(30);
	t:SetHeight(30);
	t:SetTexture("Interface\\WorldStateFrame\\Icons-Classes");
	t:SetPoint(side == "LEFT" and "RIGHT" or "LEFT", anchor, side, side == "LEFT" and -8 or 8, 0);
	t:Hide();
	return t;
end
local leftIcon = ClassIcon(subline, "LEFT");
local rightIcon = ClassIcon(subline, "RIGHT");

local function SetIcon(tex, class)
	local token = CLASSES[tonumber(class) or 0];
	local coords = token and CLASS_ICON_TCOORDS and CLASS_ICON_TCOORDS[token];
	if ( coords ) then
		tex:SetTexCoord(unpack(coords));
		tex:Show();
	else
		tex:Hide();
	end
end

local queue = {};
local age = 0;

local function Paint(a)
	title:SetText(a.title);
	title:SetTextColor(a.colour[1], a.colour[2], a.colour[3]);
	subline:SetText(a.sub or "");
	SetIcon(leftIcon, a.leftClass);
	SetIcon(rightIcon, a.rightClass);
	if ( not a.sub or a.sub == "" ) then
		leftIcon:Hide();
		rightIcon:Hide();
	end
	ruleTop:SetVertexColor(a.colour[1], a.colour[2], a.colour[3]);
	ruleBottom:SetVertexColor(a.colour[1], a.colour[2], a.colour[3]);
end

local function PlayVO(key)
	local vo = CENTURION_RIFT_VO or {};
	local lines = key and vo[key];
	-- the pack lacks a few enemy lines (enemy double kill, enemy pentakill): the plain one
	if ( not lines and key and string.sub(key, 1, 6) == "enemy_" ) then
		lines = vo[string.sub(key, 7)];
	end
	if ( lines and #lines > 0 ) then
		PlaySoundFile(lines[math.random(#lines)]);
	end
end

banner:SetScript("OnUpdate", function(self, elapsed)
	age = age + elapsed;
	if ( age < FADE_TIME ) then
		self:SetAlpha(age / FADE_TIME);
	elseif ( age > SHOW_TIME - FADE_TIME ) then
		self:SetAlpha(math.max(0, (SHOW_TIME - age) / FADE_TIME));
	else
		self:SetAlpha(1);
	end
	if ( age >= SHOW_TIME ) then
		table.remove(queue, 1);
		if ( #queue > 0 ) then
			age = 0;
			Paint(queue[1]);
			PlayVO(queue[1].vo);
		else
			self:Hide();
		end
	end
end);

-- WoW's red error text sits where the banner does: it is out of sight while a
-- banner shows (errors raised then still fade out on their own), back after.
banner:SetScript("OnShow", function()
	if ( UIErrorsFrame ) then
		UIErrorsFrame:Clear();
		UIErrorsFrame:SetAlpha(0);
	end
end);
banner:SetScript("OnHide", function()
	if ( UIErrorsFrame ) then
		UIErrorsFrame:SetAlpha(1);
	end
end);

local function Push(a)
	-- a long backlog (a teamfight) keeps the latest few
	while ( #queue > 4 ) do
		table.remove(queue, 2);
	end
	table.insert(queue, a);
	if ( #queue == 1 ) then
		age = 0;
		Paint(a);
		PlayVO(a.vo);
		banner:SetAlpha(0);
		banner:Show();
	end
end

-- ------------------------------------------------------------------ wording

local function Colour(team)
	return team == MyTeam() and ALLY or ENEMY;
end

local function Coloured(name, team)
	local c = Colour(team);
	return string.format("|cff%02x%02x%02x%s|r", math.floor(c[1] * 255), math.floor(c[2] * 255), math.floor(c[3] * 255), name);
end

-- One event, worded from the player's side; nil when there is nothing to show.
local function Word(kind, killer, victim, team, arg, kclass, vclass)
	local me = UnitName("player");
	local mine = team == MyTeam();
	local other = team == 0 and 1 or 0;
	local names = (killer ~= "" and victim ~= "") and (Coloured(killer, team) .. "  has slain  " .. Coloured(victim, other)) or nil;
	local a = { colour = Colour(team), sub = names, leftClass = kclass, rightClass = vclass };
	if ( kind == "KILL" ) then
		if ( killer == me ) then
			a.title, a.vo = "YOU HAVE SLAIN AN ENEMY", "you_slain_enemy";
		elseif ( victim == me ) then
			a.title, a.vo, a.colour = "YOU HAVE BEEN SLAIN", "you_slain", ENEMY;
		elseif ( mine ) then
			a.title, a.vo = "AN ENEMY HAS BEEN SLAIN", "enemy_slain";
		else
			a.title, a.vo = "AN ALLY HAS BEEN SLAIN", "ally_slain";
		end
	elseif ( kind == "FIRSTBLOOD" ) then
		a.title, a.vo, a.colour = "FIRST BLOOD", "first_blood", GOLD;
	elseif ( MULTI[kind] ) then
		local vo = string.lower(kind) .. "_kill";
		a.title = (mine and "" or "ENEMY ") .. MULTI[kind];
		a.vo = mine and vo or ("enemy_" .. vo);
	elseif ( kind == "SPREE" ) then
		local n = tonumber(arg) or 3;
		a.title = SPREES[n] or "LEGENDARY";
		a.vo = SPREE_VO[n] or "legendary";
		local what = n == 3 and "on a killing spree" or string.lower(SPREES[n] or "legendary");
		a.sub = Coloured(killer, team) .. "  is " .. what .. "!";
		a.rightClass = nil;
	elseif ( kind == "SHUTDOWN" ) then
		a.title, a.vo = "SHUTDOWN", "shutdown";
		a.sub = Coloured(killer, team) .. "  has shut down  " .. Coloured(victim, other) .. "  |cffffd100+" .. (arg or 0) .. " gold|r";
	elseif ( kind == "EXECUTED" ) then
		a.title, a.vo, a.colour = "EXECUTED", "executed", Colour(other);
		a.sub = Coloured(victim, other) .. "  has been executed";
		a.leftClass, a.rightClass = vclass, nil;
	elseif ( kind == "ACE" ) then
		a.title, a.vo, a.sub = "ACE", "ace", nil;
	elseif ( kind == "TURRET" ) then
		a.title = mine and "YOUR TEAM HAS DESTROYED A TURRET" or "YOUR TURRET HAS BEEN DESTROYED";
		a.vo, a.sub = mine and "ally_turret" or "enemy_turret", nil;
	elseif ( kind == "INHIB" ) then
		a.title = mine and "YOUR TEAM HAS DESTROYED AN INHIBITOR" or "YOUR INHIBITOR HAS BEEN DESTROYED";
		a.vo, a.sub = mine and "ally_inhib" or "enemy_inhib", nil;
	elseif ( kind == "INHIBRESPAWN" ) then
		a.title = mine and "YOUR INHIBITOR HAS RESPAWNED" or "THE ENEMY INHIBITOR HAS RESPAWNED";
		a.vo, a.sub = mine and "inhib_respawned" or "enemy_inhib_respawned", nil;
	elseif ( kind == "DRAGON" ) then
		a.title = mine and "YOUR TEAM HAS SLAIN THE DRAGON" or "THE ENEMY TEAM HAS SLAIN THE DRAGON";
		a.vo, a.sub = mine and "ally_dragon" or "enemy_dragon", nil;
	elseif ( kind == "BARON" ) then
		a.title = mine and "YOUR TEAM HAS SLAIN BARON NASHOR" or "THE ENEMY TEAM HAS SLAIN BARON NASHOR";
		a.vo, a.sub = mine and "ally_baron" or "enemy_baron", nil;
	elseif ( kind == "MINIONS" ) then
		a.title, a.vo, a.sub, a.colour = "MINIONS HAVE SPAWNED", "minions_spawned", nil, GOLD;
	elseif ( kind == "WELCOME" ) then
		a.title, a.vo, a.sub, a.colour = "WELCOME TO SUMMONER'S RIFT", "welcome", nil, GOLD;
	elseif ( kind == "END" ) then
		a.title = mine and "VICTORY" or "DEFEAT";
		a.vo, a.sub, a.colour = mine and "victory" or "defeat", nil, mine and GOLD or ENEMY;
	else
		return nil;
	end
	if ( kind ~= "KILL" and kind ~= "SPREE" and kind ~= "SHUTDOWN" and kind ~= "EXECUTED" and not MULTI[kind] and kind ~= "FIRSTBLOOD" ) then
		a.leftClass, a.rightClass = nil, nil;
	end
	return a;
end

CENTURION_RiftAnnouncer = { Word = Word, Push = Push };   -- for tests and /riftann

local events = CreateFrame("Frame");
events:RegisterEvent("CHAT_MSG_ADDON");
events:SetScript("OnEvent", function(self, event, prefix, message)
	if ( prefix ~= ANSWER_PREFIX or type(message) ~= "string" ) then
		return;
	end
	local kind, rest = string.match(message, "^RIFTANN:([A-Z]+):(.*)$");
	if ( not kind ) then
		return;
	end
	local f = {};
	for field in string.gmatch(rest .. ":", "([^:]*):") do
		table.insert(f, field);
	end
	local a = Word(kind, f[1] or "", f[2] or "", tonumber(f[3]) or 0, f[4], f[5], f[6]);
	if ( a ) then
		Push(a);
	end
end);
