-- BPlusBeastTrainingTradeSkill.lua
-- FrameXML-side Beast Training skin/sorter for the WotLK TradeSkillFrame.
-- Keeps the working 3.3.5 TradeSkill backend and only changes the UI when GetTradeSkillLine() == "Beast Training".

BPLUS_BEAST_TRAINING_NAME = BPLUS_BEAST_TRAINING_NAME or "Beast Training";
BPLUS_BEAST_TRAINING_SPELL_ID = BPLUS_BEAST_TRAINING_SPELL_ID or 5149;

BPlusPetTrainingData = BPlusPetTrainingData or {};
BPlusPetTrainingLookupByNameRank = BPlusPetTrainingLookupByNameRank or {};

local BPT = CreateFrame("Frame");
BPT:RegisterEvent("ADDON_LOADED");
BPT:RegisterEvent("TRADE_SKILL_SHOW");
BPT:RegisterEvent("TRADE_SKILL_UPDATE");
BPT:RegisterEvent("TRADE_SKILL_CLOSE");
BPT:RegisterEvent("UNIT_PET");
BPT:RegisterEvent("PET_BAR_UPDATE");
BPT:RegisterEvent("SPELLS_CHANGED");
BPT.hooked = false;
BPT.sortedSkillList = {};
BPT.petKnownNameRank = {};
BPT.petKnownRankByName = {};
BPT.petKnownCostByName = {};
BPT.petKnownNameUnknown = {};

local function BPlus_IsBeastTrainingTradeSkill()
    if not GetTradeSkillLine then
        return false;
    end

    local name = GetTradeSkillLine();
    return name == BPLUS_BEAST_TRAINING_NAME or name == "Beast Training";
end

local function BPlus_Show(frame)
    if frame and frame.Show then
        frame:Show();
    end
end

local function BPlus_Hide(frame)
    if frame and frame.Hide then
        frame:Hide();
    end
end

local function BPlus_ParseRankNumber(rank)
    if not rank then
        return 0;
    end

    local n = tonumber(string.match(rank, "(%d+)"));
    return n or 0;
end


local BPLUS_PET_FAMILY_IDS = {
    ["Wolf"] = 1,
    ["Cat"] = 2,
    ["Spider"] = 3,
    ["Bear"] = 4,
    ["Boar"] = 5,
    ["Crocolisk"] = 6,
    ["Crocodile"] = 6,
    ["Carrion Bird"] = 7,
    ["Crab"] = 8,
    ["Gorilla"] = 9,
    ["Raptor"] = 11,
    ["Tallstrider"] = 12,
    ["Scorpid"] = 20,
    ["Turtle"] = 21,
    ["Bat"] = 24,
    ["Hyena"] = 25,
    ["Owl"] = 26,
    ["Bird of Prey"] = 26,
    ["Wind Serpent"] = 27,
};

local BPLUS_PET_FAMILY_IDS_LOWER = {};
for familyName, familyId in pairs(BPLUS_PET_FAMILY_IDS) do
    BPLUS_PET_FAMILY_IDS_LOWER[string.lower(familyName)] = familyId;
end

local function BPlus_GetPetFamilyId()
    if not UnitExists or not UnitExists("pet") then
        return nil;
    end

    if not UnitCreatureFamily then
        return nil;
    end

    local familyName = UnitCreatureFamily("pet");
    if not familyName or familyName == "" then
        return nil;
    end

    return BPLUS_PET_FAMILY_IDS[familyName] or BPLUS_PET_FAMILY_IDS_LOWER[string.lower(familyName)];
end

local function BPlus_MaskHasBit(mask, bitValue)
    mask = tonumber(mask or 0) or 0;
    bitValue = tonumber(bitValue or 0) or 0;
    if mask <= 0 or bitValue <= 0 then
        return false;
    end

    if bit and bit.band then
        return bit.band(mask, bitValue) ~= 0;
    end

    return math.floor(mask / bitValue) % 2 >= 1;
end

local function BPlus_IsPetFamilyCompatible(data)
    if not data then
        return false;
    end

    local familyMask = tonumber(data.familyMask or 0) or 0;
    if familyMask == 0 then
        return true;
    end

    local familyId = BPlus_GetPetFamilyId();
    if not familyId then
        -- Do not hide everything if the client cannot resolve the localized pet family.
        -- The server still enforces the real family restriction.
        return true;
    end

    return BPlus_MaskHasBit(familyMask, 2 ^ familyId);
end

local function BPlus_GetTradeSkillRankText(index)
    local name, skillType, numAvailable, isExpanded, altVerb, numSkillUps = GetTradeSkillInfo(index);
    local subText = "";

    -- Some custom clients expose rank as the second return value for recipe/training rows;
    -- others return only the name.  Prefer generated data when available.
    if skillType and skillType ~= "header" and string.find(skillType, "Rank") then
        subText = skillType;
    end

    return subText or "";
end

local function BPlus_GetSourceSpellFromLink(link)
    if not link then
        return nil;
    end

    local spellID = tonumber(string.match(link, "spell:(%d+)"));
    if spellID then
        return spellID;
    end

    local enchantID = tonumber(string.match(link, "enchant:(%d+)"));
    if enchantID then
        return enchantID;
    end

    return nil;
end

local function BPlus_GetTradeSkillSourceSpell(index)
    local link;

    if GetTradeSkillRecipeLink then
        link = GetTradeSkillRecipeLink(index);
    end
    local spellID = BPlus_GetSourceSpellFromLink(link);
    if spellID then
        return spellID;
    end

    if GetTradeSkillItemLink then
        link = GetTradeSkillItemLink(index);
    end
    spellID = BPlus_GetSourceSpellFromLink(link);
    if spellID then
        return spellID;
    end

    if BPlusPetTrainingIndexToSourceSpell then
        return BPlusPetTrainingIndexToSourceSpell[index];
    end

    return nil;
