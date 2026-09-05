--=============================================================================
-- Centurion Auto Auction
--
-- Posts the gear out of your bags without you naming a price, because the price
-- is worked out for you.
--
-- Green, blue and epic EQUIPMENT only. An equip slot is what separates a green
-- sword from a green recipe, and the quality floor throws out the grey trash, so
-- between them the ore, the cloth, the reagents, the recipes and the vendor junk
-- are all left where they are. Both ends are settings (/aa gear, /aa quality) if
-- a realm wants the whole bag posted instead.
--
-- Two ways to arrive at a price, and the second wins whenever it can:
--
--   1. A multiple of the deposit. The deposit is the only number the client
--      already knows about an item's worth before anyone has listed it -
--      CalculateAuctionDeposit() reads it straight off the lot sitting in the
--      sell slot - so it is what the price is built from. Ten times deposit for
--      the buyout, five for the opening bid, by default.
--
--   2. An undercut. With the flag on, the addon searches the auction house for
--      the item first and prices a fixed number of COPPER under the cheapest
--      listing instead. Flat rather than a percentage on purpose: a percentage
--      takes a slice of an ever smaller number every pass and walks the whole
--      market down to the vendor floor, where a fixed step just moves the price
--      down by that much. This overrides rule 1, and only while there is
--      something to undercut: an item nobody else is selling falls back to the
--      deposit multiple.
--
-- Prices are compared PER ITEM, never per lot, so a stack of twenty never reads
-- as expensive next to a single one.
--
--   /aa            settings
--   /aa sell       post the bags
--   /aa undercut   the flag
--=============================================================================

local ADDON_NAME    = "CENTURION_AutoAuction"
local MAX_MONEY     = 2147483646        -- the server's own ceiling on a price
local PAGE_SIZE     = 50                -- auction results per query page
local TICK          = 0.15              -- seconds between state machine steps
local POST_TIMEOUT  = 4.0               -- seconds to wait for a post to land
local QUERY_TIMEOUT = 10.0              -- seconds to wait for a search to answer
local MAX_FAILURES  = 3                 -- consecutive failures before giving up
local MAX_ATTEMPTS  = 3                 -- tries at one item before leaving it alone

-- Equipment is anything with an equip slot, which is exactly what separates a
-- green sword from a green recipe - except for containers, which have one and
-- are not what anybody means by gear.
local EXCLUDED_EQUIP_SLOTS = {
    ["INVTYPE_BAG"]       = true,
    ["INVTYPE_QUIVER"]    = true,
    ["INVTYPE_NON_EQUIP"] = true,
}

-- Duration is an index, not a number of hours: 1 = 12h, 2 = 24h, 3 = 48h. Both
-- CalculateAuctionDeposit and StartAuction take it in that form.
local DURATION_HOURS   = { [1] = 12, [2] = 24, [3] = 48 }
local DURATION_INDEX   = { [12] = 1, [24] = 2, [48] = 3 }
-- What the deposit is multiplied by at each duration, for normalising it back
-- to the twelve hour figure.
local DURATION_BLOCKS  = { [1] = 1,  [2] = 2,  [3] = 4 }

local defaults = {
    buyoutMult    = 10,     -- buyout = this x deposit
    bidMult       = 5,      -- opening bid = this x deposit
    duration      = 1,      -- 1 = 12h, 2 = 24h, 3 = 48h
    normalize     = false,  -- price off the 12h deposit whatever the duration
    minDeposit    = 100,    -- deposit floor the multiple is taken from, in copper
    minBuyout     = 1,      -- copper floor on the finished asking price

    gearOnly      = true,   -- only things you can wear or wield
    minQuality    = 2,      -- 2 = Uncommon (green)
    maxQuality    = 4,      -- 4 = Epic (purple)

    undercut      = false,  -- THE FLAG: price against the market, not the deposit
    -- Undercutting is a FLAT amount of copper, not a percentage. A percentage
    -- walks the market down geometrically - each pass takes a slice of an
    -- ever smaller number - where a fixed step just moves the price down by
    -- that much. It is the same rule the bots undercut by. The percentage is
    -- kept at zero rather than removed so an existing setting still works.
    undercutPct   = 0,      -- percent off the cheapest listing, per item
    undercutFlat  = 1,      -- COPPER off the cheapest listing, per item
    undercutBid   = 80,     -- opening bid as a percent of an undercut buyout
    undercutFloor = 1,      -- never undercut below this multiple of the deposit
    maxPages      = 3,      -- search pages to read per item when undercutting

    auto          = false,  -- start a run as soon as the auction house opens
    panel         = true,   -- show the little window at the auction house
    verbose       = true,   -- one chat line per listing

    skip          = {},     -- [itemId] = item name, never posted
    point         = nil,    -- where the panel was dragged to
}

local db                                -- SavedVariables, filled in on load
local playerName
local atAH          = false             -- between AUCTION_HOUSE_SHOW and _CLOSED

