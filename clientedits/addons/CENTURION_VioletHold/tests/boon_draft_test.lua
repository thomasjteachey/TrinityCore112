-- lua_run.py boon_draft_test.lua ADDON=<path>/BoonDraft.lua
-- Stubs the 3.3.5 UI just far enough to load BoonDraft.lua, deal it the VHBOON
-- lines the server sends (violet_hold_boons.cpp) and check what it does.
local pass, fail, log = 0, 0, {};
local function check(ok, what) if ok then pass = pass + 1 else fail = fail + 1; log[#log + 1] = what end end

local now = 100;
function GetTime() return now end

local frames, named = {}, {};
local noop = function() end;
local function Object(kind)
	local o = { _kind = kind, shown = (kind ~= "Frame" and kind ~= "Button") or nil, scripts = {}, events = {}, enabled = true, alpha = 1 };
	local m = {};
	function m:Show() self.shown = true; if self.scripts.OnShow and not self.wasShown then self.wasShown = true; self.scripts.OnShow(self) end end
	function m:Hide() local was = self.shown; self.shown = false; self.wasShown = false; if was and self.scripts.OnHide then self.scripts.OnHide(self) end end
	function m:IsShown() return self.shown end
	function m:SetScript(k, fn) self.scripts[k] = fn end
	function m:RegisterEvent(e) self.events[e] = true end
	function m:SetText(v) self.text = v end
	function m:GetText() return self.text end
	function m:SetTextColor(r, g, b) self.color = { r, g, b } end
	function m:Enable() self.enabled = true end
	function m:Disable() self.enabled = false end
	function m:SetAlpha(a) self.alpha = a end
	function m:GetWidth() return self.w or 100 end
	function m:SetWidth(w) self.w = w end
	function m:GetFrameLevel() return 1 end
	function m:GetStringHeight() return 14 end
	function m:GetStringWidth() return #(self.text or "") * 6 end
	function m:CreateTexture() return Object("Texture") end
	function m:CreateFontString() return Object("FontString") end
	-- Methods are CamelCase; anything lower case is a field the addon set (or nil).
	setmetatable(o, { __index = function(t, k) return m[k] or (string.find(k, "^%u") and noop) or nil end });
	return o;
end
function CreateFrame(kind, name, parent, template)
	local f = Object(kind);
	f.name = name; f.template = template;
	frames[#frames + 1] = f;
	if name then named[name] = f; _G[name] = f end
	return f;
end
UIParent = Object("Frame"); UIParent.w = 1400;
GameTooltip = Object("Frame");
DEFAULT_CHAT_FRAME = Object("Frame");
UISpecialFrames, SlashCmdList = {}, {};
RAID_CLASS_COLORS = { ROGUE = { r = 1, g = 0.96, b = 0.41 }, SHAMAN = { r = 0, g = 0.44, b = 0.87 }, PRIEST = { r = 1, g = 1, b = 1 } };
function UnitName(u) if u == "player" then return "Baku" end end
function UnitClass() return "Rogue", "ROGUE" end
local npcGuid;
function UnitGUID(u) if u == "npc" then return npcGuid end end
local gossipClosed = 0;
function CloseGossip() gossipClosed = gossipClosed + 1 end
function GetSpellInfo(id) return "spell" .. id, nil, "Interface\\Icons\\Spell_" .. id end
function GetItemIcon(id) return "Interface\\Icons\\Item_" .. id end
function PlaySound() end
local sent = {};
function SendAddonMessage(prefix, text, channel, target) sent[#sent + 1] = { prefix = prefix, text = text, channel = channel, target = target } end
local function lastSent() return sent[#sent] and sent[#sent].text end

dofile(ADDON);

local frame = named.CENTURION_VioletHoldDraft;
check(frame ~= nil, "the draft frame exists");
local events;
for _, f in ipairs(frames) do if f.events.CHAT_MSG_ADDON then events = f end end
check(events and events.events.GOSSIP_SHOW, "listens for CHAT_MSG_ADDON and GOSSIP_SHOW");

local function fire(msg) events.scripts.OnEvent(events, "CHAT_MSG_ADDON", "CCGAME", msg, "WHISPER", "") end
local function cards()
	local out = {};
	for _, f in ipairs(frames) do
		if f._kind == "Button" and f.data and f.shown then out[#out + 1] = f end
	end
	return out;
end
local function card(code) for _, c in ipairs(cards()) do if c.data.code == code then return c end end end
local function button(prefix)
	for _, f in ipairs(frames) do
		if f.template == "UIPanelButtonTemplate" and f.text and string.find(f.text, prefix, 1, true) == 1 then return f end
	end
end
local function takeButton()
	for _, f in ipairs(frames) do
		if f.template == "UIPanelButtonTemplate" and f.text ~= "Close" then return f end
	end
end

local TAG = "0xF13000DBCD000123";

local function deal()
	fire("VHBOON:DEAL:" .. TAG .. "^7^30^1^1^0^Baku^4^Kaelen^7^Ysolde^5");
	fire("VHBOON:CARD:" .. TAG .. "^1^B^2^0^0^S90231^Boon of Might^+3%^damage done^6^9^255^%^0");
	fire("VHBOON:CARD:" .. TAG .. "^27^B^3^0^0^S90291^Boon of the Vampire^+3%^of damage dealt returned as health^0^3^30^%^0");
	fire("VHBOON:CARD:" .. TAG .. "^121^S^4^0^0^S51505^Lava Burst^Learn^Shaman spell, taught at the best rank your level allows.^0^0^0^^1^Kaelen");
	fire("VHBOON:CARD:" .. TAG .. "^206^I^5^0^1^I160807^Shard of the Defiler^Crusader^Yours for this run, soulbound.^0^0^0^^0");
	fire("VHBOON:CARD:" .. TAG .. "^29^B^3^1^1^S90293^Boon of Greed^+1^extra option at every broker from now on (run-wide)^1^2^3^^0");
	fire("VHBOON:DECK:" .. TAG .. "^1^Swiftness^16%^2^Might^6%^3^Phoenix^");
	fire("VHBOON:DECK:" .. TAG .. "^4^Shadow Dance^learned");
end

-- the gossip menu arrives after the cards
deal();
check(not frame.shown, "nothing shows before SHOW");
fire("VHBOON:SHOW:" .. TAG);
check(frame.shown, "SHOW puts the hand up");
check(#cards() == 5, "five cards dealt (got " .. #cards() .. ")");
check(sent[#sent].prefix == "CCGAMEREQ" and sent[#sent].target == "Baku", "requests whisper yourself on CCGAMEREQ");
check(lastSent() == "VHBOON:LOOK:" .. TAG .. ":1", "opening says which card you look at (got " .. tostring(lastSent()) .. ")");
local take = takeButton();
check(take and take.text == "Take" and take.enabled, "Take is enabled for a card you can take (got " .. tostring(take and take.text) .. ")");
check(card(1).data.held == 6 and card(1).data.after == 9 and card(1).data.cap == 255, "stacks parsed");
check(card(206).data.greedCard and not card(1).data.greedCard, "the Greed card is marked");
check(card(29).data.runWide, "run-wide boon flagged");

npcGuid = "0xf13000dbcd000123";
events.scripts.OnEvent(events, "GOSSIP_SHOW");
check(gossipClosed == 1, "the broker's gossip menu is closed when the cards are up");
npcGuid = "0xF13000AAAA000001";
events.scripts.OnEvent(events, "GOSSIP_SHOW");
check(gossipClosed == 1, "another NPC's gossip is left alone");
now = now + 5;
npcGuid = TAG;
events.scripts.OnEvent(events, "GOSSIP_SHOW");
check(gossipClosed == 1, "talking to the broker later, without a new deal, keeps the gossip");

-- somebody else's card
card(121).scripts.OnClick(card(121));
check(lastSent() == "VHBOON:LOOK:" .. TAG .. ":121", "selecting a card tells the party");
check(not take.enabled and take.text == "Kaelen only", "a card for Kaelen cannot be taken (got " .. tostring(take.text) .. ")");
local lava = card(121);
local ribbon;
for k, v in pairs(lava) do if k == "ribbonText" then ribbon = v end end
check(ribbon and ribbon.text == "Kaelen only" and ribbon.shown, "its ribbon says whose it is");

-- a party member is looking
fire("VHBOON:LOOK:" .. TAG .. "^Kaelen^1");
check(card(1).watch[1].shown and card(1).watch[1].text == "K", "Kaelen's badge on Might");
fire("VHBOON:LOOK:" .. TAG .. "^Kaelen^999");
check(not card(1).watch[1].shown, "Kaelen put it down");
fire("VHBOON:LOOK:0xF13000DBCD000999^Kaelen^1");
check(not card(1).watch[1].shown, "another broker's look is ignored");

-- take one
local count = #sent;
card(206).scripts.OnClick(card(206));
take.scripts.OnClick(take);
check(lastSent() == "VHBOON:PICK:" .. TAG .. ":206", "Take sends the pick (got " .. tostring(lastSent()) .. ")");
check(not frame.shown, "the hand goes the moment you take a card");
local before = #sent;
fire("VHBOON:TAKEN:" .. TAG .. "^Baku^206");
check(not frame.shown and #sent == before, "the server's TAKEN after your own pick changes nothing");

-- a party member takes one while your hand is up
deal();
fire("VHBOON:SHOW:" .. TAG);
check(frame.shown, "a new hand is up");
before = #sent;
fire("VHBOON:TAKEN:" .. TAG .. "^Kaelen^121");
check(not frame.shown, "the hand goes the moment a party member takes a card");
check(#sent == before, "nothing is sent after the deal is over");

-- leaving it for the party
deal();
fire("VHBOON:SHOW:" .. TAG);
check(frame.shown and not card(206).stamp.shown, "a fresh deal starts clean");
button("Close").scripts.OnClick(button("Close"));
check(not frame.shown and lastSent() == "VHBOON:LOOK:" .. TAG .. ":999", "leaving puts the cards down for the party");

-- gone
deal();
fire("VHBOON:SHOW:" .. TAG);
before = #sent;
fire("VHBOON:GONE:" .. TAG);
check(not frame.shown and #sent == before, "GONE closes the hand quietly");

-- a SHOW without its cards, other prefixes, a different broker's cards
fire("VHBOON:DEAL:" .. TAG .. "^7^30^0^0^0^Baku^4");
fire("VHBOON:SHOW:" .. TAG);
check(not frame.shown, "an empty hand never shows");
events.scripts.OnEvent(events, "CHAT_MSG_ADDON", "OTHER", "VHBOON:SHOW:" .. TAG);
check(not frame.shown, "other prefixes are ignored");

-- the deck strip
deal();
fire("VHBOON:SHOW:" .. TAG);
local chips = 0;
for _, f in ipairs(frames) do if f.dot and f.shown then chips = chips + 1 end end
check(chips == 4, "four deck chips from two DECK lines (got " .. chips .. ")");
events.scripts.OnEvent(events, "PLAYER_ENTERING_WORLD");
check(not frame.shown, "leaving the Hold puts the hand away");

-- the demo hand sends nothing
before = #sent;
SlashCmdList.CENTURIONVHDRAFT("demo");
check(frame.shown and #cards() == 4, "/vhdraft demo deals four cards");
take.scripts.OnClick(take);
frame:Hide();
check(#sent == before, "the demo never talks to the server");

OUT = string.format("pass=%d fail=%d %s", pass, fail, table.concat(log, "; "));
