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

-- Forward declarations. The addon-message handler is built well before the
-- detail panel it repaints, and a local declared further down the file is not
-- in scope inside a closure created above it - the closure would resolve the
-- name as a global and find nil.
local shownBot
local detailTab = "gear"   -- which half of the bot panel is showing
local DrawGear, DrawTalents

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

-- Blizzard's own spec names, keyed the way the server's EquipProfileIndex
-- orders them (0/1/2 per class), so a row reads the way the armory would.
local SPEC_NAME = {
	[1]  = { "Arms", "Fury", "Protection" },
	[2]  = { "Holy", "Protection", "Retribution" },
	[3]  = { "Beast Mastery", "Marksmanship", "Survival" },
	[4]  = { "Assassination", "Combat", "Subtlety" },
	[5]  = { "Discipline", "Holy", "Shadow" },
	[6]  = { "Blood", "Frost", "Unholy" },
	[7]  = { "Elemental", "Enhancement", "Restoration" },
	[8]  = { "Arcane", "Fire", "Frost" },
	[9]  = { "Affliction", "Demonology", "Destruction" },
	[11] = { "Balance", "Feral", "Restoration" },
}

local function SpecName(class, spec)
	local list = SPEC_NAME[class]
	return list and list[(spec or 0) + 1] or "?"
end