-- Run state -----------------------------------------------------------------
local running       = false
local state         = "idle"
local timer         = 0
local deadline      = 0
local filterText    = nil
local posted        = 0
local skipped       = 0
local failures      = 0
local failedIds     = {}                -- item ids that would not post this run
local attempts      = {}                -- [itemId] = tries since its last success
local priceCache    = {}                -- [itemId] = cheapest rival per item, 0 = none
local current       = nil               -- the lot being worked on
local queryItemId   = nil
local queryName     = nil
local queryPage     = 0
local queryBest     = nil
local queryWaiting  = false
local warnedDeposit = false

-- Two frames on purpose. The driver is parentless and never hidden, because a
-- hidden frame gets no OnUpdate and the run would stall the moment somebody
-- turned the panel off or hid the interface.
local frame                             -- driver: events and the ticker
local panel                             -- the little window at the auction house
local statusText, ruleText, sellButton, undercutBox, buyoutBox, bidBox, undercutFlatBox

--=============================================================================
-- Small helpers
--=============================================================================

local function Print(msg)
    DEFAULT_CHAT_FRAME:AddMessage("|cff33ff99Auto Auction|r: " .. tostring(msg))
end

local function FormatMoney(copper)
    copper = math.floor(tonumber(copper) or 0)

    local gold   = math.floor(copper / 10000)
    local silver = math.floor((copper % 10000) / 100)
    local rest   = copper % 100
    local text   = ""

    if gold > 0 then
        text = gold .. "g"
    end
    if silver > 0 then
        text = (text ~= "" and (text .. " ") or "") .. silver .. "s"
    end
    if rest > 0 or text == "" then
        text = (text ~= "" and (text .. " ") or "") .. rest .. "c"
    end

    return text
end

-- Multipliers are shown without a pointless ".0" on the whole numbers most of
-- them are.
local function NumText(n)
    n = tonumber(n) or 0
    if n == math.floor(n) then
        return tostring(math.floor(n))
    end
    return tostring(n)
end

local function QualityName(index)
    local name = _G["ITEM_QUALITY" .. index .. "_DESC"] or tostring(index)
    local color = ITEM_QUALITY_COLORS[index]
    return (color and color.hex or "") .. name .. "|r"
end

-- One line saying what a run will touch, e.g. "Uncommon to Epic gear".
local function ScopeText()
    local range
    if db.minQuality == db.maxQuality then
        range = QualityName(db.minQuality)
    else
        range = QualityName(db.minQuality) .. " to " .. QualityName(db.maxQuality)
    end

    return range .. (db.gearOnly and " gear" or " items")
end

-- "5g", "50s", "250c" or a bare number of copper.
local function ParseMoney(text)
    if not text then
        return nil
    end

    local amount, suffix = string.match(string.lower(text), "^(%d+%.?%d*)%s*([gsc]?)$")
    if not amount then
        return nil
    end

    amount = tonumber(amount)
    if not amount then
        return nil
    end

    if suffix == "g" then
        return math.floor(amount * 10000)
    elseif suffix == "s" then
        return math.floor(amount * 100)
    end

    return math.floor(amount)
end

local function ItemIdFromLink(link)
    if not link then
        return nil
    end

    local id = string.match(link, "item:(%d+)")
    return id and tonumber(id) or nil
end

-- Tracked from the events rather than read off AuctionFrame: the frame belongs
-- to Blizzard_AuctionUI, which is load-on-demand and may not exist yet.
local function AtAuctionHouse()
    return atAH
end

--=============================================================================
-- What may be sold
--
-- The client cannot see everything the server checks, so this errs towards
-- leaving an item alone. Anything that slips through is refused by the server
-- and the run just moves on to the next lot.
--=============================================================================

local scanTip = CreateFrame("GameTooltip", "CenturionAutoAuctionScanTip", nil, "GameTooltipTemplate")
scanTip:SetOwner(UIParent, "ANCHOR_NONE")

-- An item id counted as skipped once, and never looked at again this run.
local function MarkSkipped(id)
    if not failedIds[id] then
        failedIds[id] = true
        skipped = skipped + 1
    end
end

local function IsUnsellable(bag, slot)
    scanTip:SetOwner(UIParent, "ANCHOR_NONE")
    scanTip:ClearLines()
    scanTip:SetBagItem(bag, slot)

    for i = 1, scanTip:NumLines() do
        local line = _G["CenturionAutoAuctionScanTipTextLeft" .. i]
        local text = line and line:GetText()

        if text then
            -- Soulbound and conjured items cannot be auctioned at all. An item
            -- reading "Binds when picked up" while sitting in your bag has
            -- already bound, whatever the tooltip calls it.
            if text == ITEM_SOULBOUND
                or text == ITEM_BIND_ON_PICKUP
                or text == ITEM_CONJURED
                or text == ITEM_BIND_QUEST then
                return true
            end
        end
    end

    return false
end

