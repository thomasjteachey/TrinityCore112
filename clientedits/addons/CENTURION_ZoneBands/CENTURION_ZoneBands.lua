-- Zone level bands on the world map.
--
-- The realm runs on bands, not on suggestion: a zone has a level range, the
-- drifting playerbots are only ever sent to people standing inside their zone's
-- range, and a bot that lands there is re-levelled to fit it. That is invisible
-- from inside the game. Somebody at 29 in Arathi Highlands - band 30 to 40 -
-- sees an empty zone and no way to find out why.
--
-- Bands are mirrored from kClassicZoneBands in PlayerbotPveManager.cpp.
--
-- TWO keys, because the map answers two different questions and neither one
-- alone covers it.
--
-- The first cut of this read WorldMapFrameAreaLabel and nothing else, and
-- showed nothing at all in the ordinary case. That label is written by
-- WorldMapButton_OnUpdate from UpdateMapHighlight, which only returns a name
-- while the cursor is over a zone on a CONTINENT map - so opening the map in
-- your own zone, which is what pressing M does, leaves it empty and there was
-- nothing to look up.
--
-- So the zone the map is actually SHOWING comes from GetMapInfo(), which
-- returns the map's art folder name and is stable and English. Those names are
-- taken from WorldMapArea.dbc and carry Blizzard's own misspellings - Aszhara,
-- Hilsbrad - which are correct here precisely because they are what the client
-- returns.
local BY_MAP_FILE = {
	DunMorogh           = { 1, 10 },
	Elwynn              = { 1, 10 },
	Teldrassil          = { 1, 10 },
	Tirisfal            = { 1, 10 },
	Durotar             = { 1, 10 },
	Mulgore             = { 1, 10 },
	Westfall            = { 10, 20 },
	LochModan           = { 10, 20 },
	Darkshore           = { 10, 20 },
	Silverpine          = { 10, 20 },
	Barrens             = { 10, 25 },
	Redridge            = { 15, 25 },
	StonetalonMountains = { 15, 27 },
	Duskwood            = { 18, 30 },
	Ashenvale           = { 18, 30 },
	Hilsbrad            = { 20, 30 },
	Wetlands            = { 20, 30 },
	ThousandNeedles     = { 24, 35 },
	Alterac             = { 30, 40 },
	Arathi              = { 30, 40 },
	Desolace            = { 30, 40 },
	Stranglethorn       = { 30, 45 },
	Badlands            = { 35, 45 },
	SwampOfSorrows      = { 35, 45 },
	Dustwallow          = { 35, 45 },
	Tanaris             = { 40, 50 },
	Feralas             = { 40, 50 },
	SearingGorge        = { 43, 50 },
	Hinterlands         = { 45, 50 },
	Aszhara             = { 45, 55 },
	BlastedLands        = { 45, 55 },
	Felwood             = { 48, 55 },
	UngoroCrater        = { 48, 55 },
	BurningSteppes      = { 50, 58 },
	WesternPlaguelands  = { 51, 58 },
	EasternPlaguelands  = { 53, 60 },
	Winterspring        = { 55, 60 },
	DeadwindPass        = { 55, 60 },
	Silithus            = { 55, 60 },
}

-- The second key: the displayed zone NAME, for the continent view, where the
-- cursor picks a zone the map is not otherwise showing.
local BY_ZONE_NAME = {
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

-- The PARENT matters more than the anchor here, and getting it wrong is
-- invisible rather than broken.
--
-- A FontString created on WorldMapFrame draws at WorldMapFrame's own frame
-- level, and WorldMapDetailFrame - the map art itself - is a CHILD of it, so it
-- draws on top. The text was rendering the whole time, underneath the map.
-- WorldMapFrameAreaFrame is declared inside WorldMapButton, well after the
-- detail frame, so anything parented there sits above the tiles; it is also
-- where Blizzard puts its own zone label, which is the same problem solved the
-- same way.
--
-- Fallback chain because anything named here that turned out not to exist would
-- throw at load, and with script errors off - which is the default - a dead
-- addon looks exactly like a working one that has nothing to say.
local host = WorldMapFrameAreaFrame or WorldMapButton or WorldMapFrame
local label = host:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
if WorldMapFrameAreaLabel then
	label:SetPoint("TOP", WorldMapFrameAreaLabel, "BOTTOM", 0, -2)
else
	label:SetPoint("TOP", host, "TOP", 0, -24)
end
label:SetJustifyH("CENTER")

local function BandForShownMap()
	-- Hovering a zone on a continent map wins: it is the zone the reader is
	-- pointing at, which is a more specific answer than the map as a whole.
	if WorldMapFrameAreaLabel then
		local hovered = WorldMapFrameAreaLabel:GetText()
		if hovered and hovered ~= "" then
			local band = BY_ZONE_NAME[hovered]
			if band then
				return band, hovered
			end
			-- A zone we have no band for - a city, an instance, Moonglade.
			return nil, hovered
		end
	end

	-- Otherwise, whatever zone the map itself is displaying.
	local mapFile = GetMapInfo()
	if mapFile then
		return BY_MAP_FILE[mapFile], nil
	end

	return nil, nil
end

local function Paint()
	local band, hoveredName = BandForShownMap()
	if not band then
		label:SetText("")
		return
	end

	local bottom, top = band[1], band[2]
	local level = UnitLevel("player")

	-- Coloured by where the reader actually stands, so the map answers "can I
	-- be here" at a glance instead of making them do the arithmetic. The colour
	-- is the whole message: red is above the band, grey is below it, green is
	-- inside. Saying so in words as well was the addon talking rather than
	-- reporting.
	local colour
	if level < bottom then
		colour = "|cffff4040"
	elseif level > top then
		colour = "|cff808080"
	else
		colour = "|cff40ff40"
	end

	local prefix = hoveredName and (hoveredName .. "  ") or ""
	label:SetText(string.format("%s%sLevels %d-%d|r", prefix, colour, bottom, top))
end

-- Polled rather than hooked. The hover label is written by WorldMapButton's
-- OnUpdate through a plain SetText and the displayed map changes with no event
-- of its own on 3.3.5, so there is nothing to listen for. The ticker is
-- parented to the map, so it only runs while the map is open.
local ticker = CreateFrame("Frame", nil, WorldMapFrame)
local accum = 0
ticker:SetScript("OnUpdate", function()
	accum = accum + (arg1 or 0)
	if accum < 0.1 then
		return
	end
	accum = 0
	Paint()
end)

WorldMapFrame:HookScript("OnShow", Paint)
WorldMapFrame:HookScript("OnHide", function() label:SetText("") end)
