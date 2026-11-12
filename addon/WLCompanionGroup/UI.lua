-- WLCompanionGroup/UI.lua
--
-- The window. Everything here is clicked: cards for the companions you have, a panel for
-- minting a new one, and a small menu for the two destructive actions. Nothing in this
-- file expects the player to type a command.

local addonName, G = ...

local WIDTH, HEIGHT = 620, 470
local CARD_HEIGHT = 92
local MAX_CARDS = 4

local COL_MUTED = "|cff9d9d9d"
local COL_GOOD  = "|cff20ff20"
local COL_WARN  = "|cffffd200"
local COL_BAD   = "|cffff6a6a"
local COL_RESET = "|r"

local frame, cards, footer
local addPanel, raceDropdown, classDropdown, nameBox, stockLabel, createButton
local pick = { race = 1, class = 2 }   -- Human Paladin is a safe first guess for Alliance

local function InitDB()
    WLCompanionGroupDB = WLCompanionGroupDB or {}
    WLCompanionGroupDB.pos = WLCompanionGroupDB.pos or { point = "CENTER", x = 0, y = 80 }
end

local function Button(parent, label, width, onClick, tooltip)
    local button = CreateFrame("Button", nil, parent, "UIPanelButtonTemplate")
    button:SetSize(width or 64, 20)
    button:SetText(label)
    button:GetFontString():SetFontObject(GameFontNormalSmall)
    button:SetScript("OnClick", onClick)
    if tooltip then
        button:SetScript("OnEnter", function(self)
            GameTooltip:SetOwner(self, "ANCHOR_TOP")
            GameTooltip:AddLine(tooltip, 1, 1, 1, true)
            GameTooltip:Show()
        end)
        button:SetScript("OnLeave", function() GameTooltip:Hide() end)
    end
    return button
end

-- ---------------------------------------------------------------------------
-- cards
-- ---------------------------------------------------------------------------

