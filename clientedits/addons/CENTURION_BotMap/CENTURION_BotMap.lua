-- Playerbots on the world map, drawn the way party members are.
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
-- The arithmetic below is that, unchanged. The only difference is where the
-- numbers come from: GetPlayerMapPosition answers for "player", "party1-4" and
-- "raid1-40" and nothing else, so a bot's position cannot be asked for locally.
-- The server sends it already normalised, in the same 0..1 space, over the
-- CCGAME addon channel the realm already uses.
--
-- The icon is Blizzard's own party icon tinted red, so a bot reads as the same
-- kind of thing as a party member at a glance without being mistaken for one.

local BOT_ICON   = "Interface\\WorldMap\\WorldMapPartyIcon"
local ICON_SIZE  = 12
local MAX_BLIPS  = 80

local blips   = {}      -- reusable textures
local pending = {}      -- chunks arriving
local bots    = {}      -- the completed set: { name, x, y }

local frame = CreateFrame("Frame", "CenturionBotMapFrame", UIParent)

------------------------------------------------------------------
-- blip pool
------------------------------------------------------------------
local function AcquireBlip(index)
	local blip = blips[index]
	if not blip then
		-- Parented to WorldMapDetailFrame, which is the map IMAGE: its corners
		-- are the corners of the 0..1 space the server's numbers live in.
		blip = WorldMapDetailFrame:CreateTexture(nil, "OVERLAY")
		blip:SetTexture(BOT_ICON)
		blip:SetVertexColor(1.0, 0.15, 0.15)
		blip:SetWidth(ICON_SIZE)
		blip:SetHeight(ICON_SIZE)
		blips[index] = blip
	end
	return blip
end

local function HideFrom(index)
	for i = index, #blips do
		blips[i]:Hide()
	end
end

------------------------------------------------------------------
-- drawing
------------------------------------------------------------------
local function Redraw()
	if not WorldMapFrame:IsShown() then
		HideFrom(1)
		return
	end

	-- The server sends bots for the zone the VIEWER is standing in, so the
	-- blips are only meaningful while the map is showing that zone.
	--
	-- GetPlayerMapPosition("player") returns 0,0 whenever the map is showing
	-- anywhere else, which is exactly the test Blizzard's own code leans on -
	-- and it means this needs no map-id bookkeeping of its own.
	local px, py = GetPlayerMapPosition("player")
	if not px or (px == 0 and py == 0) then
		HideFrom(1)
		return
	end

	local width  = WorldMapDetailFrame:GetWidth()
	local height = WorldMapDetailFrame:GetHeight()

	local shown = 0
	for i = 1, #bots do
		if shown >= MAX_BLIPS then
			break
		end
		local bot = bots[i]
		shown = shown + 1
		local blip = AcquireBlip(shown)
		blip:SetPoint("CENTER", WorldMapDetailFrame, "TOPLEFT", bot.x * width, -bot.y * height)
		blip:Show()
	end

	HideFrom(shown + 1)
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

frame:RegisterEvent("CHAT_MSG_ADDON")
frame:RegisterEvent("WORLD_MAP_UPDATE")
frame:SetScript("OnEvent", function(self, event, arg1, arg2)
	if event == "CHAT_MSG_ADDON" then
		if arg1 ~= "CCGAME" or type(arg2) ~= "string" then
			return
		end
		local payload = string.match(arg2, "^BMAP:(.*)$")
		if payload then
			OnPayload(payload)
		end
	elseif event == "WORLD_MAP_UPDATE" then
		Redraw()
	end
end)

-- WORLD_MAP_UPDATE does not fire on close, so the blips would otherwise be left
-- anchored to a hidden frame and reappear stale on the next open.
WorldMapFrame:HookScript("OnHide", function() HideFrom(1) end)
