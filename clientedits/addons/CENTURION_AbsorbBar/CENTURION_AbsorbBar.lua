-- Centurion Absorb Bar
--
-- A 3.3.5 client is never told how much a shield has left (aura updates carry no
-- amounts), so the server adds up shields and whispers the totals on the CCGAME
-- addon channel - see custom_absorb_feed.cpp:
--
--   ABS:<total>              you
--   ABST:<0xGUID>:<total>    your target, tagged so a late value for the unit
--                            you just tabbed away from is never drawn on the next
--
-- Each total is drawn over that unit frame's health bar: a translucent segment
-- starting where health ends, slid back over the health itself when it would run
-- past the end of the bar (with a glow on the right edge), and the amount written
-- on it.
--
-- Everything is measured off the health bar's own size and value rather than the
-- stock 119px, so frame mods that resize it (UnitFramesImproved) line up too.

local PREFIX = "CCGAME"
local SHIELD_R, SHIELD_G, SHIELD_B = 0.70, 0.88, 1.00

local selfAbsorb = 0
local targetGuid, targetAbsorb = nil, 0

local function ShortNumber(n)
	if n >= 1000000 then
		return string.format("%.1fm", n / 1000000)
	elseif n >= 10000 then
		return string.format("%.1fk", n / 1000)
	end
	return tostring(n)
end

-- Builds the textures on one health bar and returns its redraw function.
-- getAbsorb() says how much to draw right now.
local function CreateOverlay(bar, unit, getAbsorb)
	-- On the health bar itself, so it sits above the health fill but under the
	-- frame border, which lives on a higher frame.
	local fill = bar:CreateTexture(nil, "OVERLAY")
	fill:SetTexture("Interface\\TargetingFrame\\UI-StatusBar")
	fill:SetVertexColor(SHIELD_R, SHIELD_G, SHIELD_B, 0.55)
	fill:Hide()

	-- A hairline where the shield begins, so it still reads when it overlaps health.
	local edge = bar:CreateTexture(nil, "OVERLAY")
	edge:SetTexture(1, 1, 1, 0.85)
	edge:SetWidth(1)
	edge:Hide()

	-- Shown when health + shield is more than the bar can hold.
	local glow = bar:CreateTexture(nil, "OVERLAY")
	glow:SetTexture("Interface\\CastingBar\\UI-CastingBar-Spark")
	glow:SetBlendMode("ADD")
	glow:SetVertexColor(SHIELD_R, SHIELD_G, SHIELD_B, 1)
	glow:SetWidth(16)
	glow:Hide()

	local text = bar:CreateFontString(nil, "OVERLAY", "TextStatusBarText")
	text:SetTextColor(SHIELD_R, SHIELD_G, SHIELD_B)
	text:Hide()

	local function HideAll()
		fill:Hide()
		edge:Hide()
		glow:Hide()
		text:Hide()
	end

	local function Update()
		local absorb = getAbsorb()
		-- In a vehicle the player bar shows the vehicle's health, not yours.
		if absorb <= 0 or (bar.unit and bar.unit ~= unit) or not UnitExists(unit) or UnitIsDeadOrGhost(unit) then
			HideAll()
			return
		end

		local width, height = bar:GetWidth(), bar:GetHeight()
		local low, high = bar:GetMinMaxValues()
		local range = (high or 0) - (low or 0)
		if not width or width <= 0 or range <= 0 then
			HideAll()
			return
		end

		local healthWidth = (bar:GetValue() - low) / range * width
		local shieldWidth = math.max(1, math.min(absorb / range, 1) * width)
		local left = healthWidth
		local overflow = healthWidth + shieldWidth > width + 0.5
		if overflow then
			left = width - shieldWidth
		end

		fill:ClearAllPoints()
		fill:SetPoint("TOPLEFT", bar, "TOPLEFT", left, 0)
		fill:SetPoint("BOTTOMLEFT", bar, "BOTTOMLEFT", left, 0)
		fill:SetWidth(shieldWidth)
		fill:Show()

		edge:ClearAllPoints()
		edge:SetPoint("TOPLEFT", bar, "TOPLEFT", left, 0)
		edge:SetPoint("BOTTOMLEFT", bar, "BOTTOMLEFT", left, 0)
		edge:Show()

		if overflow then
			glow:ClearAllPoints()
			glow:SetPoint("CENTER", bar, "RIGHT", 0, 0)
			glow:SetHeight(height * 2.2)
			glow:Show()
		else
			glow:Hide()
		end

		-- Centred on the segment, but kept inside the bar when the segment is
		-- too narrow to hold it.
		text:SetText(ShortNumber(absorb))
		local half = text:GetStringWidth() / 2
		local center = left + shieldWidth / 2
		center = math.max(half + 1, math.min(width - half - 1, center))
		text:ClearAllPoints()
		text:SetPoint("CENTER", bar, "LEFT", center, 0)
		text:Show()
	end

	bar:HookScript("OnValueChanged", Update)
	bar:HookScript("OnSizeChanged", Update)
	if bar:HasScript("OnMinMaxChanged") then
		bar:HookScript("OnMinMaxChanged", Update)
	end

	return Update
