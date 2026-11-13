-- CENTURION_VioletHold - a timer bar over every rune on the Violet Hold's floor.
--
-- The runes dropped when a wave falls are taken back after a minute. The server
-- (BattlegroundVHR::SendRuneFeed) tells the party where they are and how long is
-- left whenever that changes, on the CCGAME addon whisper:
--
--    VHRUNE:<ms left>;<x>,<y>,<z>;<x>,<y>,<z>...      "VHRUNE:0" = none left
--
-- and the bars are placed with the client DLL - CenturionUnitBar(0, x, y, z)
-- projects a world point to the screen and says how big a yard is there. With no
-- DLL there are no bars. Nothing is sent outside the Hold, so nothing shows there.

local PREFIX = "CCGAME";
local ABOVE = 2.2;            -- yards above the rune's base the bar hangs
local WHITE = "Interface\\Buttons\\WHITE8X8";

local runes, expires, total = {}, 0, 60;

local layer = CreateFrame("Frame", "CENTURION_VioletHoldRunes", WorldFrame);
layer:SetAllPoints(WorldFrame);
layer:SetFrameStrata("BACKGROUND");

local pool = {};

local function Bar(i)
	local bar = pool[i];
	if ( not bar ) then
		bar = CreateFrame("Frame", nil, layer);
		bar.bg = bar:CreateTexture(nil, "BACKGROUND");
		bar.bg:SetTexture(WHITE);
		bar.bg:SetVertexColor(0, 0, 0, 0.7);
		bar.bg:SetAllPoints(bar);
		bar.fill = bar:CreateTexture(nil, "ARTWORK");
		bar.fill:SetTexture(WHITE);
		bar.fill:SetPoint("TOPLEFT", bar, "TOPLEFT", 1, -1);
		bar.fill:SetPoint("BOTTOMLEFT", bar, "BOTTOMLEFT", 1, 1);
		bar.text = bar:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall");
		bar.text:SetPoint("BOTTOM", bar, "TOP", 0, 1);
		pool[i] = bar;
	end
	return bar;
end

local function HideFrom(i)
	for k = i, #pool do
		pool[k]:Hide();
	end
end

local function Draw()
	local left = expires - GetTime();
	if ( #runes == 0 or left <= 0 or type(CenturionUnitBar) ~= "function" ) then
		HideFrom(1);
		return;
	end
	local share = math.min(math.max(left / total, 0), 1);
	local width, height = WorldFrame:GetWidth(), WorldFrame:GetHeight();
	local used = 0;
	for _, rune in ipairs(runes) do
		local fx, fy, scale = CenturionUnitBar(0, rune.x, rune.y, rune.z + ABOVE);
		if ( fx and fx > -0.05 and fx < 1.05 and fy > -0.05 and fy < 1.05 ) then
			used = used + 1;
			local bar = Bar(used);
			-- about two yards wide where it stands, within reason
			local w = math.min(math.max((scale or 0) * height * 2, 36), 90);
			bar:SetWidth(w);
			bar:SetHeight(6);
			bar.fill:SetWidth(math.max((w - 2) * share, 0.01));
			bar.fill:SetVertexColor(1 - share, 0.35 + share * 0.5, 0.15);
			bar.text:SetText(math.ceil(left));
			bar:ClearAllPoints();
			bar:SetPoint("CENTER", WorldFrame, "BOTTOMLEFT", fx * width, fy * height);
			bar:Show();
		end
	end
	HideFrom(used + 1);
end

layer:SetScript("OnUpdate", Draw);

local events = CreateFrame("Frame");
events:RegisterEvent("CHAT_MSG_ADDON");
events:RegisterEvent("PLAYER_ENTERING_WORLD");
events:SetScript("OnEvent", function(self, event, prefix, message)
	if ( event == "PLAYER_ENTERING_WORLD" ) then
		runes = {};
		return;
	end
	if ( prefix ~= PREFIX or type(message) ~= "string" ) then
		return;
	end
	local ms, list = string.match(message, "^VHRUNE:(%d+)(.*)$");
	if ( not ms ) then
		return;
	end
	runes = {};
	for x, y, z in string.gmatch(list, ";(%-?[%d%.]+),(%-?[%d%.]+),(%-?[%d%.]+)") do
		runes[#runes + 1] = { x = tonumber(x), y = tonumber(y), z = tonumber(z) };
	end
	local left = tonumber(ms) / 1000;
	-- A pickup resends what is left, not a fresh minute; the bar keeps its scale.
	if ( left > total or expires <= GetTime() ) then
		total = math.max(left, 1);
	end
	expires = GetTime() + left;
end);
