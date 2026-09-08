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

-- nil means every role. Otherwise a role id, INCLUDING 0 - "Local" is a real
-- role worth isolating, so the filter cannot use 0 as its off switch.
local roleFilter = nil
-- The cycle order the button walks: "everything" first so one more click always
-- gets back to it, then the roles in the order ROLE_NAME declares them, with
-- Local last since it is the majority and the least interesting.
--
-- The "everything" entry is the string "all" rather than nil: a nil in a table
-- constructor is a hole, and # over a table with a hole at [1] is undefined.
local ROLE_CYCLE = { "all", 4, 1, 2, 5, 3, 0 }
local roleCycleAt = 1

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

-- Which population a bot belongs to, in the server's numbering. Coloured
-- because the whole reason to show it is to pick these out of a list of two
-- hundred: the roles that do NOT cycle through zone bands are the ones whose
-- level and position mean something, and they are the ones worth spotting.
-- "Local" is deliberately unnamed and uncoloured - it is the default, and
-- labelling the majority only adds noise to every row.
local ROLE_NAME = {
	[1] = { "Guardian",  "ffffd200" },   -- pinned to a zone, level frozen at the post
	[2] = { "Veteran",   "ffa335ee" },   -- makes the full climb to sixty and stays
	[3] = { "PvP",       "ffff7f5f" },   -- battleground fleet, outside the PvE world
	[4] = { "Drifter",   "ff40c0f0" },   -- follows a person rather than living anywhere
	[5] = { "Companion", "ff20ff20" },   -- summoned, on somebody's heel right now
}

local function RoleTag(role)
	local entry = ROLE_NAME[role or 0]
	if not entry then
		return ""
	end
	return string.format(" |c%s[%s]|r", entry[2], entry[1])
end

local function RoleName(role)
	local entry = ROLE_NAME[role or 0]
	return entry and entry[1] or "Local"
end

-- "name,level,class,spec,zone,aggr,timid,gold,hp,mp,ilvl,worn,greens,flags;"
local function ParseRoster(payload)
	for row in string.gmatch(payload, "([^;]+)") do
		-- Role is matched optionally, so a client running ahead of the server
		-- shows every bot as "local" for one build rather than dropping the
		-- whole roster on a pattern that no longer fits.
		local name, lvl, cls, spec, zone, aggr, timid, gold, hp, mp, ilvl, worn, greens, flags, disp, role =
			string.match(row,
				"^([^,]+),(%d+),(%d+),(%d+),(%d+),(%d+),(%d+),(%d+),(%d+),(%d+),(%d+),(%d+),(%d+),(%d+),(%d+),(%d+)$")
		if not name then
			name, lvl, cls, spec, zone, aggr, timid, gold, hp, mp, ilvl, worn, greens, flags, disp =
				string.match(row,
					"^([^,]+),(%d+),(%d+),(%d+),(%d+),(%d+),(%d+),(%d+),(%d+),(%d+),(%d+),(%d+),(%d+),(%d+),(%d+)$")
		end
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
				role   = tonumber(role) or 0,
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
local modelOf  = {}     -- [botName] = model file path
local talentOf = {}     -- [botName] = { t1, t2, t3 }

-- What the bot is CARRYING. Asked for the same way and for the same reason -
-- a bag list is longer than an equipment list and the fleet averages sixty-odd
-- rows a bot, so streaming it for everyone would dwarf the rest of the feed.
local bagsOf   = {}     -- [botName] = { {id, count, quality, ilvl}, ... }
local bagsDone = {}     -- [botName] = true once the sweep's terminator arrived
local bagsCap  = {}     -- [botName] = how many slots the bot's bags hold in all

-- Drawn far below, repainted from the message handler above it.
local DrawBags

-- "name|id,count,quality,ilvl;..."
local function ParseBags(payload)
	local name, rows = string.match(payload or "", "^([^|]+)|(.*)$")
	if not name then
		return
	end

	bagsOf[name] = bagsOf[name] or {}
	for id, count, quality, ilvl in string.gmatch(rows or "", "(%-?%d+),(%d+),(%d+),(%d+);") do
		table.insert(bagsOf[name], {
			id      = tonumber(id) or 0,
			count   = tonumber(count) or 1,
			quality = tonumber(quality) or 1,
			ilvl    = tonumber(ilvl) or 0,
		})
	end
end

-- Item clicks, shared by the gear panel and the bag grid.
--
-- Uncached ids are the whole difficulty here. Nothing this character has never
-- seen is in the client's item cache, which is most of what a bot owns, and the
-- stock helpers all go through GetItemInfo and quietly do nothing when it comes
-- back nil. So each of these has a hand-built fallback rather than dropping the
-- click on the floor.
local function BotStats_ItemLink(id)
	if not id then
		return nil
	end
	local _, link = GetItemInfo(id)
	-- The client resolves the name from the id when it draws the chat line, so
	-- a hand-built link is not a dead one.
	return link or ("|cffffffff|Hitem:" .. id .. ":0:0:0:0:0:0:0|h[item]|h|r")
end

local function BotStats_DressUp(id)
	if not id then
		return
	end
	local link = BotStats_ItemLink(id)
	-- IsDressableItem, which DressUpItemLink gates on, needs the cache too and
	-- says no for anything unresolved. Take the answer when it works.
	if DressUpItemLink and DressUpItemLink(link) then
		return
	end
	if not DressUpFrame or not DressUpModel then
		return
	end

	-- A pack is mostly potions and cloth. If the cache does know the item and
	-- says it is not worn, stop here rather than opening the dressing room on
	-- nothing; if the cache does not know it yet, fall through and let the
	-- model ask - refusing there would break the case this exists for.
	local _, _, _, _, _, _, _, _, equipLoc = GetItemInfo(id)
	if equipLoc and (equipLoc == "" or equipLoc == "INVTYPE_NON_EQUIP"
		or equipLoc == "INVTYPE_BAG" or equipLoc == "INVTYPE_QUIVER"
		or equipLoc == "INVTYPE_AMMO" or equipLoc == "INVTYPE_RELIC") then
		return
	end
	if not DressUpFrame:IsShown() then
		if SetPortraitTexture and DressUpFramePortrait then
			SetPortraitTexture(DressUpFramePortrait, "player")
		end
		ShowUIPanel(DressUpFrame)
		DressUpModel:SetUnit("player")
	end
	-- TryOn takes a bare id, and unlike the wrapper it does not consult the
	-- cache first - the model asks the server for the display itself.
	DressUpModel:TryOn(id)
