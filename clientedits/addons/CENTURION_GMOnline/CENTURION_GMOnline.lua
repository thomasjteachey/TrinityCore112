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
-- The feed is accepted only as a WHISPER whose sender is this character. The
-- server sends it that way, and nobody else can, so another player cannot plant
-- rows in the list. A reply is assembled on the side and only swapped in at
-- GMOE, so a list that is still arriving never replaces a complete one.

local PANEL_W, PANEL_H = 640, 500
local ROW_H = 16
local MAX_ROWS = 20
local AUTO_SECONDS = 15
local NO_REPLY_SECONDS = 10
local ECHO = "GMO1"         -- four characters, echoed back by the command channel

local FLAG_BOT, FLAG_GM, FLAG_BG, FLAG_DEAD = 1, 2, 4, 8

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

MakeCheck("Show bots", 20, function() return DB().showBots end, function(v)
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
refresh:SetScript("OnClick", function() Ask() end)

-- Column layout: x offset and width inside a row.
local COLUMNS = {
	{ key = "name",    x = 0,   w = 130, title = "Name" },
	{ key = "level",   x = 132, w = 30,  title = "Lvl" },
	{ key = "race",    x = 164, w = 70,  title = "Race" },
	{ key = "zone",    x = 236, w = 170, title = "Zone" },
	{ key = "account", x = 408, w = 110, title = "Account" },
	{ key = "tags",    x = 520, w = 76,  title = "" },
}

-- Twenty rows from here end at -462, clear of the footer at the bottom of a
-- 500-high window.
local LIST_TOP = -128
for _, col in ipairs(COLUMNS) do
	local fs = win:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
	fs:SetPoint("TOPLEFT", win, "TOPLEFT", 24 + col.x, LIST_TOP)
	fs:SetWidth(col.w)
	fs:SetJustifyH("LEFT")
	fs:SetText(col.title)
end

local rows = {}
local display = {}      -- flattened list the rows draw from

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
	GameTooltip:AddLine("Account: " .. (p.account ~= "" and p.account or "?"), 0.8, 0.8, 0.8)
	GameTooltip:AddLine(entry.realmName .. (entry.live and "" or "  (as of its last save)"), 0.8, 0.7, 0.3)
	if bit.band(p.flags, FLAG_BOT) > 0 then GameTooltip:AddLine("Playerbot", 0.6, 0.6, 0.6) end
	if bit.band(p.flags, FLAG_GM) > 0 then GameTooltip:AddLine("GM account", 1, 0.5, 0.2) end
	if bit.band(p.flags, FLAG_BG) > 0 then GameTooltip:AddLine("In a battleground or arena", 1, 0.8, 0.2) end
	if bit.band(p.flags, FLAG_DEAD) > 0 then GameTooltip:AddLine("Dead", 1, 0.3, 0.3) end
	if entry.live and bit.band(p.flags, FLAG_BOT) == 0 then
		GameTooltip:AddLine("Click to whisper", 0.5, 1, 0.5)
	end
	GameTooltip:Show()
end

for i = 1, MAX_ROWS do
	local row = CreateFrame("Button", nil, win)
	row:SetHeight(ROW_H)
	row:SetPoint("TOPLEFT", win, "TOPLEFT", 22, LIST_TOP - 14 - (i - 1) * ROW_H)
	row:SetPoint("RIGHT", win, "RIGHT", -22, 0)
	row:SetHighlightTexture("Interface\\QuestFrame\\UI-QuestTitleHighlight", "ADD")
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
	row:SetScript("OnClick", function(self)
		local entry = self.entry
		-- Only somebody on THIS realm can be whispered; a name on another realm
		-- would whisper nobody, or the wrong person.
		if entry and entry.kind == "player" and entry.live and bit.band(entry.player.flags, FLAG_BOT) == 0 then
			ChatFrame_SendTell(entry.player.name)
		end
	end)
	rows[i] = row
end

local footer = win:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
footer:SetPoint("BOTTOMLEFT", win, "BOTTOMLEFT", 24, 18)
footer:SetText("/gmo  -  wheel to scroll  -  hover for details  -  click a name on this realm to whisper")

win:EnableMouseWheel(true)
win:SetScript("OnMouseWheel", function(self, delta)
	scrollOffset = scrollOffset - (delta or 0) * 3
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
			})
		end
	end
