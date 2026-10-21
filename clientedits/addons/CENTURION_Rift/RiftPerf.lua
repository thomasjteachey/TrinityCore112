-- Summoner's Rift: a hitch recorder. Every frame in a match that takes longer
-- than HITCH seconds is written down with what came with it: the events handled
-- in that frame, the addon lines of the second before it, Lua memory and how it
-- moved (a big drop is a garbage collection), the frame rate and the latency.
-- It goes to CENTURION_RiftPerf (SavedVariables), to be read after logging out.
--   /riftperf        the latest few
--   /riftperf clear  empty the record

local HITCH, KEEP, SHOW = 0.1, 400, 10
local ZONE = "Summoner's Rift";

local watcher = CreateFrame("Frame");
local events = {};
local lines = {};          -- arrival times of addon lines, the last second's
local lastMem = collectgarbage("count");

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
	if ( elapsed >= HITCH and GetRealZoneText() == ZONE ) then
		local top = {};
		for event, n in pairs(events) do
			table.insert(top, { event, n });
		end
		table.sort(top, function(a, b) return a[2] > b[2]; end);
		local parts = {};
		for i = 1, math.min(#top, 8) do
			parts[i] = top[i][1] .. "=" .. top[i][2];
		end
		local _, _, latency = GetNetStats();
		Record(string.format("%s dt=%dms fps=%.0f mem=%dKB(%+d) addonLines/s=%d lat=%d combat=%d | %s",
			date("%H:%M:%S"), elapsed * 1000, GetFramerate(), mem, mem - lastMem, #lines, latency or 0,
			UnitAffectingCombat("player") and 1 or 0, table.concat(parts, " ")));
	end
	lastMem = mem;
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