-- The next lot worth posting, scanned fresh each time: posting shifts items
-- around the bags, so a list built up front goes stale immediately.
local function NextCandidate()
    for bag = 0, NUM_BAG_SLOTS do
        local slots = GetContainerNumSlots(bag) or 0

        for slot = 1, slots do
            local _, count, locked = GetContainerItemInfo(bag, slot)
            local link = GetContainerItemLink(bag, slot)
            local id   = ItemIdFromLink(link)

            if link and id and not locked then
                local name, _, quality, _, _, _, _, _, equipSlot = GetItemInfo(link)

                local eligible = name ~= nil
                    and not db.skip[id]
                    and not failedIds[id]

                if eligible and filterText then
                    eligible = string.find(string.lower(name), filterText, 1, true) ~= nil
                end

                -- Gear and quality first, before the tooltip read below: this is
                -- what throws out the grey vendor trash, the ore, the recipes and
                -- the reagents, and it costs two comparisons to do it.
                if eligible and db.gearOnly then
                    eligible = equipSlot ~= nil and equipSlot ~= ""
                        and not EXCLUDED_EQUIP_SLOTS[equipSlot]
                end

                if eligible then
                    eligible = quality ~= nil
                        and quality >= db.minQuality
                        and quality <= db.maxQuality
                end

                -- Marked by id so the tooltip is read once per item, not once
                -- per item per pass - the bags are rescanned after every post.
                if eligible and IsUnsellable(bag, slot) then
                    MarkSkipped(id)
                    eligible = false
                end

                if eligible then
                    return { bag = bag, slot = slot, link = link, id = id,
                             name = name, count = count or 1 }
                end
            end
        end
    end

    return nil
end

--=============================================================================
-- Pricing
--=============================================================================

-- Returns buyout, opening bid, and whether the market decided it.
local function ComputePrices(deposit, count, itemId)
    local basis = deposit or 0

    -- The deposit already scales with how long the lot is up - twice for a day,
    -- four times for two days - so a straight multiple would charge four times
    -- as much for the same item purely because you picked the longest listing.
    if db.normalize then
        basis = math.floor(basis / (DURATION_BLOCKS[db.duration] or 1))
    end

    if basis < db.minDeposit then
        basis = db.minDeposit
    end

    local buyout, bid, undercut

    -- Rule 2 first, because when it applies it overrides rule 1 outright.
    local rival = db.undercut and priceCache[itemId] or nil
    if rival and rival > 0 then
        local perItem = rival * (100 - db.undercutPct) / 100 - db.undercutFlat
        if perItem < 1 then
            perItem = 1
        end

        buyout = math.floor(perItem * count)

        -- Never undercut below what the lot cost to post. The point is to win
        -- the sale, not to race the market to one copper and lose the deposit
        -- on every listing.
        local floorPrice = math.floor(basis * db.undercutFloor)
        if buyout < floorPrice then
            buyout = floorPrice
        end

        -- The opening bid tracks the asking price here rather than the deposit:
        -- the whole point of an undercut is that the market decides the worth.
        bid = math.floor(buyout * db.undercutBid / 100)
        undercut = true
    else
        buyout = math.floor(basis * db.buyoutMult)
        bid    = math.floor(basis * db.bidMult)
        undercut = false
    end

    if buyout < db.minBuyout then buyout = db.minBuyout end
    if buyout > MAX_MONEY   then buyout = MAX_MONEY end

    -- An opening bid above the buyout would let the first bidder pay more than
    -- the asking price, and a zero one is not a legal listing.
    if bid < 1      then bid = 1 end
    if bid > buyout then bid = buyout end

    return buyout, bid, undercut
end

--=============================================================================
-- Market search, for the undercut flag
--=============================================================================

local function SendQuery()
    if not CanSendAuctionQuery() then
        return false
    end

    SortAuctionClearSort("list")
    QueryAuctionItems(queryName, "", "", nil, 0, 0, queryPage, nil, nil)
    queryWaiting = true

    return true
end

local function StartQuery(itemId, name)
    queryItemId  = itemId
    queryName    = name
    queryPage    = 0
    queryBest    = nil
    queryWaiting = false

    state    = "query"
    deadline = GetTime() + QUERY_TIMEOUT
end

local function FinishQuery()
    priceCache[queryItemId] = queryBest or 0
    queryWaiting = false
    state = "post"
end

local function OnAuctionListUpdate()
    if state ~= "query" or not queryWaiting then
        return
    end

    queryWaiting = false

    local numBatch, total = GetNumAuctionItems("list")
    numBatch = numBatch or 0
    total    = total or 0

    for i = 1, numBatch do
        local name, _, count, _, _, _, _, _, buyoutPrice, _, _, owner = GetAuctionItemInfo("list", i)

        -- Searching by name is a substring match, so only an exact name is this
        -- item. A lot with no buyout has no asking price to undercut, and one of
        -- your own is not competition - undercutting yourself walks your own
        -- price down every time you repost.
        if name == queryName and buyoutPrice and buyoutPrice > 0 and count and count > 0
            and owner ~= playerName then

            local perItem = math.floor(buyoutPrice / count)
            if perItem > 0 and (not queryBest or perItem < queryBest) then
                queryBest = perItem
            end
        end
    end

    if ((queryPage + 1) * PAGE_SIZE) < total and (queryPage + 1) < db.maxPages then
        queryPage = queryPage + 1
        deadline = GetTime() + QUERY_TIMEOUT
    else
        FinishQuery()
    end
end

--=============================================================================
-- The run
--=============================================================================

local function UpdateStatus(text)
    if statusText then
        statusText:SetText(text or "")
    end
end