end

-- Shift links into chat, ctrl sends it to the dressing room: the same two
-- gestures every other item frame in the game answers to.
local function BotStats_ItemClick(id)
	if not id then
		return
	end
	if IsModifiedClick("CHATLINK") then
		if ChatEdit_InsertLink then
			ChatEdit_InsertLink(BotStats_ItemLink(id))
		end
	elseif IsModifiedClick("DRESSUP") then
		BotStats_DressUp(id)
	end
end

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
		local spell, rank, maxRank, tree, tier, col, preTier, preCol =
			string.match(row, "^(%d+),(%d+),(%d+),(%d+),(%d+),(%d+),(%-?%d+),(%-?%d+)$")
		if spell then
			table.insert(picksOf[name], {
				spell   = tonumber(spell) or 0,
				rank    = tonumber(rank) or 0,
				maxRank = tonumber(maxRank) or 0,
				tree    = tonumber(tree) or 0,
				tier    = tonumber(tier) or 0,
				col     = tonumber(col) or 0,
				preTier = tonumber(preTier) or -1,
				preCol  = tonumber(preCol) or -1,
			})
		end
	end
end

-- "name|Character\Orc\Male\OrcMale.mdx"
local function ParseModel(payload)
	local name, path = string.match(payload, "^([^|]+)|(.+)$")
	if name and path then
		modelOf[name] = path
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
		local scope = zoneName and ("Bots in " .. zoneName) or "Every bot"
		if roleFilter then
			scope = scope .. " |cff909090- " .. RoleName(roleFilter) .. " only|r"
		end
		title:SetText(scope)

		for i = 1, #roster do
			local b = roster[i]
			-- Role is matched on the row's own value, so "Local" (0) filters
			-- exactly like the named roles rather than meaning "no filter".
			if (not zoneFilter or b.zone == zoneFilter) and
			   (roleFilter == nil or (b.role or 0) == roleFilter) then
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
					label   = string.format("%s |cff909090%d %s|r%s%s",
						b.name, b.level, CLASS_NAME[b.class] or "?", RoleTag(b.role), mark),
					botName = b.name,
					count   = b.level,
					level   = b.level,
					aggr    = b.aggr,
					timid   = b.timid,
					gold    = b.gold,
					tip     = string.format(
						"%s\nLevel %d %s\n%s\nRole: %s\n\naggression %d\ntimid %s\ncarrying %dg\n\n|cff00ff00Click for full stats|r",
						b.name, b.level, CLASS_NAME[b.class] or "?", zn, RoleName(b.role), b.aggr,
						(b.timid > 0) and (b.timid .. "s remaining") or "no", b.gold),
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
	elseif tag == "BSTB" then
		ParseBags(payload)
		if shownBot and detailTab == "bags" then DrawBags(shownBot) end
	elseif tag == "BSTC" then
		-- End of a bag sweep. Marks the list COMPLETE so the panel can tell an
		-- empty pack from a request that never came back - without it the two
		-- look identical and the pane sits on "loading" forever. It also carries
		-- the bag CAPACITY, which is the only way the client can learn how much
		-- room somebody else's character has.
		local name, _, cap = string.match(payload or "", "^([^|]+)|(%d*)|?(%d*)")
		if name then
			bagsDone[name] = true
			bagsCap[name] = tonumber(cap or "")
			-- A bot carrying nothing sends no rows at all, so without this the
			-- table stays nil and the panel reads as "still asking" forever.
			bagsOf[name] = bagsOf[name] or {}
			if shownBot == name and detailTab == "bags" then DrawBags(name) end
		end
	elseif tag == "BSTG" then
		ParseGear(payload)
		if shownBot then DrawGear(shownBot) end
	elseif tag == "BSTT" then
		ParseTalents(payload)
		if shownBot then DrawTalents(shownBot) end
	elseif tag == "BSTM" then
		ParseModel(payload)
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

local BOT_W, BOT_H = 420, 448

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
-- No model, and the layout is built for its absence rather than around a hole
-- where one used to be.
--
-- A character's body texture is a COMPOSITE the client assembles from a unit's
-- customisation fields, and only SetUnit triggers it. SetModel loads geometry
-- and nothing else, which is why every bot - druid, priest, paladin - came out
-- as the same white silhouette. There is no display-id route to a textured
-- character in 3.3.5, so the picture is gone and the space belongs to the
-- numbers now.
--
-- Two columns of slots, then the attributes underneath. Everything is either a
-- slot or a stat; there is no middle.
local gearPage = CreateFrame("Frame", nil, bot)
gearPage:SetAllPoints(bot)

local GEAR_ROWS, GEAR_ROW_H = 10, 25

local slotButtons = {}

-- Down the left in the order the paper doll reads, then down the right, so a
-- glance finds a slot where the eye expects it.
local SLOT_ORDER = {
	0, 1, 2, 14, 4, 3, 18, 8, 9, 5,
	6, 7, 10, 11, 12, 13, 15, 16, 17,
}