end

local function BPlus_GetTrainingDataForIndex(index)
    local sourceSpell = BPlus_GetTradeSkillSourceSpell(index);
    if sourceSpell and BPlusPetTrainingData then
        return BPlusPetTrainingData[sourceSpell], sourceSpell;
    end

    local name = GetTradeSkillInfo(index);
    local rank = BPlus_GetTradeSkillRankText(index);
    if name and BPlusPetTrainingLookupByNameRank then
        sourceSpell = BPlusPetTrainingLookupByNameRank[name .. ":" .. rank];
        if sourceSpell and BPlusPetTrainingData then
            return BPlusPetTrainingData[sourceSpell], sourceSpell;
        end
    end

    return nil, sourceSpell;
end

local function BPlus_GetTrainingPoints()
    local name, rank, maxRank = GetTradeSkillLine();
    rank = tonumber(rank) or 0;
    maxRank = tonumber(maxRank) or 0;
    return rank, maxRank;
end

local function BPlus_SetTextIfExists(frame, text)
    if frame and frame.SetText then
        frame:SetText(text or "");
    end
end

local function BPlus_SetFrameText(globalName, text)
    local frame = getglobal(globalName);
    BPlus_SetTextIfExists(frame, text);
end



local function BPlus_GetScanTooltip()
    if BPT.ScanTooltip then
        return BPT.ScanTooltip;
    end

    local tooltip = CreateFrame("GameTooltip", "BPlusPetTrainingScanTooltip", UIParent, "GameTooltipTemplate");
    tooltip:SetOwner(UIParent, "ANCHOR_NONE");
    BPT.ScanTooltip = tooltip;
    return tooltip;
end

local function BPlus_ReadTooltipLines()
    local lines = {};
    local tooltip = BPT.ScanTooltip;
    if not tooltip then
        return lines;
    end

    for i = 1, 20 do
        local left = getglobal("BPlusPetTrainingScanTooltipTextLeft" .. i);
        if left and left.GetText then
            local text = left:GetText();
            if text and text ~= "" then
                table.insert(lines, text);
            end
        end
    end

    return lines;
end

local function BPlus_ParseTooltipNameRankDescription(lines)
    local name = lines and lines[1] or nil;
    local rank = nil;
    local desc = nil;

    if lines then
        for i = 2, table.getn(lines) do
            local text = lines[i];
            if text and string.find(text, "Rank %d+") then
                rank = string.match(text, "Rank %d+");
                break;
            end
        end

        local descParts = {};
        for i = 2, table.getn(lines) do
            local text = lines[i];
            if text and text ~= "" and not string.find(text, "^Rank %d+") and not string.find(text, "^%d+ .- range") and not string.find(text, "^Instant") and not string.find(text, "^Requires") and not string.find(text, "^%d+ Focus") and not string.find(text, "^%d+ Mana") and not string.find(text, "cooldown") then
                table.insert(descParts, text);
            end
        end
        if table.getn(descParts) > 0 then
            desc = table.concat(descParts, "\n");
        end
    end

    return name, rank, desc;
end

local function BPlus_GetTooltipInfoForSpell(spellID)
    if not spellID then
        return nil, nil, nil;
    end

    local tooltip = BPlus_GetScanTooltip();
    if not tooltip then
        return nil, nil, nil;
    end

    tooltip:ClearLines();
    tooltip:SetOwner(UIParent, "ANCHOR_NONE");

    if tooltip.SetHyperlink then
        tooltip:SetHyperlink("spell:" .. tostring(spellID));
    elseif tooltip.SetSpellByID then
        tooltip:SetSpellByID(spellID);
    else
        return nil, nil, nil;
    end

    local lines = BPlus_ReadTooltipLines();
    tooltip:Hide();
    return BPlus_ParseTooltipNameRankDescription(lines);
end

local function BPlus_GetTooltipInfoForSpellBookItem(index, bookType)
    local tooltip = BPlus_GetScanTooltip();
    if not tooltip or not tooltip.SetSpellBookItem then
        return nil, nil, nil;
    end

    tooltip:ClearLines();
    tooltip:SetOwner(UIParent, "ANCHOR_NONE");
    tooltip:SetSpellBookItem(index, bookType);
    local lines = BPlus_ReadTooltipLines();
    tooltip:Hide();
    return BPlus_ParseTooltipNameRankDescription(lines);
end

local function BPlus_GetSpellText(spellID)
    if not spellID then
        return nil, nil, nil, nil;
    end

    local name, rank, icon;
    if GetSpellInfo then
        name, rank, icon = GetSpellInfo(spellID);
    end

    local desc;
    if GetSpellDescription then
        desc = GetSpellDescription(spellID);
    end

    local tipName, tipRank, tipDesc = BPlus_GetTooltipInfoForSpell(spellID);
    if (not name or name == "") and tipName then
        name = tipName;
    end
    if (not rank or rank == "") and tipRank then
        rank = tipRank;
    end
    if (not desc or desc == "") and tipDesc then
        desc = tipDesc;
    end

    return name, rank, icon, desc;
end

local function BPlus_SetTexture(frame, texture)
    if frame and texture then
        if frame.SetTexture then
            frame:SetTexture(texture);
        elseif frame.SetNormalTexture then
            frame:SetNormalTexture(texture);
        end
    end
end