-- Pushes the settings out to the panel. Called after anything changes one,
-- whether that was a slash command or the panel itself, so the two can never
-- disagree. A box the player is currently typing into is left alone.
local function UpdateRule()
    if buyoutBox and not buyoutBox:HasFocus() then
        buyoutBox:SetText(NumText(db.buyoutMult))
    end

    if bidBox and not bidBox:HasFocus() then
        bidBox:SetText(NumText(db.bidMult))
    end

    if undercutFlatBox and not undercutFlatBox:HasFocus() then
        undercutFlatBox:SetText(NumText(db.undercutFlat))
    end

    if undercutBox then
        undercutBox:SetChecked(db.undercut)
    end

    if not ruleText then
        return
    end

    if db.undercut then
        -- The flat amount leads, because it is the one with a box. A leftover
        -- percentage is still shown when somebody has one saved, so a price
        -- that is not simply "lowest minus N" says so rather than looking wrong.
        local by = FormatMoney(db.undercutFlat)
        if db.undercutPct > 0 then
            by = by .. " + " .. db.undercutPct .. "%"
        end
        if db.undercutFlat == 0 and db.undercutPct == 0 then
            by = "matching the lowest"
        end
        ruleText:SetText(ScopeText() .. " |cff888888- undercutting by " .. by .. "|r")
    else
        ruleText:SetText(ScopeText() .. " |cff888888only|r")
    end
end

local function StopRun(reason)
    running    = false
    state      = "idle"
    current    = nil
    filterText = nil

    if sellButton then
        sellButton:SetText("Sell Bags")
    end

    local summary = "posted " .. posted
    if skipped > 0 then
        summary = summary .. ", skipped " .. skipped
    end

    UpdateStatus(reason and (reason .. " - " .. summary) or summary)
    Print((reason and (reason .. ". ") or "Finished. ") .. "Posted " .. posted ..
          " auction" .. (posted == 1 and "" or "s") ..
          (skipped > 0 and (", skipped " .. skipped .. " item" .. (skipped == 1 and "" or "s")) or "") .. ".")
end

local function ClearSellSlot()
    if GetAuctionSellItemInfo() then
        ClickAuctionSellItemButton()
        ClearCursor()
    end
end

local function PlaceCurrent()
    ClearSellSlot()
    ClearCursor()
    PickupContainerItem(current.bag, current.slot)

    if GetCursorInfo() ~= "item" then
        ClearCursor()
        return false
    end

    ClickAuctionSellItemButton()
    ClearCursor()

    return true
end

local function PostCurrent()
    local slotName, _, slotCount = GetAuctionSellItemInfo()
    if not slotName then
        return false
    end

    slotCount = slotCount or current.count or 1

    local deposit = CalculateAuctionDeposit(db.duration) or 0
    if deposit == 0 and not warnedDeposit then
        warnedDeposit = true
        Print("|cffff8800This realm charges no deposit|r, so prices fall back to the " ..
              FormatMoney(db.minDeposit) .. " floor (/aa mindeposit). Undercutting is unaffected.")
    end

    local buyout, bid, undercut = ComputePrices(deposit, slotCount, current.id)

    StartAuction(bid, buyout, db.duration, slotCount, 1)

    current.buyout   = buyout
    current.bid      = bid
    current.undercut = undercut
    current.posted   = slotCount

    return true
end

local function AnnouncePosted()
    if not db.verbose then
        return
    end

    local how
    if current.undercut then
        local rival = priceCache[current.id] or 0
        how = "undercutting " .. FormatMoney(rival) .. " each"
    else
        how = NumText(db.buyoutMult) .. "x deposit"
    end

    Print(current.link .. " x" .. current.posted .. " at " .. FormatMoney(current.buyout) ..
          " |cff888888(bid " .. FormatMoney(current.bid) .. ", " .. how .. ")|r")
end