local function MakeSlot(slotId, index)
	local b = CreateFrame("Button", nil, gearPage)
	b:SetWidth(190)
	b:SetHeight(GEAR_ROW_H)

	local col = (index <= GEAR_ROWS) and 0 or 1
	local row = (index <= GEAR_ROWS) and index or (index - GEAR_ROWS)
	b:SetPoint("TOPLEFT", bot, "TOPLEFT", 18 + col * 196, -64 - (row - 1) * GEAR_ROW_H)

	b.icon = b:CreateTexture(nil, "ARTWORK")
	b.icon:SetWidth(21)
	b.icon:SetHeight(21)
	b.icon:SetPoint("LEFT", b, "LEFT", 0, 0)
	b.icon:SetTexCoord(0.07, 0.93, 0.07, 0.93)

	-- An empty slot still shows its outline, so a bare neck reads as missing
	-- rather than as a row that failed to draw.
	b.empty = b:CreateTexture(nil, "BACKGROUND")
	b.empty:SetAllPoints(b.icon)
	b.empty:SetTexture("Interface\\Buttons\\UI-EmptySlot")
	b.empty:SetTexCoord(0.2, 0.8, 0.2, 0.8)
	b.empty:SetVertexColor(0.35, 0.35, 0.35)

	-- Four hairlines rather than UI-ActionButton-Border, which is a soft glow
	-- with a wide transparent margin: drawn at the icon's own size it collapses
	-- into a coloured blob in the middle of the art.
	b.edge = {}
	for e = 1, 4 do
		local t = b:CreateTexture(nil, "OVERLAY")
		t:SetTexture("Interface\\Buttons\\WHITE8X8")
		b.edge[e] = t
	end
	b.edge[1]:SetPoint("TOPLEFT", b.icon, "TOPLEFT", 0, 0)
	b.edge[1]:SetPoint("TOPRIGHT", b.icon, "TOPRIGHT", 0, 0)
	b.edge[1]:SetHeight(1)
	b.edge[2]:SetPoint("BOTTOMLEFT", b.icon, "BOTTOMLEFT", 0, 0)
	b.edge[2]:SetPoint("BOTTOMRIGHT", b.icon, "BOTTOMRIGHT", 0, 0)
	b.edge[2]:SetHeight(1)
	b.edge[3]:SetPoint("TOPLEFT", b.icon, "TOPLEFT", 0, 0)
	b.edge[3]:SetPoint("BOTTOMLEFT", b.icon, "BOTTOMLEFT", 0, 0)
	b.edge[3]:SetWidth(1)
	b.edge[4]:SetPoint("TOPRIGHT", b.icon, "TOPRIGHT", 0, 0)
	b.edge[4]:SetPoint("BOTTOMRIGHT", b.icon, "BOTTOMRIGHT", 0, 0)
	b.edge[4]:SetWidth(1)

	b.SetEdgeColor = function(self, r, g, bl, show)
		for e = 1, 4 do
			if show then
				self.edge[e]:SetVertexColor(r, g, bl)
				self.edge[e]:Show()
			else
				self.edge[e]:Hide()
			end
		end
	end

	b.text = b:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
	b.text:SetPoint("LEFT", b.icon, "RIGHT", 4, 0)
	b.text:SetWidth(140)
	b.text:SetJustifyH("LEFT")

	b.ilvl = b:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
	b.ilvl:SetPoint("RIGHT", b, "RIGHT", 0, 0)

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

	-- Shift-click links the piece into chat, ctrl-click tries it on. Worth
	-- having here because "look at what this bot is wearing" usually ends in
	-- either showing somebody else or wanting the piece yourself.
	b:RegisterForClicks("LeftButtonUp")
	b:SetScript("OnClick", function()
		BotStats_ItemClick(this.itemId)
	end)

	b.slotId = slotId
	return b
end

for i = 1, #SLOT_ORDER do
	slotButtons[SLOT_ORDER[i]] = MakeSlot(SLOT_ORDER[i], i)
end

-- Attributes, in two columns under the gear, filling the width rather than
-- stacking down one side of an empty middle.
local statsRule = gearPage:CreateTexture(nil, "ARTWORK")
statsRule:SetTexture("Interface\\Buttons\\WHITE8X8")
statsRule:SetVertexColor(0.3, 0.3, 0.3)
statsRule:SetHeight(1)
statsRule:SetPoint("TOPLEFT", bot, "TOPLEFT", 18, -64 - GEAR_ROWS * GEAR_ROW_H - 6)
statsRule:SetPoint("TOPRIGHT", bot, "TOPRIGHT", -18, -64 - GEAR_ROWS * GEAR_ROW_H - 6)

local statLines = {}
for i = 1, 8 do
	local fs = gearPage:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
	local col = (i <= 4) and 0 or 1
	local row = (i <= 4) and i or (i - 4)
	fs:SetPoint("TOPLEFT", bot, "TOPLEFT", 18 + col * 196,
		-64 - GEAR_ROWS * GEAR_ROW_H - 14 - (row - 1) * 15)
	fs:SetWidth(190)
	fs:SetJustifyH("LEFT")
	statLines[i] = fs
end

local function SetStatLine(i, label, value)
	if not statLines[i] then
		return
	end
	if label then
		statLines[i]:SetText(string.format("|cff909090%s|r  %s", label, value))
	else
		statLines[i]:SetText("")
	end
end

local gearFoot = gearPage:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
gearFoot:SetPoint("BOTTOM", bot, "BOTTOM", 0, 42)

------------------------------------------------------------------
-- talent page
------------------------------------------------------------------
-- Blizzard's own tree art, on Blizzard's own grid.
--
-- The talent frame draws each tree as a 384x384 background split into four
-- quadrants under Interface\TalentFrame, with talents on a four column by
-- eleven tier grid laid over it. Both are reproduced here rather than
-- approximated: the shape of a build is only readable against the background it
-- was designed on.
--
-- The background NAME lives in TalentTab.dbc's BackgroundFile, which this
-- fork's DBC struct does not load, so the mapping is kept here instead. Ordered
-- by the same 0/1/2 the server sends.
local TREE_ART = {
	[1]  = { "WarriorArms", "WarriorFury", "WarriorProtection" },
	[2]  = { "PaladinHoly", "PaladinProtection", "PaladinCombat" },
	[3]  = { "HunterBeastMastery", "HunterMarksmanship", "HunterSurvival" },
	[4]  = { "RogueAssassination", "RogueCombat", "RogueSubtlety" },
	[5]  = { "PriestDiscipline", "PriestHoly", "PriestShadow" },
	[6]  = { "DeathKnightBlood", "DeathKnightFrost", "DeathKnightUnholy" },
	[7]  = { "ShamanElementalCombat", "ShamanEnhancement", "ShamanRestoration" },
	[8]  = { "MageArcane", "MageFire", "MageFrost" },
	[9]  = { "WarlockCurses", "WarlockSummoning", "WarlockDestruction" },
	[11] = { "DruidBalance", "DruidFeralCombat", "DruidRestoration" },
}