local function BPlus_EnsureMiddleIconOverlay(parent)
    if BPT.MiddleIconOverlay and BPT.MiddleIconOverlayParent == parent then
        return BPT.MiddleIconOverlay;
    end

    if not parent or not parent.CreateTexture then
        return nil;
    end

    local tex = parent:CreateTexture(nil, "OVERLAY");
    tex:SetAllPoints(parent);
    tex:Hide();
    BPT.MiddleIconOverlay = tex;
    BPT.MiddleIconOverlayParent = parent;
    return tex;
end

local BPlus_SetTaughtSpellTooltip;

local function BPlus_SetDetailIconToTaughtSpell(texture, data)
    if not texture or texture == "" then
        return;
    end

    -- The selected detail icon in the middle of the TradeSkillFrame is a stock
    -- TradeSkill frame region, and different 3.3.5 FrameXML builds name it
    -- differently.  Update every known candidate, and also draw our own overlay
    -- on top of the icon button so the source/teaching-spell icon cannot bleed
    -- through after the stock TradeSkillFrame refreshes.
    local candidates = {
        TradeSkillSkillIcon,
        TradeSkillSkillIconTexture,
        TradeSkillSkillIconIcon,
        TradeSkillSkillIconNormalTexture,
        TradeSkillSkillIconTextureTexture,
        getglobal("TradeSkillSkillIcon"),
        getglobal("TradeSkillSkillIconTexture"),
        getglobal("TradeSkillSkillIconIcon"),
        getglobal("TradeSkillSkillIconNormalTexture"),
    };

    local overlayParent = nil;

    for i = 1, table.getn(candidates) do
        local frame = candidates[i];
        if frame then
            if frame.SetTexture then
                frame:SetTexture(texture);
            end
            if frame.SetNormalTexture then
                frame:SetNormalTexture(texture);
                if frame.GetNormalTexture then
                    local nt = frame:GetNormalTexture();
                    if nt and nt.SetTexture then
                        nt:SetTexture(texture);
                    end
                end
            end
            if frame.SetPushedTexture then
                frame:SetPushedTexture(texture);
            end
            if frame.SetHighlightTexture then
                -- Do not use the pet spell icon as the highlight texture. Leave the
                -- normal highlight behavior alone when possible.
            end
            if (not overlayParent) and frame.GetObjectType and frame:GetObjectType() ~= "Texture" then
                overlayParent = frame;
            end
        end
    end

    overlayParent = overlayParent or TradeSkillSkillIcon;
    local overlay = BPlus_EnsureMiddleIconOverlay(overlayParent);
    if overlay then
        overlay:SetTexture(texture);
        overlay:Show();
    end

    if TradeSkillSkillIcon and data then
        BPlus_SetTaughtSpellTooltip(TradeSkillSkillIcon, data);
    end
end

BPlus_SetTaughtSpellTooltip = function(frame, data)
    if not frame then
        return;
    end

    frame.BPlusTaughtSpell = data and tonumber(data.taughtSpell) or nil;

    -- The TradeSkill row buttons are shared by every profession window.
    -- Never replace their stock OnEnter/OnLeave scripts; doing so breaks
    -- normal profession hover/background/text behavior after Beast Training.
    if frame.BPlusTaughtSpellTooltipHooked then
        return;
    end

    frame.BPlusTaughtSpellTooltipHooked = true;

    frame:HookScript("OnEnter", function(self)
        if not BPlus_IsBeastTrainingTradeSkill or not BPlus_IsBeastTrainingTradeSkill() then
            return;
        end

        local spellID = self.BPlusTaughtSpell;
        if not spellID then
            return;
        end

        GameTooltip:SetOwner(self, "ANCHOR_RIGHT");
        if GameTooltip.SetHyperlink then
            GameTooltip:SetHyperlink("spell:" .. tostring(spellID));
        elseif GameTooltip.SetSpellByID then
            GameTooltip:SetSpellByID(spellID);
        end
        GameTooltip:Show();
    end);
    frame:HookScript("OnLeave", function(self)
        if BPlus_IsBeastTrainingTradeSkill and BPlus_IsBeastTrainingTradeSkill() and self.BPlusTaughtSpell then
            GameTooltip:Hide();
        end
    end);
end

local function BPlus_SetButtonTextColor(button, r, g, b)
    if button and button.SetTextColor then
        button:SetTextColor(r, g, b);
    end

    if not button or not button.GetName then
        return;
    end

    local name = button:GetName();
    local text = getglobal(name .. "Text");
    local highlight = getglobal(name .. "HighlightText");
    local disabled = getglobal(name .. "DisabledText");

    if text and text.SetTextColor then
        text:SetTextColor(r, g, b);
    end
    if highlight and highlight.SetTextColor then
        highlight:SetTextColor(1.0, 1.0, 1.0);
    end
    if disabled and disabled.SetTextColor then
        disabled:SetTextColor(r, g, b);
    end
end

local function BPlus_EnsureRowCost(button)
    if not button then
        return nil;
    end

    if button.BPlusCostText then
        return button.BPlusCostText;
    end

    local fs = button:CreateFontString(nil, "ARTWORK", "GameFontHighlightSmall");
    fs:SetJustifyH("RIGHT");
    fs:SetPoint("RIGHT", button, "RIGHT", -10, 0);
    fs:SetTextColor(1.0, 0.82, 0.0);
    button.BPlusCostText = fs;
    return fs;
end

local function BPlus_EnsurePointsText()
    if BPT.PointsText then
        return BPT.PointsText;
    end

    local parent = TradeSkillRankFrame or TradeSkillFrame;
    if not parent or not parent.CreateFontString then
        return nil;
    end

    local fs = parent:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall");
    fs:SetJustifyH("CENTER");
    fs:SetPoint("CENTER", parent, "CENTER", 0, 0);
    fs:SetTextColor(1.0, 1.0, 1.0);
    fs:Show();
    BPT.PointsText = fs;
    return fs;