local function Step()
    if state == "next" then
        current = NextCandidate()

        if not current then
            StopRun(nil)
            return
        end

        -- A lot the server quietly refuses comes straight back to the bag, where
        -- the next scan finds it again. Counting tries is what stops that being
        -- a loop; a successful post clears the count, so five stacks of the same
        -- cloth still all go up.
        attempts[current.id] = (attempts[current.id] or 0) + 1
        if attempts[current.id] > MAX_ATTEMPTS then
            MarkSkipped(current.id)
            state = "next"
            return
        end

        UpdateStatus("Listing " .. (current.name or "?"))

        if not PlaceCurrent() then
            MarkSkipped(current.id)
            state = "next"
            return
        end

        state    = "placed"
        deadline = GetTime() + POST_TIMEOUT

    elseif state == "placed" then
        local slotName = GetAuctionSellItemInfo()

        if not slotName then
            if GetTime() > deadline then
                MarkSkipped(current.id)
                state = "next"
            end
            return
        end

        -- Undercutting needs to know what the item is already going for, and one
        -- search answers for every stack of it in this run.
        if db.undercut and priceCache[current.id] == nil then
            UpdateStatus("Checking the market for " .. (current.name or "?"))
            StartQuery(current.id, slotName)
        else
            state = "post"
        end

    elseif state == "query" then
        if queryWaiting then
            if GetTime() > deadline then
                -- The search never answered. Fall back to the deposit multiple
                -- rather than stalling the whole run on one item.
                queryBest = nil
                FinishQuery()
            end
            return
        end

        if not SendQuery() then
            if GetTime() > deadline then
                queryBest = nil
                FinishQuery()
            end
            return
        end

        deadline = GetTime() + QUERY_TIMEOUT

    elseif state == "post" then
        -- Stop before the purse is empty, not after.
        --
        -- The deposit is charged per lot, so a long run spends money the whole
        -- way down and the last few posts are the ones that cannot be paid for.
        -- Ending the run here is the honest answer: the alternative is to keep
        -- trying, be refused, and skip item after item for a reason that has
        -- nothing to do with the items.
        local needed = CalculateAuctionDeposit(db.duration) or 0
        if needed > 0 and GetMoney() < needed then
            ClearSellSlot()
            StopRun("Out of gold for the deposit (" .. FormatMoney(needed) .. " needed)")
            return
        end

        if not PostCurrent() then
            MarkSkipped(current.id)
            state = "next"
            return
        end

        state    = "posting"
        deadline = GetTime() + POST_TIMEOUT

    elseif state == "posting" then
        -- An empty sell slot is NOT proof the auction was created.
        --
        -- It reads that way, and that was the bug: the slot is equally empty
        -- when the server REFUSED the lot and handed the item straight back to
        -- the bag. The refusal that matters is "you cannot afford the deposit",
        -- which is exactly the state a run ends up in after it has spent the
        -- purse posting - so the addon announced a post that never happened,
        -- reset this item's attempt count because it thought it had succeeded,
        -- rescanned the bags, found the very same item, and did it again. That
        -- is the wall of identical "posted" lines for an item that never
        -- reached the auction house, and it never stopped because a success
        -- always clears the counter that exists to stop it.
        --
        -- What actually proves a post is the ITEM BEING GONE. Checked by id
        -- rather than by "is anything there", so a stack that was split, or a
        -- slot something else has since fallen into, cannot read as success.
        local goneFromBag = ItemIdFromLink(GetContainerItemLink(current.bag, current.slot)) ~= current.id

        if not GetAuctionSellItemInfo() and goneFromBag then
            posted   = posted + 1
            failures = 0
            attempts[current.id] = 0
            AnnouncePosted()
            UpdateStatus("Posted " .. posted .. (skipped > 0 and (", skipped " .. skipped) or ""))
            state = "next"
            return
        end

        if GetTime() > deadline then
            ClearSellSlot()
            MarkSkipped(current.id)
            failures = failures + 1

            if failures >= MAX_FAILURES then
                StopRun("Stopped: " .. failures .. " listings in a row were refused")
                return
            end

            state = "next"
        end
    end
end

local function StartRun(filter)
    if running then
        StopRun("Stopped")
        return
    end

    if not AtAuctionHouse() then
        Print("Open an auctioneer first.")
        return
    end

    filterText = filter and string.lower(filter) or nil
    posted     = 0
    skipped    = 0
    failures   = 0
    failedIds  = {}
    attempts   = {}
    priceCache = {}
    current    = nil
    running    = true
    state      = "next"
    timer      = 0

    if sellButton then
        sellButton:SetText("Stop")
    end

    Print("Selling " .. ScopeText() .. (filterText and (" matching '" .. filter .. "'") or "") ..
          " - " .. (db.undercut and "undercutting" or (NumText(db.buyoutMult) .. "x deposit")) ..
          ", " .. (DURATION_HOURS[db.duration] or 12) .. "h listings.")

    -- The first step runs from this click, which is a hardware event; the rest
    -- follow from the ticker.
    Step()
end

--=============================================================================
-- Panel
--=============================================================================

