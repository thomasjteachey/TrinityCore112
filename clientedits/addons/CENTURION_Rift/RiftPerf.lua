-- Summoner's Rift: a hitch recorder. Every frame in a match that takes longer
-- than HITCH seconds is written down with what came with it: the events handled
-- in that frame, the addon lines of the second before it, Lua memory and how it
-- moved (a big drop is a garbage collection), the frame rate and the latency.
-- It goes to CENTURION_RiftPerf (SavedVariables), to be read after logging out.
--   /riftperf        the latest few
--   /riftperf clear  empty the record

local HITCH, KEEP, SHOW = 0.1, 400, 10
local ZONE = "Summoner's Rift";
-- A jump of more than this much of the zone map in one frame is a teleport (the
-- Rift's map is about 680 yd across, so 0.04 is about 27 yd).
local TELEPORT = 0.04;

local watcher = CreateFrame("Frame");
local events = {};
local lines = {};          -- arrival times of addon lines, the last second's
local lastMem = collectgarbage("count");
-- The frame before: where the player stood, dead or not, what it was casting and
-- its level - so a hitch line says what the long frame changed. 2026-10-22: every
-- frame over 0.9 s so far came out of combat, which points at arriving at the
-- fountain (recall, respawn); these say whether that is it.
local lastX, lastY, lastDead, lastCast, lastLevel = 0, 0, false, "-", 0;

local function State()
	local x, y = GetPlayerMapPosition("player");
	local cast = UnitCastingInfo("player") or UnitChannelInfo("player") or "-";
	return x or 0, y or 0, UnitIsDeadOrGhost("player") and true or false, cast, UnitLevel("player") or 0;
end

local function Record(text)
	if ( type(CENTURION_RiftPerf) ~= "table" ) then
		CENTURION_RiftPerf = {};
	end
	table.insert(CENTURION_RiftPerf, text);
	while ( #CENTURION_RiftPerf > KEEP ) do
		table.remove(CENTURION_RiftPerf, 1);
	end
end

watcher:RegisterAllEvents();
watcher:SetScript("OnEvent", function(self, event)
	events[event] = (events[event] or 0) + 1;
	if ( event == "CHAT_MSG_ADDON" ) then
		table.insert(lines, GetTime());
	elseif ( event == "PLAYER_LOGIN" ) then
		Record(date("%Y-%m-%d %H:%M:%S") .. " session start");
	end
end);

watcher:SetScript("OnUpdate", function(self, elapsed)
	local now = GetTime();
	while ( lines[1] and now - lines[1] > 1 ) do
		table.remove(lines, 1);
	end
	local mem = collectgarbage("count");
	local inRift = GetRealZoneText() == ZONE;
	local x, y, dead, cast, level = 0, 0, false, "-", 0;
	if ( inRift ) then
		x, y, dead, cast, level = State();
	end
	if ( elapsed >= HITCH and inRift ) then
		local top = {};
		for event, n in pairs(events) do
			table.insert(top, { event, n });
		end
		table.sort(top, function(a, b) return a[2] > b[2]; end);
		local parts = {};
		for i = 1, math.min(#top, 14) do
			parts[i] = top[i][1] .. "=" .. top[i][2];
		end
		local jump = math.sqrt((x - lastX) ^ 2 + (y - lastY) ^ 2);
		local _, _, latency = GetNetStats();
		Record(string.format("%s dt=%dms fps=%.0f mem=%dKB(%+d) addonLines/s=%d lat=%d combat=%d tp=%d dead=%d>%d cast=%s lvl=%d>%d | %s",
			date("%H:%M:%S"), elapsed * 1000, GetFramerate(), mem, mem - lastMem, #lines, latency or 0,
			UnitAffectingCombat("player") and 1 or 0, (jump > TELEPORT and lastX ~= 0) and 1 or 0,
			lastDead and 1 or 0, dead and 1 or 0, lastCast, lastLevel, level, table.concat(parts, " ")));
	end
	lastMem = mem;
	lastX, lastY, lastDead, lastCast, lastLevel = x, y, dead, cast, level;
	wipe(events);
end);

SLASH_CENTURIONRIFTPERF1 = "/riftperf";
SlashCmdList["CENTURIONRIFTPERF"] = function(arg)
	if ( arg == "clear" ) then
		CENTURION_RiftPerf = {};
		DEFAULT_CHAT_FRAME:AddMessage("Rift hitch record cleared.");
		return;
	end
	local list = type(CENTURION_RiftPerf) == "table" and CENTURION_RiftPerf or {};
	DEFAULT_CHAT_FRAME:AddMessage(string.format("Rift hitches recorded: %d (frames over %dms in %s)", #list, HITCH * 1000, ZONE));
	for i = math.max(1, #list - SHOW + 1), #list do
		DEFAULT_CHAT_FRAME:AddMessage(list[i]);
	end
end
