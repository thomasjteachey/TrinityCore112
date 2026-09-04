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

local BOT_ICON  = "Interface\\WorldMap\\WorldMapPartyIcon"
local MAX_BLIPS = 80

local bots    = {}      -- the completed set: { name, x, y }
local pending = {}      -- chunks still arriving
local surfaces = {}     -- { frame = <Frame>, size = <px>, blips = {} }

local frame = CreateFrame("Frame", "CenturionBotMapFrame", UIParent)

------------------------------------------------------------------
-- surfaces
------------------------------------------------------------------
-- A surface is any frame whose corners are the corners of the zone. Both the
-- world map's detail frame and the battlefield minimap qualify, which is the
-- whole reason one set of coordinates serves both.
local function AddSurface(mapFrame, iconSize)
	if not mapFrame then
		return
	end
	for i = 1, #surfaces do
		if surfaces[i].frame == mapFrame then
			return
		end
	end
	table.insert(surfaces, { frame = mapFrame, size = iconSize, blips = {} })
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
-- drawing
------------------------------------------------------------------
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
	--    same continent, which is what GetPlayerMapPosition answers: 0,0
	--    anywhere else. This is the test Blizzard's own code leans on
	--    (BattlefieldMinimap_OnUpdate, before it calls SetMapToCurrentZone).
	--
	-- The type guard is so that a client without the API hides the blips rather
	-- than erroring every frame.
	if type(GetCurrentMapZone) ~= "function" or GetCurrentMapZone() == 0 then
		HideFrom(surface, 1)
		return
	end

	local px, py = GetPlayerMapPosition("player")
	if not px or (px == 0 and py == 0) then
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
		Redraw()
	end
end

------------------------------------------------------------------
-- wiring
------------------------------------------------------------------
AddSurface(WorldMapDetailFrame, 12)

-- The battlefield minimap is LOAD ON DEMAND - it does not exist until the
-- player opens it or enters a battleground - so it is picked up when it
-- arrives rather than at login, and hooked so it redraws the moment it is
-- shown instead of waiting for the next push.
local function TryAttachBattlefieldMinimap()
	if not BattlefieldMinimap or not BattlefieldMinimap.CENTURION_hooked then
		if BattlefieldMinimap then
			-- Smaller icon: the battlefield minimap is a fraction of the world
			-- map's size, and a 12px blip on it covers half a zone.
			AddSurface(BattlefieldMinimap, 8)
			BattlefieldMinimap:HookScript("OnShow", Redraw)
			BattlefieldMinimap.CENTURION_hooked = true
		end
	end
end
TryAttachBattlefieldMinimap()

frame:RegisterEvent("CHAT_MSG_ADDON")
frame:RegisterEvent("WORLD_MAP_UPDATE")
frame:RegisterEvent("ZONE_CHANGED_NEW_AREA")
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
	else
		Redraw()
	end
end)

-- WORLD_MAP_UPDATE does not fire on close, so blips would otherwise be left
-- anchored to a hidden frame and reappear stale on the next open.
WorldMapFrame:HookScript("OnHide", Redraw)