end


local function BPlus_EnsureSelectionHighlight()
    if BPT.SelectionHighlight then
        return BPT.SelectionHighlight;
    end

    local parent = TradeSkillFrame or UIParent;
    if not parent or not parent.CreateTexture then
        return nil;
    end

    local tex = parent:CreateTexture(nil, "ARTWORK");
    tex:SetTexture("Interface\\Buttons\\UI-Listbox-Highlight2");
    tex:SetBlendMode("ADD");
    tex:SetAlpha(0.65);
    tex:Hide();
    BPT.SelectionHighlight = tex;
    return tex;
end

local function BPlus_HideStockSelectionHighlight()
    -- The stock TradeSkill update positions its selection highlight before we rewrite
    -- the visible rows into our sorted order.  If we leave it alone, the gray bar
    -- can stay on the old physical row even though the selected TradeSkill index is correct.
    local frames = {
        "TradeSkillHighlightFrame",
        "TradeSkillHighlight",
        "TradeSkillSkillHighlightFrame",
        "TradeSkillSkillHighlight",
    };

    for i = 1, table.getn(frames) do
        local f = getglobal(frames[i]);
        if f and f.Hide then
            f:Hide();
        end
    end
end

local function BPlus_PositionSelectionHighlight(button)
    local tex = BPlus_EnsureSelectionHighlight();
    if not tex or not button then
        return;
    end

    tex:ClearAllPoints();
    tex:SetPoint("TOPLEFT", button, "TOPLEFT", 0, 0);
    tex:SetPoint("BOTTOMRIGHT", button, "BOTTOMRIGHT", 0, 0);
    tex:Show();
end

local function BPlus_HideSelectionHighlight()
    if BPT.SelectionHighlight then
        BPT.SelectionHighlight:Hide();
    end
end

local function BPlus_ClearButtonBeastTrainingState(button)
    if not button then
        return;
    end

    button.BPlusTaughtSpell = nil;

    if button.BPlusCostText then
        button.BPlusCostText:SetText("");
        button.BPlusCostText:Hide();
    end

    if button.UnlockHighlight then
        button:UnlockHighlight();
    end
end

local function BPlus_ClearBeastTrainingArtifacts()
    local maxRows = TRADE_SKILLS_DISPLAYED or 8;
    for i = 1, maxRows do
        BPlus_ClearButtonBeastTrainingState(getglobal("TradeSkillSkill" .. i));
    end

    if TradeSkillSkillIcon then
        TradeSkillSkillIcon.BPlusTaughtSpell = nil;
    end

    if BPT.PointsText then
        BPT.PointsText:SetText("");
        BPT.PointsText:Hide();
    end
    if BPT.MiddleIconOverlay then
        BPT.MiddleIconOverlay:Hide();
    end
    if BPT.ScanTooltip then
        BPT.ScanTooltip:Hide();
    end

    BPlus_HideSelectionHighlight();

    local descFrame = TradeSkillDescription or TradeSkillSkillDescription or TradeSkillDescriptionText;
    if descFrame and descFrame.GetText and descFrame.SetText then
        local text = descFrame:GetText();
        if text and string.find(text, "Training Point Cost:") then
            descFrame:SetText(string.gsub(text, "\n\nTraining Point Cost:.*", ""));
        end
    end
end

local function BPlus_ForceNormalTradeSkillRows()
    if not GetNumTradeSkills or not GetTradeSkillInfo or not TradeSkillListScrollFrame then
        return;
    end

    local numTradeSkills = GetNumTradeSkills() or 0;
    local offset = 0;
    if FauxScrollFrame_GetOffset then
        offset = FauxScrollFrame_GetOffset(TradeSkillListScrollFrame) or 0;
    end

    local selectedIndex = nil;
    if GetTradeSkillSelectionIndex then
        selectedIndex = GetTradeSkillSelectionIndex();
    end

    if TradeSkillHighlightFrame and TradeSkillHighlightFrame.Hide then
        TradeSkillHighlightFrame:Hide();
    end

    local maxRows = TRADE_SKILLS_DISPLAYED or 8;
    for i = 1, maxRows do
        local button = getglobal("TradeSkillSkill" .. i);
        if button then
            BPlus_ClearButtonBeastTrainingState(button);

            local skillIndex = i + offset;
            local skillName, skillType, numAvailable, isExpanded = GetTradeSkillInfo(skillIndex);
            local buttonName = button.GetName and button:GetName();
            local text = buttonName and getglobal(buttonName .. "Text");
            local subText = buttonName and getglobal(buttonName .. "SubText");
            local count = buttonName and getglobal(buttonName .. "Count");
            local highlight = buttonName and getglobal(buttonName .. "Highlight");

            if skillIndex <= numTradeSkills and skillName then
                local color = TradeSkillTypeColor and TradeSkillTypeColor[skillType];
                if color then
                    if button.SetNormalFontObject and color.font then
                        button:SetNormalFontObject(color.font);
                    end
                    button.r = color.r;
                    button.g = color.g;
                    button.b = color.b;

                    if text and text.SetTextColor then
                        text:SetTextColor(color.r, color.g, color.b);
                    end
                    if subText and subText.SetTextColor then
                        subText:SetTextColor(color.r, color.g, color.b);
                    end
                    if count and count.SetVertexColor then
                        count:SetVertexColor(color.r, color.g, color.b);
                    end
                end

                if skillType == "header" then
                    if isExpanded then
                        button:SetNormalTexture("Interface\\Buttons\\UI-MinusButton-Up");
                    else
                        button:SetNormalTexture("Interface\\Buttons\\UI-PlusButton-Up");
                    end
                    if highlight and highlight.SetTexture then
                        highlight:SetTexture("Interface\\Buttons\\UI-PlusButton-Hilight");
                    end
                else
                    button:SetNormalTexture("");
                    if highlight and highlight.SetTexture then
                        highlight:SetTexture("");
                    end
                end

                if selectedIndex == skillIndex then
                    if TradeSkillHighlightFrame and TradeSkillHighlightFrame.SetPoint and TradeSkillHighlightFrame.Show then
                        TradeSkillHighlightFrame:ClearAllPoints();
                        TradeSkillHighlightFrame:SetPoint("TOPLEFT", button, "TOPLEFT", 0, 0);
                        TradeSkillHighlightFrame:Show();
                    end
                    if TradeSkillHighlight and TradeSkillHighlight.SetVertexColor and color then
                        TradeSkillHighlight:SetVertexColor(color.r, color.g, color.b);
                    end
                    if count and count.SetVertexColor then
                        count:SetVertexColor(HIGHLIGHT_FONT_COLOR.r, HIGHLIGHT_FONT_COLOR.g, HIGHLIGHT_FONT_COLOR.b);
                    end
                    button:LockHighlight();
                    button.isHighlighted = true;
                else
                    button:UnlockHighlight();
                    button.isHighlighted = false;
                end
            end
        end
    end
