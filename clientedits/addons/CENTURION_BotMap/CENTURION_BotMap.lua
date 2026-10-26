-- Playerbots on the world map and the battlefield minimap, drawn the way party
-- members are.
--
-- Blizzard places a party blip by reading a unit's NORMALISED map position and
-- anchoring a texture to the map's TOPLEFT corner -- Blizzard_BattlefieldMinimap.lua
-- does exactly this at line 379:
--
--     local partyX, partyY = GetPlayerMapPosition(unit)
--     partyX =  partyX * BattlefieldMinimap:GetWidth()
--     partyY = -partyY * BattlefieldMinimap:GetHeight()
--     frame:SetPoint("CENTER", "BattlefieldMinimap", "TOPLEFT", partyX, partyY)
--
-- The arithmetic below is that, unchanged, which is also why the same code
-- serves both surfaces: a normalised position anchored to a frame's TOPLEFT
-- does not care which frame it is.
--
-- The only difference from a party member is where the numbers come from.
-- GetPlayerMapPosition answers for "player", "party1-4" and "raid1-40" and
-- nothing else, so a bot's position cannot be asked for locally; the server
-- sends it already normalised, in the same 0..1 space, over the CCGAME addon
-- channel the realm already uses.
--
-- The icon is Blizzard's own party icon tinted red, so a bot reads as the same
-- kind of thing as a party member at a glance without being mistaken for one.
--
-- Graveyards ride along on the same surfaces. Unlike bots they never move, so
-- they are not fed by the server at all: CENTURION_BotMap_Graveyards.lua is a
-- baked table of every zone's graveyards in the same 0..1 space, and they draw
-- for everyone, on any zone the map is turned to, War Mode or not.

local BOT_ICON  = "Interface\\WorldMap\\WorldMapPartyIcon"
local MAX_BLIPS = 80

-- The neutral battleground tombstone - neutral because no graveyard here
-- belongs to a side - which is landmark 8 on the map's own POI sheet. The sheet
-- is laid out on an 18-pixel grid, not 16: landmark 7 at 126-144 is the red
-- Horde banner, and a 16-pixel crop at 128 lands on that flag. Cell 8 spans
-- 144-162 and its neighbours' pixels stop at 142 and start at 163, so the cell
-- crops clean.
local GRAVE_ICON   = "Interface\\Minimap\\POIIcons"
local GRAVE_COORDS = { 144 / 256, 162 / 256, 0, 18 / 256 }

-- Spirit-healer blue. The stone is a dark brown sprite, and a vertex colour can
-- only multiply, so tinting it as it is just turns it muddy. It is greyed
-- first, so the colour lands evenly, then drawn a second time additively on top
-- to lift it back to full brightness; the black outline stays black either way.
local GRAVE_TINT   = { 0.55, 0.85, 1.0 }

local function GraveLayer(stone, layer)
	local tex = stone:CreateTexture(nil, layer)
	tex:SetAllPoints(stone)
	tex:SetTexture(GRAVE_ICON)
	tex:SetTexCoord(unpack(GRAVE_COORDS))
	tex:SetDesaturated(true)
	tex:SetVertexColor(unpack(GRAVE_TINT))
	return tex
end

local bots     = {}     -- the completed set: { name, x, y }
local pending  = {}     -- chunks still arriving
local surfaces = {}     -- see AddSurface
local lastPush = 0      -- GetTime() of the last COMPLETE set

-- The server pushes about every two seconds and says nothing when it stops -
-- and stopping is normal: standing in a capital disarms War Mode, so the feed
-- simply ends. Silence has to mean the positions have expired, or the last set
-- before walking into Orgrimmar would hang on the map forever.
local STALE_AFTER = 7

local frame = CreateFrame("Frame", "CenturionBotMapFrame", UIParent)

------------------------------------------------------------------
-- surfaces
------------------------------------------------------------------
-- A surface is any frame whose corners are the corners of the zone. Both the
-- world map's detail frame and the battlefield minimap qualify, which is the
-- whole reason one set of coordinates serves both.
--
-- graves says how that surface draws graveyards: size, the frame the stones
-- are parented to, and, where the stones should answer the mouse, the tooltip
-- to show and what a click on one should do instead.
local function AddSurface(mapFrame, iconSize, graves)
	if not mapFrame then
		return
	end
	for i = 1, #surfaces do
		if surfaces[i].frame == mapFrame then
			return
		end
	end
	graves.pool = {}
	table.insert(surfaces, { frame = mapFrame, size = iconSize, blips = {}, graves = graves })