-- The tree is 384 wide by 512 TALL, not square. All four background quadrants
-- are 256 high; only the width differs, 256 on the left and 128 on the right.
-- Drawing the bottom pair at 128 squashed the art into the top two thirds and
-- left the talents floating over the wrong part of it.
local TREE_W, TREE_H = 384, 512
local TALENT_COLS, TALENT_ROWS = 4, 11
-- Blizzard's own metrics: 37 pixel buttons on a 63 by 44 step, inset 22 from
-- the top left of the art. Four columns come to 248 wide and eleven tiers to
-- 499 tall, which is what the 384x512 background was drawn for.
local TAL_SIZE = 37
local TAL_X0, TAL_Y0, TAL_DX, TAL_DY = 22, 22, 63, 44

local talentPage = CreateFrame("Frame", nil, bot)
talentPage:SetAllPoints(bot)
talentPage:Hide()

-- The tree art is four quadrants: 256 wide on the left, 128 on the right, and
-- the same split top to bottom. Anchored as one block so the grid can be laid
-- out against its top left corner.
local treeArt = CreateFrame("Frame", nil, talentPage)
treeArt:SetWidth(TREE_W)
treeArt:SetHeight(TREE_H)
treeArt:SetPoint("TOP", bot, "TOP", 0, -96)

local artPieces = {}
local ART_LAYOUT = {
	{ "TopLeft",     256, 256,   0,    0 },
	{ "TopRight",    128, 256, 256,    0 },
	{ "BottomLeft",  256, 256,   0, -256 },
	{ "BottomRight", 128, 256, 256, -256 },
}

for i = 1, #ART_LAYOUT do
	local def = ART_LAYOUT[i]
	local t = treeArt:CreateTexture(nil, "BACKGROUND")
	t:SetWidth(def[2])
	t:SetHeight(def[3])
	t:SetPoint("TOPLEFT", treeArt, "TOPLEFT", def[4], def[5])
	artPieces[i] = { tex = t, corner = def[1] }
end

local talentIcons = {}
local activeTree = 0
-- Filled in further down, where the buttons are built, but declared here beside
-- the rest of the talent state: DrawTalents closes over it and is defined
-- before that point.
local treeButtons = {}

for i = 1, TALENT_COLS * TALENT_ROWS do
	local t = CreateFrame("Button", nil, talentPage)
	t:SetWidth(TAL_SIZE)
	t:SetHeight(TAL_SIZE)

	local col = math.fmod(i - 1, TALENT_COLS)
	local row = math.floor((i - 1) / TALENT_COLS)
	t:SetPoint("TOPLEFT", treeArt, "TOPLEFT", TAL_X0 + col * TAL_DX, -(TAL_Y0 + row * TAL_DY))

	t.icon = t:CreateTexture(nil, "ARTWORK")
	t.icon:SetPoint("TOPLEFT", t, "TOPLEFT", 3, -3)
	t.icon:SetPoint("BOTTOMRIGHT", t, "BOTTOMRIGHT", -3, 3)
	t.icon:SetTexCoord(0.07, 0.93, 0.07, 0.93)

	-- Four hairlines, NOT UI-EmptySlot. That texture is a filled slot rather
	-- than a ring, and drawn on OVERLAY it covered the icon completely - which
	-- is why every talent was a blank plate with a rank number on it. The same
	-- border the gear slots use, which is known to work at this size.
	t.edge = {}
	for e = 1, 4 do
		local x = t:CreateTexture(nil, "OVERLAY")
		x:SetTexture("Interface\\Buttons\\WHITE8X8")
		t.edge[e] = x
	end
	t.edge[1]:SetPoint("TOPLEFT", t, "TOPLEFT", 0, 0)
	t.edge[1]:SetPoint("TOPRIGHT", t, "TOPRIGHT", 0, 0)
	t.edge[1]:SetHeight(1)
	t.edge[2]:SetPoint("BOTTOMLEFT", t, "BOTTOMLEFT", 0, 0)
	t.edge[2]:SetPoint("BOTTOMRIGHT", t, "BOTTOMRIGHT", 0, 0)
	t.edge[2]:SetHeight(1)
	t.edge[3]:SetPoint("TOPLEFT", t, "TOPLEFT", 0, 0)
	t.edge[3]:SetPoint("BOTTOMLEFT", t, "BOTTOMLEFT", 0, 0)
	t.edge[3]:SetWidth(1)
	t.edge[4]:SetPoint("TOPRIGHT", t, "TOPRIGHT", 0, 0)
	t.edge[4]:SetPoint("BOTTOMRIGHT", t, "BOTTOMRIGHT", 0, 0)
	t.edge[4]:SetWidth(1)

	t.SetEdgeColor = function(self, r, g, b)
		for e = 1, 4 do
			self.edge[e]:SetVertexColor(r, g, b)
		end
	end

	-- Rank in the bottom right on a dark plate, the way the talent frame does.
	t.rankBg = t:CreateTexture(nil, "OVERLAY")
	t.rankBg:SetWidth(15)
	t.rankBg:SetHeight(13)
	t.rankBg:SetPoint("BOTTOMRIGHT", t, "BOTTOMRIGHT", 2, -2)
	t.rankBg:SetTexture("Interface\\Buttons\\WHITE8X8")
	t.rankBg:SetVertexColor(0, 0, 0, 0.75)

	t.rank = t:CreateFontString(nil, "OVERLAY", "NumberFontNormalSmall")
	t.rank:SetPoint("CENTER", t.rankBg, "CENTER", 0, 0)

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

-- Prerequisite links, pooled. A tree needs at most a dozen and they are rebuilt
-- on every draw, so they are handed out by index rather than kept per talent.
local links = {}

local function AcquireLink(i)
	if not links[i] then
		local t = talentPage:CreateTexture(nil, "ARTWORK")
		t:SetTexture("Interface\\Buttons\\WHITE8X8")
		links[i] = t
	end
	links[i]:Show()
	return links[i]