end

local function BPlus_HideTradeSkillControlsForBeastTraining()
    BPlus_Hide(TradeSkillFilterButton);
    BPlus_Hide(TradeSkillOnlyShowMakeableButton);
    BPlus_Hide(TradeSkillFrameAvailableFilterCheckButton);
    BPlus_Hide(TradeSkillInvSlotDropDown);
    BPlus_Hide(TradeSkillSubClassDropDown);
    BPlus_Hide(TradeSkillFrameSearchBox);
    BPlus_Hide(TradeSkillLinkButton);
    BPlus_Hide(TradeSkillFrameFilterDropDown);
    BPlus_Hide(TradeSkillFrameFilterButton);
end

local function BPlus_RestoreTradeSkillControls()
    BPlus_ClearBeastTrainingArtifacts();
    BPlus_ForceNormalTradeSkillRows();

    BPlus_Show(TradeSkillFilterButton);
    BPlus_Show(TradeSkillOnlyShowMakeableButton);
    BPlus_Show(TradeSkillFrameAvailableFilterCheckButton);
    BPlus_Show(TradeSkillInvSlotDropDown);
    BPlus_Show(TradeSkillSubClassDropDown);
    BPlus_Show(TradeSkillFrameSearchBox);
    BPlus_Show(TradeSkillLinkButton);
    BPlus_Show(TradeSkillFrameFilterDropDown);

    if BPT.PointsText then
        BPT.PointsText:Hide();
    end
    if BPT.MiddleIconOverlay then
        BPT.MiddleIconOverlay:Hide();
    end
    BPlus_HideSelectionHighlight();
end

local function BPlus_GetSpellBookSpellID(index, bookType)
    local link;

    if GetSpellLink then
        link = GetSpellLink(index, bookType);
    end

    if (not link) and GetSpellBookItemLink then
        link = GetSpellBookItemLink(index, bookType);
    end

    return BPlus_GetSourceSpellFromLink(link);
end

local function BPlus_GetSpellBookNameRank(index, bookType)
    local spellName, spellRank;

    -- 3.3.5 usually has GetSpellBookItemName, but some custom clients/FrameXML
    -- still behave like older clients where GetSpellName is the reliable path.
    if GetSpellBookItemName then
        spellName, spellRank = GetSpellBookItemName(index, bookType);
    end

    if (not spellName) and GetSpellName then
        spellName, spellRank = GetSpellName(index, bookType);
    end

    return spellName, spellRank;
end

local function BPlus_EnsureTaughtSpellLookup()
    BPT.taughtSpellLookup = {};
    if not BPlusPetTrainingData then
        return;
    end

    for sourceSpell, data in pairs(BPlusPetTrainingData) do
        if data and data.taughtSpell then
            BPT.taughtSpellLookup[tonumber(data.taughtSpell)] = data;
        end

        if data then
            -- Harmless fallback if any pet spellbook link resolves to the source/training spell.
            BPT.taughtSpellLookup[tonumber(sourceSpell)] = data;
        end
    end
end

local function BPlus_RecordKnownPetTrainingData(data)
    if not data then
        return;
    end

    local rankText = data.rank or "";
    local key = (data.name or "") .. ":" .. rankText;
    BPT.petKnownNameRank[key] = true;

    local rankNumber = BPlus_ParseRankNumber(rankText);
    local knownRank = BPT.petKnownRankByName[data.name] or 0;
    local knownCost = BPT.petKnownCostByName[data.name] or 0;

    if rankNumber > knownRank then
        BPT.petKnownRankByName[data.name] = rankNumber;
    end

    if tonumber(data.cost or 0) > knownCost then
        BPT.petKnownCostByName[data.name] = tonumber(data.cost or 0);
    end
end


local function BPlus_RecordKnownPetTrainingNameUnknownRank(name)
    if not name or name == "" then
        return;
    end

    BPT.petKnownNameUnknown[name] = true;
end

local function BPlus_RebuildPetKnownCache()
    BPT.petKnownNameRank = {};
    BPT.petKnownRankByName = {};
    BPT.petKnownCostByName = {};
