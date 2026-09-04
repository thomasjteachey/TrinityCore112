-- Live readout of the playerbot fleet, for whoever is running the realm.
--
-- The server pushes an aggregate every few seconds over the CCGAME addon
-- whisper channel the bot map already uses, and it only pushes to a GM - so
-- this window is simply empty for anybody else and there is nothing to gate
-- client-side. Five record types arrive independently:
--
--   BSTA  one line of totals
--   BSTZ  one row per zone, chunked, carrying the zone NAME because the 3.3.5
--         client has no zone-id lookup of its own
--   BSTL  one row per ten-level band, chunked
--   BSTI  one row per BOT, chunked, on a slower timer than the aggregates
--   BSTE  end of a roster sweep, carrying the count
--
-- The roster arrives unasked rather than on request. 3.3.5 has no clean
-- client-to-server addon channel that would not mean editing the core's chat
-- handler, and pushing the lot is only about forty whispers - which also makes
-- the drill-down instant, because every row the window could want is already
-- here and filtering is local.
--
-- Chunked feeds need a generation counter, not a "clear on first chunk" rule:
-- the rows for one push arrive as several whispers, and clearing on each would
-- leave only the last chunk standing. Instead every chunk is stamped with the
-- aggregate that preceded it, and a panel draws only rows from the newest
-- stamp it has seen.

local ADDON = "CENTURION_BotStats"

local PANEL_W, PANEL_H = 560, 420
local ROW_H = 16
local BAR_H = 12
local MAX_ROWS = 16

-- Row colours by metric, so a column reads the same on every panel.
local C_POP    = { 0.35, 0.55, 0.95 }
local C_AGGR   = { 0.90, 0.35, 0.25 }
local C_TIMID  = { 0.85, 0.75, 0.25 }
local C_GOLD   = { 0.95, 0.82, 0.20 }

local totals   = { bots = 0, combat = 0, dead = 0, travelling = 0, timid = 0, aggr = 0, gold = 0, auctions = 0 }
local zones    = {}     -- [zoneId] = { name, count, level, aggr, timid, gold, gen }
local bands    = {}     -- [band]   = { count, level, aggr, timid, gold, gen }
local gen      = 0      -- bumped by each BSTA; chunks inherit the current value
local lastPush = 0

local roster   = {}     -- completed per-bot list
local incoming = {}     -- roster rows still arriving
local scrollOffset = 0
local zoneFilter = nil  -- when set, the Bots tab shows only this zone

local activeTab = "zones"
local sortKey   = "count"
local selected  = nil   -- a zoneId, when drilled in

------------------------------------------------------------------
-- parsing
------------------------------------------------------------------
local function ParseTotals(payload)
	local b, c, d, t, ti, ag, g, au = string.match(payload,
		"^(%-?%d+)|(%-?%d+)|(%-?%d+)|(%-?%d+)|(%-?%d+)|(%-?%d+)|(%-?%d+)|(%-?%d+)$")
	if not b then
		return
	end

	totals.bots       = tonumber(b)  or 0
	totals.combat     = tonumber(c)  or 0
	totals.dead       = tonumber(d)  or 0
	totals.travelling = tonumber(t)  or 0
	totals.timid      = tonumber(ti) or 0
	totals.aggr       = tonumber(ag) or 0
	totals.gold       = tonumber(g)  or 0
	totals.auctions   = tonumber(au) or 0

	-- A new aggregate opens a new generation. Rows still carrying the previous
	-- one are dropped when the panel next draws, which is how a zone that has
	-- emptied disappears instead of lingering at its last population.
	gen = gen + 1
	lastPush = GetTime()
end

-- "id,name,count,level,aggr,timid,gold;" repeated
local function ParseZones(payload)
	for row in string.gmatch(payload, "([^;]+)") do
		local id, name, count, level, aggr, timid, gold =
			string.match(row, "^(%d+),([^,]*),(%d+),(%d+),(%d+),(%d+),(%d+)$")
		if id then
			zones[tonumber(id)] = {
				name  = (name ~= "" and name) or ("Zone " .. id),
				count = tonumber(count) or 0,
				level = tonumber(level) or 0,
				aggr  = tonumber(aggr)  or 0,
				timid = tonumber(timid) or 0,
				gold  = tonumber(gold)  or 0,
				gen   = gen,
			}
		end
	end
end