end

local function HideLinksFrom(i)
	for n = i, #links do
		links[n]:Hide()
	end
end

-- A vertical drop when the two share a column, an elbow otherwise. Drawn
-- BEHIND the buttons, so it runs to the edge of each rather than across them.
local function PlaceLink(line, from, to, straight)
	line:ClearAllPoints()
	if straight then
		line:SetWidth(3)
		line:SetPoint("TOP", from, "BOTTOM", 0, 1)
		line:SetPoint("BOTTOM", to, "TOP", 0, -1)
	else
		-- One horizontal bar at the destination's height is enough to show the
		-- relationship without a second segment and a corner to get wrong.
		line:SetHeight(3)
		line:SetPoint("LEFT", from, "RIGHT", -1, 0)
		line:SetPoint("RIGHT", to, "LEFT", 1, 0)
	end
end

------------------------------------------------------------------
-- drawing
------------------------------------------------------------------
DrawGear = function(name)
	for _, b in pairs(slotButtons) do
		b.itemId = nil
		b.icon:SetTexture(nil)
		b.empty:Show()
		b:SetEdgeColor(0, 0, 0, false)
		b.text:SetText("|cff606060" .. (SLOT_NAME[b.slotId] or "?") .. "|r")
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
			local itemName = GetItemInfo(g.id)
			local r, gr, bl = GetItemQualityColor(g.quality)
			-- Blizzard has no colour for a repurposed artifact tier, so the
			-- field kit is painted red rather than the gold it would inherit.
			if g.quality == 6 then
				r, gr, bl = 0.8, 0.27, 0.27
			end

			b.itemId = g.id
			b.icon:SetTexture(GetItemIcon(g.id))
			b.empty:Hide()
			b:SetEdgeColor(r, gr, bl, true)
			b.text:SetText(itemName or (SLOT_NAME[g.slot] or "?"))
			b.text:SetTextColor(r, gr, bl)
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

	-- Point the background at this class and tree.
	local b = nil
	for i = 1, #roster do
		if roster[i].name == name then b = roster[i] break end
	end
	local art = b and TREE_ART[b.class] and TREE_ART[b.class][activeTree + 1]
	for i = 1, #artPieces do
		if art then
			artPieces[i].tex:SetTexture("Interface\\TalentFrame\\" .. art .. "-" .. artPieces[i].corner)
			artPieces[i].tex:Show()
		else
			artPieces[i].tex:Hide()
		end
	end

	local shown, spent = 0, 0
	local placed = {}

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
				slot.rank:SetText(p.rank .. "/" .. p.maxRank)
				placed[idx] = p

				-- Taken talents are lit with a gold edge; untaken ones are
				-- drawn anyway, greyed and dimmed. A tree with the empty
				-- sockets missing is not a tree - the shape only reads against
				-- what was passed over.
				if p.rank > 0 then
					slot.icon:SetDesaturated(false)
					slot.icon:SetAlpha(1)
					slot:SetEdgeColor(1, 0.82, 0.2)
					slot.rank:SetTextColor(1, 0.82, 0.2)
					spent = spent + p.rank
				else
					slot.icon:SetDesaturated(true)
					slot.icon:SetAlpha(0.45)
					slot:SetEdgeColor(0.35, 0.35, 0.35)
					slot.rank:SetTextColor(0.5, 0.5, 0.5)
				end

				slot:Show()
				shown = shown + 1
			end
		end
	end

	-- Arrows, as plain lines from the prerequisite to the talent that needs it.
	-- Blizzard draws these out of the UI-TalentArrows atlas; guessing at atlas
	-- coordinates is what smeared the last two textures, and a line carries the
	-- same information - which talent gates which - without the guesswork.
	local usedLinks = 0
	for idx, p in pairs(placed) do
		if p.preTier >= 0 and p.preCol >= 0 then
			local fromIdx = p.preTier * TALENT_COLS + p.preCol + 1
			local from, to = talentIcons[fromIdx], talentIcons[idx]
			if from and to and placed[fromIdx] then
				usedLinks = usedLinks + 1
				local line = AcquireLink(usedLinks)
				-- Lit only when the prerequisite is actually met, so a dead
				-- branch reads as dead.
				local lit = placed[fromIdx].rank >= placed[fromIdx].maxRank
				line:SetVertexColor(lit and 1 or 0.35, lit and 0.82 or 0.35, lit and 0.2 or 0.35)
				PlaceLink(line, from, to, p.preCol == p.col)
			end
		end
	end
	HideLinksFrom(usedLinks + 1)

	talentFoot:SetText(spent > 0 and "" or "|cff808080nothing spent in this tree|r")
end

------------------------------------------------------------------
-- page switching
------------------------------------------------------------------
-- The two pages want very different heights - a 512 tall talent tree against
-- ten rows of gear - so the pane takes the height of whichever is showing
-- rather than sizing for the taller and leaving the other half empty.
local GEAR_PAGE_H, TALENT_PAGE_H = 448, 660

local function ShowGearPage()
	detailTab = "gear"
	talentPage:Hide()
	bot:SetHeight(GEAR_PAGE_H)
	gearPage:Show()
	if shownBot then DrawGear(shownBot) end
end

local function ShowTalentPage(tree)
	detailTab = "talents"
	activeTree = tree or activeTree
	gearPage:Hide()
	bot:SetHeight(TALENT_PAGE_H)
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

