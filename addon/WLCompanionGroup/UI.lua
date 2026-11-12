-- WLCompanionGroup/UI.lua
--
-- The window. Two regions: a roster of cards (one per companion) and an overlay panel for
-- minting a new one. Every action is a button; nothing here expects typing except the name.
--
-- Layout rules that keep it from turning into soup:
--   * a card has exactly three text baselines (name, race/class/level, status) and one
--     column of buttons pinned to the right, so text never runs under a button;
--   * the status line is width-limited rather than allowed to wrap into the buttons;
--   * the panel uses a labelled grid: label above control, controls on a 24 px rhythm.

local addonName, G = ...

local WIDTH, HEIGHT = 660, 540
local CARD_HEIGHT = 98
local CARD_GAP = 8
local MAX_CARDS = 4
local TEXT_RIGHT_INSET = 236        -- leaves room for the button column

local COL_MUTED = "|cff9d9d9d"
local COL_GOOD  = "|cff20ff20"
local COL_WARN  = "|cffffd200"
local COL_BAD   = "|cffff6a6a"
local COL_RESET = "|r"

local frame, cards, addPanel
local raceDropdown, classDropdown, nameBox, stockLabel, serverLine, createButton
local pick = { race = 1, class = 5 }

local function InitDB()
    WLCompanionGroupDB = WLCompanionGroupDB or {}
    WLCompanionGroupDB.pos = WLCompanionGroupDB.pos or { point = "CENTER", x = 0, y = 60 }
end

local function Button(parent, label, width, onClick, tooltip)
    local button = CreateFrame("Button", nil, parent, "UIPanelButtonTemplate")
    button:SetSize(width or 64, 22)
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

local function Label(parent, text, x, y)
    local label = parent:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    label:SetPoint("TOPLEFT", x, y)
    label:SetText(COL_MUTED .. text .. COL_RESET)
    return label
end

-- ---------------------------------------------------------------------------
-- cards
-- ---------------------------------------------------------------------------

