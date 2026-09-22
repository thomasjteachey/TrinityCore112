-- Who is online on every realm, for whoever is running them.
--
-- The realm you are standing on answers from its live session list; the other
-- realms answer from their own characters database (as fresh as their last
-- character save), which this realm can read because all of them share one
-- MySQL server. None of that is visible to the client, so the server does the
-- work and this window only draws.
--
-- Asked for with the GM command "gmonline addon [bots]", sent over the core's
-- own addon command channel ("TrinityCore" prefix, opcode i, a four character
-- echo, then the command). That channel runs the same RBAC-gated command as
-- typing it, never shows or broadcasts anything, and answers "f<echo>" when
-- the account may not run it - so a non-GM gets a quiet "not available" here
-- rather than saying ".gmonline" out loud, which a dot-command SAY from a
-- player account does in this fork.
--
-- Answered over the CCGAME addon whisper the other Centurion windows use,
-- addressed from this character to itself:
--
--   GMOB  begin a reply            <seq>|<withBots>
--   GMOR  one realm's header       <realmId>|<name>|<people>|<bots>|<live>
--   GMOZ  zone names               <zoneId>,<name>;...
--   GMOP  character rows           <realmId>|<name>,<lvl>,<class>,<race>,<zone>,<flags>,<account>;...
--   GMOE  end of the reply         <seq>
--
-- The window has a second page for the other half of the same question: who
-- ELSE is this person. "gmonline addon alts <who>" takes an account name or
-- any character name and answers with every character that account owns, on
-- every realm, offline and deleted ones included - all of it from the
-- databases, so nobody has to be logged in for it to answer:
--
--   GMAB  begin an account reply   <seq>|<asked>|<found>
--   GMAI  the account itself       <id>|<account>|<security>|<online>|<banned>|<muted>|<lastLoginAgo>|<joinedAgo>|<lastIp>
--   GMAR  one realm's header       <realmId>|<name>|<characters>|<live>
--   GMAZ  zone names               <zoneId>,<name>;...
--   GMAC  character rows           <realmId>|<name>,<lvl>,<class>,<race>,<zone>,<flags>,<idleSecs>,<gold>,<hours>;...
--   GMAE  end of the reply         <seq>
--
-- The third page, Server, is the realm's world tick as the playerbot resource
-- governor sees it: "tick addon" answers on the command channel itself, one
-- "m" line per record (a different echo, so the two never mix):
--
--   T1  world tick     last|avg10|p95_10|max10|ticks10|avg60|p95_60|max60|ticks60|span60s|maps|sessions|bgs|scripts
--   T2  governor       enabled|level|emaMs|softMs|hardMs|softMatchBotCap|maxTotalCustomMatchBots
--   T3  online         players|bots
--   TM  one map        mapId|instanceId|name|avg|max|last|players|bots   (slowest first)
--   TE  end of the reply
--
-- The world tick is the SLOWEST map, not the sum, so the map table is the
-- answer to "why is it slow".
--
-- Either feed is accepted only as a WHISPER whose sender is this character.
-- The server sends it that way, and nobody else can, so another player cannot
-- plant rows in the list. A reply is assembled on the side and only swapped in
-- at its end tag, so a list that is still arriving never replaces a complete
-- one - and the two pages assemble separately, so a background auto-refresh of
-- one never half-writes the other.

local PANEL_W, PANEL_H = 640, 500
local ROW_H = 16
local MAX_ROWS = 20
local AUTO_SECONDS = 15
local NO_REPLY_SECONDS = 10
local ECHO = "GMO1"         -- four characters, echoed back by the command channel
local TICK_ECHO = "GMT1"    -- the Server page's own echo
local TICK_AUTO_SECONDS = 3

local FLAG_BOT, FLAG_GM, FLAG_BG, FLAG_DEAD = 1, 2, 4, 8
-- Only an account listing carries these: an online listing is online by
-- definition and never holds a deleted character.
local FLAG_ONLINE, FLAG_DELETED = 16, 32

local CLASS_FILE = {
	[1] = "WARRIOR", [2] = "PALADIN", [3] = "HUNTER", [4] = "ROGUE", [5] = "PRIEST",
	[6] = "DEATHKNIGHT", [7] = "SHAMAN", [8] = "MAGE", [9] = "WARLOCK", [11] = "DRUID",
}
local CLASS_NAME = {
	[1] = "Warrior", [2] = "Paladin", [3] = "Hunter", [4] = "Rogue", [5] = "Priest",
	[6] = "Death Knight", [7] = "Shaman", [8] = "Mage", [9] = "Warlock", [11] = "Druid",
}
local RACE_NAME = {
	[1] = "Human", [2] = "Orc", [3] = "Dwarf", [4] = "Night Elf", [5] = "Undead",
	[6] = "Tauren", [7] = "Gnome", [8] = "Troll", [10] = "Blood Elf", [11] = "Draenei",
}

local data     = nil    -- the last complete reply
local incoming = nil    -- the reply still arriving
local lastReply = 0
local lastAsked = 0
local answered = false  -- a reply has arrived at least once this session
local unavailable = false -- the command channel said "failed": not a GM here
local noReply = false   -- asked, and never once answered
local serverNote = nil  -- text the command channel sent back, if any
local scrollOffset = 0

-- The account page. Its own reply, its own scroll position, so switching back
-- to the online list lands where it was left.
local mode = "online"   -- "online" or "account"
local account = nil     -- the last complete account reply
local incomingAccount = nil
local accountAsked = 0
local accountReply = 0
local accountName = ""  -- what was typed, echoed back with the reply
local accountScroll = 0

-- The Server page. Everything it shows comes in one short reply, so it has no
-- account-style "found" state - only answered, not available, or silent.
local tick = nil        -- the last complete tick reply
local incomingTick = nil
local tickAsked = 0
local tickReply = 0
local tickAnswered = false
local tickUnavailable = false
local tickScroll = 0

------------------------------------------------------------------
-- settings
------------------------------------------------------------------
local function DB()
	if type(CENTURION_GMOnlineDB) ~= "table" then
		CENTURION_GMOnlineDB = {}
	end
	if CENTURION_GMOnlineDB.showBots == nil then CENTURION_GMOnlineDB.showBots = false end
	if CENTURION_GMOnlineDB.auto == nil then CENTURION_GMOnlineDB.auto = true end
	return CENTURION_GMOnlineDB