-- The "Go to" and "Bring" buttons used to sit here. This window is for looking
-- at the fleet, and a teleport is one misclick away from moving a bot out of
-- the zone it was drafted for; .appear and .summon are still a chat line away
-- for the times that is actually wanted.
local pageRefresh = MakeBotButton(bot, "Refresh", 68, function()
	if shownBot then
		RequestGear(shownBot)
		DrawGear(shownBot)
		DrawTalents(shownBot)
	end
end)
-- Anchored below, once the Bags button it follows has been created.

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
		state = "|cff5f9ffftravelling|r"
	elseif b.timid > 0 then
		state = string.format("|cffe0c020timid %ds|r", b.timid)
	end

	local zn = zones[b.zone] and zones[b.zone].name or ("Zone " .. b.zone)
	bSub:SetText(string.format("Level |cffffd200%d|r %s %s   %s%s",
		b.level, SpecName(b.class, b.spec), CLASS_NAME[b.class] or "?", zn, RoleTag(b.role)))

	-- The column that replaced the model.
	SetStatLine(1, "State",      state)
	SetStatLine(2, "Health",     string.format("|cffffd200%d%%|r", b.hp))
	SetStatLine(3, b.mp > 0 and "Mana" or nil, string.format("|cffffd200%d%%|r", b.mp))
	SetStatLine(4, b.pvp and "Role" or nil, "|cffff7f5fPvP only, no PvE|r")
	SetStatLine(5, "Aggression", string.format("|cffffd200%d|r / 100", b.aggr))
	SetStatLine(6, "Timid",      b.timid > 0 and string.format("|cffe0c020%ds|r", b.timid) or "no")
	SetStatLine(7, "Gold",       string.format("|cffffd200%dg|r", b.gold))
	SetStatLine(8, "Item level", string.format("|cffffd200%d|r", b.ilvl))

	-- Name the tree buttons for this class, so "Tree 2" reads as Fury.
	local specs = SPEC_NAME[b.class]
	for i = 1, 3 do
		treeButtons[i].treeName = specs and specs[i] or ("Tree " .. i)
	end

	-- The model arrives separately, as a FILE PATH, with the gear response.
	-- SetDisplayInfo does not exist on a 3.3.5 model frame - it arrives in 4.0 -
	-- and the two methods that do exist take a unit or a creature id, neither of
	-- which a remote player bot has.

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

------------------------------------------------------------------
-- bags page
------------------------------------------------------------------
-- What the bot is carrying, beside what it is wearing.
--
-- Requested rather than streamed. The roster is pushed unasked because it is
-- only about forty whispers for the whole fleet; a bag list is sixty-odd rows
-- for ONE bot, so the same trick does not scale and this asks the way the gear
-- panel does - ".botstats bags <name>" out, BSTB rows back.
-- The pane is 420 wide, which takes ten 32px icons and their gaps with room to
-- spare. Ninety visible covers the fleet average outright - a bot carries 78.6
-- inventory rows - and the tail scrolls: bag size is doubled on this realm and
-- the worst pack measured held 162.
local BAGS_PAGE_H = 500
local BAG_COLS, BAG_ROWS_N = 10, 9
local BAG_SLOTS = BAG_COLS * BAG_ROWS_N
local BAG_ICON = 32
local bagScrollRow = 0

local bagsPage = CreateFrame("Frame", nil, bot)
bagsPage:SetAllPoints(bot)
bagsPage:Hide()

local bagsTitle = bagsPage:CreateFontString(nil, "OVERLAY", "GameFontNormal")
bagsTitle:SetPoint("TOPLEFT", bagsPage, "TOPLEFT", 20, -40)
bagsTitle:SetText("Carried")

local bagsFoot = bagsPage:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
bagsFoot:SetPoint("TOPLEFT", bagsPage, "TOPLEFT", 20, -58)

-- An off-screen tooltip, used to WARM THE CACHE rather than to show anything.
--
-- GetItemInfo answers only from the client's own item cache, and that cache is
-- cold for anything this character has never seen - which is most of what a bot
-- is carrying. That is why the first cut of this page could only print raw ids.
-- Pointing a tooltip at "item:<id>" makes the client ask the server for the
-- item, exactly as hovering it would, and the answer lands in the cache a
-- moment later. So: ask for everything unresolved, then repaint.
local scanTip = CreateFrame("GameTooltip", "CENTURION_BotStatsScanTooltip", UIParent, "GameTooltipTemplate")
scanTip:SetOwner(UIParent, "ANCHOR_NONE")

local bagSlots = {}
for i = 1, BAG_SLOTS do
	local b = CreateFrame("Button", "CENTURION_BotStatsBag" .. i, bagsPage)
	b:SetWidth(BAG_ICON)
	b:SetHeight(BAG_ICON)
	local col = (i - 1) % BAG_COLS
	local row = math.floor((i - 1) / BAG_COLS)
	b:SetPoint("TOPLEFT", bagsPage, "TOPLEFT", 22 + col * (BAG_ICON + 4), -78 - row * (BAG_ICON + 6))

	b.icon = b:CreateTexture(nil, "BACKGROUND")
	b.icon:SetAllPoints(b)

	-- The empty-slot backdrop, so an unfilled grid reads as a bag rather than
	-- as a hole in the frame.
	b.slotBg = b:CreateTexture(nil, "BORDER")
	b.slotBg:SetAllPoints(b)
	b.slotBg:SetTexture("Interface\\Buttons\\UI-EmptySlot-White")
	b.slotBg:SetTexCoord(0.15, 0.85, 0.15, 0.85)
	b.slotBg:SetVertexColor(0.35, 0.35, 0.35, 0.7)

	b.count = b:CreateFontString(nil, "OVERLAY", "NumberFontNormalSmall")
	b.count:SetPoint("BOTTOMRIGHT", b, "BOTTOMRIGHT", -1, 1)

	-- Quality as a border tint on the icon itself. A four-texture frame per slot
	-- would be forty-two more textures for the same information.
	b.edge = b:CreateTexture(nil, "OVERLAY")
	b.edge:SetPoint("TOPLEFT", b, "TOPLEFT", -2, 2)
	b.edge:SetPoint("BOTTOMRIGHT", b, "BOTTOMRIGHT", 2, -2)
	b.edge:SetTexture("Interface\\Buttons\\UI-ActionButton-Border")
	b.edge:SetBlendMode("ADD")
	b.edge:Hide()

	b:SetScript("OnEnter", function()
		GameTooltip:SetOwner(this, "ANCHOR_RIGHT")
		if this.itemId then
			GameTooltip:SetHyperlink("item:" .. this.itemId)
		else
			GameTooltip:SetText("Empty slot", 0.6, 0.6, 0.6)
		end
		GameTooltip:Show()
	end)
	b:SetScript("OnLeave", function() GameTooltip:Hide() end)

	b:RegisterForClicks("LeftButtonUp")
	b:SetScript("OnClick", function()
		BotStats_ItemClick(this.itemId)
	end)

	bagSlots[i] = b