-- "band,count,level,aggr,timid,gold;" repeated
local function ParseBands(payload)
	for row in string.gmatch(payload, "([^;]+)") do
		local band, count, level, aggr, timid, gold =
			string.match(row, "^(%d+),(%d+),(%d+),(%d+),(%d+),(%d+)$")
		if band then
			bands[tonumber(band)] = {
				count = tonumber(count) or 0,
				level = tonumber(level) or 0,
				aggr  = tonumber(aggr)  or 0,
				timid = tonumber(timid) or 0,
				gold  = tonumber(gold)  or 0,
				gen   = gen,
			}
		end
	end
end

local CLASS_NAME = {
	[1] = "Warrior", [2] = "Paladin", [3] = "Hunter",  [4] = "Rogue",
	[5] = "Priest",  [6] = "DK",      [7] = "Shaman",  [8] = "Mage",
	[9] = "Warlock", [11] = "Druid",
}

-- "name,level,class,zone,aggr,timid,gold,flags;" repeated
local function ParseRoster(payload)
	for row in string.gmatch(payload, "([^;]+)") do
		local name, lvl, cls, zone, aggr, timid, gold, flags =
			string.match(row, "^([^,]+),(%d+),(%d+),(%d+),(%d+),(%d+),(%d+),(%d+)$")
		if name then
			local f = tonumber(flags) or 0
			table.insert(incoming, {
				name  = name,
				level = tonumber(lvl)   or 0,
				class = tonumber(cls)   or 0,
				zone  = tonumber(zone)  or 0,
				aggr  = tonumber(aggr)  or 0,
				timid = tonumber(timid) or 0,
				gold  = tonumber(gold)  or 0,
				combat = bit.band(f, 1) > 0,
				dead   = bit.band(f, 2) > 0,
				travel = bit.band(f, 4) > 0,
				pvp    = bit.band(f, 8) > 0,
			})
		end
	end
end

-- The sweep is only swapped in when the server says it finished, so a
-- half-arrived roster never replaces a complete one.
local function CommitRoster()
	if #incoming > 0 then
		roster = incoming
	end
	incoming = {}
end

------------------------------------------------------------------
-- window
------------------------------------------------------------------
local win = CreateFrame("Frame", "CenturionBotStatsFrame", UIParent)
win:SetWidth(PANEL_W)
win:SetHeight(PANEL_H)
win:SetPoint("CENTER")
win:SetBackdrop({
	bgFile   = "Interface\\DialogFrame\\UI-DialogBox-Background",
	edgeFile = "Interface\\DialogFrame\\UI-DialogBox-Border",
	tile = true, tileSize = 32, edgeSize = 32,
	insets = { left = 11, right = 12, top = 12, bottom = 11 },
})
win:SetMovable(true)
win:EnableMouse(true)
win:RegisterForDrag("LeftButton")
win:SetScript("OnDragStart", win.StartMoving)
win:SetScript("OnDragStop", win.StopMovingOrSizing)
win:SetClampedToScreen(true)
win:Hide()
tinsert(UISpecialFrames, "CenturionBotStatsFrame")

local title = win:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
title:SetPoint("TOP", win, "TOP", 0, -16)
title:SetText("Fleet")

local close = CreateFrame("Button", nil, win, "UIPanelCloseButton")
close:SetPoint("TOPRIGHT", win, "TOPRIGHT", -8, -8)

-- Totals strip
local summary = win:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
summary:SetPoint("TOPLEFT", win, "TOPLEFT", 22, -44)
summary:SetJustifyH("LEFT")

local summary2 = win:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
summary2:SetPoint("TOPLEFT", summary, "BOTTOMLEFT", 0, -4)
summary2:SetJustifyH("LEFT")

local stale = win:CreateFontString(nil, "OVERLAY", "GameFontRedSmall")
stale:SetPoint("TOPRIGHT", win, "TOPRIGHT", -30, -46)
stale:SetText("")

local footer = win:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
footer:SetPoint("BOTTOMLEFT", win, "BOTTOMLEFT", 24, 24)
footer:SetJustifyH("LEFT")
footer:SetText("")

-- Wheel scrolling over the whole window rather than a FauxScrollFrame: the
-- rows are redrawn from scratch on every push anyway, so there is nothing for
-- a real scroll child to hold, and an offset is the entire mechanism.
win:EnableMouseWheel(true)
win:SetScript("OnMouseWheel", function()
	scrollOffset = scrollOffset - (arg1 * 3)
	if scrollOffset < 0 then
		scrollOffset = 0
	end
	CENTURION_BotStats_Refresh()
end)