end

------------------------------------------------------------------
-- asking
------------------------------------------------------------------
local function Ask()
	lastAsked = GetTime()
	noReply = false
	unavailable = false
	serverNote = nil
	SendAddonMessage("TrinityCore", "i" .. ECHO .. "gmonline addon" .. (DB().showBots and " bots" or ""),
		"WHISPER", UnitName("player"))
end

-- A name the command parser will not read as a keyword: an account name, a
-- first name, or a character's first and last name ("Elgrom Fernbloom" - the
-- server looks that up by the pair). Anything else is a typo that would quietly
-- turn into "list who is online" instead. Not always a string either: a slash
-- command hands over whatever it was given and the minimap button hands over
-- its own frame.
local function CleanName(text)
	if type(text) ~= "string" then
		return nil
	end
	local first, last = string.match(text, "^%s*(%S*)%s*(%S*)")
	first, last = first or "", last or ""
	if first == "" or first == "addon" or first == "bots" or first == "alts" or first == "account" then
		return nil
	end
	if last ~= "" then
		first = first .. " " .. last
	end
	return string.sub(first, 1, 32)
end

local function AskAccount(who)
	who = CleanName(who)
	if not who then
		return
	end
	accountName = who
	accountAsked = GetTime()
	noReply = false
	unavailable = false
	serverNote = nil
	SendAddonMessage("TrinityCore", "i" .. ECHO .. "gmonline addon alts " .. who, "WHISPER", UnitName("player"))
end

local function AskTick()
	tickAsked = GetTime()
	tickUnavailable = false
	SendAddonMessage("TrinityCore", "i" .. TICK_ECHO .. "tick addon", "WHISPER", UnitName("player"))
end

------------------------------------------------------------------
-- parsing
------------------------------------------------------------------
local function Begin(payload)
	local seq, bots = string.match(payload or "", "^(%d+)|(%d)")
	if not seq then
		return
	end
	incoming = { seq = seq, withBots = bots == "1", realms = {}, order = {}, zones = {}, rows = {} }
end

local function ParseRealm(payload)
	if not incoming then return end
	local id, name, people, bots, live = string.match(payload or "", "^(%d+)|([^|]*)|(%d+)|(%d+)|(%d)$")
	if not id then
		return
	end
	id = tonumber(id)
	incoming.realms[id] = {
		id = id,
		name = (name ~= "" and name) or ("Realm " .. id),
		people = tonumber(people) or 0,
		bots = tonumber(bots) or 0,
		live = live == "1",
	}
	table.insert(incoming.order, id)
end

local function ParseZones(payload)
	if not incoming then return end
	for id, name in string.gmatch(payload or "", "(%d+),([^;]*);") do
		incoming.zones[tonumber(id)] = name
	end
end

local function ParseRows(payload)
	if not incoming then return end
	local realmId, rows = string.match(payload or "", "^(%d+)|(.*)$")
	if not realmId then
		return
	end
	realmId = tonumber(realmId)
	for name, lvl, cls, race, zone, flags, account in
		string.gmatch(rows, "([^,;]+),(%d+),(%d+),(%d+),(%d+),(%d+),([^;]*);") do
		-- The server scrubs every pipe out of what it sends, so one here is not
		-- from the server; never hand a stray escape sequence to SetText.
		if not string.find(name, "|", 1, true) and not string.find(account, "|", 1, true) then
			table.insert(incoming.rows, {
				realm   = realmId,
				name    = name,
				level   = tonumber(lvl) or 0,
				class   = tonumber(cls) or 0,
				race    = tonumber(race) or 0,
				zone    = tonumber(zone) or 0,
				flags   = tonumber(flags) or 0,
				account = account,
			})
		end
	end
end

local function Commit(payload)
	if not incoming or payload ~= incoming.seq then
		return
	end
	data = incoming
	incoming = nil
	lastReply = GetTime()
	answered = true
	noReply = false
	unavailable = false
end

------------------------------------------------------------------
-- parsing: the account page
------------------------------------------------------------------
local function BeginAccount(payload)
	local seq, asked, found = string.match(payload or "", "^(%d+)|([^|]*)|(%d)$")
	if not seq then
		return
	end
	incomingAccount = {
		seq = seq, asked = asked, found = found == "1",
		info = nil, realms = {}, order = {}, zones = {}, rows = {},
	}
end

local function ParseAccountInfo(payload)
	if not incomingAccount then return end
	local id, name, security, online, banned, muted, seen, joined, ip =
		string.match(payload or "", "^(%d+)|([^|]*)|(%d+)|(%d)|(%d)|(%d)|(%d+)|(%d+)|([^|]*)$")
	if not id then
		return
	end
	incomingAccount.info = {
		id       = tonumber(id) or 0,
		name     = name,
		security = tonumber(security) or 0,
		online   = online == "1",
		banned   = banned == "1",
		muted    = muted == "1",
		seen     = tonumber(seen) or 0,
		joined   = tonumber(joined) or 0,
		ip       = ip,
	}
end

local function ParseAccountRealm(payload)
	if not incomingAccount then return end
	local id, name, count, live = string.match(payload or "", "^(%d+)|([^|]*)|(%d+)|(%d)$")
	if not id then
		return
	end
	id = tonumber(id)
	incomingAccount.realms[id] = {
		id = id,
		name = (name ~= "" and name) or ("Realm " .. id),
		count = tonumber(count) or 0,
		live = live == "1",
	}
	table.insert(incomingAccount.order, id)
end

local function ParseAccountZones(payload)
	if not incomingAccount then return end
	for id, name in string.gmatch(payload or "", "(%d+),([^;]*);") do
		incomingAccount.zones[tonumber(id)] = name
	end
end