end

local function AcquireBlip(surface, index)
	local blip = surface.blips[index]
	if not blip then
		blip = surface.frame:CreateTexture(nil, "OVERLAY")
		blip:SetTexture(BOT_ICON)
		blip:SetVertexColor(1.0, 0.15, 0.15)
		blip:SetWidth(surface.size)
		blip:SetHeight(surface.size)
		surface.blips[index] = blip
	end
	return blip
end

local function HideFrom(surface, index)
	for i = index, #surface.blips do
		surface.blips[i]:Hide()
	end
end

------------------------------------------------------------------
-- graveyards
------------------------------------------------------------------
local function Grave_OnEnter(self)
	local tooltip = self.tooltip
	-- Open away from the nearer edge, as WorldMapUnit_OnEnter does.
	if self:GetCenter() > self:GetParent():GetCenter() then
		tooltip:SetOwner(self, "ANCHOR_LEFT")
	else
		tooltip:SetOwner(self, "ANCHOR_RIGHT")
	end
	tooltip:SetText("Graveyard")
	tooltip:AddLine(self.label, 1, 1, 1)
	tooltip:Show()
end

local function Grave_OnLeave(self)
	self.tooltip:Hide()
end

-- A stone can vanish under the cursor when the map is turned to another zone;
-- its tooltip must not outlive it.
local function Grave_OnHide(self)
	if self.tooltip:IsOwned(self) then
		self.tooltip:Hide()
	end
end

-- Taking the mouse for the tooltip also means catching the map's clicks, so
-- hand them to the map underneath: right-click still zooms out over a stone.
local function Grave_OnMouseUp(self, mouseButton)
	self.onClick(mouseButton)
end

local function AcquireGrave(surface, index)
	local graves = surface.graves
	local stone = graves.pool[index]
	if not stone then
		stone = CreateFrame("Frame", nil, graves.parent)
		stone:SetWidth(graves.size)
		stone:SetHeight(graves.size)
		GraveLayer(stone, "ARTWORK")
		GraveLayer(stone, "OVERLAY"):SetBlendMode("ADD")

		if graves.tooltip then
			stone.tooltip = graves.tooltip
			stone.onClick = graves.onClick
			stone:EnableMouse(true)
			stone:SetScript("OnEnter", Grave_OnEnter)
			stone:SetScript("OnLeave", Grave_OnLeave)
			stone:SetScript("OnHide", Grave_OnHide)
			stone:SetScript("OnMouseUp", Grave_OnMouseUp)
		end
		graves.pool[index] = stone
	end
	return stone
end

-- The WorldMapArea row the map is showing, or nil when what it shows is not
-- drawn in a zone's rectangle.
local function ShownMapArea()
	if type(GetCurrentMapAreaID) ~= "function" then
		return nil
	end

	-- A floor of a multi-level map is drawn in that floor's own space, not the
	-- zone's. The one exception is a map whose first "floor" is the terrain
	-- itself, which WorldMapFrame_Update draws as the plain zone map.
	local level = GetCurrentMapDungeonLevel()
	if level > 0 and not (level == 1 and DungeonUsesTerrainMap()) then
		return nil
	end

	-- This client answers WorldMapArea.ID + 1, which is why its own
	-- WORLDMAP_WINTERGRASP_ID is 502 for row 501 and why
	-- WorldMapFrame_ToggleWindowSize subtracts one before using it.
	return GetCurrentMapAreaID() - 1
end

local function DrawGraves(surface)
	local mapFrame = surface.frame
	local pool = surface.graves.pool

	local list
	if mapFrame:IsShown() and CENTURION_BOTMAP_GRAVEYARDS then
		local area = ShownMapArea()
		list = area and CENTURION_BOTMAP_GRAVEYARDS[area]
	end

	local shown = 0
	if list then
		local width  = mapFrame:GetWidth()
		local height = mapFrame:GetHeight()
		for i = 1, #list do
			local grave = list[i]
			local stone = AcquireGrave(surface, i)
			stone.label = grave[3]
			stone:SetPoint("CENTER", mapFrame, "TOPLEFT", grave[1] * width, -grave[2] * height)
			stone:Show()
		end
		shown = #list
	end

	for i = shown + 1, #pool do
		pool[i]:Hide()
	end