local function StatusText(companion)
    local parts = {}

    if not G.HasFlag(companion, "o") then
        parts[#parts + 1] = COL_WARN .. "not in the world - summon it" .. COL_RESET
        return table.concat(parts, "")
    end

    parts[#parts + 1] = ("%s%d quest%s%s"):format(COL_MUTED, companion.quests,
        companion.quests == 1 and "" or "s", COL_RESET)

    if companion.free > 0 then
        parts[#parts + 1] = ("%s%d free slots%s"):format(COL_MUTED, companion.free, COL_RESET)
    end

    if not G.HasFlag(companion, "g") then
        parts[#parts + 1] = COL_WARN .. "not in your party - sync skips it" .. COL_RESET
    end

    if not G.HasFlag(companion, "s") then
        parts[#parts + 1] = COL_WARN .. "level differs" .. COL_RESET
    end

    if G.HasFlag(companion, "b") then
        parts[#parts + 1] = COL_GOOD .. "waiting on you" .. COL_RESET
    end

    return table.concat(parts, COL_MUTED .. "   " .. COL_RESET)
end

local function CreateCard(index)
    local card = CreateFrame("Frame", nil, frame)
    card:SetSize(WIDTH - 28, CARD_HEIGHT)
    card:SetPoint("TOPLEFT", 14, -54 - (index - 1) * (CARD_HEIGHT + 6))

    card.bg = card:CreateTexture(nil, "BACKGROUND")
    card.bg:SetAllPoints()
    card.bg:SetTexture(1, 1, 1, 0.05)

    card.stripe = card:CreateTexture(nil, "ARTWORK")
    card.stripe:SetPoint("TOPLEFT", 0, 0)
    card.stripe:SetPoint("BOTTOMLEFT", 0, 0)
    card.stripe:SetWidth(3)

    card.dot = card:CreateTexture(nil, "ARTWORK")
    card.dot:SetSize(9, 9)
    card.dot:SetPoint("TOPLEFT", 14, -13)

    card.name = card:CreateFontString(nil, "OVERLAY", "GameFontNormal")
    card.name:SetPoint("TOPLEFT", 30, -9)

    card.subtitle = card:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    card.subtitle:SetPoint("TOPLEFT", 14, -28)

    card.status = card:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    card.status:SetPoint("TOPLEFT", 14, -47)
    card.status:SetPoint("TOPRIGHT", -14, -47)
    card.status:SetJustifyH("LEFT")

    card.primary = Button(card, "Summon", 68, function()
        if not card.companion then return end
        local verb = G.HasFlag(card.companion, "o") and "dismiss" or "summon"
        G.Action(verb, card.companion.name)
    end, "Call this companion out, or send it away. It stays bound either way.")
    card.primary:SetPoint("BOTTOMRIGHT", -84, 8)

    card.levelButton = Button(card, "Level", 56, function()
        if card.companion then G.Action("level", card.companion.name) end
    end, "Bring its level up to yours.")
    card.levelButton:SetPoint("BOTTOMRIGHT", -152, 8)

    card.mapButton = Button(card, "Map", 48, function()
        G.Run(".companionsync pins")
        ToggleWorldMap()
    end, "Show this companion's quest markers on the world map.")
    card.mapButton:SetPoint("BOTTOMRIGHT", -208, 8)

    card.moreButton = Button(card, "...", 30, function(self)
        G.CardMenu(self, card)
    end, "Unbind (keep the bot) or release it back to the pool for good.")
    card.moreButton:SetPoint("BOTTOMRIGHT", -14, 8)

    return card
end

--- The two rare, sharp actions live behind one small menu instead of on the card.
function G.CardMenu(anchor, card)
    if not G.cardMenu then
        local menu = CreateFrame("Frame", "WLCGCardMenu", UIParent, "UIDropDownMenuTemplate")
        G.cardMenu = menu
    end

    UIDropDownMenu_Initialize(G.cardMenu, function(self, level)
        local companion = card and card.companion
        if not companion then return end

        UIDropDownMenu_AddButton({
            text = "Stop tracking (keep the bot)",
            notCheckable = true,
            func = function() G.ConfirmUnbind(companion.name) end,
        })

        UIDropDownMenu_AddButton({
            text = "|cffff6a6aRelease to the pool|r",
            notCheckable = true,
            func = function() G.ConfirmRelease(companion.name) end,
        })

        UIDropDownMenu_AddButton({
            text = "Open quest log view in chat",
            notCheckable = true,
            func = function() G.Run(".companionsync quests") end,
        })
    end)

    ToggleDropDownMenu(1, nil, G.cardMenu, anchor, 0, 0)
end

-- ---------------------------------------------------------------------------
-- add companion panel
-- ---------------------------------------------------------------------------

local function RaceList()
    local list = {}
    local races = G.PlayerRaces()

    for _, raceId in ipairs(races) do
        local stock = 0
        for classId in pairs(G.CLASSES) do
            stock = stock + G.StockFor(raceId, classId)
        end

        list[#list + 1] = { id = raceId, text = G.RaceName(raceId), stock = stock }
    end

    return list
end

local function ClassList(raceId)
    local list = {}
    for classId in pairs(G.CLASSES) do
        list[#list + 1] = { id = classId, text = G.ClassName(classId), stock = G.StockFor(raceId, classId) }
    end

    table.sort(list, function(a, b)
        if (a.stock > 0) ~= (b.stock > 0) then return a.stock > 0 end
        return a.text < b.text
    end)

    return list
end

local function RefreshAddPanel()
    if not addPanel or not addPanel:IsShown() then return end

    local stock = G.StockFor(pick.race, pick.class)
    UIDropDownMenu_SetText(raceDropdown, G.RaceName(pick.race))
    UIDropDownMenu_SetText(classDropdown, G.ClassName(pick.class))

    if stock > 0 then
        stockLabel:SetText(("%s%d free in the pool%s"):format(COL_GOOD, stock, COL_RESET))
        createButton:Enable()
    else
        stockLabel:SetText(COL_BAD .. "none free - pick another race or class" .. COL_RESET)
        createButton:Disable()
    end
end

local function CreateAddPanel()
    addPanel = CreateFrame("Frame", nil, frame)
    addPanel:SetPoint("TOPLEFT", 14, -54)
    addPanel:SetPoint("BOTTOMRIGHT", -14, 52)
    addPanel:Hide()

    addPanel.bg = addPanel:CreateTexture(nil, "BACKGROUND")
    addPanel.bg:SetAllPoints()
    addPanel.bg:SetTexture(1, 1, 1, 0.06)

    local title = addPanel:CreateFontString(nil, "OVERLAY", "GameFontNormal")
    title:SetPoint("TOPLEFT", 14, -12)
    title:SetText("New companion")

    local hint = addPanel:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    hint:SetPoint("TOPLEFT", 14, -32)
    hint:SetPoint("TOPRIGHT", -14, -32)
    hint:SetJustifyH("LEFT")
    hint:SetText(COL_MUTED .. "One click takes a free body from the pool, wipes it, names it and calls it out." .. COL_RESET)

    raceDropdown = CreateFrame("Frame", "WLCGRaceDropdown", addPanel, "UIDropDownMenuTemplate")
    raceDropdown:SetPoint("TOPLEFT", 6, -52)
    UIDropDownMenu_SetWidth(raceDropdown, 150)
    UIDropDownMenu_Initialize(raceDropdown, function(self, level)
        for _, race in ipairs(RaceList()) do
            UIDropDownMenu_AddButton({
                text = race.text .. (race.stock > 0 and ("  " .. COL_MUTED .. race.stock .. COL_RESET) or
                                     ("  " .. COL_BAD .. "none" .. COL_RESET)),
                value = race.id,
                checked = (pick.race == race.id),
                func = function()
                    pick.race = race.id
                    RefreshAddPanel()
                    UIDropDownMenu_SetText(raceDropdown, G.RaceName(pick.race))
                end,
            })
        end
    end)

    classDropdown = CreateFrame("Frame", "WLCGClassDropdown", addPanel, "UIDropDownMenuTemplate")
    classDropdown:SetPoint("TOPLEFT", 196, -52)
    UIDropDownMenu_SetWidth(classDropdown, 150)
    UIDropDownMenu_Initialize(classDropdown, function(self, level)
        for _, class in ipairs(ClassList(pick.race)) do
            UIDropDownMenu_AddButton({
                text = class.stock > 0 and (class.text .. "  " .. COL_MUTED .. class.stock .. COL_RESET)
                                        or ("|cff5a5a5a" .. class.text .. COL_RESET),
                value = class.id,
                checked = (pick.class == class.id),
                disabled = class.stock <= 0,
                func = function()
                    pick.class = class.id
                    RefreshAddPanel()
                    UIDropDownMenu_SetText(classDropdown, G.ClassName(pick.class))
                end,
            })
        end
    end)

    nameBox = CreateFrame("EditBox", "WLCGNameBox", addPanel)
    nameBox:SetSize(170, 22)
    nameBox:SetPoint("TOPLEFT", 20, -96)
    nameBox:SetAutoFocus(false)
    nameBox:SetMaxLetters(12)
    nameBox:SetFontObject(GameFontNormal)
    nameBox:SetTextInsets(6, 6, 0, 0)

    nameBox.bg = nameBox:CreateTexture(nil, "BACKGROUND")
    nameBox.bg:SetAllPoints()
    nameBox.bg:SetTexture(0, 0, 0, 0.55)

    nameBox.border = CreateFrame("Frame", nil, nameBox)
    nameBox.border:SetAllPoints()
    nameBox.border:SetBackdrop({
        edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
        edgeSize = 10,
        insets = { left = 2, right = 2, top = 2, bottom = 2 },
    })
    nameBox.border:SetBackdropBorderColor(1, 1, 1, 0.2)

    local suggest = Button(addPanel, "Suggest a name", 120, function()
        nameBox:SetText(G.SuggestName())
    end, "Fill in a name that nobody is using.")
    suggest:SetPoint("TOPLEFT", 200, -96)

    stockLabel = addPanel:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    stockLabel:SetPoint("TOPLEFT", 20, -128)

    createButton = Button(addPanel, "Create companion", 150, function()
        if G.CreateCompanion(pick.race, pick.class, nameBox:GetText() or "") then
            addPanel:Hide()
        end
    end, "Claim a body from the pool with the race, class and name above.")
    createButton:SetPoint("BOTTOMLEFT", 20, 16)

    local cancel = Button(addPanel, "Cancel", 80, function() addPanel:Hide() end)
    cancel:SetPoint("BOTTOMLEFT", 178, 16)

    return addPanel
end

function G.ShowAddPanel()
    if not addPanel then CreateAddPanel() end

    pick.race = G.PlayerRaces()[1] or 1
    pick.class = 2
    nameBox:SetText(G.SuggestName())

    addPanel:Show()
    UIDropDownMenu_SetText(raceDropdown, G.RaceName(pick.race))
    UIDropDownMenu_SetText(classDropdown, G.ClassName(pick.class))
    RefreshAddPanel()

    -- a fresh stock line, so the picker reflects what the pool has right now
    G.Run(".companionsync pool")
end

function G.OnStockChanged() RefreshAddPanel() end

-- ---------------------------------------------------------------------------
-- window
-- ---------------------------------------------------------------------------

function G.Refresh()
    if not frame then return end

    local count = #G.companions
    frame.count:SetText(("%s%d of %d companions%s"):format(COL_MUTED, count, MAX_CARDS, COL_RESET))

    for i = 1, MAX_CARDS do
        local card = cards[i]
        local companion = G.companions[i]

        if companion then
            card.companion = companion

            local r, g, b = G.ClassColor(companion.classId)
            local online = G.HasFlag(companion, "o")

            card.name:SetText(online and companion.name or ("|cff808080" .. companion.name .. "|r"))
            card.name:SetTextColor(online and r or 0.5, online and g or 0.5, online and b or 0.5)
            card.dot:SetTexture(r, g, b, online and 1 or 0.3)
            card.stripe:SetTexture(r, g, b, online and 0.85 or 0.25)

            card.subtitle:SetText(("%s%s  ·  %s  ·  level %d%s"):format(
                COL_MUTED, G.RaceName(companion.raceId), G.ClassName(companion.classId),
                companion.level, COL_RESET))

            card.status:SetText(StatusText(companion))

            card.primary:SetText(online and "Dismiss" or "Summon")
            if companion.level > 0 and not G.HasFlag(companion, "s") then
                card.levelButton:Show()
            else
                card.levelButton:Hide()
            end

            if online then card.mapButton:Show() else card.mapButton:Hide() end

            card:Show()
        else
            card.companion = nil
            card:Hide()
        end
    end

    if count == 0 then frame.empty:Show() else frame.empty:Hide() end

    if addPanel and addPanel:IsShown() and count > 0 then addPanel:Hide() end
end

local function CreateWindow()
    frame = CreateFrame("Frame", "WLCompanionGroupFrame", UIParent)
    frame:SetSize(WIDTH, HEIGHT)
    frame:SetFrameStrata("DIALOG")
    frame:SetClampedToScreen(true)
    frame:SetMovable(true)
    frame:EnableMouse(true)
    frame:RegisterForDrag("LeftButton")
    frame:SetScript("OnDragStart", frame.StartMoving)
    frame:SetScript("OnDragStop", function(self)
        self:StopMovingOrSizing()
        local point, _, _, x, y = self:GetPoint()
        WLCompanionGroupDB.pos = { point = point, x = x, y = y }
    end)

    frame.bg = frame:CreateTexture(nil, "BACKGROUND")
    frame.bg:SetAllPoints()
    frame.bg:SetTexture(0, 0, 0, 0.78)

    frame.border = CreateFrame("Frame", nil, frame)
    frame.border:SetAllPoints()
    frame.border:SetBackdrop({
        edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
        edgeSize = 14,
        insets = { left = 4, right = 4, top = 4, bottom = 4 },
    })
    frame.border:SetBackdropBorderColor(1, 1, 1, 0.18)

    frame.title = frame:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
    frame.title:SetPoint("TOPLEFT", 16, -12)
    frame.title:SetText("|cffffc94dCompanions|r")

    frame.count = frame:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    frame.count:SetPoint("TOPLEFT", 16, -34)

    local add = Button(frame, "+ Add companion", 118, function()
        if addPanel and addPanel:IsShown() then
            addPanel:Hide()
        else
            G.ShowAddPanel()
        end
    end, "Mint a new companion from the pool: pick a race and class, and we suggest a name.")
    add:SetPoint("TOPRIGHT", -34, -14)

    frame.close = CreateFrame("Button", nil, frame, "UIPanelCloseButton")
    frame.close:SetPoint("TOPRIGHT", -6, -6)

    cards = {}
    for i = 1, MAX_CARDS do cards[i] = CreateCard(i) end

    frame.empty = frame:CreateFontString(nil, "OVERLAY", "GameFontNormal")
    frame.empty:SetPoint("TOPLEFT", 20, -80)
    frame.empty:SetPoint("TOPRIGHT", -20, -80)
    frame.empty:SetJustifyH("LEFT")
    frame.empty:SetText(COL_MUTED ..
        "No companions yet.\n\nClick |cffffff00+ Add companion|r and pick a race and class - " ..
        "a free body is taken from the pool, named, and called to your side. Four is the most, " ..
        "a group of five with you." .. COL_RESET)

    footer = CreateFrame("Frame", nil, frame)
    footer:SetPoint("BOTTOMLEFT", 14, 12)
    footer:SetPoint("BOTTOMRIGHT", -14, 12)
    footer:SetHeight(22)

    local summonAll = Button(footer, "Summon missing", 110, function() G.Action("summon") end,
        "Bring out every companion that is not with you.")
    summonAll:SetPoint("LEFT", 0, 0)

    local dismissAll = Button(footer, "Dismiss all", 88, function() G.Action("dismiss") end,
        "Send every companion away. They stay bound and keep following your quest log.")
    dismissAll:SetPoint("LEFT", 116, 0)

    local refresh = Button(footer, "Refresh", 70, function()
        G.Run(".companionsync pins")
        G.Run(".companionsync pool")
    end, "Ask the server for the current state.")
    refresh:SetPoint("LEFT", 210, 0)

    local quests = Button(footer, "Quest log", 82, function() G.Run(".companionsync quests") end,
        "Print what each companion is carrying to chat.")
    quests:SetPoint("LEFT", 286, 0)

    frame:SetPoint("CENTER", UIParent, "CENTER", 0, 80)
    return frame
end

function G.Toggle()
    if not WLCompanionGroupDB then InitDB() end
    if not frame then CreateWindow() end

    if frame:IsShown() then
        frame:Hide()
    else
        local pos = WLCompanionGroupDB.pos
        frame:ClearAllPoints()
        frame:SetPoint(pos.point or "CENTER", UIParent, pos.point or "CENTER", pos.x or 0, pos.y or 80)
        frame:Show()
        G.Refresh()
        G.Run(".companionsync pins")
        G.Run(".companionsync pool")
    end
end

G.OnCompanionsChanged = function()
    if frame and frame:IsShown() then G.Refresh() end
end

-- ---------------------------------------------------------------------------
-- launcher + startup
-- ---------------------------------------------------------------------------

local launcher = CreateFrame("Button", "WLCompanionGroupLauncher", Minimap)
launcher:SetSize(28, 28)
launcher:SetPoint("TOPLEFT", Minimap, "TOPLEFT", 4, -62)
launcher:SetFrameStrata("MEDIUM")

launcher.icon = launcher:CreateTexture(nil, "BACKGROUND")
launcher.icon:SetAllPoints()
launcher.icon:SetTexture("Interface\\Icons\\Ability_Hunter_BeastCall")
launcher.icon:SetTexCoord(0.08, 0.92, 0.08, 0.92)

launcher.border = launcher:CreateTexture(nil, "OVERLAY")
launcher.border:SetPoint("TOPLEFT", -1, 1)
launcher.border:SetPoint("BOTTOMRIGHT", 1, -1)
launcher.border:SetTexture("Interface\\Minimap\\MiniMap-TrackingBorder")

launcher:SetScript("OnClick", function() G.Toggle() end)
launcher:SetScript("OnEnter", function(self)
    GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
    GameTooltip:AddLine("Companions", 1, 0.79, 0.30)
    GameTooltip:AddLine("Click to open", 1, 1, 1)
    GameTooltip:Show()
end)
launcher:SetScript("OnLeave", function() GameTooltip:Hide() end)

local boot = CreateFrame("Frame")
boot:RegisterEvent("ADDON_LOADED")
boot:RegisterEvent("PLAYER_ENTERING_WORLD")
boot:SetScript("OnEvent", function(self, event, arg1)
    if event == "ADDON_LOADED" then
        if arg1 == addonName then InitDB() end
        return
    end

    self:UnregisterEvent("PLAYER_ENTERING_WORLD")
    if not WLCompanionGroupDB then InitDB() end
    if not frame then CreateWindow() end

    local pos = WLCompanionGroupDB.pos
    frame:ClearAllPoints()
    frame:SetPoint(pos.point or "CENTER", UIParent, pos.point or "CENTER", pos.x or 0, pos.y or 80)
end)