local function ParseAccountRows(payload)
	if not incomingAccount then return end
	local realmId, rows = string.match(payload or "", "^(%d+)|(.*)$")
	if not realmId then
		return
	end
	realmId = tonumber(realmId)
	for name, lvl, cls, race, zone, flags, idle, gold, hours in
		string.gmatch(rows, "([^,;]+),(%d+),(%d+),(%d+),(%d+),(%d+),(%d+),(%d+),(%d+);") do
		-- The server scrubs every pipe out of what it sends, so one here is not
		-- from the server; never hand a stray escape sequence to SetText.
		if not string.find(name, "|", 1, true) then
			table.insert(incomingAccount.rows, {
				realm = realmId,
				name  = name,
				level = tonumber(lvl) or 0,
				class = tonumber(cls) or 0,
				race  = tonumber(race) or 0,
				zone  = tonumber(zone) or 0,
				flags = tonumber(flags) or 0,
				idle  = tonumber(idle) or 0,
				gold  = tonumber(gold) or 0,
				hours = tonumber(hours) or 0,
			})
		end
	end
end

local function CommitAccount(payload)
	if not incomingAccount or payload ~= incomingAccount.seq then
		return
	end
	account = incomingAccount
	incomingAccount = nil
	accountReply = GetTime()
	answered = true
	noReply = false
	unavailable = false
end

------------------------------------------------------------------
-- parsing: the Server page
------------------------------------------------------------------
local function Numbers(text)
	local list = {}
	for n in string.gmatch(text or "", "[^|]+") do
		table.insert(list, tonumber(n) or 0)
	end
	return list
end

-- One "m" line of a tick reply. T1 starts a reply and TE swaps it in, like the
-- other two pages, so a half-arrived reply never replaces a whole one.
local function ParseTickLine(line)
	local kind, rest = string.match(line or "", "^(T[123ME])|?(.*)$")
	if not kind then
		return false
	end

	if kind == "T1" then
		local n = Numbers(rest)
		incomingTick = {
			last = n[1] or 0, avg10 = n[2] or 0, p95_10 = n[3] or 0, max10 = n[4] or 0, ticks10 = n[5] or 0,
			avg60 = n[6] or 0, p95_60 = n[7] or 0, max60 = n[8] or 0, ticks60 = n[9] or 0, span60 = n[10] or 0,
			maps = n[11] or 0, sessions = n[12] or 0, battlegrounds = n[13] or 0, scripts = n[14] or 0,
			gov = nil, players = 0, bots = 0, mapRows = {},
		}
	elseif not incomingTick then
		return true
	elseif kind == "T2" then
		local n = Numbers(rest)
		incomingTick.gov = {
			enabled = n[1] == 1, level = n[2] or 0, ema = n[3] or 0,
			soft = n[4] or 110, hard = n[5] or 180, softCap = n[6] or 0, maxTotal = n[7] or 0,
		}
	elseif kind == "T3" then
		local n = Numbers(rest)
		incomingTick.players, incomingTick.bots = n[1] or 0, n[2] or 0
	elseif kind == "TM" then
		local id, instance, name, avg, max, last, players, bots =
			string.match(rest, "^(%d+)|(%d+)|([^|]*)|(%d+)|(%d+)|(%d+)|(%d+)|(%d+)$")
		if id then
			table.insert(incomingTick.mapRows, {
				id = tonumber(id), instance = tonumber(instance),
				name = (name ~= "" and name) or ("Map " .. id),
				avg = tonumber(avg), max = tonumber(max), last = tonumber(last),
				players = tonumber(players), bots = tonumber(bots),
			})
		end
	elseif kind == "TE" then
		tick = incomingTick
		incomingTick = nil
		tickReply = GetTime()
		tickAnswered = true
		tickUnavailable = false
	end
	return true
end

------------------------------------------------------------------
-- window
------------------------------------------------------------------
local win = CreateFrame("Frame", "CenturionGMOnlineFrame", UIParent)
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
win:SetFrameStrata("HIGH")
win:Hide()
tinsert(UISpecialFrames, "CenturionGMOnlineFrame")

local title = win:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
title:SetPoint("TOP", win, "TOP", 0, -16)
title:SetText("Online")

local closeButton = CreateFrame("Button", nil, win, "UIPanelCloseButton")
closeButton:SetPoint("TOPRIGHT", win, "TOPRIGHT", -8, -8)

-- Page tabs, top left: the online list (with its account page behind it) and
-- the Server page. The one you are on is greyed out.
local function MakeTab(label, x, onClick)
	local tab = CreateFrame("Button", nil, win, "UIPanelButtonTemplate")
	tab:SetWidth(64)
	tab:SetHeight(20)
	tab:SetPoint("TOPLEFT", win, "TOPLEFT", x, -14)
	tab:SetText(label)
	tab:SetScript("OnClick", onClick)
	return tab
end

local onlineTab = MakeTab("Online", 18, function() CENTURION_GMOnline_ShowOnline() end)
local serverTab = MakeTab("Server", 84, function() CENTURION_GMOnline_ShowServer() end)

-- One summary line per realm, up to four.
local summaries = {}
for i = 1, 4 do
	local fs = win:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
	fs:SetPoint("TOPLEFT", win, "TOPLEFT", 22, -42 - (i - 1) * 14)
	fs:SetJustifyH("LEFT")
	summaries[i] = fs
end

local status = win:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
status:SetPoint("TOPRIGHT", win, "TOPRIGHT", -34, -44)
status:SetJustifyH("RIGHT")

local function MakeCheck(label, x, getter, setter)
	local check = CreateFrame("CheckButton", nil, win, "UICheckButtonTemplate")
	check:SetWidth(22)
	check:SetHeight(22)
	check:SetPoint("TOPLEFT", win, "TOPLEFT", x, -100)
	local text = check:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
	text:SetPoint("LEFT", check, "RIGHT", 2, 0)
	text:SetText(label)
	check:SetScript("OnShow", function(self) self:SetChecked(getter()) end)
	check:SetScript("OnClick", function(self) setter(self:GetChecked() and true or false) end)
	return check
end

local showBotsCheck = MakeCheck("Show bots", 20, function() return DB().showBots end, function(v)
	DB().showBots = v
	scrollOffset = 0
	Ask()
end)
MakeCheck("Auto refresh", 120, function() return DB().auto end, function(v) DB().auto = v end)

local refresh = CreateFrame("Button", nil, win, "UIPanelButtonTemplate")
refresh:SetWidth(80)
refresh:SetHeight(20)
refresh:SetPoint("TOPRIGHT", win, "TOPRIGHT", -24, -100)
refresh:SetText("Refresh")
refresh:SetScript("OnClick", function()
	if mode == "tick" then AskTick() else Ask() end
end)