-- "name,level,class,spec,zone,aggr,timid,gold,hp,mp,ilvl,worn,greens,flags;"
local function ParseRoster(payload)
	for row in string.gmatch(payload, "([^;]+)") do
		local name, lvl, cls, spec, zone, aggr, timid, gold, hp, mp, ilvl, worn, greens, flags, disp =
			string.match(row,
				"^([^,]+),(%d+),(%d+),(%d+),(%d+),(%d+),(%d+),(%d+),(%d+),(%d+),(%d+),(%d+),(%d+),(%d+),(%d+)$")
		if name then
			local f = tonumber(flags) or 0
			table.insert(incoming, {
				name   = name,
				level  = tonumber(lvl)   or 0,
				class  = tonumber(cls)   or 0,
				spec   = tonumber(spec)  or 0,
				zone   = tonumber(zone)  or 0,
				aggr   = tonumber(aggr)  or 0,
				timid  = tonumber(timid) or 0,
				gold   = tonumber(gold)  or 0,
				hp     = tonumber(hp)    or 0,
				mp     = tonumber(mp)    or 0,
				ilvl   = tonumber(ilvl)  or 0,
				worn   = tonumber(worn)  or 0,
				greens = tonumber(greens) or 0,
				combat = bit.band(f, 1) > 0,
				dead   = bit.band(f, 2) > 0,
				travel = bit.band(f, 4) > 0,
				pvp    = bit.band(f, 8) > 0,
				display = tonumber(disp) or 0,
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
-- GM commands
------------------------------------------------------------------
-- Server-side commands are parsed out of an ordinary chat message, so this is
-- a SAY that never reaches anybody: the handler eats anything starting with a
-- dot before it is broadcast. None of this is protected in 3.3.5.
-- Straight down the wire, with no UI involved.
--
-- The earlier version went through ChatEdit_ChooseBoxForSend, which OPENS the
-- chat box - so every click on a bot popped the edit frame open in the player's
-- face. SendChatMessage sends the literal string as a SAY, the server's chat
-- handler eats anything beginning with a dot before it is ever broadcast, and
-- nothing on screen moves.
local function RunGmCommand(cmd)
	SendChatMessage(cmd, "SAY")
end

-- Blizzard's Inspect frame cannot be reused for this, and it is worth writing
-- down why so nobody tries again.
--
-- InspectUnit takes a UNIT TOKEN. The only way to point one at an arbitrary bot
-- is TargetByName, which has been protected since 2.0 - an addon calling it does
-- nothing at all, silently, which is exactly what the button did. Even if it
-- were callable, the client only has a unit for something already in its object
-- manager, so a bot in another zone could never be named. Nothing in the 3.3.5
-- Lua API inspects by GUID.
--
-- So the gear and talents are served instead, and drawn here. The request goes
-- out as the GM command ".botstats gear <name>" - a slash command IS the inbound
-- channel 3.3.5 otherwise lacks, and it carries the RBAC gate for free - and the
-- answer comes back over the same CCGAME whisper as the rest of the feed.
local gearOf   = {}     -- [botName] = { {slot, id, quality, ilvl}, ... }
local picksOf  = {}     -- [botName] = { {spell, rank, tree, tier}, ... }
local talentOf = {}     -- [botName] = { t1, t2, t3 }

local SLOT_NAME = {
	[0]  = "Head",     [1]  = "Neck",    [2]  = "Shoulder", [3]  = "Shirt",
	[4]  = "Chest",    [5]  = "Waist",   [6]  = "Legs",     [7]  = "Feet",
	[8]  = "Wrist",    [9]  = "Hands",   [10] = "Finger 1", [11] = "Finger 2",
	[12] = "Trinket 1",[13] = "Trinket 2", [14] = "Back",   [15] = "Main hand",
	[16] = "Off hand", [17] = "Ranged",  [18] = "Tabard",
}

-- ARTIFACT is not the top tier on this realm, it is the BOTTOM one: the red
-- field-kit gear handed out to replace whatever a death took. It is deliberately
-- listed below grey here so the panel sorts and reads the way the realm treats
-- it, and the server's green-or-better count excludes it for the same reason.
local QUALITY_HEX = {
	[0] = "9d9d9d", [1] = "ffffff", [2] = "1eff00",
	[3] = "0070dd", [4] = "a335ee", [5] = "ff8000",
	[6] = "cc4444", [7] = "e6cc80",
}

-- The field kit is copied from WHITE items, so it ranks as white.
local QUALITY_RANK = {
	[0] = 0, [1] = 1, [2] = 2, [3] = 3, [4] = 4, [5] = 5, [6] = 1, [7] = 4,
}

-- "name|slot,id,quality,ilvl;..." - chunked, so rows accumulate per bot and the
-- set is replaced only when a request for that bot starts.
local function ParseGear(payload)
	local name, rows = string.match(payload, "^([^|]+)|(.*)$")
	if not name then
		return
	end

	gearOf[name] = gearOf[name] or {}
	for row in string.gmatch(rows, "([^;]+)") do
		local slot, id, q, ilvl = string.match(row, "^(%d+),(%d+),(%d+),(%d+)$")
		if slot then
			table.insert(gearOf[name], {
				slot    = tonumber(slot) or 0,
				id      = tonumber(id) or 0,
				quality = tonumber(q) or 1,
				ilvl    = tonumber(ilvl) or 0,
			})
		end
	end
end

-- "name|t1,t2,t3"
local function ParseTalents(payload)
	local name, a, b, c = string.match(payload, "^([^|]+)|(%d+),(%d+),(%d+)$")
	if name then
		talentOf[name] = { tonumber(a) or 0, tonumber(b) or 0, tonumber(c) or 0 }
	end
end

-- "name|spellId,rank,tree,tier;..." - the talents actually taken.
local function ParseTalentPicks(payload)
	local name, rows = string.match(payload, "^([^|]+)|(.*)$")
	if not name then
		return
	end

	picksOf[name] = picksOf[name] or {}
	for row in string.gmatch(rows, "([^;]+)") do
		local spell, rank, tree, tier, col = string.match(row, "^(%d+),(%d+),(%d+),(%d+),(%d+)$")
		if spell then
			table.insert(picksOf[name], {
				spell = tonumber(spell) or 0,
				rank  = tonumber(rank) or 0,
				tree  = tonumber(tree) or 0,
				tier  = tonumber(tier) or 0,
				col   = tonumber(col) or 0,
			})
		end
	end
end

local function RequestGear(name)
	gearOf[name] = nil
	talentOf[name] = nil
	picksOf[name] = nil
	RunGmCommand(".botstats gear " .. name)
end

------------------------------------------------------------------
-- rows
------------------------------------------------------------------
local rows = {}

local function AcquireRow(i)
	if rows[i] then
		return rows[i]
	end

	local r = CreateFrame("Button", nil, win)
	r:RegisterForClicks("LeftButtonUp", "RightButtonUp")
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
			if this.botName then
				CENTURION_BotStats_ShowBot(this.botName)
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
						"%s\nLevel %d %s\n%s\n\naggression %d\ntimid %s\ncarrying %dg%s\n\n|cff00ff00Click for full stats|r",
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
	elseif tag == "BSTG" then
		ParseGear(payload)
		if shownBot then DrawGear(shownBot) end
	elseif tag == "BSTT" then
		ParseTalents(payload)
		if shownBot then DrawTalents(shownBot) end
	elseif tag == "BSTP" then
		ParseTalentPicks(payload)
		if shownBot then DrawTalents(shownBot) end
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

------------------------------------------------------------------
-- one bot: a character pane and a talent frame of our own
------------------------------------------------------------------
-- Laid out the way Blizzard's own panes are, because that is the layout every
-- one of these numbers already lives in for anybody reading them. Slot squares
-- down both sides of a model, weapons along the bottom; talents as three trees
-- of icons placed on their real tier and column.
--
-- It has to be ours rather than theirs: InspectFrame reads gear through
-- GetInventoryItemLink(unit, slot) and the model through SetUnit(unit), and the
-- client has no unit for a bot it has never loaded. Everything here is keyed by
-- ITEM ID and DISPLAY ID instead, which the server can simply tell us.

local BOT_W, BOT_H = 420, 520

local bot = CreateFrame("Frame", "CenturionBotPane", UIParent)
bot:SetWidth(BOT_W)
bot:SetHeight(BOT_H)
bot:SetPoint("LEFT", win, "RIGHT", 6, 0)
bot:SetBackdrop({
	bgFile   = "Interface\\DialogFrame\\UI-DialogBox-Background",
	edgeFile = "Interface\\DialogFrame\\UI-DialogBox-Border",
	tile = true, tileSize = 32, edgeSize = 32,
	insets = { left = 11, right = 12, top = 12, bottom = 11 },
})
bot:SetMovable(true)
bot:EnableMouse(true)
bot:RegisterForDrag("LeftButton")
bot:SetScript("OnDragStart", bot.StartMoving)
bot:SetScript("OnDragStop", bot.StopMovingOrSizing)
bot:SetClampedToScreen(true)
bot:Hide()
tinsert(UISpecialFrames, "CenturionBotPane")

local bTitle = bot:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
bTitle:SetPoint("TOP", bot, "TOP", 0, -14)

local bSub = bot:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
bSub:SetPoint("TOP", bTitle, "BOTTOM", 0, -2)

local bClose = CreateFrame("Button", nil, bot, "UIPanelCloseButton")
bClose:SetPoint("TOPRIGHT", bot, "TOPRIGHT", -6, -6)

------------------------------------------------------------------
-- gear page
------------------------------------------------------------------
local gearPage = CreateFrame("Frame", nil, bot)
gearPage:SetAllPoints(bot)

-- The bot's own model. SetDisplayInfo takes a DISPLAY ID, so it needs no unit -
-- which is the whole reason this pane can show somebody on another continent.
local model = CreateFrame("PlayerModel", nil, gearPage)
model:SetWidth(150)
model:SetHeight(280)
model:SetPoint("TOP", bot, "TOP", 0, -74)

local SLOT_LAYOUT = {
	-- slot id, side, row
	{ 0,  "L", 1 }, { 1,  "L", 2 }, { 2,  "L", 3 }, { 14, "L", 4 },
	{ 4,  "L", 5 }, { 3,  "L", 6 }, { 18, "L", 7 }, { 8,  "L", 8 },
	{ 9,  "R", 1 }, { 5,  "R", 2 }, { 6,  "R", 3 }, { 7,  "R", 4 },
	{ 10, "R", 5 }, { 11, "R", 6 }, { 12, "R", 7 }, { 13, "R", 8 },
	{ 15, "B", 1 }, { 16, "B", 2 }, { 17, "B", 3 },
}

local slotButtons = {}

local function MakeSlot(slotId, side, row)
	local b = CreateFrame("Button", nil, gearPage)
	b:SetWidth(34)
	b:SetHeight(34)

	if side == "L" then
		b:SetPoint("TOPLEFT", bot, "TOPLEFT", 22, -70 - (row - 1) * 38)
	elseif side == "R" then
		b:SetPoint("TOPRIGHT", bot, "TOPRIGHT", -22, -70 - (row - 1) * 38)
	else
		b:SetPoint("BOTTOM", bot, "BOTTOM", (row - 2) * 40, 66)
	end

	b.bg = b:CreateTexture(nil, "BACKGROUND")
	b.bg:SetAllPoints(b)
	b.bg:SetTexture("Interface\\Buttons\\UI-EmptySlot")
	b.bg:SetTexCoord(0.2, 0.8, 0.2, 0.8)
	b.bg:SetVertexColor(0.4, 0.4, 0.4)

	b.icon = b:CreateTexture(nil, "ARTWORK")
	b.icon:SetPoint("TOPLEFT", b, "TOPLEFT", 2, -2)
	b.icon:SetPoint("BOTTOMRIGHT", b, "BOTTOMRIGHT", -2, 2)
	b.icon:SetTexCoord(0.07, 0.93, 0.07, 0.93)

	-- Quality border, the way an item button reads it.
	b.border = b:CreateTexture(nil, "OVERLAY")
	b.border:SetPoint("TOPLEFT", b, "TOPLEFT", -1, 1)
	b.border:SetPoint("BOTTOMRIGHT", b, "BOTTOMRIGHT", 1, -1)
	b.border:SetTexture("Interface\\Buttons\\UI-ActionButton-Border")
	b.border:SetBlendMode("ADD")
	b.border:Hide()

	b.ilvl = b:CreateFontString(nil, "OVERLAY", "NumberFontNormalSmall")
	b.ilvl:SetPoint("BOTTOMRIGHT", b, "BOTTOMRIGHT", 0, 1)

	b:SetScript("OnEnter", function()
		GameTooltip:SetOwner(this, "ANCHOR_RIGHT")
		if this.itemId then
			GameTooltip:SetHyperlink("item:" .. this.itemId)
		else
			GameTooltip:SetText(SLOT_NAME[this.slotId] or "Slot", 1, 1, 1)
			GameTooltip:AddLine("empty", 0.6, 0.6, 0.6)
		end
		GameTooltip:Show()
	end)
	b:SetScript("OnLeave", function() GameTooltip:Hide() end)

	b.slotId = slotId
	return b
end

for i = 1, #SLOT_LAYOUT do
	local def = SLOT_LAYOUT[i]
	slotButtons[def[1]] = MakeSlot(def[1], def[2], def[3])
end

local gearFoot = gearPage:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
gearFoot:SetPoint("BOTTOM", bot, "BOTTOM", 0, 40)

------------------------------------------------------------------
-- talent page
------------------------------------------------------------------
local talentPage = CreateFrame("Frame", nil, bot)
talentPage:SetAllPoints(bot)
talentPage:Hide()

local treeButtons = {}
local activeTree = 0

local talentIcons = {}
local TALENT_COLS, TALENT_ROWS = 4, 11

for i = 1, TALENT_COLS * TALENT_ROWS do
	local t = CreateFrame("Button", nil, talentPage)
	t:SetWidth(30)
	t:SetHeight(30)

	local col = math.fmod(i - 1, TALENT_COLS)
	local row = math.floor((i - 1) / TALENT_COLS)
	t:SetPoint("TOPLEFT", bot, "TOPLEFT", 60 + col * 74, -104 - row * 34)

	t.icon = t:CreateTexture(nil, "ARTWORK")
	t.icon:SetAllPoints(t)
	t.icon:SetTexCoord(0.07, 0.93, 0.07, 0.93)

	t.rank = t:CreateFontString(nil, "OVERLAY", "NumberFontNormalSmall")
	t.rank:SetPoint("BOTTOMRIGHT", t, "BOTTOMRIGHT", 2, -1)

	t:SetScript("OnEnter", function()
		if not this.spellId then
			return
		end
		GameTooltip:SetOwner(this, "ANCHOR_RIGHT")
		GameTooltip:SetHyperlink("spell:" .. this.spellId)
		GameTooltip:Show()
	end)
	t:SetScript("OnLeave", function() GameTooltip:Hide() end)
	t:Hide()

	talentIcons[i] = t
end

local talentFoot = talentPage:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
talentFoot:SetPoint("BOTTOM", bot, "BOTTOM", 0, 40)

------------------------------------------------------------------
-- drawing
------------------------------------------------------------------
DrawGear = function(name)
	for _, b in pairs(slotButtons) do
		b.itemId = nil
		b.icon:SetTexture(nil)
		b.border:Hide()
		b.ilvl:SetText("")
	end

	local list = gearOf[name]
	if not list or #list == 0 then
		gearFoot:SetText("|cff808080waiting for the server...|r")
		return
	end

	local worn, best = 0, 0
	for i = 1, #list do
		local g = list[i]
		local b = slotButtons[g.slot]
		if b then
			b.itemId = g.id
			b.icon:SetTexture(GetItemIcon(g.id))

			local r, gr, bl = GetItemQualityColor(g.quality)
			-- Blizzard has no colour for a repurposed artifact tier, so the
			-- field kit is painted red rather than the gold it would inherit.
			if g.quality == 6 then
				r, gr, bl = 0.8, 0.27, 0.27
			end
			b.border:SetVertexColor(r, gr, bl)
			b.border:Show()
			b.ilvl:SetText(g.ilvl)
			worn = worn + 1
			if QUALITY_RANK[g.quality] and QUALITY_RANK[g.quality] >= 2 then
				best = best + 1
			end
		end
	end

	gearFoot:SetText(string.format("%d equipped, |cff1eff00%d|r green or better", worn, best))
end

DrawTalents = function(name)
	local t = talentOf[name]

	for i = 1, #treeButtons do
		local points = t and t[i] or 0
		treeButtons[i]:SetText(string.format("%s (%d)", treeButtons[i].treeName or ("Tree " .. i), points))
	end

	for i = 1, #talentIcons do
		talentIcons[i]:Hide()
		talentIcons[i].spellId = nil
	end

	local picks = picksOf[name]
	if not picks or #picks == 0 then
		talentFoot:SetText("|cff808080waiting for the server...|r")
		return
	end

	local shown = 0
	for i = 1, #picks do
		local p = picks[i]
		if p.tree == activeTree then
			-- Real tier and column, so the shape of the build is the shape on
			-- screen rather than a list in pick order.
			local idx = p.tier * TALENT_COLS + p.col + 1
			local slot = talentIcons[idx]
			if slot then
				local _, _, icon = GetSpellInfo(p.spell)
				slot.spellId = p.spell
				slot.icon:SetTexture(icon)
				slot.rank:SetText(p.rank)
				slot:Show()
				shown = shown + 1
			end
		end
	end

	talentFoot:SetText(shown > 0 and "" or "|cff808080nothing spent in this tree|r")
end

------------------------------------------------------------------
-- page switching
------------------------------------------------------------------
local function ShowGearPage()
	detailTab = "gear"
	talentPage:Hide()
	gearPage:Show()
	if shownBot then DrawGear(shownBot) end
end

local function ShowTalentPage(tree)
	detailTab = "talents"
	activeTree = tree or activeTree
	gearPage:Hide()
	talentPage:Show()
	if shownBot then DrawTalents(shownBot) end
end

local function MakeBotButton(parent, label, width, onClick)
	local b = CreateFrame("Button", nil, parent, "UIPanelButtonTemplate")
	b:SetWidth(width)
	b:SetHeight(20)
	b:SetText(label)
	b:SetScript("OnClick", onClick)
	return b
end

local pageGear = MakeBotButton(bot, "Character", 80, ShowGearPage)
pageGear:SetPoint("BOTTOMLEFT", bot, "BOTTOMLEFT", 20, 16)

local pageTalents = MakeBotButton(bot, "Talents", 70, function() ShowTalentPage(0) end)
pageTalents:SetPoint("LEFT", pageGear, "RIGHT", 3, 0)

-- One button per tree, filled in with the spec names when a bot is shown.
for i = 1, 3 do
	local b = MakeBotButton(talentPage, "Tree " .. i, 108, nil)
	b:SetPoint("TOPLEFT", bot, "TOPLEFT", 18 + (i - 1) * 128, -70)
	b:SetScript("OnClick", function() ShowTalentPage(i - 1) end)
	treeButtons[i] = b
end

local pageGo = MakeBotButton(bot, "Go to", 62, function()
	if shownBot then RunGmCommand(".appear " .. shownBot) end
end)
pageGo:SetPoint("LEFT", pageTalents, "RIGHT", 3, 0)

local pageBring = MakeBotButton(bot, "Bring", 62, function()
	if shownBot then RunGmCommand(".summon " .. shownBot) end
end)
pageBring:SetPoint("LEFT", pageGo, "RIGHT", 3, 0)

local pageRefresh = MakeBotButton(bot, "Refresh", 68, function()
	if shownBot then
		RequestGear(shownBot)
		DrawGear(shownBot)
		DrawTalents(shownBot)
	end
end)
pageRefresh:SetPoint("LEFT", pageBring, "RIGHT", 3, 0)

function CENTURION_BotStats_ShowBot(name)
	local b
	for i = 1, #roster do
		if roster[i].name == name then
			b = roster[i]
			break
		end
	end

	-- Show the pane FIRST, and say why when there is nothing to put in it.
	-- Returning silently here is indistinguishable from a dead button, which is
	-- exactly how this looked: the roster arrives on its own fifteen second
	-- timer, so for the first few seconds after login there is a list of bots
	-- on screen built from an EARLIER sweep and nothing behind it yet.
	shownBot = name
	bot:Show()
	bTitle:SetText(name)

	if not b then
		bSub:SetText("|cffff7f5fno roster entry yet|r - the fleet sweep lands every 15s")
		return
	end

	local state = "idle"
	if b.dead then
		state = "|cff9f9f9fdead|r"
	elseif b.combat then
		state = "|cffff7f5ffighting|r"
	elseif b.travel then
		state = "|cff5f9fftravelling|r"
	elseif b.timid > 0 then
		state = string.format("|cffe0c020timid %ds|r", b.timid)
	end

	local zn = zones[b.zone] and zones[b.zone].name or ("Zone " .. b.zone)
	bSub:SetText(string.format(
		"Level |cffffd200%d|r %s %s   %s   |cffffd200%dg|r   aggression |cffffd200%d|r   %s",
		b.level, SpecName(b.class, b.spec), CLASS_NAME[b.class] or "?", zn, b.gold, b.aggr, state))

	-- Name the tree buttons for this class, so "Tree 2" reads as Fury.
	local specs = SPEC_NAME[b.class]
	for i = 1, 3 do
		treeButtons[i].treeName = specs and specs[i] or ("Tree " .. i)
	end

	if b.display and b.display > 0 then
		model:SetDisplayInfo(b.display)
	end

	-- The gear request goes out over chat, and anything that can throw between
	-- here and the end would leave the pane half-drawn. Ask last, and never let
	-- it take the window down with it.
	ShowGearPage()
	DrawTalents(name)
	pcall(RequestGear, name)
end

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
