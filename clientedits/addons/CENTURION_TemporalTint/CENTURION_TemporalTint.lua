-- Centurion Temporal Tint
--
-- Washes the icon of every Temporal (artifact-quality) item red, so a piece you
-- cannot miss in the tooltip is also one you cannot miss in your bags.
--
-- Why an overlay and not recoloured icon art: item icons are keyed off
-- displayid, and on this realm 913 of the 919 displayids used by Temporal items
-- are ALSO used by items of other qualities. Tinting the .blp files would turn
-- greens and blues red across the whole game. The quality belongs to the item,
-- not to the picture, so the tint has to be applied where the item is drawn.
--
-- The overlay is a plain white texture tinted red and laid over the icon in the
-- OVERLAY layer, so it sits above the art but below the border, the stack count
-- and the cooldown swipe.

local QUALITY_TINT = {
    -- [itemQuality] = { r, g, b, alpha }
    [6] = { 1.0, 0.05, 0.05, 0.40 },   -- Temporal / artifact
}

-- BLEND, not ADD. Additive blending only ever brightens toward the tint, so it
-- does nothing at all to the bright parts of an icon - a white-hot sword blade
-- is already at 1.0 and cannot go redder. BLEND mixes toward red everywhere,
-- which is what a wash has to do to read at bag-icon size.
local BLEND_MODE = "BLEND"

-- Set to true and reload to also wash heirlooms; left off because they already
-- read clearly and two washes in one bag is noise.
local TINT_HEIRLOOMS = false
if TINT_HEIRLOOMS then
    QUALITY_TINT[7] = { 0.15, 0.85, 1.0, 0.25 }
end

local OVERLAY_KEY = "CenturionTemporalTint"

-- One texture per button, created lazily and then reused. Buttons are recycled
-- by the default UI, so this must be idempotent.
local function GetOverlay(button)
    local overlay = button[OVERLAY_KEY]
    if overlay then
        return overlay
    end

    overlay = button:CreateTexture(nil, "OVERLAY")
    overlay:SetTexture("Interface\\Buttons\\WHITE8X8")
    overlay:SetBlendMode(BLEND_MODE)

    -- Match the icon rather than the button: the icon is inset on some frames,
    -- and a wash that spills onto the border looks like a bug.
    local icon = button.icon or _G[(button:GetName() or "") .. "IconTexture"]
    if icon then
        overlay:SetAllPoints(icon)
    else
        overlay:SetAllPoints(button)
    end

    overlay:Hide()
    button[OVERLAY_KEY] = overlay
    return overlay
end

local function ApplyQuality(button, quality)
    if not button then
        return
    end

    local tint = quality and QUALITY_TINT[quality]
    if not tint then
        -- Only pay for the texture on buttons that have ever needed one.
        local existing = button[OVERLAY_KEY]
        if existing then
            existing:Hide()
        end
        return
    end

    local overlay = GetOverlay(button)
    overlay:SetVertexColor(tint[1], tint[2], tint[3], tint[4])
    overlay:Show()
end

local function ApplyLink(button, link)
    if not link then
        ApplyQuality(button, nil)
        return
    end

    local _, _, quality = GetItemInfo(link)
    ApplyQuality(button, quality)
end

-- ---------------------------------------------------------------- bags -----
hooksecurefunc("ContainerFrame_Update", function(frame)
    local bag = frame:GetID()
    local name = frame:GetName()
    local size = frame.size or 0

    for slot = 1, size do
        local button = _G[name .. "Item" .. slot]
        if button then
            -- Container slots number top-left to bottom-right, but the buttons
            -- are laid out in reverse. This is the default UI's own mapping.
            ApplyLink(button, GetContainerItemLink(bag, button:GetID()))
        end
    end
end)

-- The bank's own bag frames go through ContainerFrame_Update above; these are
-- the fixed bank slots, which do not.
hooksecurefunc("BankFrameItemButton_Update", function(button)
    if not button or button.isBag then
        ApplyQuality(button, nil)
        return
    end

    ApplyLink(button, GetContainerItemLink(BANK_CONTAINER, button:GetID()))
end)