-- Type an account or a character name here and the window turns into that
-- account's roster. A character name is enough - the server resolves it.
local search = CreateFrame("EditBox", "CenturionGMOnlineSearch", win, "InputBoxTemplate")
search:SetWidth(150)
search:SetHeight(20)
search:SetPoint("TOPRIGHT", win, "TOPRIGHT", -116, -100)
search:SetAutoFocus(false)
search:SetMaxLetters(32)
search:SetScript("OnEnterPressed", function(self)
	local who = self:GetText()
	self:ClearFocus()
	CENTURION_GMOnline_ShowAccount(who)
end)
search:SetScript("OnEscapePressed", function(self) self:ClearFocus() end)

local searchLabel = win:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
searchLabel:SetPoint("RIGHT", search, "LEFT", -4, 0)
searchLabel:SetText("Look up")

-- Only on the account page, and where Refresh sits on the online one.
local back = CreateFrame("Button", nil, win, "UIPanelButtonTemplate")
back:SetWidth(80)
back:SetHeight(20)
back:SetPoint("TOPRIGHT", win, "TOPRIGHT", -24, -100)
back:SetText("Back")
back:Hide()
back:SetScript("OnClick", function() CENTURION_GMOnline_ShowOnline() end)

-- Column layout: x offset and width inside a row. The fifth column carries the
-- account name on the online page and, on the account page - where every row
-- is the same account - when that character was last seen instead.
local COLUMNS = {
	-- Wide enough for a first AND last name; the zone gave up the room.
	{ key = "name",    x = 0,   w = 150, title = "Name" },
	{ key = "level",   x = 152, w = 30,  title = "Lvl" },
	{ key = "race",    x = 184, w = 70,  title = "Race" },
	{ key = "zone",    x = 256, w = 150, title = "Zone" },
	{ key = "account", x = 408, w = 110, title = "Account", altTitle = "Last seen" },
	{ key = "tags",    x = 520, w = 76,  title = "" },
}

-- The Server page reuses the same cells for its map table.
local TICK_TITLES = {
	name = "Map", level = "Avg", race = "Max", zone = "Players / bots", account = "Last", tags = "",
}

-- Twenty rows from here end at -462, clear of the footer at the bottom of a
-- 500-high window.
local LIST_TOP = -128
local headers = {}
local headerCol = {}
for _, col in ipairs(COLUMNS) do
	local fs = win:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
	fs:SetPoint("TOPLEFT", win, "TOPLEFT", 24 + col.x, LIST_TOP)
	fs:SetWidth(col.w)
	fs:SetJustifyH("LEFT")
	fs:SetText(col.title)
	headers[col.key] = fs
	headerCol[col.key] = col
end

local rows = {}
local display = {}      -- flattened list the rows draw from

-- Ages arrive as plain seconds - the client has no idea what the server thinks
-- the time is, so the server sends the difference and this only reads it out.
local function FormatAge(seconds)
	seconds = tonumber(seconds) or 0
	if seconds <= 0 then return "just now" end
	if seconds < 60 then return seconds .. "s ago" end
	if seconds < 3600 then return math.floor(seconds / 60) .. "m ago" end
	if seconds < 86400 then return math.floor(seconds / 3600) .. "h ago" end
	return math.floor(seconds / 86400) .. "d ago"
end

local function ShowTooltip(row)
	local entry = row.entry
	if not entry or entry.kind ~= "player" then
		return
	end
	local p = entry.player
	GameTooltip:SetOwner(row, "ANCHOR_RIGHT")
	GameTooltip:AddLine(p.name, 1, 1, 1)
	GameTooltip:AddLine(string.format("Level %d %s %s", p.level, RACE_NAME[p.race] or "?", CLASS_NAME[p.class] or "?"), 0.9, 0.9, 0.9)
	GameTooltip:AddLine(entry.zoneName, 0.6, 0.8, 1)
	if entry.alt then
		if bit.band(p.flags, FLAG_ONLINE) > 0 then
			GameTooltip:AddLine("Online now", 0.5, 1, 0.5)
		else
			GameTooltip:AddLine("Last seen " .. (p.idle > 0 and FormatAge(p.idle) or "never"), 0.8, 0.8, 0.8)
		end
		GameTooltip:AddDoubleLine("Played", p.hours .. "h", 0.8, 0.8, 0.8, 1, 1, 1)
		GameTooltip:AddDoubleLine("Gold", tostring(p.gold), 0.8, 0.8, 0.8, 1, 0.82, 0)
	else
		GameTooltip:AddLine("Account: " .. (p.account ~= "" and p.account or "?"), 0.8, 0.8, 0.8)
	end
	GameTooltip:AddLine(entry.realmName .. (entry.live and "" or "  (as of its last save)"), 0.8, 0.7, 0.3)
	if bit.band(p.flags, FLAG_BOT) > 0 then GameTooltip:AddLine("Playerbot", 0.6, 0.6, 0.6) end
	if bit.band(p.flags, FLAG_GM) > 0 then GameTooltip:AddLine("GM account", 1, 0.5, 0.2) end
	if bit.band(p.flags, FLAG_BG) > 0 then GameTooltip:AddLine("In a battleground or arena", 1, 0.8, 0.2) end
	if bit.band(p.flags, FLAG_DEAD) > 0 then GameTooltip:AddLine("Dead", 1, 0.3, 0.3) end
	if bit.band(p.flags, FLAG_DELETED) > 0 then GameTooltip:AddLine("Deleted character", 1, 0.3, 0.3) end
	if entry.whisperable then
		GameTooltip:AddLine("Click to whisper", 0.5, 1, 0.5)
	end
	if not entry.alt then
		GameTooltip:AddLine("Right-click for this account's characters", 0.5, 0.8, 1)
	end
	GameTooltip:Show()
end

