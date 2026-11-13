-- CENTURION_VioletHold - every rune on the Violet Hold's floor named over it, the
-- freeze's 30-yard reach and the timebomb's 20-yard blast drawn round theirs, and
-- the blast round anyone carrying a ticking core.
--
-- The runes dropped when a wave falls stay until the next wave falls. The server
-- (BattlegroundVHR::SendRuneFeed) tells the party where they are whenever that
-- changes, on the CCGAME addon whisper:
--
--    VHRUNE:<locked>;<x>,<y>,<z>,<entry>;...  "VHRUNE:0" alone = none left
--
-- <locked> is 1 while the runes cannot be taken yet - they wait for the next
-- wave to be out - and their names are faded and grey until then; 0 once they
-- can (it was the milliseconds left back when runes expired, and any other
-- number reads as taken-able). The labels are placed with the client DLL -
-- CenturionUnitBar(0, x, y, z) projects a world point to the screen and says how
-- big a yard is there. With no DLL there are no labels. Nothing is sent outside
-- the Hold, so nothing shows there.

local PREFIX = "CCGAME";
local ABOVE = 2.2;            -- yards above the rune's base the name hangs
local WHITE = "Interface\\Buttons\\WHITE8X8";

local runes = {};
local runesLocked = false;    -- the wave is not out: names faded and grey
local bombs = {};             -- carriers of a Volatile Core: { guid, x, y, z }

-- What each rune does, said over it in its crystal's colour.
local NAMES = {
	[179905] = { "Berserking",           1.00, 0.55, 0.20 },
	[300500] = { "Reset Cooldowns",      1.00, 0.45, 0.75 },
	[300501] = { "Freeze Enemies",       0.45, 0.75, 1.00 },
	[300502] = { "You're a Timebomb",    1.00, 0.60, 0.20 },
	[300503] = { "Spawn a Clone of You", 1.00, 0.90, 0.35 },
};

local layer = CreateFrame("Frame", "CENTURION_VioletHoldRunes", WorldFrame);
layer:SetAllPoints(WorldFrame);
layer:SetFrameStrata("BACKGROUND");

local pool = {};

-- What a rune will catch, as a ring of dots on the floor round it (the same
-- dots the Rift draws its turret range with). Each is taken standing on the
-- rune, so its reach starts there:
--   Freeze Enemies    every enemy within 30 yards (94313, SpellRadius 10)
--   You're a Timebomb  the blast, 20 yards round whoever carries it (94314,
--                     SpellRadius 9) - drawn round the rune, and round every
--                     carrier for as long as the core ticks (VHBOMB)
local RINGS = {
	[300501] = { radius = 30, dots = 80, r = 0.45, g = 0.75, b = 1.00 },
	[300502] = { radius = 20, dots = 60, r = 1.00, g = 0.55, b = 0.15 },
};
local BOMB_RING = RINGS[300502];
local dots, dotsShown = {}, 0;

local function Dot(i)
	local dot = dots[i];
	if ( not dot ) then
		dot = layer:CreateTexture(nil, "ARTWORK");
		dot:SetTexture(WHITE);
		dots[i] = dot;
	end
	return dot;
end

local function Ring(used, ring, x, y, z, width, height)
	for k = 0, ring.dots - 1 do
		local a = 2 * math.pi * k / ring.dots;
		local fx, fy, scale = CenturionUnitBar(0, x + math.cos(a) * ring.radius, y + math.sin(a) * ring.radius, z + 0.15);
		if ( fx and fx > -0.05 and fx < 1.05 and fy > -0.05 and fy < 1.05 ) then
			used = used + 1;
			local dot = Dot(used);
			local size = math.min(math.max((scale or 0) * height * 0.3, 1.5), 4);
			dot:SetVertexColor(ring.r, ring.g, ring.b, 0.35);
			dot:SetWidth(size);
			dot:SetHeight(size);
			dot:ClearAllPoints();
			dot:SetPoint("CENTER", WorldFrame, "BOTTOMLEFT", fx * width, fy * height);
			dot:Show();
		end
	end
	return used;
end