-- ------------------------------------------------------------ equipped -----
-- Which unit a paper-doll slot button is actually showing.
--
-- This is asked of the BUTTON rather than assumed, because the character sheet
-- and the inspect window share PaperDollItemSlotButton_Update and that function
-- hardcodes "player". Tinting an inspect button from the player's own gear puts
-- a red wash on somebody else's empty shoulders - which is exactly what it did:
-- a mage wearing ten pieces of Temporal lit up ten slots on every stranger they
-- inspected.
--
-- The default UI names these buttons for the frame they belong to
-- (CharacterHeadSlot vs InspectHeadSlot), so the name is the reliable signal.
local function UnitForSlotButton(button)
    local name = button:GetName()
    if name and name:find("^Inspect") then
        return InspectFrame and InspectFrame.unit
    end

    return "player"
end

-- Quality by unit and slot, without depending on the item cache.
--
-- GetItemInfo returns nil for an item the client has never seen, which is the
-- normal case when inspecting a stranger - so asking the link first would leave
-- their Temporal gear untinted until something else happened to cache it.
-- GetInventoryItemQuality answers straight from the inventory, and the link is
-- only the fallback.
local function QualityForUnitSlot(unit, slotId)
    if not unit or not slotId then
        return nil
    end

    local quality = GetInventoryItemQuality(unit, slotId)
    if quality then
        return quality
    end

    local link = GetInventoryItemLink(unit, slotId)
    if not link then
        return nil
    end

    local _, _, linkQuality = GetItemInfo(link)
    return linkQuality
end

hooksecurefunc("PaperDollItemSlotButton_Update", function(button)
    -- A nil unit CLEARS rather than returns. The inspect frame updates its
    -- buttons while closing and again before the next target resolves, and an
    -- early return there leaves the previous player's wash sitting on screen.
    ApplyQuality(button, QualityForUnitSlot(UnitForSlotButton(button), button:GetID()))
end)

-- ------------------------------------------------------------- looting -----
hooksecurefunc("LootFrame_UpdateButton", function(index)
    local button = _G["LootButton" .. index]
    if not button then
        return
    end

    -- The slot is derived, not stored on the button. This is the default UI's
    -- own arithmetic from LootFrame_UpdateButton; a coin slot has no link and
    -- ApplyLink clears in that case.
    local total = LootFrame.numLootItems or 0
    local page = LootFrame.page or 1
    local numLootToShow = LOOTFRAME_NUMBUTTONS
    if total > LOOTFRAME_NUMBUTTONS then
        numLootToShow = numLootToShow - 1
    end

    local slot = (numLootToShow * (page - 1)) + index
    ApplyLink(button, GetLootSlotLink(slot))
end)

-- ------------------------------------------------------------ inspect ------
-- Registered defensively: Blizzard_InspectUI is a load-on-demand addon, so the
-- function does not exist until somebody inspects a player.
local loader = CreateFrame("Frame")
loader:RegisterEvent("ADDON_LOADED")
loader:SetScript("OnEvent", function(_, _, addon)
    if addon ~= "Blizzard_InspectUI" then
        return
    end

    -- Belt and braces. Whether this function exists at all varies, and when it
    -- does not the inspect buttons are driven by PaperDollItemSlotButton_Update
    -- instead - which is why the resolver above asks the button who it belongs
    -- to rather than trusting either hook to be the one that fires. Both routes
    -- now end in the same answer, and hooking both twice is harmless because
    -- ApplyQuality is idempotent.
    if type(InspectPaperDollItemSlotButton_Update) == "function" then
        hooksecurefunc("InspectPaperDollItemSlotButton_Update", function(button)
            ApplyQuality(button, QualityForUnitSlot(UnitForSlotButton(button), button:GetID()))
        end)
    end

    -- Clear the whole inspect sheet when it closes. Nothing guarantees a slot
    -- update on the way out, and a stale wash that reappears on the next
    -- stranger reads as the bug this replaced.
    if InspectFrame then
        InspectFrame:HookScript("OnHide", function()
            for _, slotName in ipairs({
                "Head", "Neck", "Shoulder", "Back", "Chest", "Shirt", "Tabard",
                "Wrist", "Hands", "Waist", "Legs", "Feet", "Finger0", "Finger1",
                "Trinket0", "Trinket1", "MainHand", "SecondaryHand", "Ranged",
            }) do
                local button = _G["Inspect" .. slotName .. "Slot"]
                if button then
                    ApplyQuality(button, nil)
                end
            end
        end)
    end

    loader:UnregisterEvent("ADDON_LOADED")
end)