end

------------------------------------------------------------------
-- drawing
------------------------------------------------------------------
-- Whether the map is showing the zone the player is actually standing in.
--
-- Both halves are stable values that do not flicker: GetCurrentMapZone indexes
-- into the continent's zone list, and GetRealZoneText is the player's own zone.
-- Names rather than ids because the client has no zone id for the map, and both
-- come from the same client tables so the localisation always matches.
local function ViewingOwnZone()
	if type(GetMapZones) ~= "function" or type(GetCurrentMapContinent) ~= "function" then
		return true   -- no way to ask; fall back to drawing rather than blanking
	end

	local continent = GetCurrentMapContinent()
	if not continent or continent < 1 then
		return false
	end

	local shown = select(GetCurrentMapZone(), GetMapZones(continent))
	return shown ~= nil and shown == GetRealZoneText()
end

local function DrawOn(surface)
	local mapFrame = surface.frame
	if not mapFrame:IsShown() then
		HideFrom(surface, 1)
		return
	end

	-- The server sends bots for the zone the VIEWER is standing in, and the
	-- coordinates are normalised INSIDE that zone's rectangle. On any other
	-- view they are meaningless, so both of these have to hold.
	--
	-- 1. A zone must actually be selected. Right-clicking the map zooms out to
	--    the continent, where GetCurrentMapZone() is 0 - and, crucially,
	--    GetPlayerMapPosition still returns a perfectly valid position, because
	--    the continent map shows where you are too. Testing only that let zone
	--    coordinates be painted onto a continent, which is what put bots in the
	--    ocean off the coast.
	-- 2. And it must be the player's OWN zone rather than another zone on the
	--    same continent.
	--
	-- The type guard is so that a client without the API hides the blips rather
	-- than erroring every frame.
	if type(GetCurrentMapZone) ~= "function" or GetCurrentMapZone() == 0 then
		HideFrom(surface, 1)
		return
	end

	-- Positions are only good for the zone they were sent for, and only while
	-- they keep arriving.
	if #bots == 0 or (GetTime() - lastPush) > STALE_AFTER then
		HideFrom(surface, 1)
		return
	end

	-- Test 2 by NAME, not by GetPlayerMapPosition.
	--
	-- That API was the obvious way to ask "is this my zone" - it answers 0,0
	-- for any other zone - but it also answers 0,0 for a frame or two every
	-- time the map is re-pointed: on open, after SetMapToCurrentZone, through a
	-- zone change, and while the map animates. Every one of those readings hid
	-- every blip until the next redraw, and redraws only happen on the 1s
	-- heartbeat or a 2s push. That is the blinking: not the feed going stale,
	-- not a dropped chunk, just a transient 0,0 being trusted as an answer.
	--
	-- The zone the map is SHOWING, compared against the zone the player is
	-- STANDING in, is the same question asked of two stable values.
	if not ViewingOwnZone() then
		HideFrom(surface, 1)
		return
	end

	local width  = mapFrame:GetWidth()
	local height = mapFrame:GetHeight()

	local shown = 0
	for i = 1, #bots do
		if shown >= MAX_BLIPS then
			break
		end
		local bot = bots[i]
		shown = shown + 1
		local blip = AcquireBlip(surface, shown)
		blip:SetPoint("CENTER", mapFrame, "TOPLEFT", bot.x * width, -bot.y * height)
		blip:Show()
	end

	HideFrom(surface, shown + 1)
end

local function Redraw()
	for i = 1, #surfaces do
		DrawOn(surfaces[i])
		DrawGraves(surfaces[i])
	end
end