for i = 1, MAX_ROWS do
	local row = CreateFrame("Button", nil, win)
	row:SetHeight(ROW_H)
	row:SetPoint("TOPLEFT", win, "TOPLEFT", 22, LIST_TOP - 14 - (i - 1) * ROW_H)
	row:SetPoint("RIGHT", win, "RIGHT", -22, 0)
	row:SetHighlightTexture("Interface\\QuestFrame\\UI-QuestTitleHighlight", "ADD")
	row:RegisterForClicks("LeftButtonUp", "RightButtonUp")
	row.cells = {}
	for _, col in ipairs(COLUMNS) do
		local fs = row:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
		fs:SetPoint("LEFT", row, "LEFT", 2 + col.x, 0)
		fs:SetWidth(col.w)
		fs:SetJustifyH("LEFT")
		row.cells[col.key] = fs
	end
	row:SetScript("OnEnter", ShowTooltip)
	row:SetScript("OnLeave", function() GameTooltip:Hide() end)
	row:SetScript("OnClick", function(self, button)
		local entry = self.entry
		if not entry or entry.kind ~= "player" then
			return
		end
		-- Right-click asks who else that account plays. On the online page the
		-- account name is right there in the row; on the account page every row
		-- is already the same account, so there is nothing to ask.
		if button == "RightButton" then
			if not entry.alt and entry.player.account ~= "" then
				CENTURION_GMOnline_ShowAccount(entry.player.account)
			end
			return
		end
		-- Only somebody on THIS realm can be whispered; a name on another realm
		-- would whisper nobody, or the wrong person.
		if entry.whisperable then
			ChatFrame_SendTell(entry.player.name)
		end
	end)
	rows[i] = row
end

local footer = win:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
footer:SetPoint("BOTTOMLEFT", win, "BOTTOMLEFT", 24, 18)
footer:SetText("/gmo [name]  -  wheel to scroll  -  right-click a row for that account's characters")

win:EnableMouseWheel(true)
win:SetScript("OnMouseWheel", function(self, delta)
	local step = (delta or 0) * 3
	if mode == "tick" then
		tickScroll = tickScroll - step
	elseif mode == "account" then
		accountScroll = accountScroll - step
	else
		scrollOffset = scrollOffset - step
	end
	CENTURION_GMOnline_Refresh()
end)

------------------------------------------------------------------
-- drawing
------------------------------------------------------------------
local function ClassColor(class)
	local color = RAID_CLASS_COLORS and RAID_CLASS_COLORS[CLASS_FILE[class] or ""]
	if not color then
		return "ffffffff"
	end
	return string.format("ff%02x%02x%02x", color.r * 255, color.g * 255, color.b * 255)
end

local function Plural(n, one, many)
	return n .. " " .. (n == 1 and one or many)
end

local function BuildDisplay()
	display = {}
	if not data then
		return
	end

	for _, realmId in ipairs(data.order) do
		local realm = data.realms[realmId]
		table.insert(display, { kind = "realm", realm = realm })

		local list = {}
		for _, p in ipairs(data.rows) do
			if p.realm == realmId then
				table.insert(list, p)
			end
		end
		-- People first, then bots; each by name.
		table.sort(list, function(a, b)
			local aBot = bit.band(a.flags, FLAG_BOT) > 0
			local bBot = bit.band(b.flags, FLAG_BOT) > 0
			if aBot ~= bBot then
				return not aBot
			end
			return a.name < b.name
		end)

		if #list == 0 then
			table.insert(display, { kind = "empty" })
		end
		for _, p in ipairs(list) do
			table.insert(display, {
				kind = "player",
				player = p,
				live = realm.live,
				realmName = realm.name,
				zoneName = data.zones[p.zone] or ("Zone " .. p.zone),
				whisperable = realm.live and bit.band(p.flags, FLAG_BOT) == 0,
			})
		end
	end
end

-- The same flattened list for the account page. The server has already put the
-- rows in order, so this only groups them under their realm headers.
local function BuildAccountDisplay()
	display = {}
	if not account or not account.found then
		return
	end

	for _, realmId in ipairs(account.order) do
		local realm = account.realms[realmId]
		table.insert(display, { kind = "realm", realm = realm, alt = true })

		local any = false
		for _, p in ipairs(account.rows) do
			if p.realm == realmId then
				any = true
				table.insert(display, {
					kind = "player",
					player = p,
					alt = true,
					live = realm.live,
					realmName = realm.name,
					zoneName = account.zones[p.zone] or ("Zone " .. p.zone),
					whisperable = realm.live and bit.band(p.flags, FLAG_ONLINE) > 0
						and bit.band(p.flags, FLAG_BOT) == 0,
				})
			end
		end
		if not any then
			table.insert(display, { kind = "empty", alt = true })
		end
	end
end

-- The Server page's map table, slowest first as the server sent it.
local function BuildTickDisplay()
	display = {}
	if not tick then
		return
	end
	for _, m in ipairs(tick.mapRows) do
		table.insert(display, { kind = "map", map = m })
	end
	if #display == 0 then
		table.insert(display, { kind = "empty" })
	end
end

-- Green well under the governor's soft line, yellow approaching it, orange
-- past it (matches capped), red past hard (no bots added at all).
local function TickColor(ms)
	local soft = tick and tick.gov and tick.gov.soft or 110
	local hard = tick and tick.gov and tick.gov.hard or 180
	if ms >= hard then return "|cffff4040" end
	if ms >= soft then return "|cffff8030" end
	if ms >= soft * 0.75 then return "|cffffd200" end
	return "|cff40ff40"
end

local function Ms(ms)
	return TickColor(ms) .. ms .. "|r"
end

local GOVERNOR_LEVEL = { [0] = "|cff40ff40NORMAL|r", [1] = "|cffff8030SOFT|r", [2] = "|cffff4040HARD|r" }

local function GovernorText(gov)
	if not gov then
		return ""
	end
	if not gov.enabled then
		return "Governor |cff999999off|r - bot adds are never throttled"
	end
	local text = string.format("Governor %s (avg %d ms)  ", GOVERNOR_LEVEL[gov.level] or "?", gov.ema)
	if gov.level == 2 then
		return text .. "no new bots; if it holds, bots are shed"
	elseif gov.level == 1 then
		return text .. string.format("each match capped at %d bots", gov.softCap)
	end
	return text .. string.format("|cff999999soft %d ms caps a match at %d bots, hard %d ms stops bot adds|r",
		gov.soft, gov.softCap, gov.hard)
end