------------------------------------------------------------------
-- rows
------------------------------------------------------------------
local rows = {}

local function AcquireRow(i)
	if rows[i] then
		return rows[i]
	end

	local r = CreateFrame("Button", nil, win)
	r:SetWidth(PANEL_W - 48)
	r:SetHeight(ROW_H)
	if i == 1 then
		r:SetPoint("TOPLEFT", win, "TOPLEFT", 24, -108)
	else
		r:SetPoint("TOPLEFT", rows[i - 1], "BOTTOMLEFT", 0, -2)
	end

	r.bar = r:CreateTexture(nil, "ARTWORK")
	r.bar:SetTexture("Interface\\TargetingFrame\\UI-StatusBar")
	r.bar:SetPoint("LEFT", r, "LEFT", 150, 0)
	r.bar:SetHeight(BAR_H)

	r.label = r:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
	r.label:SetPoint("LEFT", r, "LEFT", 0, 0)
	r.label:SetWidth(146)
	r.label:SetJustifyH("LEFT")

	r.value = r:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
	r.value:SetPoint("RIGHT", r, "RIGHT", 0, 0)
	r.value:SetJustifyH("RIGHT")

	r:SetScript("OnEnter", function()
		if this.tip then
			GameTooltip:SetOwner(this, "ANCHOR_RIGHT")
			GameTooltip:SetText(this.tip, 1, 1, 1, 1, true)
			GameTooltip:Show()
		end
	end)
	r:SetScript("OnLeave", function() GameTooltip:Hide() end)

	rows[i] = r
	return r
end

local function HideRowsFrom(n)
	for i = n, MAX_ROWS do
		if rows[i] then
			rows[i]:Hide()
		end
	end
end

------------------------------------------------------------------
-- drawing
------------------------------------------------------------------
local function MetricOf(entry, key)
	if key == "count" then return entry.count end
	if key == "aggr"  then return entry.aggr  end
	if key == "timid" then return entry.timid end
	if key == "gold"  then return entry.gold  end
	if key == "level" then return entry.level end
	return entry.count
end

local function ColourOf(key)
	if key == "aggr"  then return C_AGGR  end
	if key == "timid" then return C_TIMID end
	if key == "gold"  then return C_GOLD  end
	return C_POP
end