local function BuildPanel()
    panel = CreateFrame("Frame", "CenturionAutoAuctionPanel", UIParent)
    panel:SetWidth(252)
    panel:SetHeight(214)
    panel:SetBackdrop({
        bgFile   = "Interface\\DialogFrame\\UI-DialogBox-Background",
        edgeFile = "Interface\\DialogFrame\\UI-DialogBox-Border",
        tile = true, tileSize = 32, edgeSize = 32,
        insets = { left = 11, right = 12, top = 12, bottom = 11 },
    })
    panel:SetMovable(true)
    panel:EnableMouse(true)
    panel:RegisterForDrag("LeftButton")
    panel:SetScript("OnDragStart", function(self) self:StartMoving() end)
    panel:SetScript("OnDragStop", function(self)
        self:StopMovingOrSizing()
        local point, _, relPoint, x, y = self:GetPoint()
        db.point = { point, relPoint, x, y }
    end)

    local title = panel:CreateFontString(nil, "OVERLAY", "GameFontNormal")
    title:SetPoint("TOP", 0, -14)
    title:SetText("Auto Auction")

    -- One numeric field per multiplier. Committed on enter or on losing focus,
    -- reverted on escape, and rejected outright if it is not a positive number -
    -- a zero or a stray letter here would post the whole run at one copper.
    -- allowZero: the undercut amount is legitimately zero (match the lowest
    -- price rather than beat it), where a multiplier of zero would post the
    -- whole run at one copper.
    local function MakeNumberBox(key, label, yOffset, suffixText, allowZero)
        local caption = panel:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
        caption:SetPoint("TOPLEFT", 20, yOffset - 4)
        caption:SetText(label)

        local box = CreateFrame("EditBox", "CenturionAutoAuction" .. key, panel, "InputBoxTemplate")
        box:SetWidth(46)
        box:SetHeight(18)
        box:SetPoint("TOPLEFT", 82, yOffset)
        box:SetAutoFocus(false)
        box:SetMaxLetters(7)
        box:SetJustifyH("RIGHT")

        local suffix = panel:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
        suffix:SetPoint("LEFT", box, "RIGHT", 8, 0)
        suffix:SetText(suffixText)

        local function Commit()
            local value = tonumber(box:GetText())
            if value and (value > 0 or (allowZero and value == 0)) then
                db[key] = value
                -- The cache holds prices worked out under the OLD undercut, so
                -- it has to go or the next run posts at yesterday's number.
                if allowZero then
                    priceCache = {}
                end
            end

            box:ClearFocus()
            UpdateRule()             -- writes the stored value back into the box
        end

        box:SetScript("OnEnterPressed", Commit)
        box:SetScript("OnEditFocusLost", Commit)
        box:SetScript("OnEscapePressed", function()
            box:SetText(NumText(db[key]))
            box:ClearFocus()
        end)

        return box
    end

    buyoutBox = MakeNumberBox("buyoutMult", "Buyout", -38, "x the deposit")
    bidBox    = MakeNumberBox("bidMult",    "Bid",    -62, "x the deposit")

    undercutBox = CreateFrame("CheckButton", "CenturionAutoAuctionUndercut", panel, "UICheckButtonTemplate")
    undercutBox:SetWidth(24)
    undercutBox:SetHeight(24)
    undercutBox:SetPoint("TOPLEFT", 18, -88)

    local boxLabel = _G["CenturionAutoAuctionUndercutText"]
    if boxLabel then
        boxLabel:SetFontObject(GameFontHighlightSmall)
        boxLabel:SetText("Undercut the market")
    end

    undercutBox:SetScript("OnClick", function(self)
        db.undercut = self:GetChecked() and true or false
        priceCache = {}
        UpdateRule()
    end)

    -- Flat copper, not a percentage. A percentage walks the market down
    -- geometrically - every pass takes a slice of an ever smaller number, so
    -- prices collapse toward the vendor floor. A fixed step just moves the
    -- price down by that much, which is the same rule the bots undercut by.
    undercutFlatBox = MakeNumberBox("undercutFlat", "Undercut", -112,
        "copper below the lowest", true)

    ruleText = panel:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    ruleText:SetPoint("TOPLEFT", 22, -138)
    ruleText:SetPoint("TOPRIGHT", -16, -138)
    ruleText:SetJustifyH("LEFT")

    sellButton = CreateFrame("Button", nil, panel, "UIPanelButtonTemplate")
    sellButton:SetWidth(96)
    sellButton:SetHeight(22)
    sellButton:SetPoint("BOTTOMLEFT", 18, 16)
    sellButton:SetText("Sell Bags")
    sellButton:SetScript("OnClick", function() StartRun(nil) end)

    statusText = panel:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
    statusText:SetPoint("BOTTOMRIGHT", -16, 22)
    statusText:SetPoint("BOTTOMLEFT", 124, 22)
    statusText:SetJustifyH("RIGHT")

    if db.point then
        panel:SetPoint(db.point[1], UIParent, db.point[2], db.point[3], db.point[4])
    else
        panel:SetPoint("CENTER", UIParent, "CENTER", 300, 0)
    end

    panel:Hide()
end

--=============================================================================
-- Slash commands
--=============================================================================

local function ShowSettings()
    Print("selling " .. ScopeText())
    Print("buyout |cffffff00" .. NumText(db.buyoutMult) .. "x|r deposit, bid |cffffff00" .. NumText(db.bidMult) ..
          "x|r deposit, listings |cffffff00" .. (DURATION_HOURS[db.duration] or 12) .. "h|r" ..
          (db.normalize and " |cff888888(normalised)|r" or ""))

    if db.undercut then
        Print("undercut |cff00ff00ON|r - |cffffff00" .. db.undercutPct .. "%|r" ..
              (db.undercutFlat > 0 and (" + |cffffff00" .. FormatMoney(db.undercutFlat) .. "|r") or "") ..
              " per item, bid |cffffff00" .. db.undercutBid .. "%|r of buyout, floor |cffffff00" ..
              db.undercutFloor .. "x|r deposit")
    else
        Print("undercut |cffff0000OFF|r - |cffffff00" .. db.undercutPct .. "%|r" ..
              (db.undercutFlat > 0 and (" + " .. FormatMoney(db.undercutFlat)) or "") .. " when switched on")
    end

    local n = 0
    for _ in pairs(db.skip) do n = n + 1 end
    Print("auto-start " .. (db.auto and "|cff00ff00on|r" or "|cffff0000off|r") ..
          ", minimum buyout " .. FormatMoney(db.minBuyout) ..
          ", skip list " .. n .. " item" .. (n == 1 and "" or "s"))
end