------------------------------------------------------------------
-- the feed
------------------------------------------------------------------
-- Records are "name:x:y;" repeated. The first character of a payload is the
-- continuation marker: C means more is coming, E means this is the last chunk
-- and the set may be swapped in. Building into a scratch table and swapping on
-- E means a half-arrived world is never drawn.
local function OnPayload(payload)
	local marker  = string.sub(payload, 1, 1)
	local records = string.sub(payload, 2)

	for name, x, y in string.gmatch(records, "([^:;]+):([%-%d%.]+):([%-%d%.]+);") do
		table.insert(pending, { name = name, x = tonumber(x), y = tonumber(y) })
	end

	if marker == "E" then
		bots = pending
		pending = {}
		lastPush = GetTime()
		Redraw()
	end
end

------------------------------------------------------------------
-- wiring
------------------------------------------------------------------
-- Stones on the world map go on WorldMapButton, the click layer over the detail
-- frame where Blizzard keeps its own landmarks and party icons, so they can
-- carry a tooltip.
AddSurface(WorldMapDetailFrame, 12, {
	size    = 16,
	parent  = WorldMapButton,
	tooltip = WorldMapTooltip,
	onClick = function(mouseButton)
		WorldMapButton_OnClick(WorldMapButton, mouseButton)
	end,
})

-- The battlefield minimap is LOAD ON DEMAND - it does not exist until the
-- player opens it or enters a battleground - so it is picked up when it
-- arrives rather than at login, and hooked so it redraws the moment it is
-- shown instead of waiting for the next push.
local function TryAttachBattlefieldMinimap()
	if not BattlefieldMinimap or not BattlefieldMinimap.CENTURION_hooked then
		if BattlefieldMinimap then
			-- Smaller icon: the battlefield minimap is a fraction of the world
			-- map's size, and a 12px blip on it covers half a zone.
			-- Its stones take no mouse: the frame is dragged by its body.
			AddSurface(BattlefieldMinimap, 8, { size = 10, parent = BattlefieldMinimap })
			BattlefieldMinimap:HookScript("OnShow", Redraw)
			BattlefieldMinimap.CENTURION_hooked = true
		end
	end
end
TryAttachBattlefieldMinimap()

-- Everything the set is scoped to has changed, so it is not stale data - it is
-- wrong data, immediately. Dropped rather than redrawn: a zone change used to
-- be the very event that painted the old zone's bots onto the new map.
local function Invalidate()
	bots = {}
	pending = {}
	lastPush = 0
	Redraw()
end

frame:RegisterEvent("CHAT_MSG_ADDON")
frame:RegisterEvent("WORLD_MAP_UPDATE")
frame:RegisterEvent("ZONE_CHANGED_NEW_AREA")
frame:RegisterEvent("ZONE_CHANGED")
frame:RegisterEvent("PLAYER_ENTERING_WORLD")
frame:RegisterEvent("ADDON_LOADED")
frame:SetScript("OnEvent", function(self, event, arg1, arg2)
	if event == "CHAT_MSG_ADDON" then
		if arg1 ~= "CCGAME" or type(arg2) ~= "string" then
			return
		end
		local payload = string.match(arg2, "^BMAP:(.*)$")
		if payload then
			OnPayload(payload)
		end
	elseif event == "ADDON_LOADED" then
		if arg1 == "Blizzard_BattlefieldMinimap" then
			TryAttachBattlefieldMinimap()
			Redraw()
		end
	elseif event == "ZONE_CHANGED_NEW_AREA" or event == "ZONE_CHANGED"
		or event == "PLAYER_ENTERING_WORLD" then
		Invalidate()
	else
		Redraw()
	end
end)

-- Expiry needs a heartbeat: with the map open and nothing else happening, no
-- event would fire to notice the feed had stopped, and the last set would sit
-- there looking current. Throttled to once a second, and it does nothing at all
-- unless a surface is actually visible.
local sinceCheck = 0
frame:SetScript("OnUpdate", function(self, elapsed)
	sinceCheck = sinceCheck + elapsed
	if sinceCheck < 1.0 then
		return
	end
	sinceCheck = 0

	if #bots == 0 then
		return
	end
	for i = 1, #surfaces do
		if surfaces[i].frame:IsShown() then
			Redraw()
			return
		end
	end
end)

-- WORLD_MAP_UPDATE does not fire on close, so blips would otherwise be left
-- anchored to a hidden frame and reappear stale on the next open.
WorldMapFrame:HookScript("OnHide", Redraw)