local function StatusText(companion)
    -- a release is a server-side sequence (dismiss, wipe, rename, unbind, ledger); until the
    -- roster drops the name, say so rather than looking like a button that did nothing
    if G.releasing and G.releasing[companion.name] then
        return COL_WARN .. "releasing - wiping and returning it to the pool..." .. COL_RESET
    end

    if not G.HasFlag(companion, "o") then
        return COL_WARN .. "not in the world - Summon brings it back" .. COL_RESET
    end

    local parts = {}
    parts[#parts + 1] = ("%d quest%s"):format(companion.quests, companion.quests == 1 and "" or "s")

    if companion.free > 0 then
        parts[#parts + 1] = ("%d free slots"):format(companion.free)
    end

    if not G.HasFlag(companion, "g") then
        parts[#parts + 1] = COL_WARN .. "not in your party" .. COL_RESET
    end

    if not G.HasFlag(companion, "s") then
        parts[#parts + 1] = COL_WARN .. "level differs" .. COL_RESET
    end

    if G.HasFlag(companion, "b") then
        parts[#parts + 1] = COL_GOOD .. "waiting on you" .. COL_RESET
    end

    return COL_MUTED .. table.concat(parts, "  ·  ") .. COL_RESET
end

local function CreateCard(index)
    local card = CreateFrame("Frame", nil, frame)
    card:SetSize(WIDTH - 28, CARD_HEIGHT)
    card:SetPoint("TOPLEFT", 14, -58 - (index - 1) * (CARD_HEIGHT + CARD_GAP))

    card.bg = card:CreateTexture(nil, "BACKGROUND")
    card.bg:SetAllPoints()
    card.bg:SetTexture(1, 1, 1, 0.05)

    card.stripe = card:CreateTexture(nil, "ARTWORK")
    card.stripe:SetPoint("TOPLEFT", 0, 0)
    card.stripe:SetPoint("BOTTOMLEFT", 0, 0)
    card.stripe:SetWidth(3)

    card.dot = card:CreateTexture(nil, "ARTWORK")
    card.dot:SetSize(9, 9)
    card.dot:SetPoint("TOPLEFT", 16, -18)

    card.name = card:CreateFontString(nil, "OVERLAY", "GameFontNormal")
    card.name:SetPoint("TOPLEFT", 32, -12)

    card.subtitle = card:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    card.subtitle:SetPoint("TOPLEFT", 16, -32)

    card.status = card:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    card.status:SetPoint("TOPLEFT", 16, -54)
    card.status:SetWidth(WIDTH - 28 - TEXT_RIGHT_INSET - 24)
    card.status:SetJustifyH("LEFT")

    card.primary = Button(card, "Summon", 84, function()
        if not card.companion then return end
        G.Action(G.HasFlag(card.companion, "o") and "dismiss" or "summon", card.companion.name)
    end, "Call this companion out, or send it away. It stays bound either way.")
    card.primary:SetPoint("RIGHT", -12, 18)

    card.levelButton = Button(card, "Level", 56, function()
        if card.companion then G.Action("level", card.companion.name) end
    end, "Bring its level up to yours.")
    card.levelButton:SetPoint("RIGHT", -100, 18)

    card.mapButton = Button(card, "Map", 44, function()
        G.Run(".companionsync pins")
        ToggleWorldMap()
    end, "Show this companion's quest markers on the world map.")

    -- Release and untrack used to hide behind a "..." dropdown. A control the player has to
    -- discover is a control that does not get used: the release never reached the server at all.
    -- They are plain buttons now, release in warning colour because it cannot be undone.
    card.untrackButton = Button(card, "Untrack", 62, function()
        if card.companion then G.ConfirmUnbind(card.companion.name) end
    end, "Stop following this companion's quests. The bot stays yours.")
    card.untrackButton:SetPoint("BOTTOMRIGHT", -12, -12)

    card.releaseButton = Button(card, "Release", 62, function()
        if card.companion then G.ConfirmRelease(card.companion.name) end
    end, "Wipe it and give the body back to the pool. This cannot be undone.")
    card.releaseButton:SetPoint("BOTTOMRIGHT", -78, -12)
    card.releaseButton:GetFontString():SetTextColor(1, 0.6, 0.6)

    card.mapButton:SetPoint("BOTTOMRIGHT", -144, -12)

    return card
end

-- ---------------------------------------------------------------------------
-- add panel
-- ---------------------------------------------------------------------------

local function RaceList()
    local list = {}
    for _, raceId in ipairs(G.PlayerRaces()) do
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
    createButton:SetText("Create " .. G.RaceName(pick.race) .. " " .. G.ClassName(pick.class))

    if stock > 0 then
        stockLabel:SetText(("%s%d free in the pool%s"):format(COL_GOOD, stock, COL_RESET))
    elseif not next(G.stock) then
        stockLabel:SetText(COL_MUTED .. "asking the server what the pool holds..." .. COL_RESET)
    else
        stockLabel:SetText(COL_BAD .. "none free - the server will refuse this combination" .. COL_RESET)
    end
end

local DROPDOWN_TEXT_INSET = 16      -- UIDropDownMenuTemplate puts its text inside a button

local function CreateAddPanel()
    addPanel = CreateFrame("Frame", nil, frame)
    addPanel:SetPoint("TOPLEFT", 14, -58)
    addPanel:SetPoint("BOTTOMRIGHT", -14, 56)
    addPanel:Hide()

    addPanel.bg = addPanel:CreateTexture(nil, "BACKGROUND")
    addPanel.bg:SetAllPoints()
    addPanel.bg:SetTexture(1, 1, 1, 0.06)

    addPanel.title = addPanel:CreateFontString(nil, "OVERLAY", "GameFontNormal")
    addPanel.title:SetPoint("TOPLEFT", 18, -14)
    addPanel.title:SetText("New companion")

    addPanel.hint = addPanel:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    addPanel.hint:SetPoint("TOPLEFT", 18, -34)
    addPanel.hint:SetPoint("TOPRIGHT", -18, -34)
    addPanel.hint:SetJustifyH("LEFT")
    addPanel.hint:SetText(COL_MUTED ..
        "A free body is taken from the pool, wiped to level 1, named and called to your side." .. COL_RESET)

    -- row 1: race and class
    Label(addPanel, "Race", 20, -62)
    Label(addPanel, "Class", 216, -62)

    raceDropdown = CreateFrame("Frame", "WLCGRaceDropdown", addPanel, "UIDropDownMenuTemplate")
    raceDropdown:SetPoint("TOPLEFT", 4, -74)
    UIDropDownMenu_SetWidth(raceDropdown, 150)
    UIDropDownMenu_Initialize(raceDropdown, function()
        for _, race in ipairs(RaceList()) do
            UIDropDownMenu_AddButton({
                text = race.text .. (race.stock > 0 and ("   " .. COL_MUTED .. race.stock .. COL_RESET)
                                                       or ("   " .. COL_BAD .. "none" .. COL_RESET)),
                value = race.id,
                checked = (pick.race == race.id),
                func = function()
                    pick.race = race.id
                    RefreshAddPanel()
                end,
            })
        end
    end)

    classDropdown = CreateFrame("Frame", "WLCGClassDropdown", addPanel, "UIDropDownMenuTemplate")
    classDropdown:SetPoint("TOPLEFT", 200, -74)
    UIDropDownMenu_SetWidth(classDropdown, 150)
    UIDropDownMenu_Initialize(classDropdown, function()
        for _, class in ipairs(ClassList(pick.race)) do
            UIDropDownMenu_AddButton({
                text = class.stock > 0 and (class.text .. "   " .. COL_MUTED .. class.stock .. COL_RESET)
                                       or ("|cff5a5a5a" .. class.text .. COL_RESET),
                value = class.id,
                checked = (pick.class == class.id),
                disabled = class.stock <= 0,
                func = function()
                    pick.class = class.id
                    RefreshAddPanel()
                end,
            })
        end
    end)

    -- row 2: name
    Label(addPanel, "Name", 20, -132)

    nameBox = CreateFrame("EditBox", "WLCGNameBox", addPanel)
    nameBox:SetSize(210, 24)
    nameBox:SetPoint("TOPLEFT", 20, -148)
    nameBox:SetAutoFocus(false)
    nameBox:SetMaxLetters(12)
    nameBox:SetFontObject(GameFontNormal)
    nameBox:SetTextInsets(6, 6, 0, 0)
    nameBox:SetScript("OnEscapePressed", function(self) self:ClearFocus() end)

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

    local suggest = Button(addPanel, "Suggest", 80, function()
        nameBox:SetText(G.SuggestName())
    end, "Fill in a name nobody is using.")
    suggest:SetPoint("TOPLEFT", 240, -148)

    -- row 3: pool answer and the server's reply
    stockLabel = addPanel:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    stockLabel:SetPoint("TOPLEFT", 20, -186)

    serverLine = addPanel:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    serverLine:SetPoint("TOPLEFT", 20, -208)
    serverLine:SetPoint("TOPRIGHT", -20, -208)
    serverLine:SetJustifyH("LEFT")
    serverLine:SetText(COL_MUTED .. "the server's answer appears here" .. COL_RESET)

    -- row 4: actions
    createButton = Button(addPanel, "Create", 190, function()
        if G.CreateCompanion(pick.race, pick.class, nameBox:GetText() or "") then
            nameBox:ClearFocus()
        end
    end, "Claim a body from the pool with this race, class and name.")
    createButton:SetPoint("BOTTOMLEFT", 20, 16)

    local cancel = Button(addPanel, "Cancel", 80, function() addPanel:Hide() end)
    cancel:SetPoint("BOTTOMLEFT", 218, 16)

    return addPanel
end

function G.ShowAddPanel()
    if not addPanel then CreateAddPanel() end

    pick.race = G.PlayerRaces()[1] or 1
    pick.class = 5
    nameBox:SetText(G.SuggestName())
    serverLine:SetText(COL_MUTED .. "the server's answer appears here" .. COL_RESET)

    addPanel:Show()
    RefreshAddPanel()
    G.Run(".companionsync pool")
end

function G.OnStockChanged() RefreshAddPanel() end

-- While the window is open, ask the server for the current state every few seconds. Without
-- this the panel only knew what the server chose to push, so a companion dismissed a moment
-- ago still showed as with you until the window was closed and reopened.
local POLL_SECONDS = 8

function G.PollState()
    G.Run(".companionsync refresh")
end

function G.OnServerMessage(text)
    if addPanel and addPanel:IsShown() and serverLine then
        serverLine:SetText(COL_WARN .. text .. COL_RESET)
    end
end

-- ---------------------------------------------------------------------------
-- window
-- ---------------------------------------------------------------------------

function G.Refresh()
    if not frame then return end

    local count = #G.companions

    for i = 1, MAX_CARDS do
        local card = cards[i]
        local companion = G.companions[i]

        if companion then
            card.companion = companion

            local r, g, b = G.ClassColor(companion.classId)
            local online = G.HasFlag(companion, "o")

            card.name:SetText(companion.name)
            card.name:SetTextColor(online and r or 0.5, online and g or 0.5, online and b or 0.5)
            card.dot:SetTexture(r, g, b, online and 1 or 0.3)
            card.stripe:SetTexture(r, g, b, online and 0.85 or 0.25)

            card.subtitle:SetText(("%s%s · %s · level %d%s"):format(
                COL_MUTED, G.RaceName(companion.raceId), G.ClassName(companion.classId),
                companion.level, COL_RESET))

            card.status:SetText(StatusText(companion))

            card.primary:SetText(online and "Dismiss" or "Summon")

            if companion.level > 0 and not G.HasFlag(companion, "s") then card.levelButton:Show()
            else card.levelButton:Hide() end

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
    frame.bg:SetTexture(0, 0, 0, 0.8)

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
    frame.count:SetPoint("TOPLEFT", 16, -36)

    local add = Button(frame, "+ Add companion", 124, function()
        if addPanel and addPanel:IsShown() then addPanel:Hide() else G.ShowAddPanel() end
    end, "Mint a new companion from the pool.")
    add:SetPoint("TOPRIGHT", -36, -14)

    frame.elapsed = 0
    frame:SetScript("OnUpdate", function(self, delta)
        self.elapsed = self.elapsed + delta
        if self.elapsed >= POLL_SECONDS then
            self.elapsed = 0
            G.PollState()
        end
    end)

    frame.close = CreateFrame("Button", nil, frame, "UIPanelCloseButton")
    frame.close:SetPoint("TOPRIGHT", -6, -6)

    cards = {}
    for i = 1, MAX_CARDS do cards[i] = CreateCard(i) end

    frame.empty = frame:CreateFontString(nil, "OVERLAY", "GameFontNormal")
    frame.empty:SetPoint("TOPLEFT", 20, -84)
    frame.empty:SetPoint("TOPRIGHT", -20, -84)
    frame.empty:SetJustifyH("LEFT")
    frame.empty:SetText(COL_MUTED ..
        "No companions yet.\n\nPress |cffffff00+ Add companion|r, choose a race and a class, and press Create. " ..
        "Four is the most - a group of five with you." .. COL_RESET)

    local footer = CreateFrame("Frame", nil, frame)
    footer:SetPoint("BOTTOMLEFT", 14, 14)
    footer:SetPoint("BOTTOMRIGHT", -14, 14)
    footer:SetHeight(22)

    local summonAll = Button(footer, "Summon missing", 110, function() G.Action("summon") end,
        "Bring out every companion that is not with you.")
    summonAll:SetPoint("LEFT", 0, 0)

    local dismissAll = Button(footer, "Dismiss all", 88, function() G.Action("dismiss") end,
        "Send every companion away. They stay bound.")
    dismissAll:SetPoint("LEFT", 116, 0)

    local refresh = Button(footer, "Refresh", 70, function()
        G.Run(".companionsync pins")
        G.Run(".companionsync pool")
    end, "Ask the server for the current state.")
    refresh:SetPoint("LEFT", 210, 0)

    local quests = Button(footer, "Quest log", 82, function() G.Run(".companionsync quests") end,
        "Print what each companion is carrying to chat.")
    quests:SetPoint("LEFT", 286, 0)

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
        frame:SetPoint(pos.point or "CENTER", UIParent, pos.point or "CENTER", pos.x or 0, pos.y or 60)
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
    frame:SetPoint(pos.point or "CENTER", UIParent, pos.point or "CENTER", pos.x or 0, pos.y or 60)
end)