BPT.petKnownNameUnknown = {};

    BPlus_EnsureTaughtSpellLookup();

    local bookType = BOOKTYPE_PET or "pet";

    -- Do not break on the first nil spell.  Pet spellbooks can contain empty visual
    -- slots/holes in some 3.3.5 client builds, so breaking early makes the UI think
    -- the pet knows nothing and colors every row green.
    local maxSlots = 300;
    local blanksAfterSeen = 0;
    local sawAnySpell = false;

    for i = 1, maxSlots do
        local spellName, spellRank = BPlus_GetSpellBookNameRank(i, bookType);

        if spellName and spellName ~= "" then
            sawAnySpell = true;
            blanksAfterSeen = 0;
            spellRank = spellRank or "";

            -- Tooltip fallback is important on 3.3.5 custom clients: some pet spellbook
            -- APIs return a name but no rank/link. The tooltip usually still has Rank X.
            local tipName, tipRank = BPlus_GetTooltipInfoForSpellBookItem(i, bookType);
            if (not spellName or spellName == "") and tipName then
                spellName = tipName;
            end
            if (not spellRank or spellRank == "") and tipRank then
                spellRank = tipRank;
            end

            -- Preferred path: resolve the actual pet spell ID from the pet spellbook link.
            -- Beast Training data is keyed by source_spell, but the pet spellbook contains
            -- taughtSpell IDs, e.g. source 24510 teaches actual pet spell 24501.
            local spellID = BPlus_GetSpellBookSpellID(i, bookType);
            local data = spellID and BPT.taughtSpellLookup and BPT.taughtSpellLookup[tonumber(spellID)];

            -- Fallback path for clients that do not expose pet spellbook links.
            if not data and spellName then
                local key = spellName .. ":" .. (spellRank or "");
                local sourceSpell = BPlusPetTrainingLookupByNameRank and BPlusPetTrainingLookupByNameRank[key];
                data = sourceSpell and BPlusPetTrainingData[sourceSpell];
            end

            if data then
                BPlus_RecordKnownPetTrainingData(data);
            elseif spellName then
                -- Last-resort safety: if the pet spellbook exposes the ability name but
                -- hides the rank, do not color every row green. Treat the ability name
                -- as already known until the server-side state can be used.
                BPlus_RecordKnownPetTrainingNameUnknownRank(spellName);
            end
        elseif sawAnySpell then
            blanksAfterSeen = blanksAfterSeen + 1;
            if blanksAfterSeen >= 30 then
                break;
            end
        end
    end

    -- Extra fallback: scan pet action bar names.  It helps custom clients that
    -- expose active pet abilities on the action bar before the pet spellbook
    -- APIs are populated.
    --
    -- The rank comes from the action bar's OWN subtext, never from guesswork.
    -- This used to pick the highest-ranked entry that merely shared the name,
    -- and since the recorder keeps the maximum rank it is ever told about, one
    -- "Growl" button set the known rank to 7 - marking every rank as already
    -- known and hiding Growl 4 from a pet sitting on Growl 3. It only ever
    -- looked wrong for ACTIVE abilities, because passives like Great Stamina
    -- are not on the pet bar and so never reached this loop.
    --
    -- An entry we cannot resolve is now skipped rather than rounded up: the
    -- pet spellbook pass above has already recorded the real rank, and a bad
    -- guess here overwrites a good answer.
    if GetPetActionInfo then
        for slot = 1, 10 do
            local name, subtext, _, isToken = GetPetActionInfo(slot);
            -- isToken means name/subtext are UI tokens (Attack, Follow, the
            -- stance buttons), not a spell and not something to train.
            if name and name ~= "" and not isToken then
                local key = name .. ":" .. (subtext or "");
                local sourceSpell = BPlusPetTrainingLookupByNameRank
                    and BPlusPetTrainingLookupByNameRank[key];
                local data = sourceSpell and BPlusPetTrainingData[sourceSpell];
                if data then
                    BPlus_RecordKnownPetTrainingData(data);
                end
            end
        end
    end
end

local function BPlus_GetTrainingState(data, skillType)
    if skillType == "used" then
        return "known", 0.50, 0.50, 0.50, 0;
    elseif skillType == "unavailable" then
        return "unavailable", 1.00, 0.10, 0.10, 0;
    end

    if not data then
        return "normal", 1.0, 0.82, 0.0, 0;
    end

    if not BPlus_IsPetFamilyCompatible(data) then
        return "unavailable", 1.00, 0.10, 0.10, 0;
    end

    local rankNumber = BPlus_ParseRankNumber(data.rank);
    local knownRank = BPT.petKnownRankByName[data.name] or 0;
    local knownCost = BPT.petKnownCostByName[data.name] or 0;

    if data.name and BPT.petKnownNameUnknown and BPT.petKnownNameUnknown[data.name] then
        return "known", 0.50, 0.50, 0.50, 0;
    end

    if rankNumber > 0 and knownRank >= rankNumber then
        return "known", 0.50, 0.50, 0.50, 0;
    end

    local petLevel = UnitLevel("pet") or 0;
    local requiredLevel = tonumber(data.requiredLevel or 0) or 0;
    if requiredLevel > 0 and petLevel > 0 and petLevel < requiredLevel then
        return "unavailable", 1.00, 0.10, 0.10, 0;
    end

    local points = BPlus_GetTrainingPoints();
    local cost = tonumber(data.cost or 0) or 0;
    local needed = cost - knownCost;
    if needed < 0 then
        needed = 0;
    end

    if needed > points then
        return "unavailable", 1.00, 0.10, 0.10, needed;
    end

    return "available", 0.25, 0.75, 0.25, needed;
end

