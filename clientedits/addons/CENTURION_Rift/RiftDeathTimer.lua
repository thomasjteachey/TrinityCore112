-- Summoner's Rift: League's death timer. Every champion comes back at its own
-- fountain on its own clock - no spirit-guide waves (BattlegroundSRF::DeathTimer:
-- League's base wait for the level, longer late in the match). The server says
-- how long when the champion falls:
--   CCGAME  RIFTDEATH:<ms>
-- and the count runs in the middle of the screen until it is back.

local ANSWER_PREFIX = "CCGAME";

local frame = CreateFrame("Frame", "CENTURION_RiftDeathTimer", UIParent);
frame:SetWidth(320);
frame:SetHeight(70);
frame:SetPoint("CENTER", UIParent, "CENTER", 0, 140);
frame:SetFrameStrata("HIGH");
frame:Hide();

local label = frame:CreateFontString(nil, "OVERLAY", "GameFontNormal");
label:SetPoint("TOP", frame, "TOP", 0, 0);
label:SetText("Respawning in");
label:SetTextColor(0.78, 0.66, 0.43);

local count = frame:CreateFontString(nil, "OVERLAY", "GameFontNormalHuge");
count:SetPoint("TOP", label, "BOTTOM", 0, -4);
count:SetTextColor(1, 1, 1);

local backAt;
frame:SetScript("OnUpdate", function(self)
	local left = backAt and (backAt - GetTime()) or 0;
	if ( left <= 0 or not UnitIsDeadOrGhost("player") ) then
		backAt = nil;
		self:Hide();
		return;
	end
	count:SetText(string.format("%d", math.ceil(left)));
end);

local events = CreateFrame("Frame");
events:RegisterEvent("CHAT_MSG_ADDON");
events:RegisterEvent("PLAYER_ALIVE");
events:RegisterEvent("PLAYER_UNGHOST");
events:SetScript("OnEvent", function(self, event, prefix, message)
	if ( event ~= "CHAT_MSG_ADDON" ) then
		if ( not UnitIsDeadOrGhost("player") ) then
			backAt = nil;
			frame:Hide();
		end
		return;
	end
	local ms = prefix == ANSWER_PREFIX and type(message) == "string" and string.match(message, "^RIFTDEATH:(%d+)$");
	if ( ms ) then
		backAt = GetTime() + tonumber(ms) / 1000;
		frame:Show();
	end
end);
