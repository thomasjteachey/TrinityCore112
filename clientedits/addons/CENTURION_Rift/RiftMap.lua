-- Summoner's Rift on the world map and the battlefield map: every structure,
-- in its side's colour (grey once destroyed) with its health on hover, and the
-- enemy champions your team can see - League's vision (BattlegroundSRF::SeenBy:
-- near an allied champion, minion or turret, and not hidden in brush).
-- The server sends both once a second:
--   CCGAME  RIFTSTR:<slot>,<kind>,<x>,<y>,<health %>;...   (slots 0-14 Order, 15-29 Chaos)
--   CCGAME  RIFTSEEN:<guid>,<class>,<x>,<y>;...
-- Names come from the scoreboard's lines (RIFTSCORE).

local PREFIX = "CCGAME";
local ZONE = "Summoner's Rift";
local MAP_AREA = 9535;                 -- WorldMapArea row; GetCurrentMapAreaID answers it + 1
-- That row's rectangle, in world coordinates: Left/Right are y (west/east), Top/Bottom x.
local LEFT, RIGHT, TOP, BOTTOM = 343.532, -339.702, 227.489, -227.096;
local PER_TEAM = 15;
local KIND = { [0] = "Turret", [1] = "Inhibitor", [2] = "Nexus" };
local TEAM_NAME = { [0] = "Order", [1] = "Chaos" };
local TEAM_COLOUR = { [0] = { 0.30, 0.62, 1.00 }, [1] = { 1.00, 0.30, 0.25 } };
local CLASS_TOKEN = { "WARRIOR", "PALADIN", "HUNTER", "ROGUE", "PRIEST", "DEATHKNIGHT", "SHAMAN", "MAGE", "WARLOCK", nil, "DRUID" };
local STALE = 3;                       -- seconds without a feed: the enemies come off
local WHITE = "Interface\\Buttons\\WHITE8X8";
local DOT = "Interface\\WorldMap\\WorldMapPartyIcon";

local structures, enemies, names = {}, {}, {};
local enemiesAt = 0;

local function ToMap(x, y)
	return (LEFT - y) / (LEFT - RIGHT), (TOP - x) / (TOP - BOTTOM);
end

local function ShowingRift()
	if ( GetRealZoneText() ~= ZONE or type(GetCurrentMapAreaID) ~= "function" ) then
		return false;
	end
	return GetCurrentMapAreaID() - 1 == MAP_AREA;
end

-- ------------------------------------------------------------------ icons

local function Icon_OnEnter(self)
	local tip = self.surface.tooltip;
	if ( self:GetCenter() > self:GetParent():GetCenter() ) then
		tip:SetOwner(self, "ANCHOR_LEFT");
	else
		tip:SetOwner(self, "ANCHOR_RIGHT");
	end
	if ( self.structure ) then
		local s = self.structure;
		local c = TEAM_COLOUR[s.team];
		tip:SetText(TEAM_NAME[s.team] .. " " .. (KIND[s.kind] or "Structure"), c[1], c[2], c[3]);
		if ( s.hp > 0 ) then
			tip:AddLine(string.format("Health: %d%%", s.hp), 1, 1, 1);
		else
			tip:AddLine("Destroyed", 0.6, 0.6, 0.6);
		end
	elseif ( self.enemy ) then
		local e = self.enemy;
		tip:SetText(names[e.id] or "Enemy champion", 1, 0.3, 0.25);
		local token = CLASS_TOKEN[e.class];
		if ( token and LOCALIZED_CLASS_NAMES_MALE ) then
			tip:AddLine(LOCALIZED_CLASS_NAMES_MALE[token] or token, 1, 1, 1);
		end
	end
	tip:Show();
end

local function Icon_OnLeave(self)
	self.surface.tooltip:Hide();
end

local function Icon_OnHide(self)
	if ( self.surface.tooltip:IsOwned(self) ) then
		self.surface.tooltip:Hide();
	end
end

local function Icon_OnMouseUp(self, button)
	if ( self.surface.onClick ) then
		self.surface.onClick(button);
	end
end

local function NewIcon(surface)
	local icon = CreateFrame("Frame", nil, surface.parent);
	icon:SetFrameLevel(surface.parent:GetFrameLevel() + 6);
	icon.surface = surface;
	icon.edge = icon:CreateTexture(nil, "BORDER");
	icon.edge:SetTexture(WHITE);
	icon.edge:SetVertexColor(0, 0, 0, 0.85);
	icon.edge:SetAllPoints(icon);
	icon.fill = icon:CreateTexture(nil, "ARTWORK");
	icon.fill:SetPoint("TOPLEFT", icon, "TOPLEFT", 1, -1);
	icon.fill:SetPoint("BOTTOMRIGHT", icon, "BOTTOMRIGHT", -1, 1);
	icon:EnableMouse(true);
	icon:SetScript("OnEnter", Icon_OnEnter);
	icon:SetScript("OnLeave", Icon_OnLeave);
	icon:SetScript("OnHide", Icon_OnHide);
	icon:SetScript("OnMouseUp", Icon_OnMouseUp);
	return icon;
end

local surfaces = {};

local function Acquire(surface, i)
	local icon = surface.pool[i];
	if ( not icon ) then
		icon = NewIcon(surface);
		surface.pool[i] = icon;
	end
	return icon;
end