end

function CENTURION_GMOnline_Refresh()
	if not win:IsShown() then
		return
	end

	-- Summary lines.
	for i = 1, #summaries do
		summaries[i]:SetText("")
	end
	if data then
		for i, realmId in ipairs(data.order) do
			if summaries[i] then
				local realm = data.realms[realmId]
				summaries[i]:SetText(string.format("|cffffd200%s|r  %s, %s%s", realm.name,
					Plural(realm.people, "person", "people"), Plural(realm.bots, "bot", "bots"),
					realm.live and "  |cff80ff80live|r" or "  |cff999999last save|r"))
			end
		end
	end

	-- Status.
	local now = GetTime()
	if unavailable then
		status:SetText("|cffff5050Not available - needs a GM account, on a realm built with .gmonline.|r")
	elseif noReply then
		status:SetText("|cffff5050No reply - is this a GM account?|r")
	elseif answered and lastAsked > lastReply and now - lastAsked > NO_REPLY_SECONDS then
		status:SetText("no reply, retrying")
	elseif data then
		status:SetText(string.format("updated %ds ago", math.floor(now - lastReply)))
	else
		status:SetText(serverNote or "asking...")
	end

	BuildDisplay()
	local maxOffset = math.max(0, #display - MAX_ROWS)
	if scrollOffset > maxOffset then scrollOffset = maxOffset end
	if scrollOffset < 0 then scrollOffset = 0 end

	for i = 1, MAX_ROWS do
		local row = rows[i]
		local entry = display[i + scrollOffset]
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
			elseif entry.kind == "empty" then
				row.cells.name:SetText("|cff808080nobody|r")
			else
				local p = entry.player
				local isBot = bit.band(p.flags, FLAG_BOT) > 0
				local dim = isBot and "|cff808080" or ""
				local dimEnd = isBot and "|r" or ""
				row.cells.name:SetText(isBot and ("|cff808080" .. p.name .. "|r")
					or ("|c" .. ClassColor(p.class) .. p.name .. "|r"))
				row.cells.level:SetText(dim .. p.level .. dimEnd)
				row.cells.race:SetText(dim .. (RACE_NAME[p.race] or "?") .. dimEnd)
				row.cells.zone:SetText(dim .. entry.zoneName .. dimEnd)
				row.cells.account:SetText(dim .. p.account .. dimEnd)

				local tags = ""
				if bit.band(p.flags, FLAG_GM) > 0 then tags = tags .. "|cffff8030GM|r " end
				if bit.band(p.flags, FLAG_BG) > 0 then tags = tags .. "|cffffd200BG|r " end
				if bit.band(p.flags, FLAG_DEAD) > 0 then tags = tags .. "|cffff5050dead|r" end
				row.cells.tags:SetText(tags)
			end
		end
	end
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
		if win:IsShown() and answered then
			Ask()
		end
		return
	end

	if event ~= "CHAT_MSG_ADDON" or type(message) ~= "string" then
		return
	end

	-- The core's command channel: "a"/"o" ack and ok, "m" a message, "f" failed.
	if prefix == "TrinityCore" then
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

	local tag, payload = string.match(message, "^(GMO%u):(.*)$")
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
	if not answered and lastAsked > lastReply and now - lastAsked > NO_REPLY_SECONDS then
		noReply = true
	end

	if DB().auto and not noReply and not unavailable and now - lastAsked >= AUTO_SECONDS then
		Ask()
	end

	CENTURION_GMOnline_Refresh()
end)

SLASH_CENTURIONGMONLINE1 = "/gmo"
SLASH_CENTURIONGMONLINE2 = "/gmonline"
SlashCmdList["CENTURIONGMONLINE"] = function()
	if win:IsShown() then
		win:Hide()
	else
		win:Show()
		Ask()
		CENTURION_GMOnline_Refresh()
	end
end