-- Note for anyone editing these: a bare "|" starts an escape sequence in a chat
-- string, so option lists are written with slashes, never "on|off".
local function ShowHelp()
    Print("|cffffff00/aa sell [text]|r post the bags (optionally only names containing text)")
    Print("|cffffff00/aa stop|r   |cffffff00/aa buyout <n>|r   |cffffff00/aa bid <n>|r   (multiples of the deposit)")
    Print("|cffffff00/aa undercut on / off|r   |cffffff00/aa undercut <n>%|r   |cffffff00/aa undercut <n>g|r   (per item)")
    Print("|cffffff00/aa undercutbid <n>|r   |cffffff00/aa floor <n>|r   |cffffff00/aa duration 12 / 24 / 48|r   |cffffff00/aa normalize on / off|r")
    Print("|cffffff00/aa gear on / off|r   |cffffff00/aa quality <min> [max]|r   (0 poor, 2 uncommon, 3 rare, 4 epic)")
    Print("|cffffff00/aa min <price>|r   |cffffff00/aa mindeposit <price>|r   |cffffff00/aa pages <n>|r   |cffffff00/aa auto on / off|r")
    Print("|cffffff00/aa skip [link]|r   |cffffff00/aa unskip [link]|r   |cffffff00/aa skiplist|r   |cffffff00/aa panel|r   |cffffff00/aa reset|r")
end

local function SkipCommand(rest, remove)
    local link = string.match(rest or "", "(|c.-|h.-|h|r)")
    local id   = ItemIdFromLink(link)
    local name = link and GetItemInfo(link)

    if not id then
        Print("Shift-click an item into the command, e.g. |cffffff00/aa " ..
              (remove and "unskip" or "skip") .. " [item]|r")
        return
    end

    if remove then
        db.skip[id] = nil
        Print("Will sell " .. (link or name or id) .. " again.")
    else
        db.skip[id] = name or tostring(id)
        Print("Never selling " .. (link or name or id) .. ".")
    end
end

local function HandleCommand(msg)
    msg = msg or ""

    local cmd, rest = string.match(msg, "^(%S*)%s*(.*)$")
    cmd = string.lower(cmd or "")

    if cmd == "" then
        ShowSettings()

    elseif cmd == "help" then
        ShowHelp()

    elseif cmd == "sell" or cmd == "start" then
        StartRun(rest ~= "" and rest or nil)

    elseif cmd == "stop" then
        if running then StopRun("Stopped") else Print("Not running.") end

    elseif cmd == "buyout" then
        local n = tonumber(rest)
        if n and n > 0 then
            db.buyoutMult = n
            UpdateRule()
            Print("Buyout is now |cffffff00" .. n .. "x|r the deposit.")
        else
            Print("Usage: /aa buyout <number>")
        end

    elseif cmd == "bid" then
        local n = tonumber(rest)
        if n and n > 0 then
            db.bidMult = n
            UpdateRule()
            Print("Opening bid is now |cffffff00" .. n .. "x|r the deposit.")
        else
            Print("Usage: /aa bid <number>")
        end

    elseif cmd == "undercut" then
        local arg = string.lower(rest)

        if arg == "on" or arg == "off" then
            db.undercut = (arg == "on")
            priceCache = {}
            UpdateRule()
            Print("Undercutting " .. (db.undercut and "|cff00ff00on|r - it overrides the deposit multiple whenever the item is already listed."
                                                  or "|cffff0000off|r - back to the deposit multiple."))
        elseif string.match(arg, "^%d+%.?%d*%%$") then
            db.undercutPct = tonumber(string.match(arg, "^(%d+%.?%d*)%%$"))
            priceCache = {}
            UpdateRule()
            Print("Undercutting by |cffffff00" .. db.undercutPct .. "%|r per item.")
        elseif ParseMoney(arg) then
            db.undercutFlat = ParseMoney(arg)
            priceCache = {}
            UpdateRule()
            Print("Undercutting by a flat |cffffff00" .. FormatMoney(db.undercutFlat) .. "|r per item.")
        else
            Print("Usage: /aa undercut on / off, /aa undercut 5%, /aa undercut 50s")
        end

    elseif cmd == "undercutbid" then
        local n = tonumber(rest)
        if n and n > 0 and n <= 100 then
            db.undercutBid = n
            Print("An undercut lot opens at |cffffff00" .. n .. "%|r of its buyout.")
        else
            Print("Usage: /aa undercutbid <1-100>")
        end

    elseif cmd == "floor" then
        local n = tonumber(rest)
        if n and n >= 0 then
            db.undercutFloor = n
            UpdateRule()
            Print("Undercutting stops at |cffffff00" .. n .. "x|r the deposit.")
        else
            Print("Usage: /aa floor <number> (0 removes the floor)")
        end

    elseif cmd == "duration" then
        local hours = tonumber(rest)
        local index = hours and DURATION_INDEX[hours]
        if index then
            db.duration = index
            Print("Listings run for |cffffff00" .. hours .. "h|r.")
        else
            Print("Usage: /aa duration 12 / 24 / 48")
        end

    elseif cmd == "normalize" then
        db.normalize = (string.lower(rest) == "on")
        Print("Duration normalising " .. (db.normalize and "|cff00ff00on|r - every duration prices off the 12h deposit."
                                                       or "|cffff0000off|r - a longer listing costs more."))

    elseif cmd == "gear" then
        db.gearOnly = (string.lower(rest) ~= "off")
        UpdateRule()
        Print(db.gearOnly and "Only things you can wear or wield - no recipes, reagents or trade goods."
                           or "Anything in the quality range, gear or not.")

    elseif cmd == "quality" then
        local minText, maxText = string.match(rest, "^(%d)%s*(%d?)$")
        local minQ = tonumber(minText)

        if minQ then
            db.minQuality = minQ
            local maxQ = tonumber(maxText)
            if maxQ then
                db.maxQuality = maxQ
            end
            if db.maxQuality < db.minQuality then
                db.maxQuality = db.minQuality
            end
            UpdateRule()
            Print("Selling " .. ScopeText() .. ".")
        else
            Print("Usage: /aa quality <min> [max] - 0 poor, 1 common, 2 uncommon, 3 rare, 4 epic, 5 legendary")
        end

    elseif cmd == "min" then
        local n = ParseMoney(rest)
        if n then
            db.minBuyout = math.max(1, n)
            Print("Minimum buyout is |cffffff00" .. FormatMoney(db.minBuyout) .. "|r.")
        else
            Print("Usage: /aa min <price>, e.g. /aa min 1g")
        end

    elseif cmd == "mindeposit" then
        local n = ParseMoney(rest)
        if n then
            db.minDeposit = math.max(1, n)
            Print("Deposit floor is |cffffff00" .. FormatMoney(db.minDeposit) .. "|r.")
        else
            Print("Usage: /aa mindeposit <price>, e.g. /aa mindeposit 1s")
        end

    elseif cmd == "pages" then
        local n = tonumber(rest)
        if n and n >= 1 then
            db.maxPages = math.floor(n)
            Print("Reading up to |cffffff00" .. db.maxPages .. "|r search page(s) per item.")
        else
            Print("Usage: /aa pages <number>")
        end

    elseif cmd == "auto" then
        db.auto = (string.lower(rest) == "on")
        Print("Auto-start at the auction house " .. (db.auto and "|cff00ff00on|r" or "|cffff0000off|r") .. ".")

    elseif cmd == "panel" then
        db.panel = not db.panel
        if db.panel and AtAuctionHouse() then panel:Show() else panel:Hide() end
        Print("Panel " .. (db.panel and "shown" or "hidden") .. ".")

    elseif cmd == "skip" then
        SkipCommand(rest, false)

    elseif cmd == "unskip" then
        SkipCommand(rest, true)

    elseif cmd == "skiplist" then
        local any = false
        for id, name in pairs(db.skip) do
            Print("never selling: " .. name .. " |cff888888(" .. id .. ")|r")
            any = true
        end
        if not any then
            Print("Skip list is empty.")
        end

    elseif cmd == "reset" then
        local skip = db.skip
        for key, value in pairs(defaults) do
            if key ~= "skip" and key ~= "point" then
                db[key] = value
            end
        end
        db.skip = skip
        UpdateRule()
        Print("Settings back to defaults (skip list kept).")

    else
        ShowHelp()
    end