local function BPlus_RebuildSortedSkillList()
    BPT.sortedSkillList = {};

    if not GetNumTradeSkills or not GetTradeSkillInfo then
        return;
    end

    local numSkills = GetNumTradeSkills() or 0;
    for index = 1, numSkills do
        local name, skillType = GetTradeSkillInfo(index);
        if name and skillType ~= "header" then
            local data, sourceSpell = BPlus_GetTrainingDataForIndex(index);

            -- Hide junk/disabled Beast Training rows like Cobra Reflexes, and hide
            -- known-but-family-incompatible rows like Charge while a raptor is active.
            -- The server still rejects incompatible training; this just keeps the UI
            -- Classic-clean for the current pet.
            if data and BPlus_IsPetFamilyCompatible(data) then
                local sortName = data.name or name or "";
                local rankNumber = BPlus_ParseRankNumber(data.rank);
                local requiredLevel = tonumber(data.requiredLevel or 0) or 0;

                table.insert(BPT.sortedSkillList, {
                    index = index,
                    name = sortName,
                    rankNumber = rankNumber,
                    requiredLevel = requiredLevel,
                    sourceSpell = sourceSpell or 0,
                });
            end
        end
    end

    table.sort(BPT.sortedSkillList, function(a, b)
        local an = string.lower(a.name or "");
        local bn = string.lower(b.name or "");
        if an ~= bn then
            return an < bn;
        end
        if a.rankNumber ~= b.rankNumber then
            return a.rankNumber < b.rankNumber;
        end
        if a.requiredLevel ~= b.requiredLevel then
            return a.requiredLevel < b.requiredLevel;
        end
        return a.index < b.index;
    end);
end

local function BPlus_GetSelectedTradeSkillIndex()
    if TradeSkillFrame and TradeSkillFrame.selectedSkill then
        return TradeSkillFrame.selectedSkill;
    end

    if GetTradeSkillSelectionIndex then
        return GetTradeSkillSelectionIndex();
    end

    if TradeSkillFrame_GetSelection then
        return TradeSkillFrame_GetSelection();
    end

    return nil;
end

local function BPlus_UpdateRowCostsAndSortedRows()
    if not TradeSkillListScrollFrame then
        return;
    end

    BPlus_RebuildPetKnownCache();
    BPlus_RebuildSortedSkillList();

    if FauxScrollFrame_Update then
        FauxScrollFrame_Update(TradeSkillListScrollFrame, table.getn(BPT.sortedSkillList), TRADE_SKILLS_DISPLAYED or 8, TRADE_SKILL_HEIGHT or 16);
    end

    local offset = 0;
    if FauxScrollFrame_GetOffset then
        offset = FauxScrollFrame_GetOffset(TradeSkillListScrollFrame) or 0;
    end

    local selectedIndex = BPlus_GetSelectedTradeSkillIndex();
    local selectedVisible = false;
    BPlus_HideStockSelectionHighlight();
    BPlus_HideSelectionHighlight();

    local maxRows = TRADE_SKILLS_DISPLAYED or 8;
    for i = 1, maxRows do
        local button = getglobal("TradeSkillSkill" .. i);
        local costText = BPlus_EnsureRowCost(button);
        local sortedEntry = BPT.sortedSkillList[offset + i];

        if button and sortedEntry then
            BPlus_ClearButtonBeastTrainingState(button);

            local actualIndex = sortedEntry.index;
            local name, skillType = GetTradeSkillInfo(actualIndex);
            local data = BPlus_GetTrainingDataForIndex(actualIndex);
            local state, r, g, b = BPlus_GetTrainingState(data, skillType);
            local rank = data and data.rank or "";
            local displayName = data and data.name or name or "";

            if rank and rank ~= "" then
                button:SetText(" " .. displayName .. " (" .. rank .. ")");
            else
                button:SetText(" " .. displayName);
            end

            button:SetID(actualIndex);
            if data then
                BPlus_SetTaughtSpellTooltip(button, data);
            end
            BPlus_SetButtonTextColor(button, r, g, b);

            if selectedIndex == actualIndex then
                button:LockHighlight();
                BPlus_PositionSelectionHighlight(button);
                selectedVisible = true;
            else
                button:UnlockHighlight();
            end

            if costText and data and data.cost then
                costText:SetText(tostring(data.cost) .. " TP");
                costText:SetTextColor(r, g, b);
                costText:Show();
            elseif costText then
                costText:SetText("");
                costText:Hide();
            end

            button:Show();
        elseif button then
            BPlus_ClearButtonBeastTrainingState(button);
            button:Hide();
            if costText then
                costText:SetText("");
                costText:Hide();
            end
        end
    end
end

