-- lua_run.py rune_bars_test.lua ADDON=<path>/CENTURION_VioletHold.lua
-- Stubs just enough of the 3.3.5 UI to load the addon, feed it VHRUNE lines and
-- watch the bars it draws.
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
		local t = {};
		function t:SetTexture() end
		function t:SetVertexColor(r, g, b) self.color = { r, g, b } end
		function t:SetAllPoints() end
		function t:SetPoint() end
		function t:SetWidth(w) self.w = w end
		return t;
	end
	function f:CreateFontString()
		local s = {};
		function s:SetPoint() end
		function s:SetText(v) self.text = v end
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

-- every rune projects to the middle of the screen, a yard = 2% of its height
function CenturionUnitBar(i, x, y, z)
	if ( i == 0 ) then return 0.5, 0.5, 0.02 end
end

dofile(ADDON);
local layer, events = frames[1], frames[2];
check(events.events.CHAT_MSG_ADDON, "listens for CHAT_MSG_ADDON");

local function bars()
	local n = 0;
	for i = 3, #frames do if frames[i].shown then n = n + 1 end end
	return n;
end
local function fire(msg) events.scripts.OnEvent(events, "CHAT_MSG_ADDON", "CCGAME", msg, "WHISPER", "") end
local function draw() layer.scripts.OnUpdate(layer, 0.016) end

draw();
check(bars() == 0, "no bars before any feed");

fire("VHRUNE:60000;1880.5,800.2,38.4;1900.0,780.0,38.4");
draw();
check(bars() == 2, "two runes, two bars (got " .. bars() .. ")");
check(frames[3].text.text == 60, "60 seconds shown");
check(frames[3].fill.w and frames[3].fill.w > 30, "full bar at the start");

now = now + 30;
draw();
check(frames[3].text.text == 30, "30 seconds after half a minute (got " .. tostring(frames[3].text.text) .. ")");

-- one taken: the server resends what is left
fire("VHRUNE:30000;1900.0,780.0,38.4");
draw();
check(bars() == 1, "a taken rune loses its bar");
local w = frames[3].w;
check(math.abs(frames[3].fill.w - (w - 2) * 0.5) < 0.6, "the bar keeps its minute scale after a pickup");

fire("VHRUNE:0");
draw();
check(bars() == 0, "VHRUNE:0 clears every bar");

fire("VHRUNE:60000;1.0,2.0,3.0");
now = now + 61;
draw();
check(bars() == 0, "an expired rune's bar goes by itself");

fire("RIFTTURRET:1");
fire("VHRUNE:60000;1.0,2.0,3.0");
events.scripts.OnEvent(events, "PLAYER_ENTERING_WORLD");
draw();
check(bars() == 0, "leaving the Hold clears the bars");

events.scripts.OnEvent(events, "CHAT_MSG_ADDON", "OTHER", "VHRUNE:60000;1.0,2.0,3.0");
draw();
check(bars() == 0, "other prefixes are ignored");

CenturionUnitBar = nil;
fire("VHRUNE:60000;1.0,2.0,3.0");
draw();
check(bars() == 0, "no DLL, no bars, no error");

OUT = string.format("pass=%d fail=%d %s", pass, fail, table.concat(log, "; "));