local function DrawList(list)
	-- Sort descending on the active metric, largest bar first.
	table.sort(list, function(a, b)
		local av, bv = MetricOf(a, sortKey), MetricOf(b, sortKey)
		if av == bv then
			return (a.label or "") < (b.label or "")
		end
		return av > bv
	end)

	local peak = 0
	for i = 1, #list do
		local v = MetricOf(list[i], sortKey)
		if v > peak then peak = v end
	end
	if peak <= 0 then peak = 1 end

	local colour = ColourOf(sortKey)
	local maxBar = PANEL_W - 48 - 150 - 60
	local shown  = 0

	-- Clamp here rather than at the wheel, so a list that shrinks under a
	-- scrolled view (a zone emptying, a filter narrowing) snaps back into range
	-- instead of showing nothing.
	local maxOffset = math.max(0, #list - MAX_ROWS)
	if scrollOffset > maxOffset then
		scrollOffset = maxOffset
	end
	if scrollOffset < 0 then
		scrollOffset = 0
	end

	for i = 1, math.min(#list - scrollOffset, MAX_ROWS) do
		local e = list[i + scrollOffset]
		local r = AcquireRow(i)
		local v = MetricOf(e, sortKey)

		r.label:SetText(e.label)
		r.bar:SetWidth(math.max(1, maxBar * (v / peak)))
		r.bar:SetVertexColor(colour[1], colour[2], colour[3])

		if sortKey == "gold" then
			r.value:SetText(v .. "g")
		elseif sortKey == "count" and totals.bots > 0 then
			r.value:SetText(string.format("%d  (%.0f%%)", v, v / totals.bots * 100))
		else
			r.value:SetText(v)
		end

		r.tip = e.tip
		r.zoneId = e.zoneId
		r.botName = e.botName
		r:SetScript("OnClick", function()
			if IsShiftKeyDown() and this.botName then
				-- Shift-click puts the name in chat, which is how you actually
				-- go and look at one: .go creature / target by name.
				local edit = ChatEdit_ChooseBoxForSend()
				ChatEdit_ActivateChat(edit)
				edit:SetText(edit:GetText() .. this.botName)
				return
			end
			if this.zoneId then
				-- A zone row jumps straight to the bots standing in it, which
				-- is what you wanted the zone for.
				zoneFilter = this.zoneId
				activeTab = "bots"
				scrollOffset = 0
				CENTURION_BotStats_Refresh()
			end
		end)
		r:Show()
		shown = i
	end

	HideRowsFrom(shown + 1)

	if #list > MAX_ROWS then
		footer:SetText(string.format("showing %d-%d of %d   (mouse wheel to scroll)",
			scrollOffset + 1, scrollOffset + shown, #list))
	elseif #list > 0 then
		footer:SetText(string.format("%d rows", #list))
	else
		footer:SetText("")
	end
end

function CENTURION_BotStats_Refresh()
	if not win:IsShown() then
		return
	end

	summary:SetText(string.format(
		"|cffffd200%d|r bots   |cffff7f5f%d|r fighting   |cff9f9f9f%d|r dead   |cff5f9fff%d|r travelling   |cffe0c020%d|r timid",
		totals.bots, totals.combat, totals.dead, totals.travelling, totals.timid))
	summary2:SetText(string.format(
		"avg aggression |cffffd200%d|r   fleet gold |cffffd200%dg|r   auctions live |cffffd200%d|r",
		totals.aggr, totals.gold, totals.auctions))

	if lastPush > 0 and (GetTime() - lastPush) > 12 then
		stale:SetText("feed stale - are you still a GM?")
	else
		stale:SetText("")
	end

	local list = {}

	if activeTab == "zones" then
		if selected and zones[selected] then
			-- Drill-down: one zone against the fleet, as its own little table.
			local z = zones[selected]
			title:SetText(z.name)
			local function add(label, value, tip)
				table.insert(list, { label = label, count = value, aggr = value,
					timid = value, gold = value, level = value, tip = tip })
			end
			add("bots here", z.count, "Population of this zone.")
			add("avg level", z.level, "Mean level of the bots standing in it.")
			add("avg aggression", z.aggr, "0-100. Drives how long a beaten bot stays timid and how far it will travel for a fight.")
			add("timid now", z.timid, "Bots here that recently lost to a person and are avoiding people.")
			add("gold held", z.gold, "Sum of coin carried by bots in this zone.")
			sortKey = "count"
		else
			title:SetText("Fleet by zone")
			for id, z in pairs(zones) do
				if z.gen == gen and z.count > 0 then
					table.insert(list, {
						label = z.name, zoneId = id,
						count = z.count, aggr = z.aggr, timid = z.timid,
						gold = z.gold, level = z.level,
						tip = string.format("%s\n%d bots, avg level %d\navg aggression %d, %d timid\n%dg carried\n\nClick to drill in.",
							z.name, z.count, z.level, z.aggr, z.timid, z.gold),
					})
				end
			end
		end
	elseif activeTab == "bots" then
		local zoneName = zoneFilter and zones[zoneFilter] and zones[zoneFilter].name
		title:SetText(zoneName and ("Bots in " .. zoneName) or "Every bot")

		for i = 1, #roster do
			local b = roster[i]
			if not zoneFilter or b.zone == zoneFilter then
				-- State reads at a glance in the name, because that is the
				-- column the eye is already on.
				local mark = ""
				if b.dead then
					mark = " |cff9f9f9f(dead)|r"
				elseif b.combat then
					mark = " |cffff7f5f(fighting)|r"
				elseif b.travel then
					mark = " |cff5f9fff(travelling)|r"
				elseif b.timid > 0 then
					mark = string.format(" |cffe0c020(timid %ds)|r", b.timid)
				end

				local zn = zones[b.zone] and zones[b.zone].name or ("Zone " .. b.zone)
				table.insert(list, {
					label   = string.format("%s |cff909090%d %s|r%s",
						b.name, b.level, CLASS_NAME[b.class] or "?", mark),
					botName = b.name,
					count   = b.level,
					level   = b.level,
					aggr    = b.aggr,
					timid   = b.timid,
					gold    = b.gold,
					tip     = string.format(
						"%s\nLevel %d %s\n%s\n\naggression %d\ntimid %s\ncarrying %dg%s\n\nShift-click to put the name in chat.",
						b.name, b.level, CLASS_NAME[b.class] or "?", zn, b.aggr,
						(b.timid > 0) and (b.timid .. "s remaining") or "no",
						b.gold, b.pvp and "\n\n|cffff7f5fPvP-only bot|r" or ""),
				})
			end
		end
	else
		title:SetText("Fleet by level")
		selected = nil
		for band, b in pairs(bands) do
			if b.gen == gen and b.count > 0 then
				table.insert(list, {
					label = string.format("%d - %d", band, band + 9),
					count = b.count, aggr = b.aggr, timid = b.timid,
					gold = b.gold, level = b.level,
					tip = string.format("Levels %d-%d\n%d bots\navg aggression %d, %d timid\n%dg carried",
						band, band + 9, b.count, b.aggr, b.timid, b.gold),
				})
			end
		end
	end

	DrawList(list)
end

------------------------------------------------------------------
-- tabs and sort buttons
------------------------------------------------------------------
local function MakeButton(parent, label, width, onClick)
	local b = CreateFrame("Button", nil, parent, "UIPanelButtonTemplate")
	b:SetWidth(width)
	b:SetHeight(20)
	b:SetText(label)
	b:SetScript("OnClick", onClick)
	return b
end

local tabZones = MakeButton(win, "Zones", 62, function()
	activeTab = "zones"; selected = nil; zoneFilter = nil; scrollOffset = 0; CENTURION_BotStats_Refresh()
end)
tabZones:SetPoint("TOPLEFT", win, "TOPLEFT", 22, -80)

local tabLevels = MakeButton(win, "Levels", 62, function()
	activeTab = "levels"; scrollOffset = 0; CENTURION_BotStats_Refresh()
end)
tabLevels:SetPoint("LEFT", tabZones, "RIGHT", 4, 0)

-- Clears the zone filter as well: reaching the tab from the button means
-- "all of them", where reaching it by clicking a zone means "these".
local tabBots = MakeButton(win, "Bots", 56, function()
	activeTab = "bots"; zoneFilter = nil; scrollOffset = 0; CENTURION_BotStats_Refresh()
end)
tabBots:SetPoint("LEFT", tabLevels, "RIGHT", 4, 0)

local sortButtons = {
	{ key = "count", text = "Pop" },
	{ key = "aggr",  text = "Aggr" },
	{ key = "timid", text = "Timid" },
	{ key = "gold",  text = "Gold" },
}

local prev
for i = 1, #sortButtons do
	local def = sortButtons[i]
	local b = MakeButton(win, def.text, 58, function()
		sortKey = def.key
		scrollOffset = 0
		CENTURION_BotStats_Refresh()
	end)
	if prev then
		b:SetPoint("LEFT", prev, "RIGHT", 2, 0)
	else
		b:SetPoint("LEFT", tabBots, "RIGHT", 14, 0)
	end
	prev = b
end

local back = MakeButton(win, "Back", 60, function()
	selected = nil
	zoneFilter = nil
	scrollOffset = 0
	if activeTab == "bots" then activeTab = "zones" end
	CENTURION_BotStats_Refresh()
end)
back:SetPoint("BOTTOMRIGHT", win, "BOTTOMRIGHT", -22, 18)

------------------------------------------------------------------
-- events
------------------------------------------------------------------
local driver = CreateFrame("Frame")
driver:RegisterEvent("CHAT_MSG_ADDON")
driver:SetScript("OnEvent", function()
	if event ~= "CHAT_MSG_ADDON" or arg1 ~= "CCGAME" or type(arg2) ~= "string" then
		return
	end

	local tag, payload = string.match(arg2, "^(%u+):(.*)$")
	if not tag then
		return
	end

	if tag == "BSTA" then
		ParseTotals(payload)
	elseif tag == "BSTZ" then
		ParseZones(payload)
	elseif tag == "BSTL" then
		ParseBands(payload)
	elseif tag == "BSTI" then
		ParseRoster(payload)
	elseif tag == "BSTE" then
		CommitRoster()
	else
		return
	end

	CENTURION_BotStats_Refresh()
end)

-- Repaint on a timer as well as on arrival, so the stale warning appears when
-- the feed stops rather than waiting for a push that is never coming.
local ticker = 0
driver:SetScript("OnUpdate", function()
	ticker = ticker + arg1
	if ticker < 2 then
		return
	end
	ticker = 0
	CENTURION_BotStats_Refresh()
end)

SLASH_CENTURIONBOTSTATS1 = "/botstats"
SLASH_CENTURIONBOTSTATS2 = "/bstats"
SlashCmdList["CENTURIONBOTSTATS"] = function()
	if win:IsShown() then
		win:Hide()
	else
		win:Show()
		CENTURION_BotStats_Refresh()
	end
end
