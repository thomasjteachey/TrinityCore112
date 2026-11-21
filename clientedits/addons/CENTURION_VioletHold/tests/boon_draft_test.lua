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
		if f.template == "UIPanelButtonTemplate" then return f end
	end
end

local TAG = "0xF13000DBCD000123";

local function deal()
	fire("VHBOON:DEAL:" .. TAG .. "^7^30^1^1^0^2^0^Baku^4^Kaelen^7^Ysolde^5");
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
local reroll = button("Second opinion");
check(reroll and reroll.shown and reroll.enabled and reroll.text == "Second opinion (2)", "Second Opinion button shows its run-wide charges");

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

-- spend a run-wide reroll; the server closes this fixed hand and follows it
-- with a newly dealt one from the same broker.
reroll.scripts.OnClick(reroll);
check(lastSent() == "VHBOON:REROLL:" .. TAG .. ":0" and not reroll.enabled, "Second Opinion requests one reroll and waits for the server");
fire("VHBOON:REROLLED:" .. TAG .. "^Baku");
check(not frame.shown, "REROLLED closes the obsolete hand");
deal();
fire("VHBOON:SHOW:" .. TAG);

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
frame:Hide();   -- Escape or the X: the frame is in UISpecialFrames
check(not frame.shown and lastSent() == "VHBOON:LOOK:" .. TAG .. ":999", "leaving puts the cards down for the party");