end

local function NoUpdate() end

local UpdateSelf = PlayerFrameHealthBar and CreateOverlay(PlayerFrameHealthBar, "player", function()
	return selfAbsorb
end) or NoUpdate

local UpdateTarget = TargetFrameHealthBar and CreateOverlay(TargetFrameHealthBar, "target", function()
	local guid = UnitGUID("target")
	if guid and targetGuid and string.upper(guid) == targetGuid then
		return targetAbsorb
	end
	return 0
end) or NoUpdate

local frame = CreateFrame("Frame")
frame:RegisterEvent("CHAT_MSG_ADDON")
frame:RegisterEvent("UNIT_HEALTH")
frame:RegisterEvent("UNIT_MAXHEALTH")
frame:RegisterEvent("UNIT_ENTERED_VEHICLE")
frame:RegisterEvent("UNIT_EXITED_VEHICLE")
frame:RegisterEvent("PLAYER_TARGET_CHANGED")
frame:RegisterEvent("PLAYER_ENTERING_WORLD")
frame:RegisterEvent("PLAYER_DEAD")
frame:RegisterEvent("PLAYER_ALIVE")
frame:RegisterEvent("PLAYER_UNGHOST")
frame:SetScript("OnEvent", function(self, event, arg1, arg2)
	if event == "CHAT_MSG_ADDON" then
		if arg1 ~= PREFIX or type(arg2) ~= "string" then
			return
		end
		local total = tonumber(string.match(arg2, "^ABS:(%d+)$"))
		if total then
			selfAbsorb = total
			UpdateSelf()
			UpdateTarget()
			return
		end
		local guid, targetTotal = string.match(arg2, "^ABST:(0[xX]%x+):(%d+)$")
		if guid then
			targetGuid = string.upper(guid)
			targetAbsorb = tonumber(targetTotal)
			UpdateTarget()
		end
	elseif event == "UNIT_HEALTH" or event == "UNIT_MAXHEALTH"
		or event == "UNIT_ENTERED_VEHICLE" or event == "UNIT_EXITED_VEHICLE" then
		if arg1 == "player" then
			UpdateSelf()
		end
		if arg1 == "target" then
			UpdateTarget()
		end
	elseif event == "PLAYER_TARGET_CHANGED" then
		UpdateTarget()
	else
		UpdateSelf()
		UpdateTarget()
	end
end)

SLASH_CENTURIONABSORBBAR1 = "/absorbbar"
SlashCmdList["CENTURIONABSORBBAR"] = function()
	local line = string.format("|cff9fd8ffAbsorb Bar:|r you %d", selfAbsorb)
	if UnitExists("target") then
		local guid = UnitGUID("target")
		local shown = (guid and targetGuid and string.upper(guid) == targetGuid) and targetAbsorb or 0
		line = line .. string.format(", %s %d", UnitName("target") or "target", shown)
	end
	DEFAULT_CHAT_FRAME:AddMessage(line)
end
