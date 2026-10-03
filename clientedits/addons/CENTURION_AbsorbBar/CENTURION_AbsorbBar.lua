-- Centurion Absorb Bar
--
-- A 3.3.5 client is never told how much a shield has left (aura updates carry no
-- amounts), so the server adds up your shields and whispers the total on the
-- CCGAME addon channel as "ABS:<total>" - see custom_absorb_feed.cpp. This draws
-- that total over the player frame's health bar: a translucent segment starting
-- where your health ends, slid back over the health itself when it would run past
-- the end of the bar, with a glow on the right edge when it does.
--
-- Everything is measured off the health bar's own size and value rather than the
-- stock 119px, so frame mods that resize it (UnitFramesImproved) line up too.

local PREFIX = "CCGAME"
local SHIELD_R, SHIELD_G, SHIELD_B = 0.70, 0.88, 1.00

local bar = PlayerFrameHealthBar
if not bar then
	return
end

local absorb = 0

-- On the health bar itself, so it sits above the health fill but under the
-- frame border and the health text, which live on higher frames.
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

local function HideAll()
	fill:Hide()
	edge:Hide()
	glow:Hide()
end

local function Update()
	-- In a vehicle the bar shows the vehicle's health, not yours.
	if absorb <= 0 or (bar.unit and bar.unit ~= "player") or UnitIsDeadOrGhost("player") then
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
end

bar:HookScript("OnValueChanged", Update)
bar:HookScript("OnSizeChanged", Update)
if bar:HasScript("OnMinMaxChanged") then
	bar:HookScript("OnMinMaxChanged", Update)
end

local frame = CreateFrame("Frame")
frame:RegisterEvent("CHAT_MSG_ADDON")
frame:RegisterEvent("UNIT_HEALTH")
frame:RegisterEvent("UNIT_MAXHEALTH")
frame:RegisterEvent("UNIT_ENTERED_VEHICLE")
frame:RegisterEvent("UNIT_EXITED_VEHICLE")
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
			absorb = total
			Update()
		end
	elseif event == "UNIT_HEALTH" or event == "UNIT_MAXHEALTH"
		or event == "UNIT_ENTERED_VEHICLE" or event == "UNIT_EXITED_VEHICLE" then
		if arg1 == "player" then
			Update()
		end
	else
		Update()
	end
end)

SLASH_CENTURIONABSORBBAR1 = "/absorbbar"
SlashCmdList["CENTURIONABSORBBAR"] = function()
	DEFAULT_CHAT_FRAME:AddMessage(string.format("|cff9fd8ffAbsorb Bar:|r %d damage absorbed by your shields.", absorb))
end