-- gone
deal();
fire("VHBOON:SHOW:" .. TAG);
before = #sent;
fire("VHBOON:GONE:" .. TAG);
check(not frame.shown and #sent == before, "GONE closes the hand quietly");

-- a SHOW without its cards, other prefixes, a different broker's cards
fire("VHBOON:DEAL:" .. TAG .. "^7^30^0^0^0^0^1^Baku^4");
fire("VHBOON:SHOW:" .. TAG);
check(not frame.shown, "an empty hand never shows");
events.scripts.OnEvent(events, "CHAT_MSG_ADDON", "OTHER", "VHBOON:SHOW:" .. TAG);
check(not frame.shown, "other prefixes are ignored");

-- Take and Second Opinion, no side panels
deal();
fire("VHBOON:SHOW:" .. TAG);
local buttons = 0;
for _, f in ipairs(frames) do if f.template == "UIPanelButtonTemplate" then buttons = buttons + 1 end end
check(buttons == 3, "Take, Second Opinion and the shop's Ready are the only buttons (got " .. buttons .. ")");
events.scripts.OnEvent(events, "PLAYER_ENTERING_WORLD");
check(not frame.shown, "leaving the Hold puts the hand away");

-- Numeric units belong only on the new value and cap. This keeps longer
-- values such as Tempo's milliseconds inside their two fixed footer columns.
fire("VHBOON:DEAL:" .. TAG .. "^7^30^0^0^0^0^1^Baku^4");
fire("VHBOON:CARD:" .. TAG .. "^60^B^4^0^0^S94318^Boon of Tempo^+250 ms^off your global cooldown (never below 0.5 sec)^0^250^500^ ms^0");
fire("VHBOON:SHOW:" .. TAG);
local tempo = card(60);
check(tempo and tempo.stack.text == "0 > |cffffffff250 ms|r", "Tempo range uses one unit (got " .. tostring(tempo and tempo.stack.text) .. ")");
check(tempo and tempo.cap.text == "500 ms max", "Tempo cap stays in its column (got " .. tostring(tempo and tempo.cap.text) .. ")");
check(tempo and tempo.stack.w == 80 and tempo.cap.w == 50, "footer labels have fixed non-overlapping widths");
fire("VHBOON:GONE:" .. TAG);

-- the demo hand sends nothing
before = #sent;
SlashCmdList.CENTURIONVHDRAFT("demo");
check(frame.shown and #cards() == 4, "/vhdraft demo deals four cards");
take.scripts.OnClick(take);
frame:Hide();
check(#sent == before, "the demo never talks to the server");

-- The shop (Centurion.VioletHold.Shop.Enable): the same block plus SHOP and
-- PRICE lines, drawn as cards on display with hanging price tags. Buying keeps
-- the window open and waits for the restocked board.
local SHOPTAG = "0xF13000DBCD000500";
local SHOPCARDS = {
	[302] = { line = "^302^I^5^0^0^I19019^Thunderfury^Crusader^Lost when you leave the Hold.^0^0^0^^0", base = 20 },
	[1] = { line = "^1^B^1^0^0^S90231^Boon of Might^+3%^damage done^6^9^255^%^0", base = 3 },
	[27] = { line = "^27^B^3^0^0^S90291^Boon of the Vampire^+3%^of damage dealt returned as health^0^3^30^%^0", base = 8 },
	[4] = { line = "^4^B^2^0^0^S90243^Boon of Haste^+6%^attack speed^0^6^255^%^0", base = 5 },
};
-- list: { { code, waves discounted [, locked by, locked by me (1/0) [, may unlock (1/0)]] }, ... }
-- extra: "^<reroll cost>^<free rerolls>^<locks used>^<locks>^<interest>" (default: 2 shards, nothing held)
local function shopDeal(shards, list, extra)
	fire("VHBOON:DEAL:" .. SHOPTAG .. "^9^25^0^0^0^0^0^Baku^4^Kaelen^7");
	fire("VHBOON:SHOP:" .. SHOPTAG .. "^" .. shards .. "^4^1^50^25" .. (extra or "^2^0^0^0^0"));
	for _, entry in ipairs(list) do
		local def = SHOPCARDS[entry[1]];
		local price = math.max(1, math.floor(def.base * 0.75 ^ entry[2] + 0.5));
		fire("VHBOON:CARD:" .. SHOPTAG .. def.line);
		fire("VHBOON:PRICE:" .. SHOPTAG .. "^" .. entry[1] .. "^" .. price .. "^" .. def.base .. "^" .. entry[2]
			.. "^" .. (entry[3] or "") .. "^" .. (entry[4] or 0) .. (entry[5] and ("^" .. entry[5]) or ""));
	end
	fire("VHBOON:SHOW:" .. SHOPTAG);
end
local sounds = {};
PlaySound = function(name) sounds[#sounds + 1] = name end
local function heard(name) for _, s in ipairs(sounds) do if s == name then return true end end return false end
local function tagOf(code) return card(code) and tostring(card(code).col.tagText.text) end
local function noteOf(code) return card(code) and card(code).col.note.text end
local function flagOf(code) local c = card(code); return c and c.flag.shown and c.flag.text or nil end

shopDeal(11, { { 302, 0 }, { 1, 0 }, { 27, 2 } });
check(frame.shown and #cards() == 3, "the shop's board is up (got " .. #cards() .. " cards)");
check(tagOf(1) == "3", "a card's price hangs on its tag (got " .. tostring(tagOf(1)) .. ")");
check(tagOf(27) == "5" and noteOf(27) == "38% OFF  ·  WAS 8", "a discounted card says how far it has come down (got " .. tostring(noteOf(27)) .. ")");
check(noteOf(302) == "9 SHORT", "a card you cannot pay for says how short you are (got " .. tostring(noteOf(302)) .. ")");
check(noteOf(1) == "", "a card at full price you can pay for has no note");
check(not flagOf(1) and not flagOf(27), "the first look at a shop tags nothing");
check(lastSent() == "VHBOON:LOOK:" .. SHOPTAG .. ":1", "the shop opens on the first card you can pay for (got " .. tostring(lastSent()) .. ")");
check(take.shown and take.enabled and take.text == "Buy", "Buy sits under the picked card (got " .. tostring(take.text) .. ")");
check(not button("Second opinion") or not button("Second opinion").shown, "no Second Opinion at the shop");

card(302).scripts.OnClick(card(302));
check(take.shown and not take.enabled and take.text == "Not enough", "Buy is off for a card you cannot pay for (got " .. tostring(take.text) .. ")");
card(1).scripts.OnClick(card(1));
take.scripts.OnClick(take);
check(lastSent() == "VHBOON:BUY:" .. SHOPTAG .. ":1", "Buy sends BUY (got " .. tostring(lastSent()) .. ")");
check(frame.shown, "buying keeps the shop open");
check(not take.enabled and take.text == "Buying...", "Buy waits for the server (got " .. tostring(take.text) .. ")");
take.scripts.OnClick(take);
check(lastSent() == "VHBOON:BUY:" .. SHOPTAG .. ":1" and not take.enabled, "a second click while waiting sends nothing");

-- the server restocks: Might is gone, Haste took its slot
sounds = {};
shopDeal(8, { { 302, 0 }, { 4, 0 }, { 27, 2 } });
check(heard("LOOTWINDOWCOINSOUND"), "the sale going through rings the coins");
check(flagOf(4) == "NEW" and not flagOf(27), "the restocked card is tagged NEW (got " .. tostring(flagOf(4)) .. ")");
check(take.enabled and take.text == "Buy", "the restocked board frees the button (got " .. tostring(take.text) .. ")");
check(lastSent() == "VHBOON:LOOK:" .. SHOPTAG .. ":4", "with the bought card gone, the next one you can pay for is picked (got " .. tostring(lastSent()) .. ")");

-- a wave clears: two cards stayed and came down
before = #sent;
sounds = {};
shopDeal(12, { { 302, 1 }, { 4, 0 }, { 27, 3 } });
check(flagOf(302) == "CHEAPER" and flagOf(27) == "CHEAPER" and not flagOf(4), "cards that came down are tagged CHEAPER");
check(tagOf(302) == "15" and noteOf(302) == "3 SHORT", "Thunderfury is now 15, three short (got " .. tostring(tagOf(302)) .. ", " .. tostring(noteOf(302)) .. ")");
check(#sent == before and not heard("LOOTWINDOWCOINSOUND"), "the same card stays picked, quietly");

take.scripts.OnClick(take);
now = now + 3;
frame.scripts.OnUpdate(frame);
check(take.enabled and take.text == "Buy", "a buy nobody answers stops waiting (got " .. tostring(take.text) .. ")");

frame:Hide();
check(lastSent() == "VHBOON:LOOK:" .. SHOPTAG .. ":999", "closing the shop tells the server (got " .. tostring(lastSent()) .. ")");

shopDeal(0, {});
check(frame.shown and #cards() == 0, "a sold-out shop still opens");
check(not take.shown, "no Buy in a sold-out shop");
frame:Hide();

before = #sent;
sounds = {};
SlashCmdList.CENTURIONVHDRAFT("shop");
check(frame.shown and #cards() == 5, "/vhdraft shop shows five cards (got " .. #cards() .. ")");
check(flagOf(27) == "CHEAPER" and flagOf(208) == "NEW", "the demo shows both tags");
take.scripts.OnClick(take);
check(frame.shown and #sent == before and heard("LOOTWINDOWCOINSOUND"), "the shop demo buys locally and never talks to the server");
frame:Hide();

-- Reroll and locks (Boon of Safekeeping, Boon of the Gambler)
shopDeal(10, { { 302, 0, "Baku", 1 }, { 4, 0 }, { 27, 2, "Kaelen", 0 } }, "^2^0^1^3^1");
local rerollButton = button("Reroll");
check(rerollButton and rerollButton.shown and rerollButton.enabled and rerollButton.text == "Reroll  ·  2 shards",
	"Reroll shows its price (got " .. tostring(rerollButton and rerollButton.text) .. ")");
local myLock, theirLock, freeLock = card(302).col.lock, card(27).col.lock, card(4).col.lock;
check(myLock.shown and myLock.clickable, "your lock shows and can be taken off");
check(theirLock.shown and not theirLock.clickable, "a party member's lock shows and cannot be touched");
check(freeLock.shown and freeLock.clickable, "with a lock to spare, an open card can be locked");
check(string.find(theirLock.tip or "", "Kaelen", 1, true), "the lock says whose it is");
myLock.scripts.OnClick(myLock);
check(lastSent() == "VHBOON:LOCK:" .. SHOPTAG .. ":302", "clicking your lock sends LOCK (got " .. tostring(lastSent()) .. ")");
card(27).scripts.OnClick(card(27));
rerollButton.scripts.OnClick(rerollButton);
check(lastSent() == "VHBOON:REROLL:" .. SHOPTAG .. ":0", "Reroll sends REROLL (got " .. tostring(lastSent()) .. ")");
check(not rerollButton.enabled and rerollButton.text == "Rerolling...", "Reroll waits for the new board");
now = now + 3;
frame.scripts.OnUpdate(frame);
check(rerollButton.enabled, "a reroll nobody answers stops waiting");

-- Run-wide Safekeeping: the party shares its locks, so a party member's lock
-- can be taken off when the server says so
shopDeal(10, { { 302, 0, "Baku", 1, 1 }, { 4, 0 }, { 27, 2, "Kaelen", 0, 1 } }, "^2^0^2^2^0");
theirLock = card(27).col.lock;
check(theirLock.shown and theirLock.clickable, "a shared lock somebody else put on can be taken off");
check(string.find(theirLock.tip or "", "Kaelen", 1, true) and string.find(theirLock.tip or "", "shares", 1, true),
	"the shared lock still says whose it is, and that locks are shared (got " .. tostring(theirLock.tip) .. ")");
theirLock.scripts.OnClick(theirLock);
check(lastSent() == "VHBOON:LOCK:" .. SHOPTAG .. ":27", "clicking it sends LOCK (got " .. tostring(lastSent()) .. ")");
check(card(4).col.lock.shown and not card(4).col.lock.clickable, "with every shared lock in use, an open card cannot be locked");
check(string.find(card(4).col.lock.tip or "", "2 of the party's 2", 1, true), "the open padlock counts the party's locks (got " .. tostring(card(4).col.lock.tip) .. ")");

shopDeal(1, { { 302, 0 }, { 4, 0 } });
check(not rerollButton.enabled, "Reroll is off when you cannot pay for it");
check(not card(302).col.lock.shown and not card(4).col.lock.shown, "no padlocks without Boon of Safekeeping");
shopDeal(0, { { 302, 0 }, { 4, 0 } }, "^0^1^0^0^0");
check(rerollButton.enabled and rerollButton.text == "Reroll (free)", "a Gambler reroll is free, shards or not (got " .. tostring(rerollButton.text) .. ")");
frame:Hide();

-- The next-wave button: any one player starts the waiting wave, or calls
-- the next one in on top of a fight
local function readyDeal(waveLine)
	fire("VHBOON:DEAL:" .. SHOPTAG .. "^9^25^0^0^0^0^0^Baku^4^Kaelen^7");
	fire("VHBOON:SHOP:" .. SHOPTAG .. "^10^4^1^50^25^2^0^0^0^0");
	fire("VHBOON:CARD:" .. SHOPTAG .. SHOPCARDS[4].line);
	fire("VHBOON:PRICE:" .. SHOPTAG .. "^4^5^5^0^^0");
	if waveLine then fire("VHBOON:WAVE:" .. SHOPTAG .. waveLine) end
	fire("VHBOON:SHOW:" .. SHOPTAG);
end
readyDeal("^1^0^1^0^1^9");
local readyBtn = button("Start wave");
check(readyBtn and readyBtn.shown and readyBtn.enabled and readyBtn.text == "Start wave 9", "between waves it starts the waiting wave (got " .. tostring(readyBtn and readyBtn.text) .. ")");
readyBtn.scripts.OnClick(readyBtn);
check(lastSent() == "VHBOON:READY:" .. SHOPTAG .. ":0", "the button sends READY (got " .. tostring(lastSent()) .. ")");
check(not readyBtn.enabled and readyBtn.text == "Sending...", "it waits for the server (got " .. tostring(readyBtn.text) .. ")");
local before = #sent;
readyBtn.scripts.OnClick(readyBtn);
check(#sent == before, "a second click while sending sends nothing");
now = now + 3;
frame.scripts.OnUpdate(frame);
readyDeal("^1^0^1^0^2^10");
check(readyBtn.shown and readyBtn.enabled and readyBtn.text == "Call wave 10 now", "mid-fight it calls the next wave in (got " .. tostring(readyBtn.text) .. ")");
readyDeal("^1^0^1^0");
check(readyBtn.shown and readyBtn.enabled and readyBtn.text == "Start next wave", "a line without the call fields still works (got " .. tostring(readyBtn.text) .. ")");
readyDeal("^0^0^1^0^0^9");
check(not readyBtn.shown, "no button while there is nothing to start");
readyDeal(nil);
check(not readyBtn.shown, "no button from a server that sends no WAVE line");
frame:Hide();

-- a broker after the shop: back to the plain hand
deal();
fire("VHBOON:SHOW:" .. TAG);
check(frame.shown and #cards() == 5, "a broker's hand after the shop shows only its own five cards (got " .. #cards() .. ")");
check(take.shown and take.text == "Take", "the broker's Take button is back (got " .. tostring(take.text) .. ")");
fire("VHBOON:GONE:" .. TAG);

OUT = string.format("pass=%d fail=%d %s", pass, fail, table.concat(log, "; "));