local function BPlus_UpdateSelectedDetails()
    local index = BPlus_GetSelectedTradeSkillIndex();
    if not index then
        return;
    end

    local data = BPlus_GetTrainingDataForIndex(index);
    if not data then
        return;
    end

    -- The backend TradeSkill row is the source/teaching spell, but Classic Beast
    -- Training should display the actual pet spell that will be learned.  Use
    -- data.taughtSpell for icon, name, rank, tooltip, and description.
    local taughtName, taughtRank, taughtIcon, taughtDescription = BPlus_GetSpellText(data.taughtSpell);
    taughtName = taughtName or data.name;
    taughtRank = taughtRank or data.rank or "";

    if TradeSkillSkillName and TradeSkillSkillName.SetText then
        if taughtRank and taughtRank ~= "" then
            TradeSkillSkillName:SetText(taughtName .. " (" .. taughtRank .. ")");
        else
            TradeSkillSkillName:SetText(taughtName or "");
        end
    end

    if taughtIcon then
        BPlus_SetDetailIconToTaughtSpell(taughtIcon, data);
    elseif TradeSkillSkillIcon then
        BPlus_SetTaughtSpellTooltip(TradeSkillSkillIcon, data);
    end

    local descFrame = TradeSkillDescription or TradeSkillSkillDescription or TradeSkillDescriptionText;
    if descFrame and descFrame.SetText then
        local base = taughtDescription;
        if not base or base == "" then
            if descFrame.GetText then
                base = descFrame:GetText() or "";
                base = string.gsub(base, "\n\nTraining Point Cost:.-$", "");
            else
                base = "";
            end
        end

        local cost = tonumber(data.cost) or 0;
        local req = tonumber(data.requiredLevel) or 0;
        local _, skillType = GetTradeSkillInfo(index);
        local state, r, g, b, needed = BPlus_GetTrainingState(data, skillType);

        local extra = "\n\nTraining Point Cost: " .. tostring(cost);
        if needed and needed > 0 and needed ~= cost then
            extra = extra .. " (" .. tostring(needed) .. " more)";
        end
        if req > 0 then
            extra = extra .. "\nRequires pet level " .. tostring(req);
        end
        if state == "known" then
            extra = extra .. "\nAlready known by your pet.";
        elseif state == "unavailable" then
            extra = extra .. "\nCannot train right now.";
        end

        descFrame:SetText(base .. extra);
    end

    if TradeSkillCreateButton then
        local _, skillType = GetTradeSkillInfo(index);
        local state = BPlus_GetTrainingState(data, skillType);
        if state == "available" then
            TradeSkillCreateButton:Enable();
        else
            TradeSkillCreateButton:Disable();
        end
    end
end

local function BPlus_UpdateBeastTrainingFrame()
    if not TradeSkillFrame then
        return;
    end

    if BPT.stockRefreshInProgress then
        return;
    end

    if not BPlus_IsBeastTrainingTradeSkill() then
        local wasBeastTraining = BPT.beastTrainingWasActive;
        BPT.beastTrainingWasActive = false;
        BPlus_RestoreTradeSkillControls();

        -- If we just switched away from Beast Training, force the stock TradeSkill UI
        -- to rebuild the reused row buttons after our cleanup.  This restores normal
        -- profession text colors, selected-row background, hover state, and tooltips.
        if wasBeastTraining and TradeSkillFrame_Update then
            BPT.stockRefreshInProgress = true;
            TradeSkillFrame_Update();
            BPT.stockRefreshInProgress = false;
            BPlus_ClearBeastTrainingArtifacts();
            BPlus_ForceNormalTradeSkillRows();
        end
        return;
    end

    BPT.beastTrainingWasActive = true;

    BPlus_HideTradeSkillControlsForBeastTraining();

    BPlus_SetFrameText("TradeSkillFrameTitleText", "Beast Training");

    local points, maxPoints = BPlus_GetTrainingPoints();

    BPlus_SetFrameText("TradeSkillRankFrameSkillName", "");
    BPlus_SetFrameText("TradeSkillRankFrameSkillRank", "");

    local pointsText = BPlus_EnsurePointsText();
    if pointsText then
        pointsText:SetText("Training Points: " .. tostring(points) .. "/" .. tostring(maxPoints));
        pointsText:Show();
    end

    if TradeSkillCreateButton then
        TradeSkillCreateButton:SetText("Train");
    end
    if TradeSkillCancelButton then
        TradeSkillCancelButton:SetText(EXIT or "Exit");
    end

    BPlus_UpdateRowCostsAndSortedRows();
    BPlus_UpdateSelectedDetails();

    BPlus_SetFrameText("TradeSkillRankFrameSkillName", "");
    BPlus_SetFrameText("TradeSkillRankFrameSkillRank", "");
    if BPT.PointsText then
        BPT.PointsText:SetText("Training Points: " .. tostring(points) .. "/" .. tostring(maxPoints));
        BPT.PointsText:Show();
    end
end

local function BPlus_HookTradeSkillFrame()
    if BPT.hooked then
        return;
    end

    if not TradeSkillFrame then
        return;
    end

    BPT.hooked = true;

    if TradeSkillFrame_Update then
        hooksecurefunc("TradeSkillFrame_Update", BPlus_UpdateBeastTrainingFrame);
    end

    if TradeSkillFrame_SetSelection then
        hooksecurefunc("TradeSkillFrame_SetSelection", BPlus_UpdateBeastTrainingFrame);
    end

    if TradeSkillFrame_Show then
        hooksecurefunc("TradeSkillFrame_Show", BPlus_UpdateBeastTrainingFrame);
    end

    if TradeSkillSkillButton_OnClick then
        hooksecurefunc("TradeSkillSkillButton_OnClick", BPlus_UpdateBeastTrainingFrame);
    end

    BPlus_UpdateBeastTrainingFrame();
end

BPT:SetScript("OnEvent", function(self, event, arg1)
    if event == "ADDON_LOADED" then
        if arg1 == "Blizzard_TradeSkillUI" or arg1 == "Blizzard_TradeSkill" or arg1 == "Blizzard_CraftUI" then
            BPlus_HookTradeSkillFrame();
        end
        return;
    end

    if event == "TRADE_SKILL_CLOSE" then
        BPlus_ClearBeastTrainingArtifacts();
        BPT.beastTrainingWasActive = false;
        return;
    end

    BPlus_HookTradeSkillFrame();

    if event == "TRADE_SKILL_SHOW" or event == "TRADE_SKILL_UPDATE" or event == "UNIT_PET" or event == "PET_BAR_UPDATE" or event == "SPELLS_CHANGED" then
        BPlus_UpdateBeastTrainingFrame();
    end
end);

BPlus_IsBeastTrainingTradeSkill = BPlus_IsBeastTrainingTradeSkill;
BPlus_UpdateBeastTrainingFrame = BPlus_UpdateBeastTrainingFrame;