-- The four lines under the title: one per realm on the online page, the
-- account itself on the account page, the world tick on the Server page.
local function DrawSummaries()
	for i = 1, #summaries do
		summaries[i]:SetText("")
	end

	if mode == "tick" then
		if not tick then
			return
		end
		summaries[1]:SetText(string.format("|cffffd200World tick|r %s ms   %s, %s online", Ms(tick.last),
			Plural(tick.players, "player", "players"), Plural(tick.bots, "bot", "bots")))
		summaries[2]:SetText(string.format("10 s: avg %s, p95 %s, max %s      %d s: avg %s, p95 %s, max %s",
			Ms(tick.avg10), Ms(tick.p95_10), Ms(tick.max10), math.max(tick.span60, 1),
			Ms(tick.avg60), Ms(tick.p95_60), Ms(tick.max60)))
		summaries[3]:SetText(string.format("|cff999999work per tick: maps %d, sessions %d, battlegrounds %d, scripts %d ms|r",
			tick.maps, tick.sessions, tick.battlegrounds, tick.scripts))
		summaries[4]:SetText(GovernorText(tick.gov))
		return
	end

	if mode == "account" then
		if not account then
			return
		end
		if not account.found then
			summaries[1]:SetText(string.format("|cffff5050No account or character named '%s'.|r", account.asked))
			return
		end

		local info = account.info
		if not info then
			return
		end
		summaries[1]:SetText(string.format("|cffffd200%s|r  account %d, %s", info.name, info.id,
			Plural(#account.rows, "character", "characters")))

		local marks = ""
		if info.security > 0 then marks = marks .. string.format("  |cffff8030GM level %d|r", info.security) end
		if info.banned then marks = marks .. "  |cffff5050banned|r" end
		if info.muted then marks = marks .. "  |cffff8030muted|r" end
		if info.online then marks = marks .. "  |cff80ff80logged in|r" end
		summaries[2]:SetText(string.format("last login %s%s%s", info.seen > 0 and FormatAge(info.seen) or "never",
			info.ip ~= "" and ("  from " .. info.ip) or "", marks))

		if info.joined > 0 then
			summaries[3]:SetText(string.format("|cff999999created %s|r", FormatAge(info.joined)))
		end
		return
	end

	if not data then
		return
	end
	for i, realmId in ipairs(data.order) do
		if summaries[i] then
			local realm = data.realms[realmId]
			summaries[i]:SetText(string.format("|cffffd200%s|r  %s, %s%s", realm.name,
				Plural(realm.people, "person", "people"), Plural(realm.bots, "bot", "bots"),
				realm.live and "  |cff80ff80live|r" or "  |cff999999last save|r"))
		end
	end
end

local function DrawStatus()
	local now = GetTime()

	if mode == "tick" then
		if tickUnavailable then
			status:SetText("|cffff5050Not available - needs a GM account, on a realm built with .tick.|r")
		elseif tickAsked > tickReply and now - tickAsked > NO_REPLY_SECONDS then
			status:SetText(tickAnswered and "no reply, retrying" or "|cffff5050No reply|r")
		elseif tick then
			status:SetText(string.format("updated %ds ago", math.floor(now - tickReply)))
		else
			status:SetText("asking...")
		end
		return
	end

	local asked = mode == "account" and accountAsked or lastAsked
	local replied = mode == "account" and accountReply or lastReply
	local have = mode == "account" and account or data

	if unavailable then
		status:SetText("|cffff5050Not available - needs a GM account, on a realm built with .gmonline.|r")
	elseif noReply then
		status:SetText("|cffff5050No reply - is this a GM account?|r")
	elseif answered and asked > replied and now - asked > NO_REPLY_SECONDS then
		status:SetText("no reply, retrying")
	elseif have then
		status:SetText(string.format("updated %ds ago", math.floor(now - replied)))
	else
		status:SetText(serverNote or "asking...")
	end
end

function CENTURION_GMOnline_Refresh()
	if not win:IsShown() then
		return
	end

	local onAccount = mode == "account"
	local onTick = mode == "tick"
	if onTick then
		title:SetText("Server")
	else
		title:SetText(onAccount and ("Account: " .. (accountName ~= "" and accountName or "?")) or "Online")
	end
	for _, col in ipairs(COLUMNS) do
		if onTick then
			headers[col.key]:SetText(TICK_TITLES[col.key] or "")
		elseif col.key == "account" and onAccount then
			headers[col.key]:SetText(col.altTitle)
		else
			headers[col.key]:SetText(col.title)
		end
	end
	if onAccount then back:Show() else back:Hide() end
	if onAccount then refresh:Hide() else refresh:Show() end
	if onTick then showBotsCheck:Hide() else showBotsCheck:Show() end
	if onTick then serverTab:Disable() else serverTab:Enable() end
	if onTick then onlineTab:Enable() else onlineTab:Disable() end
	footer:SetText(onTick
		and "Colours: green fine, yellow nearing the governor's soft line, orange past it, red past hard"
		or "/gmp [name]  -  wheel to scroll  -  right-click a row for that account's characters")

	DrawSummaries()
	DrawStatus()

	if onTick then
		BuildTickDisplay()
	elseif onAccount then
		BuildAccountDisplay()
	else
		BuildDisplay()
	end

	local maxOffset = math.max(0, #display - MAX_ROWS)
	local offset = (onTick and tickScroll) or (onAccount and accountScroll) or scrollOffset
	if offset > maxOffset then offset = maxOffset end
	if offset < 0 then offset = 0 end
	if onTick then tickScroll = offset elseif onAccount then accountScroll = offset else scrollOffset = offset end

	for i = 1, MAX_ROWS do
		local row = rows[i]
		local entry = display[i + offset]
		row.entry = entry
		for _, fs in pairs(row.cells) do
			fs:SetText("")
		end

		if not entry then
			row:Hide()
		else
			row:Show()
			if entry.kind == "realm" then
				row.cells.name:SetText("|cffffd200" .. entry.realm.name .. "|r")
				if entry.alt and not entry.realm.live then
					row.cells.account:SetText("|cff999999last save|r")
				end
			elseif entry.kind == "map" then
				local m = entry.map
				row.cells.name:SetText(m.instance > 0 and (m.name .. " |cff808080#" .. m.instance .. "|r") or m.name)
				row.cells.level:SetText(Ms(m.avg))
				row.cells.race:SetText(Ms(m.max))
				row.cells.zone:SetText(string.format("%d / %d", m.players, m.bots))
				row.cells.account:SetText(Ms(m.last))
			elseif entry.kind == "empty" then
				row.cells.name:SetText((onTick and "|cff808080no map updates recorded|r")
					or (entry.alt and "|cff808080no characters|r") or "|cff808080nobody|r")
			else
				local p = entry.player
				-- A bot on the online page and a deleted character on the
				-- account page both read as "not really here": greyed out.
				local faded = bit.band(p.flags, FLAG_BOT) > 0 or bit.band(p.flags, FLAG_DELETED) > 0
				local dim = faded and "|cff808080" or ""
				local dimEnd = faded and "|r" or ""
				row.cells.name:SetText(faded and ("|cff808080" .. p.name .. "|r")
					or ("|c" .. ClassColor(p.class) .. p.name .. "|r"))
				row.cells.level:SetText(dim .. p.level .. dimEnd)
				row.cells.race:SetText(dim .. (RACE_NAME[p.race] or "?") .. dimEnd)
				row.cells.zone:SetText(dim .. entry.zoneName .. dimEnd)
				if entry.alt then
					if bit.band(p.flags, FLAG_ONLINE) > 0 then
						row.cells.account:SetText("|cff80ff80online|r")
					elseif p.idle > 0 then
						row.cells.account:SetText(dim .. FormatAge(p.idle) .. dimEnd)
					else
						row.cells.account:SetText("|cff808080never played|r")
					end
				else
					row.cells.account:SetText(dim .. p.account .. dimEnd)
				end

				local tags = ""
				if bit.band(p.flags, FLAG_GM) > 0 then tags = tags .. "|cffff8030GM|r " end
				if bit.band(p.flags, FLAG_BG) > 0 then tags = tags .. "|cffffd200BG|r " end
				if bit.band(p.flags, FLAG_DELETED) > 0 then tags = tags .. "|cffff5050del|r " end
				if bit.band(p.flags, FLAG_DEAD) > 0 then tags = tags .. "|cffff5050dead|r" end
				row.cells.tags:SetText(tags)
			end
		end
	end
end

-- Switching pages. Both ask straight away: an account page that is a minute
-- old is worse than a blank one, and the online page auto-refreshes anyway.
function CENTURION_GMOnline_ShowAccount(who)
	who = CleanName(who)
	if not who then
		return
	end
	if mode ~= "account" or accountName ~= who then
		accountScroll = 0
	end
	-- A different account than the one on screen: drop the old rows rather than
	-- showing them under the new name until the reply lands.
	if accountName ~= who then
		account = nil
		incomingAccount = nil
	end
	mode = "account"
	search:SetText(who)
	if not win:IsShown() then
		win:Show()
	end
	AskAccount(who)
	CENTURION_GMOnline_Refresh()
end

function CENTURION_GMOnline_ShowOnline()
	mode = "online"
	search:SetText("")
	search:ClearFocus()
	Ask()
	CENTURION_GMOnline_Refresh()
end

function CENTURION_GMOnline_ShowServer()
	mode = "tick"
	search:SetText("")
	search:ClearFocus()
	if not win:IsShown() then
		win:Show()
	end
	AskTick()
	CENTURION_GMOnline_Refresh()
end

------------------------------------------------------------------
-- events
------------------------------------------------------------------
local driver = CreateFrame("Frame")
driver:RegisterEvent("CHAT_MSG_ADDON")
driver:RegisterEvent("PLAYER_ENTERING_WORLD")
driver:SetScript("OnEvent", function(self, event, prefix, message, channel, sender)
	-- A request that went out just as a loading screen began is dropped by the
	-- server, so ask again the moment the world is back.
	if event == "PLAYER_ENTERING_WORLD" then
		if win:IsShown() and mode == "tick" then
			AskTick()
		elseif win:IsShown() and answered then
			if mode == "account" then
				AskAccount(accountName)
			else
				Ask()
			end
		end
		return
	end

	if event ~= "CHAT_MSG_ADDON" or type(message) ~= "string" then
		return
	end

	-- The core's command channel: "a"/"o" ack and ok, "m" a message, "f" failed.
	if prefix == "TrinityCore" then
		-- The command channel only ever answers this character, from itself.
		if string.sub(message, 2, 5) == TICK_ECHO and sender == UnitName("player") then
			local opcode = string.sub(message, 1, 1)
			if opcode == "f" then
				tickUnavailable = true
				incomingTick = nil
			elseif opcode == "m" then
				ParseTickLine(string.sub(message, 6))
			end
			if opcode == "f" or opcode == "o" then
				CENTURION_GMOnline_Refresh()
			end
			return
		end
		if string.sub(message, 2, 5) ~= ECHO then
			return
		end
		local opcode = string.sub(message, 1, 1)
		if opcode == "f" then
			unavailable = true
		elseif opcode == "m" then
			serverNote = string.sub(message, 6)
		end
		CENTURION_GMOnline_Refresh()
		return
	end

	if prefix ~= "CCGAME" or channel ~= "WHISPER" or sender ~= UnitName("player") then
		return
	end

	local tag, payload = string.match(message, "^(GM[OA]%u):(.*)$")
	if not tag then
		return
	end

	if tag == "GMOB" then
		Begin(payload)
	elseif tag == "GMOR" then
		ParseRealm(payload)
	elseif tag == "GMOZ" then
		ParseZones(payload)
	elseif tag == "GMOP" then
		ParseRows(payload)
	elseif tag == "GMOE" then
		Commit(payload)
		CENTURION_GMOnline_Refresh()
	elseif tag == "GMAB" then
		BeginAccount(payload)
	elseif tag == "GMAI" then
		ParseAccountInfo(payload)
	elseif tag == "GMAR" then
		ParseAccountRealm(payload)
	elseif tag == "GMAZ" then
		ParseAccountZones(payload)
	elseif tag == "GMAC" then
		ParseAccountRows(payload)
	elseif tag == "GMAE" then
		CommitAccount(payload)
		CENTURION_GMOnline_Refresh()
	end
end)

-- Auto refresh while the window is open, and a repaint every second so the
-- "updated Ns ago" line keeps moving. An account that has never been answered
-- is asked once and left alone; one that has been answered is simply asked
-- again next cycle when a reply goes missing (a request sent at the start of a
-- loading screen is dropped by the server).
local ticker = 0
driver:SetScript("OnUpdate", function(self, elapsed)
	ticker = ticker + (elapsed or 0)
	if ticker < 1 then
		return
	end
	ticker = 0

	if not win:IsShown() then
		return
	end

	local now = GetTime()

	-- The Server page asks every few seconds whatever happened last time: its
	-- whole point is watching the number move.
	if mode == "tick" then
		if DB().auto and not tickUnavailable and now - tickAsked >= TICK_AUTO_SECONDS then
			AskTick()
		end
		CENTURION_GMOnline_Refresh()
		return
	end

	local asked = mode == "account" and accountAsked or lastAsked
	local replied = mode == "account" and accountReply or lastReply
	if not answered and asked > replied and now - asked > NO_REPLY_SECONDS then
		noReply = true
	end

	if DB().auto and not noReply and not unavailable and now - asked >= AUTO_SECONDS then
		if mode ~= "account" then
			Ask()
		elseif accountName ~= "" then
			AskAccount(accountName)
		end
	end

	CENTURION_GMOnline_Refresh()
end)

local function ToggleWindow(arg)
	local word = type(arg) == "string" and string.lower(string.match(arg, "^%s*(%S*)") or "") or ""
	if word == "server" or word == "tick" then
		CENTURION_GMOnline_ShowServer()
		return
	end
	local who = CleanName(arg)
	if who then
		CENTURION_GMOnline_ShowAccount(who)
		return
	end
	if win:IsShown() then
		win:Hide()
	else
		win:Show()
		if mode == "tick" then
			AskTick()
		elseif mode == "account" then
			AskAccount(accountName)
		else
			Ask()
		end
		CENTURION_GMOnline_Refresh()
	end
end

SLASH_CENTURIONGMONLINE1 = "/gmp"
SLASH_CENTURIONGMONLINE2 = "/gmpanel"
SLASH_CENTURIONGMONLINE3 = "/gmo"
SLASH_CENTURIONGMONLINE4 = "/gmonline"
SlashCmdList["CENTURIONGMONLINE"] = ToggleWindow

SLASH_CENTURIONGMTICK1 = "/gmtick"
SlashCmdList["CENTURIONGMTICK"] = function() CENTURION_GMOnline_ShowServer() end

------------------------------------------------------------------
-- minimap button
------------------------------------------------------------------
-- The same button Bot Stats has: draggable around the rim, remembers where it
-- was put. It starts at a different angle so the two do not sit on each other.
--
-- Placed again once the saved variables are in. They load AFTER this file
-- runs, so a position read at load time is always the default - which is why
-- a button placed only then forgets where it was dragged on every login.
local DEFAULT_ANGLE = 235

local minimapButton = CreateFrame("Button", "CENTURION_GMOnlineMinimapButton", Minimap)
minimapButton:SetWidth(31)
minimapButton:SetHeight(31)
minimapButton:SetFrameStrata("MEDIUM")
minimapButton:SetMovable(true)
minimapButton:RegisterForClicks("LeftButtonUp", "RightButtonUp")
minimapButton:RegisterForDrag("LeftButton")

local mmIcon = minimapButton:CreateTexture(nil, "BACKGROUND")
mmIcon:SetWidth(20)
mmIcon:SetHeight(20)
mmIcon:SetTexture("Interface\\Icons\\INV_Misc_GroupNeedMore")
mmIcon:SetPoint("TOPLEFT", minimapButton, "TOPLEFT", 6, -5)

local mmBorder = minimapButton:CreateTexture(nil, "OVERLAY")
mmBorder:SetWidth(53)
mmBorder:SetHeight(53)
mmBorder:SetTexture("Interface\\Minimap\\MiniMap-TrackingBorder")
mmBorder:SetPoint("TOPLEFT", minimapButton, "TOPLEFT", 0, 0)

-- 3.3.5's global cos/sin take DEGREES; the math library with an explicit
-- conversion says which is meant.
local function PlaceMinimapButton()
	local angle = math.rad(DB().minimapAngle or DEFAULT_ANGLE)
	minimapButton:ClearAllPoints()
	minimapButton:SetPoint("CENTER", Minimap, "CENTER", 80 * math.cos(angle), 80 * math.sin(angle))
end
PlaceMinimapButton()

minimapButton:SetScript("OnDragStart", function(self)
	self:SetScript("OnUpdate", function()
		local mx, my = Minimap:GetCenter()
		local cx, cy = GetCursorPosition()
		local scale = Minimap:GetEffectiveScale()
		cx, cy = cx / scale, cy / scale
		DB().minimapAngle = math.deg(math.atan2(cy - my, cx - mx))
		PlaceMinimapButton()
	end)
end)
minimapButton:SetScript("OnDragStop", function(self)
	self:SetScript("OnUpdate", nil)
end)

minimapButton:SetScript("OnClick", ToggleWindow)

-- The counts from the last reply, so a hover answers "is anybody on" without
-- opening the window.
minimapButton:SetScript("OnEnter", function(self)
	GameTooltip:SetOwner(self, "ANCHOR_LEFT")
	GameTooltip:AddLine("GM Panel")
	if tick then
		GameTooltip:AddDoubleLine("World tick", string.format("%s ms (10 s avg %s)", Ms(tick.last), Ms(tick.avg10)),
			1, 0.82, 0, 1, 1, 1)
	end
	if data then
		for _, realmId in ipairs(data.order) do
			local realm = data.realms[realmId]
			GameTooltip:AddDoubleLine(realm.name, string.format("%s, %s",
				Plural(realm.people, "person", "people"), Plural(realm.bots, "bot", "bots")),
				1, 0.82, 0, 1, 1, 1)
		end
	end
	GameTooltip:AddLine("Click to open, drag to move", 0.8, 0.8, 0.8)
	GameTooltip:Show()
end)
minimapButton:SetScript("OnLeave", function() GameTooltip:Hide() end)

local loader = CreateFrame("Frame")
loader:RegisterEvent("ADDON_LOADED")
loader:SetScript("OnEvent", function(self, event, name)
	if name == "CENTURION_GMOnline" then
		PlaceMinimapButton()
		self:UnregisterEvent("ADDON_LOADED")
	end
end)