local function DrawRings(width, height)
	local used = 0;
	for _, rune in ipairs(runes) do
		local ring = RINGS[rune.entry];
		if ( ring ) then
			used = Ring(used, ring, rune.x, rune.y, rune.z, width, height);
		end
	end
	-- Your own core follows you every frame; a party member's moves with the
	-- server's reports, five a second.
	local me = UnitGUID and UnitGUID("player");
	for _, bomb in ipairs(bombs) do
		local x, y, z = bomb.x, bomb.y, bomb.z;
		if ( me and string.upper(bomb.guid) == string.upper(me) ) then
			local px, py, pz = CenturionUnitBar(-1);
			if ( px ) then x, y, z = px, py, pz; end
		end
		used = Ring(used, BOMB_RING, x, y, z, width, height);
	end
	for i = used + 1, dotsShown do
		dots[i]:Hide();
	end
	dotsShown = used;
end

local function Label(i)
	local label = pool[i];
	if ( not label ) then
		label = CreateFrame("Frame", nil, layer);
		label:SetWidth(1);
		label:SetHeight(1);
		label.name = label:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall");
		label.name:SetPoint("BOTTOM", label, "CENTER", 0, 0);
		pool[i] = label;
	end
	return label;
end

local function HideAll()
	for k = 1, #pool do
		pool[k]:Hide();
	end
	for i = 1, dotsShown do
		dots[i]:Hide();
	end
	dotsShown = 0;
end

local function Draw()
	if ( (#runes == 0 and #bombs == 0) or type(CenturionUnitBar) ~= "function" ) then
		HideAll();
		return;
	end
	local width, height = WorldFrame:GetWidth(), WorldFrame:GetHeight();
	DrawRings(width, height);
	local used = 0;
	for _, rune in ipairs(runes) do
		local fx, fy = CenturionUnitBar(0, rune.x, rune.y, rune.z + ABOVE);
		local named = NAMES[rune.entry];
		if ( named and fx and fx > -0.05 and fx < 1.05 and fy > -0.05 and fy < 1.05 ) then
			used = used + 1;
			local label = Label(used);
			label.name:SetText(named[1]);
			if ( runesLocked ) then
				label.name:SetTextColor(0.5, 0.5, 0.5);
				label:SetAlpha(0.5);
			else
				label.name:SetTextColor(named[2], named[3], named[4]);
				label:SetAlpha(1);
			end
			label:ClearAllPoints();
			label:SetPoint("CENTER", WorldFrame, "BOTTOMLEFT", fx * width, fy * height);
			label:Show();
		end
	end
	for k = used + 1, #pool do
		pool[k]:Hide();
	end
end

layer:SetScript("OnUpdate", Draw);

local events = CreateFrame("Frame");
events:RegisterEvent("CHAT_MSG_ADDON");
events:RegisterEvent("PLAYER_ENTERING_WORLD");
events:SetScript("OnEvent", function(self, event, prefix, message)
	if ( event == "PLAYER_ENTERING_WORLD" ) then
		runes = {};
		runesLocked = false;
		bombs = {};
		return;
	end
	if ( prefix ~= PREFIX or type(message) ~= "string" ) then
		return;
	end
	local carriers = string.match(message, "^VHBOMB(.*)$");
	if ( carriers ) then
		bombs = {};
		for item in string.gmatch(carriers, ";([^;]+)") do
			local guid, x, y, z = string.match(item, "^(0x%x+),(%-?[%d%.]+),(%-?[%d%.]+),(%-?[%d%.]+)$");
			if ( guid ) then
				bombs[#bombs + 1] = { guid = guid, x = tonumber(x), y = tonumber(y), z = tonumber(z) };
			end
		end
		return;
	end
	local state, list = string.match(message, "^VHRUNE:(%d+)(.*)$");
	if ( not list ) then
		return;
	end
	runesLocked = (state == "1");
	runes = {};
	for item in string.gmatch(list, ";([^;]+)") do
		local x, y, z, entry = string.match(item, "^(%-?[%d%.]+),(%-?[%d%.]+),(%-?[%d%.]+),?(%d*)$");
		if ( x ) then
			runes[#runes + 1] = { x = tonumber(x), y = tonumber(y), z = tonumber(z), entry = tonumber(entry) };
		end
	end
end);