local function Draw(surface)
	local frame = surface.frame;
	local used = 0;
	if ( frame:IsShown() and ShowingRift() ) then
		local width, height = frame:GetWidth(), frame:GetHeight();
		for slot, s in pairs(structures) do
			used = used + 1;
			local icon = Acquire(surface, used);
			local size = (s.kind == 2 and 1.6 or (s.kind == 1 and 1.25 or 1)) * surface.size;
			icon:SetWidth(size);
			icon:SetHeight(size);
			icon.fill:SetTexture(WHITE);
			icon.fill:SetTexCoord(0, 1, 0, 1);
			local c = TEAM_COLOUR[s.team];
			if ( s.hp > 0 ) then
				icon.fill:SetVertexColor(c[1], c[2], c[3], 1);
				icon.edge:Show();
			else
				icon.fill:SetVertexColor(0.45, 0.45, 0.45, 0.6);
				icon.edge:Hide();
			end
			icon.structure, icon.enemy = s, nil;
			local fx, fy = ToMap(s.x, s.y);
			icon:ClearAllPoints();
			icon:SetPoint("CENTER", frame, "TOPLEFT", fx * width, -fy * height);
			icon:Show();
		end
		if ( GetTime() - enemiesAt <= STALE ) then
			for _, e in ipairs(enemies) do
				used = used + 1;
				local icon = Acquire(surface, used);
				local size = 1.4 * surface.size;
				icon:SetWidth(size);
				icon:SetHeight(size);
				icon.edge:Hide();
				icon.fill:SetTexture(DOT);
				icon.fill:SetVertexColor(1, 0.25, 0.2, 1);
				icon.structure, icon.enemy = nil, e;
				local fx, fy = ToMap(e.x, e.y);
				icon:ClearAllPoints();
				icon:SetPoint("CENTER", frame, "TOPLEFT", fx * width, -fy * height);
				icon:Show();
			end
		end
	end
	for i = used + 1, #surface.pool do
		surface.pool[i]:Hide();
	end
end

local function Redraw()
	for _, surface in ipairs(surfaces) do
		Draw(surface);
	end
end

-- Structures and enemies go on WorldMapButton, the map's click layer, where
-- Blizzard keeps its own icons; a click on one is handed to the map below.
table.insert(surfaces, { frame = WorldMapDetailFrame, parent = WorldMapButton, tooltip = WorldMapTooltip, size = 10, pool = {},
	onClick = function(button) WorldMapButton_OnClick(WorldMapButton, button); end });

-- The battlefield map loads on demand: picked up when it arrives.
local function AttachBattlefieldMap()
	if ( BattlefieldMinimap and not BattlefieldMinimap.CENTURION_RiftMap ) then
		BattlefieldMinimap.CENTURION_RiftMap = true;
		table.insert(surfaces, { frame = BattlefieldMinimap, parent = BattlefieldMinimap, tooltip = GameTooltip, size = 7, pool = {} });
		BattlefieldMinimap:HookScript("OnShow", Redraw);
	end
end
AttachBattlefieldMap();

-- ------------------------------------------------------------------ the feed

local events = CreateFrame("Frame");
events:RegisterEvent("CHAT_MSG_ADDON");
events:RegisterEvent("WORLD_MAP_UPDATE");
events:RegisterEvent("ADDON_LOADED");
events:RegisterEvent("ZONE_CHANGED_NEW_AREA");
events:RegisterEvent("PLAYER_ENTERING_WORLD");
events:SetScript("OnEvent", function(self, event, arg1, arg2)
	if ( event == "CHAT_MSG_ADDON" ) then
		if ( arg1 ~= PREFIX or type(arg2) ~= "string" ) then
			return;
		end
		local list = string.match(arg2, "^RIFTSTR:(.*)$");
		if ( list ) then
			for slot, kind, x, y, hp in string.gmatch(list, "(%d+),(%d+),(%-?%d+),(%-?%d+),(%d+)") do
				slot = tonumber(slot);
				structures[slot] = { team = slot < PER_TEAM and 0 or 1, kind = tonumber(kind), x = tonumber(x), y = tonumber(y), hp = tonumber(hp) };
			end
			Redraw();
			return;
		end
		list = string.match(arg2, "^RIFTSEEN:(.*)$");
		if ( list ) then
			enemies = {};
			for id, class, x, y in string.gmatch(list, "(%d+),(%d+),(%-?%d+),(%-?%d+)") do
				table.insert(enemies, { id = id, class = tonumber(class), x = tonumber(x), y = tonumber(y) });
			end
			enemiesAt = GetTime();
			Redraw();
			return;
		end
		local id, name = string.match(arg2, "^RIFTSCORE:(%d+):([^:]*):");
		if ( id ) then
			names[id] = name;
		end
	elseif ( event == "ADDON_LOADED" ) then
		if ( arg1 == "Blizzard_BattlefieldMinimap" ) then
			AttachBattlefieldMap();
			Redraw();
		end
	elseif ( event == "ZONE_CHANGED_NEW_AREA" or event == "PLAYER_ENTERING_WORLD" ) then
		if ( GetRealZoneText() ~= ZONE ) then
			structures, enemies, names = {}, {}, {};
		end
		Redraw();
	else
		Redraw();
	end
end);

-- WORLD_MAP_UPDATE does not fire on close.
WorldMapFrame:HookScript("OnHide", Redraw);
