-- CENTURION_VioletHold - every rune on the Violet Hold's floor named over it, and
-- Glacial Stasis's 30-yard reach drawn round it.
--
-- The runes dropped when a wave falls stay until the next wave falls. The server
-- (BattlegroundVHR::SendRuneFeed) tells the party where they are whenever that
-- changes, on the CCGAME addon whisper:
--
--    VHRUNE:0;<x>,<y>,<z>,<entry>;...         "VHRUNE:0" alone = none left
--
-- (the number was the milliseconds left when runes expired; they no longer do,
-- and it is ignored), and the labels are placed with the client DLL -
-- CenturionUnitBar(0, x, y, z) projects a world point to the screen and says how
-- big a yard is there. With no DLL there are no labels. Nothing is sent outside
-- the Hold, so nothing shows there.

local PREFIX = "CCGAME";
local ABOVE = 2.2;            -- yards above the rune's base the name hangs
local WHITE = "Interface\\Buttons\\WHITE8X8";

local runes = {};

-- What each rune is called, in its crystal's colour.
local NAMES = {
	[179905] = { "Berserking",        1.00, 0.55, 0.20 },
	[300500] = { "Recharge",          1.00, 0.45, 0.75 },
	[300501] = { "Glacial Stasis",    0.45, 0.75, 1.00 },
	[300502] = { "Volatile Core",     1.00, 0.60, 0.20 },
	[300503] = { "Bright Reflection", 1.00, 0.90, 0.35 },
};

local layer = CreateFrame("Frame", "CENTURION_VioletHoldRunes", WorldFrame);
layer:SetAllPoints(WorldFrame);
layer:SetFrameStrata("BACKGROUND");

local pool = {};

-- Glacial Stasis freezes every enemy within 30 yards of whoever takes it, and
-- they take it standing on the rune: a ring of dots on the floor that far out
-- shows what it will catch (the same dots the Rift draws its turret range with).
local RING_ENTRY = 300501;
local RING_RADIUS = 30;       -- yards: spell 94313's radius (SpellRadius 10)
local RING_DOTS = 80;
local dots, dotsShown = {}, 0;

local function Dot(i)
	local dot = dots[i];
	if ( not dot ) then
		dot = layer:CreateTexture(nil, "ARTWORK");
		dot:SetTexture(WHITE);
		dot:SetVertexColor(0.45, 0.75, 1.00, 0.85);
		dots[i] = dot;
	end
	return dot;
end

local function DrawRings(width, height)
	local used = 0;
	for _, rune in ipairs(runes) do
		if ( rune.entry == RING_ENTRY ) then
			for k = 0, RING_DOTS - 1 do
				local a = 2 * math.pi * k / RING_DOTS;
				local fx, fy, scale = CenturionUnitBar(0, rune.x + math.cos(a) * RING_RADIUS,
					rune.y + math.sin(a) * RING_RADIUS, rune.z + 0.15);
				if ( fx and fx > -0.05 and fx < 1.05 and fy > -0.05 and fy < 1.05 ) then
					used = used + 1;
					local dot = Dot(used);
					local size = math.min(math.max((scale or 0) * height * 0.45, 2), 7);
					dot:SetWidth(size);
					dot:SetHeight(size);
					dot:ClearAllPoints();
					dot:SetPoint("CENTER", WorldFrame, "BOTTOMLEFT", fx * width, fy * height);
					dot:Show();
				end
			end
		end
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
	if ( #runes == 0 or type(CenturionUnitBar) ~= "function" ) then
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
			label.name:SetTextColor(named[2], named[3], named[4]);
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
		return;
	end
	if ( prefix ~= PREFIX or type(message) ~= "string" ) then
		return;
	end
	local list = string.match(message, "^VHRUNE:%d+(.*)$");
	if ( not list ) then
		return;
	end
	runes = {};
	for item in string.gmatch(list, ";([^;]+)") do
		local x, y, z, entry = string.match(item, "^(%-?[%d%.]+),(%-?[%d%.]+),(%-?[%d%.]+),?(%d*)$");
		if ( x ) then
			runes[#runes + 1] = { x = tonumber(x), y = tonumber(y), z = tonumber(z), entry = tonumber(entry) };
		end
	end
end);