end

SLASH_CENTURIONAUTOAUCTION1 = "/aa"
SLASH_CENTURIONAUTOAUCTION2 = "/autoauction"
SlashCmdList["CENTURIONAUTOAUCTION"] = HandleCommand

--=============================================================================
-- Events and the ticker
--=============================================================================

frame = CreateFrame("Frame", "CenturionAutoAuctionDriver")
frame:RegisterEvent("ADDON_LOADED")
frame:RegisterEvent("PLAYER_LOGIN")
frame:RegisterEvent("AUCTION_HOUSE_SHOW")
frame:RegisterEvent("AUCTION_HOUSE_CLOSED")
frame:RegisterEvent("AUCTION_ITEM_LIST_UPDATE")

frame:SetScript("OnEvent", function(self, event, arg1)
    if event == "ADDON_LOADED" then
        if arg1 ~= ADDON_NAME then
            return
        end

        CENTURION_AUTOAUCTION_DB = CENTURION_AUTOAUCTION_DB or {}
        db = CENTURION_AUTOAUCTION_DB

        for key, value in pairs(defaults) do
            if db[key] == nil then
                if type(value) == "table" then
                    db[key] = {}
                else
                    db[key] = value
                end
            end
        end

        playerName = UnitName("player")

        BuildPanel()
        UpdateRule()

    elseif event == "PLAYER_LOGIN" then
        -- Read again here because the owner name is what keeps the undercut off
        -- your own listings, and it is not worth trusting to load order.
        playerName = UnitName("player")

    elseif event == "AUCTION_HOUSE_SHOW" then
        atAH = true
        priceCache = {}
        if db.panel then
            UpdateStatus("")
            panel:Show()
        end
        if db.auto then
            StartRun(nil)
        end

    elseif event == "AUCTION_HOUSE_CLOSED" then
        atAH = false
        if running then
            StopRun("Auction house closed")
        end
        panel:Hide()

    elseif event == "AUCTION_ITEM_LIST_UPDATE" then
        OnAuctionListUpdate()
    end
end)

frame:SetScript("OnUpdate", function(self, elapsed)
    if not running then
        return
    end

    timer = timer + elapsed
    if timer < TICK then
        return
    end
    timer = 0

    if not AtAuctionHouse() then
        StopRun("Auction house closed")
        return
    end

    Step()
end)
