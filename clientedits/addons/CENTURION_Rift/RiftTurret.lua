-- Summoner's Rift: League's turret warnings.
--
--  - A sound when a turret turns on you (the server: CCGAME RIFTTURRET:1, sent
--    once each time a turret makes you its target).
--  - League's range ring: a dotted circle on the ground round each standing
--    enemy turret you are near. The server sends each turret's reach once a
--    minute as 36 ground heights (BattlegroundSRF::SendRings):
--      CCGAME RIFTRING:<slot>:<enemy 0/1>:<x>,<y>,<z>:<dz1>,...,<dz36>
--    (heights in tenths of a yard off the turret's own). The dots are placed by
--    the client DLL - CenturionUnitBar(0, x, y, z) projects a world point the
--    way the unit bars are placed, CenturionUnitBar(-1) is the player's
--    position; without them there is no ring.

local PREFIX = "CCGAME";
local ZONE = "Summoner's Rift";
local RANGE = 50;              -- yards: BattlegroundSRF's SRF_TURRET_RANGE
local SHOW_WITHIN = 25;        -- yards outside the ring it starts to show
local POINTS = 36;             -- heights a ring is sent with
local DOTS = 90;               -- dots drawn round it (every 3.5 yards)

local WHITE = "Interface\\Buttons\\WHITE8X8";

local rings, standing = {}, {};

-- ------------------------------------------------------------------ the dots

local layer = CreateFrame("Frame", "CENTURION_RiftTurretRings", WorldFrame);
layer:SetAllPoints(WorldFrame);
layer:SetFrameStrata("BACKGROUND");
local pool, shown = {}, 0;

local function Dot(i)
	local dot = pool[i];
	if ( not dot ) then
		dot = layer:CreateTexture(nil, "ARTWORK");
		dot:SetTexture(WHITE);
		pool[i] = dot;
	end
	return dot;
end

local function HeightAt(ring, a)
	-- the ring's ground height at angle a (radians), between its two nearest samples
	local f = a / (2 * math.pi) * POINTS;
	local i = math.floor(f);
	local t = f - i;
	local h0 = ring.dz[(i % POINTS) + 1] or 0;
	local h1 = ring.dz[((i + 1) % POINTS) + 1] or 0;
	return ring.z + (h0 + (h1 - h0) * t) / 10;
end

local function Draw()
	local used = 0;
	if ( type(CenturionUnitBar) == "function" and GetRealZoneText() == ZONE ) then
		local px, py = CenturionUnitBar(-1);
		if ( px ) then
			local width, height = WorldFrame:GetWidth(), WorldFrame:GetHeight();
			for slot, ring in pairs(rings) do
				local dx, dy = ring.x - px, ring.y - py;
				if ( ring.enemy and standing[slot] and dx * dx + dy * dy <= (RANGE + SHOW_WITHIN) ^ 2 ) then
					for k = 0, DOTS - 1 do
						local a = 2 * math.pi * k / DOTS;
						local x, y = ring.x + math.cos(a) * RANGE, ring.y + math.sin(a) * RANGE;
						local fx, fy, scale = CenturionUnitBar(0, x, y, HeightAt(ring, a) + 0.15);
						if ( fx and fx > -0.05 and fx < 1.05 and fy > -0.05 and fy < 1.05 ) then
							used = used + 1;
							local dot = Dot(used);
							local size = math.min(math.max((scale or 0) * height * 0.45, 2), 7);
							dot:SetWidth(size);
							dot:SetHeight(size);
							dot:SetVertexColor(1, 0.18, 0.12, 0.85);
							dot:ClearAllPoints();
							dot:SetPoint("CENTER", WorldFrame, "BOTTOMLEFT", fx * width, fy * height);
							dot:Show();
						end
					end
				end
			end
		end
	end
	for i = used + 1, shown do
		pool[i]:Hide();
	end
	shown = used;
end

layer:SetScript("OnUpdate", Draw);

-- ------------------------------------------------------------------ the feed

local events = CreateFrame("Frame");
events:RegisterEvent("CHAT_MSG_ADDON");
events:RegisterEvent("ZONE_CHANGED_NEW_AREA");
events:RegisterEvent("PLAYER_ENTERING_WORLD");
events:SetScript("OnEvent", function(self, event, prefix, message)
	if ( event ~= "CHAT_MSG_ADDON" ) then
		if ( GetRealZoneText() ~= ZONE ) then
			rings, standing = {}, {};
		end
		return;
	end
	if ( prefix ~= PREFIX or type(message) ~= "string" ) then
		return;
	end
	if ( message == "RIFTTURRET:1" ) then
		PlaySound("RaidWarning");
		return;
	end
	local slot, enemy, x, y, z, list = string.match(message, "^RIFTRING:(%d+):(%d):(%-?%d+),(%-?%d+),(%-?%d+):(.*)$");
	if ( slot ) then
		local dz = {};
		for v in string.gmatch(list, "(%-?%d+)") do
			dz[#dz + 1] = tonumber(v);
		end
		rings[tonumber(slot)] = { enemy = enemy == "1", x = tonumber(x), y = tonumber(y), z = tonumber(z), dz = dz };
		return;
	end
	-- Which turrets still stand: RiftMap's feed (RIFTSTR:<slot>,<kind>,<x>,<y>,<health %>;...)
	local structures = string.match(message, "^RIFTSTR:(.*)$");
	if ( structures ) then
		for s, hp in string.gmatch(structures, "(%d+),%d+,%-?%d+,%-?%d+,(%d+)") do
			standing[tonumber(s)] = tonumber(hp) > 0;
		end
	end
end);
