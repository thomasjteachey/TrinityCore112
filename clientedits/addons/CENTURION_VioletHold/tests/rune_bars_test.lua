-- lua_run.py rune_bars_test.lua ADDON=<path>/CENTURION_VioletHold.lua
-- Stubs just enough of the 3.3.5 UI to load the addon, feed it VHRUNE lines and
-- watch the rune labels and the Glacial Stasis ring it draws.
local pass, fail, log = 0, 0, {};
local function check(ok, what) if ok then pass = pass + 1 else fail = fail + 1; log[#log + 1] = what end end

local now = 100;
function GetTime() return now end

local function Frame()
	local f = { shown = false, scripts = {}, events = {}, points = {} };
	function f:SetAllPoints() end
	function f:SetFrameStrata() end
	function f:SetScript(k, fn) self.scripts[k] = fn end
	function f:RegisterEvent(e) self.events[e] = true end
	function f:CreateTexture()
		local t = { shown = false };
		function t:SetTexture() end
		function t:SetVertexColor(r, g, b) self.color = { r, g, b } end
		function t:SetAllPoints() end
		function t:SetPoint() end
		function t:ClearAllPoints() end
		function t:SetWidth(w) self.w = w end
		function t:SetHeight(h) self.h = h end
		function t:Show() self.shown = true end
		function t:Hide() self.shown = false end
		self.textures = self.textures or {};
		table.insert(self.textures, t);
		return t;
	end
	function f:CreateFontString()
		local s = {};
		function s:SetPoint() end
		function s:SetText(v) self.text = v end
		function s:SetTextColor(r, g, b) self.color = { r, g, b } end
		return s;
	end
	function f:SetWidth(w) self.w = w end
	function f:SetHeight(h) self.h = h end
	function f:ClearAllPoints() self.points = {} end
	function f:SetPoint(a, rel, b, x, y) self.points = { x, y } end
	function f:Show() self.shown = true end
	function f:Hide() self.shown = false end
	function f:GetWidth() return 1000 end
	function f:GetHeight() return 500 end
	return f;
end
local frames = {};
function CreateFrame(kind, name, parent) local f = Frame(); frames[#frames + 1] = f; return f end
WorldFrame = Frame();

-- every point projects to the middle of the screen, a yard = 2% of its height
function CenturionUnitBar(i, x, y, z)
	if ( i == 0 ) then return 0.5, 0.5, 0.02 end
end

dofile(ADDON);
local layer, events = frames[1], frames[2];
check(events.events.CHAT_MSG_ADDON, "listens for CHAT_MSG_ADDON");

local function labels()
	local n = 0;
	for i = 3, #frames do if frames[i].shown then n = n + 1 end end
	return n;
end
local function fire(msg) events.scripts.OnEvent(events, "CHAT_MSG_ADDON", "CCGAME", msg, "WHISPER", "") end
local function draw() layer.scripts.OnUpdate(layer, 0.016) end
local function ringDots()
	local n = 0;
	for _, t in ipairs(layer.textures or {}) do if t.shown then n = n + 1 end end
	return n;
end

draw();
check(labels() == 0, "nothing before any feed");

fire("VHRUNE:0;1880.5,800.2,38.4,300501;1900.0,780.0,38.4,300500");
draw();
check(labels() == 2, "two runes, two labels (got " .. labels() .. ")");
check(frames[3].name.text == "Glacial Stasis", "first named Glacial Stasis (got " .. tostring(frames[3].name.text) .. ")");
check(frames[4].name.text == "Recharge", "second named Recharge");
check(frames[3].name.color and frames[3].name.color[3] == 1.00, "Glacial Stasis in blue");
check(frames[3].text == nil and frames[3].fill == nil, "no seconds, no bar");
check(ringDots() == 80, "a Glacial Stasis rune gets its 80-dot ring (got " .. ringDots() .. ")");

-- no timer: still there long after the old minute
now = now + 600;
draw();
check(labels() == 2, "runes stay up with no timer");

-- one taken: the server resends the rest
fire("VHRUNE:0;1900.0,780.0,38.4,300500");
draw();
check(labels() == 1, "a taken rune loses its label");
check(ringDots() == 0, "no ring without a Glacial Stasis rune (got " .. ringDots() .. ")");

-- two Glacial runes: two rings
fire("VHRUNE:0;1880.5,800.2,38.4,300501;1900.0,780.0,38.4,300501");
draw();
check(ringDots() == 160, "two Glacial Stasis runes, two rings (got " .. ringDots() .. ")");

fire("VHRUNE:0");
draw();
check(labels() == 0, "VHRUNE:0 clears every label");
check(ringDots() == 0, "VHRUNE:0 clears the ring");

-- a server still sending a lifetime: the number is ignored, the runes shown
fire("VHRUNE:60000;1.0,2.0,3.0,179905");
draw();
check(labels() == 1 and frames[3].name.text == "Berserking", "an old-format line still names the rune");

events.scripts.OnEvent(events, "PLAYER_ENTERING_WORLD");
draw();
check(labels() == 0, "leaving the Hold clears the labels");

events.scripts.OnEvent(events, "CHAT_MSG_ADDON", "OTHER", "VHRUNE:0;1.0,2.0,3.0,300500");
draw();
check(labels() == 0, "other prefixes are ignored");

CenturionUnitBar = nil;
fire("VHRUNE:0;1.0,2.0,3.0,300500");
draw();
check(labels() == 0 and ringDots() == 0, "no DLL, nothing drawn, no error");

OUT = string.format("pass=%d fail=%d %s", pass, fail, table.concat(log, "; "));