end

-- How many cells the grid is: the bot's whole carrying capacity, not just the
-- part of it that is full. The free ones are drawn as empty slots, so the page
-- answers "has it got room" as well as "what has it got".
local function BagCellCount(name)
	local rows = bagsOf[name]
	local held = rows and #rows or 0
	local cap = bagsCap[name]
	if not cap then
		-- Older worldserver, which sent no capacity. Fall back to drawing only
		-- what came back rather than inventing a bag size.
		return held
	end
	return math.max(cap, held)
end

function DrawBags(name)
	local rows = bagsOf[name]
	local done = bagsDone[name]

	for i = 1, BAG_SLOTS do
		local b = bagSlots[i]
		b.itemId = nil
		b.icon:SetTexture(nil)
		b.count:SetText("")
		b.edge:Hide()
		b:Hide()
	end

	if not rows or (not done and #rows == 0) then
		bagsFoot:SetText("|cff808080asking the server...|r")
		-- Non-zero: the rows themselves have not landed yet, so the repaint
		-- ticker must keep running. Returning nothing here read as "nothing
		-- outstanding" and stopped it before there was anything to draw.
		return 1
	end

	local cells = BagCellCount(name)
	if cells == 0 then
		bagsFoot:SetText("|cff808080carrying nothing|r")
		return
	end

	-- Best first: a pack is mostly consumables and the thing worth seeing is
	-- what the bot has hoarded, not the order the bags happen to be walked in.
	table.sort(rows, function(a, b)
		if a.quality ~= b.quality then return a.quality > b.quality end
		return (a.ilvl or 0) > (b.ilvl or 0)
	end)

	-- Clamp before drawing: the bag can shrink under a scrolled view (an item
	-- sold, a smaller bot picked) and an offset left past the end would show an
	-- empty grid with no way to tell why.
	local maxRow = math.max(0, math.ceil(cells / BAG_COLS) - BAG_ROWS_N)
	if bagScrollRow > maxRow then bagScrollRow = maxRow end
	if bagScrollRow < 0 then bagScrollRow = 0 end

	local first = bagScrollRow * BAG_COLS
	local pending = 0
	local shown = math.min(cells - first, BAG_SLOTS)
	for i = 1, shown do
		local b = bagSlots[i]
		b:Show()

		-- Past the end of the item list is a free slot: shown, but left as the
		-- bare backdrop the button already carries.
		local r = rows[first + i]
		if not r then
			b.itemId = nil
		else
			b.itemId = r.id

			local _, _, quality, _, _, _, _, _, _, texture = GetItemInfo(r.id)
			if texture then
				b.icon:SetTexture(texture)
			else
				-- Not cached yet. Ask for it, show the placeholder, and repaint
				-- when the answer arrives.
				b.icon:SetTexture("Interface\\Icons\\INV_Misc_QuestionMark")
				scanTip:SetOwner(UIParent, "ANCHOR_NONE")
				scanTip:SetHyperlink("item:" .. r.id)
				pending = pending + 1
			end

			local q = quality or r.quality or 1
			local colour = ITEM_QUALITY_COLORS[q]
			if colour and q > 1 then
				b.edge:SetVertexColor(colour.r, colour.g, colour.b, 0.85)
				b.edge:Show()
			end

			if (r.count or 1) > 1 then
				b.count:SetText(r.count)
			end
		end
	end

	local note
	if bagsCap[name] then
		note = string.format("|cff808080%d items, %d of %d slots free|r",
			#rows, math.max(0, bagsCap[name] - #rows), bagsCap[name])
	else
		note = string.format("|cff808080%d items|r", #rows)
	end
	if cells > BAG_SLOTS then
		note = note .. string.format("  |cff808080- showing %d-%d, scroll for more|r",
			first + 1, first + shown)
	end
	if pending > 0 then
		note = note .. string.format("  |cff606060(%d loading)|r", pending)
	end
	bagsFoot:SetText(note)
	return pending
end

bagsPage:EnableMouseWheel(true)
bagsPage:SetScript("OnMouseWheel", function()
	if not shownBot or BagCellCount(shownBot) <= BAG_SLOTS then
		return
	end
	bagScrollRow = bagScrollRow - arg1      -- wheel up is +1, and up means earlier rows
	DrawBags(shownBot)
end)

-- Repaint while the item cache is still filling.
--
-- 3.3.5 has no GET_ITEM_INFO_RECEIVED event - that arrives in a later expansion
-- - so there is nothing to listen for. Polling a few times a second for a few
-- seconds is what is left, and it stops the moment nothing is outstanding
-- rather than running forever.
local bagsTicker = CreateFrame("Frame")
local tickAccum, tickTries = 0, 0
bagsTicker:Hide()
bagsTicker:SetScript("OnUpdate", function()
	tickAccum = tickAccum + arg1
	if tickAccum < 0.4 then
		return
	end
	tickAccum = 0
	tickTries = tickTries + 1

	if not shownBot or detailTab ~= "bags" or tickTries > 20 then
		bagsTicker:Hide()
		return
	end
	if (DrawBags(shownBot) or 0) == 0 then
		bagsTicker:Hide()
	end
end)

local function RequestBags(name)
	bagsOf[name] = nil
	bagsDone[name] = nil
	bagsCap[name] = nil
	RunGmCommand(".botstats bags " .. name)
end

local function ShowBagsPage()
	detailTab = "bags"
	gearPage:Hide()
	talentPage:Hide()
	bot:SetHeight(BAGS_PAGE_H)
	bagsPage:Show()
	if shownBot then
		bagScrollRow = 0
		DrawBags(shownBot)
		tickAccum, tickTries = 0, 0
		bagsTicker:Show()
		pcall(RequestBags, shownBot)
	end
end

-- The other two pages are switched by functions defined above this point, and
-- their buttons captured those function VALUES - so rebinding the names here
-- would not affect the buttons. Hooking the frames is what actually holds.
gearPage:HookScript("OnShow", function() bagsPage:Hide() end)
talentPage:HookScript("OnShow", function() bagsPage:Hide() end)

local pageBags = MakeBotButton(bot, "Bags", 56, function()
	if shownBot then ShowBagsPage() end
end)
pageBags:SetPoint("LEFT", pageTalents, "RIGHT", 3, 0)

-- The row is laid out end to end, and Refresh is the tail of it. It could not
-- anchor to Bags where it is defined, because Bags does not exist until here.
pageRefresh:SetPoint("LEFT", pageBags, "RIGHT", 3, 0)

------------------------------------------------------------------
-- role filter
------------------------------------------------------------------
-- A cycling button rather than a dropdown: there are seven states including
-- "everything", UIDropDownMenu in 3.3.5 is a lot of ceremony for that, and one
-- click to step is faster than two to pick.
local roleButton = CreateFrame("Button", nil, win, "UIPanelButtonTemplate")
roleButton:SetWidth(104)
roleButton:SetHeight(20)
-- Left of Back, not anchored to the window corner: Back already lives there
-- (BOTTOMRIGHT -22, 18) and anchoring both to the same corner put this straight
-- on top of it.
roleButton:SetPoint("RIGHT", back, "LEFT", -6, 0)

local function RoleButtonLabel()
	if roleFilter == nil then
		return "Role: all"
	end
	return "Role: " .. RoleName(roleFilter)
end

roleButton:SetText(RoleButtonLabel())
roleButton:SetScript("OnClick", function()
	roleCycleAt = roleCycleAt + 1
	if roleCycleAt > #ROLE_CYCLE then
		roleCycleAt = 1
	end

	-- Spelled out rather than `(pick == "all") and nil or pick`, which does not
	-- work: `and nil` yields nil, nil is falsy, so `or pick` always wins and the
	-- "everything" entry set the filter to the STRING "all". That never equalled
	-- a numeric role, so the list came back empty and could not be cleared - and
	-- RoleName did not recognise it either, so the button read "Local" a second
	-- time instead of "all".
	local pick = ROLE_CYCLE[roleCycleAt]
	if pick == "all" then
		roleFilter = nil
	else
		roleFilter = pick
	end
	roleButton:SetText(RoleButtonLabel())

	-- Filtering by role only means anything on the bot list, so land there -
	-- and drop the zone drill-in, or "Drifters" would silently mean "drifters
	-- in Tanaris" and read as an empty fleet.
	activeTab = "bots"
	zoneFilter = nil
	scrollOffset = 0
	CENTURION_BotStats_Refresh()
end)

roleButton:SetScript("OnEnter", function()
	GameTooltip:SetOwner(roleButton, "ANCHOR_TOPLEFT")
	GameTooltip:AddLine("Filter the bot list by what a bot IS")
	GameTooltip:AddLine("Guardian, Veteran, PvP, Drifter, Companion, Local", 0.8, 0.8, 0.8, true)
	GameTooltip:Show()
end)
roleButton:SetScript("OnLeave", function() GameTooltip:Hide() end)

------------------------------------------------------------------
-- minimap button
------------------------------------------------------------------
-- Draggable around the rim, like TrinketMenu's. Pinning it to a fixed angle is
-- what makes these things a nuisance - the default spot is already crowded on
-- most setups - so it remembers where it was put.
CENTURION_BotStatsMinimapAngle = CENTURION_BotStatsMinimapAngle or 200

local minimapButton = CreateFrame("Button", "CENTURION_BotStatsMinimapButton", Minimap)
minimapButton:SetWidth(31)
minimapButton:SetHeight(31)
minimapButton:SetFrameStrata("MEDIUM")
minimapButton:SetMovable(true)
minimapButton:RegisterForClicks("LeftButtonUp", "RightButtonUp")

local mmIcon = minimapButton:CreateTexture(nil, "BACKGROUND")
mmIcon:SetWidth(20)
mmIcon:SetHeight(20)
mmIcon:SetTexture("Interface\\Icons\\INV_Misc_GroupLooking")
mmIcon:SetPoint("TOPLEFT", minimapButton, "TOPLEFT", 6, -5)

local mmBorder = minimapButton:CreateTexture(nil, "OVERLAY")
mmBorder:SetWidth(53)
mmBorder:SetHeight(53)
mmBorder:SetTexture("Interface\\Minimap\\MiniMap-TrackingBorder")
mmBorder:SetPoint("TOPLEFT", minimapButton, "TOPLEFT", 0, 0)

-- 3.3.5's global cos/sin take DEGREES, unlike math.cos/math.sin. Using the
-- math library with an explicit conversion says which is meant instead of
-- relying on remembering that.
local function PlaceMinimapButton()
	local angle = math.rad(CENTURION_BotStatsMinimapAngle or 200)
	minimapButton:SetPoint("CENTER", Minimap, "CENTER",
		80 * math.cos(angle), 80 * math.sin(angle))
end
PlaceMinimapButton()

minimapButton:SetScript("OnDragStart", function()
	minimapButton:SetScript("OnUpdate", function()
		local mx, my = Minimap:GetCenter()
		local cx, cy = GetCursorPosition()
		local scale = Minimap:GetEffectiveScale()
		cx, cy = cx / scale, cy / scale
		CENTURION_BotStatsMinimapAngle = math.deg(math.atan2(cy - my, cx - mx))
		PlaceMinimapButton()
	end)
end)
minimapButton:SetScript("OnDragStop", function()
	minimapButton:SetScript("OnUpdate", nil)
end)
minimapButton:RegisterForDrag("LeftButton")

minimapButton:SetScript("OnClick", function()
	if win:IsShown() then
		win:Hide()
	else
		win:Show()
		CENTURION_BotStats_Refresh()
	end
end)

minimapButton:SetScript("OnEnter", function()
	GameTooltip:SetOwner(minimapButton, "ANCHOR_LEFT")
	GameTooltip:AddLine("Centurion Bot Stats")
	GameTooltip:AddLine("Click to open, drag to move", 0.8, 0.8, 0.8)
	GameTooltip:Show()
end)
minimapButton:SetScript("OnLeave", function() GameTooltip:Hide() end)
