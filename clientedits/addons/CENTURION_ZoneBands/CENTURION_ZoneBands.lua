-- Zone level bands on the world map.
--
-- The realm runs on bands, not on suggestion: a zone has a level range, the
-- drifting playerbots are only ever sent to people standing inside their zone's
-- range, and a bot that lands there is re-levelled to fit it. That is invisible
-- from inside the game. Somebody at 29 in Arathi Highlands - band 30 to 40 -
-- sees an empty zone and no way to find out why, which is exactly how this was
-- reported.
--
-- So the band goes on the map, next to the zone name, coloured by where the
-- reader stands relative to it, and it says in as many words whether drifters
-- will follow them there.
--
-- Bands are mirrored from kClassicZoneBands in PlayerbotPveManager.cpp. Keyed by
-- name rather than by area id because the world map hands out a NAME: the
-- continent view's hover label is set from UpdateMapHighlight, which returns the
-- zone's name and nothing else. Single-locale realm, so a name is a safe key.
local BANDS = {
	["Dun Morogh"]           = { 1, 10 },
	["Elwynn Forest"]        = { 1, 10 },
	["Teldrassil"]           = { 1, 10 },
	["Tirisfal Glades"]      = { 1, 10 },
	["Durotar"]              = { 1, 10 },
	["Mulgore"]              = { 1, 10 },
	["Westfall"]             = { 10, 20 },
	["Loch Modan"]           = { 10, 20 },
	["Darkshore"]            = { 10, 20 },
	["Silverpine Forest"]    = { 10, 20 },
	["The Barrens"]          = { 10, 25 },
	["Redridge Mountains"]   = { 15, 25 },
	["Stonetalon Mountains"] = { 15, 27 },
	["Duskwood"]             = { 18, 30 },
	["Ashenvale"]            = { 18, 30 },
	["Hillsbrad Foothills"]  = { 20, 30 },
	["Wetlands"]             = { 20, 30 },
	["Thousand Needles"]     = { 24, 35 },
	["Alterac Mountains"]    = { 30, 40 },
	["Arathi Highlands"]     = { 30, 40 },
	["Desolace"]             = { 30, 40 },
	["Stranglethorn Vale"]   = { 30, 45 },
	["Badlands"]             = { 35, 45 },
	["Swamp of Sorrows"]     = { 35, 45 },
	["Dustwallow Marsh"]     = { 35, 45 },
	["Tanaris"]              = { 40, 50 },
	["Feralas"]              = { 40, 50 },
	["Searing Gorge"]        = { 43, 50 },
	["The Hinterlands"]      = { 45, 50 },
	["Azshara"]              = { 45, 55 },
	["Blasted Lands"]        = { 45, 55 },
	["Felwood"]              = { 48, 55 },
	["Un'Goro Crater"]       = { 48, 55 },
	["Burning Steppes"]      = { 50, 58 },
	["Western Plaguelands"]  = { 51, 58 },
	["Eastern Plaguelands"]  = { 53, 60 },
	["Winterspring"]         = { 55, 60 },
	["Deadwind Pass"]        = { 55, 60 },
	["Silithus"]             = { 55, 60 },
}

-- The label sits under the map's own zone name, which is a big centred
-- GameFontNormalHuge. Matching its anchor rather than picking a corner means it
-- follows the name whichever view the map is in.
local label = WorldMapFrame:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
label:SetPoint("TOP", WorldMapFrameAreaLabel, "BOTTOM", 0, -2)
label:SetJustifyH("CENTER")

local function Paint(zoneName)
	local band = zoneName and BANDS[zoneName]
	if not band then
		label:SetText("")
		return
	end

	local bottom, top = band[1], band[2]
	local level = UnitLevel("player")

	-- Coloured by where the reader actually stands, so the map answers "can I
	-- be here" at a glance rather than making them do the arithmetic. The three
	-- colours are the client's own difficulty palette, so they read the way
	-- quest and mob colours already do.
	local colour, note
	if level < bottom then
		colour = "|cffff4040"      -- above you
		note = "too high for you"
	elseif level > top then
		colour = "|cff808080"      -- below you
		note = "below you"
	else
		colour = "|cff40ff40"      -- yours
		note = "drifters follow you here"
	end

	label:SetText(string.format("%sLevels %d-%d|r  |cff909090(%s)|r", colour, bottom, top, note))
end

-- Which zone the map is actually showing.
--
-- Two cases, and the map does not distinguish them for us. On a CONTINENT map
-- the reader hovers a zone and WorldMapFrameAreaLabel carries that zone's name;
-- on a ZONE map nothing is hovered and the label carries the zone the map is
-- already showing. Reading the label covers both without caring which is which,
-- and it updates as the cursor moves for free.
local function Refresh()
	Paint(WorldMapFrameAreaLabel:GetText())
end

-- Polled rather than hooked. WorldMapFrameAreaLabel is written by
-- WorldMapButton's OnUpdate through a plain SetText, so there is no event to
-- listen for, and hooking SetText on the label would fire from inside the very
-- call we would then be reacting to. Ten times a second is far below what the
-- cursor can do and costs one table lookup.
local ticker = CreateFrame("Frame", nil, WorldMapFrame)
local accum = 0
ticker:SetScript("OnUpdate", function()
	accum = accum + arg1
	if accum < 0.1 then
		return
	end
	accum = 0
	Refresh()
end)

WorldMapFrame:HookScript("OnShow", Refresh)
WorldMapFrame:HookScript("OnHide", function() label:SetText("") end)
