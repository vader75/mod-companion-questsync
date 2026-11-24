-- CompanionSync/UI.lua
--
-- The roster picks one companion. Its card groups quests, professions and supplies.

local addonName, G = ...

-- DragonUI is optional. Borrow its chrome helpers, never its saved variables or layout.
local function Chrome(method)
    local dragon = type(DragonUI) == "table" and DragonUI.ForeverUI
    return type(dragon) == "table" and type(dragon[method]) == "function" and dragon[method] or nil
end
local panelTone, buttonIdle, buttonHover

local WIDTH = 680
local PAD = 16
local CARD_HEIGHT = 246
local ROSTER_HEIGHT = 32
local HEADER_HEIGHT = 62
local FOOTER_HEIGHT = 90          -- buttons, roster hint, notices and delivery result
local EMPTY_HEIGHT = 108         -- the "no companions yet" copy needs room of its own
local ADD_PANEL_HEIGHT = 278      -- race/class, name, pool stock, answer, Create/Cancel
local MAX_CARDS = 4
local SKILL_WIDTH = 182

local COL_MUTED = "|cff9d9d9d"
local COL_GOOD  = "|cff20ff20"
local COL_WARN  = "|cffffd200"
local COL_BAD   = "|cffff6a6a"
local COL_RESET = "|r"

local frame, cards, roster, addPanel, selectedName
local raceDropdown, classDropdown, nameBox, stockLabel, serverLine, createButton
local pick = { race = 1, class = 5 }
local supplyPopup, supplyScope, supplyChoice, supplyOpen, supplyOffset
local SUPPLY_VISIBLE = 8

local function InitDB()
    -- an install from before the rename keeps its window position
    if type(CompanionSyncDB) ~= "table" and type(WLCompanionGroupDB) == "table" then
        CompanionSyncDB = WLCompanionGroupDB
    end

    CompanionSyncDB = CompanionSyncDB or {}
    CompanionSyncDB.pos = CompanionSyncDB.pos or { point = "CENTER", x = 0, y = 60 }
end

local function Button(parent, label, width, onClick, tooltip, primary)
    local button = CreateFrame("Button", nil, parent, "UIPanelButtonTemplate")
    button:SetSize(width or 64, 22)
    button:SetText(label)

    local text = button:GetFontString()
    text:SetFontObject(GameFontNormalSmall)
    text:SetTextColor(0.88, 0.88, 0.9)

    local skin = primary and Chrome("SkinButton")
    local plate
    if skin then
        skin(button)
    else
        -- Leave navigation, list rows and secondary actions quiet next to red buttons.
        for _, getter in ipairs({ "GetNormalTexture", "GetPushedTexture", "GetDisabledTexture" }) do
            local tex = button[getter] and button[getter](button)
            if tex and tex.SetAlpha then tex:SetAlpha(0) end
        end

        local highlight = button.GetHighlightTexture and button:GetHighlightTexture()
        if highlight then highlight:SetTexture(1, 1, 1, 0.05) end

        plate = button:CreateTexture(nil, "BACKGROUND")
        plate:SetAllPoints()
        plate:SetTexture(unpack(buttonIdle))
    end

    button:SetScript("OnEnter", function(self)
        if plate then plate:SetTexture(unpack(buttonHover)) end

        if tooltip then
            GameTooltip:SetOwner(self, "ANCHOR_TOP")
            GameTooltip:AddLine(tooltip, 1, 1, 1, true)
            GameTooltip:Show()
        end
    end)

    button:SetScript("OnLeave", function()
        if plate then plate:SetTexture(unpack(buttonIdle)) end
        GameTooltip:Hide()
    end)

    button:SetScript("OnClick", onClick)
    return button
end

-- 3.3.5 only wraps a font string whose width is set explicitly. Anchor a string to both
-- horizontal edges and the client simply clips it with an ellipsis, which is how a whole
-- instruction became "Press + Add companion, choose a race and a class, and press Creat...".
-- Every sentence in this window goes through here so it wraps at word boundaries instead.
local function Wrap(fontString, width)
    fontString:SetWidth(width)
    fontString:SetJustifyH("LEFT")
    fontString:SetNonSpaceWrap(false)  -- break at spaces, never mid-word
    return fontString
end

local function Label(parent, text, x, y, width)
    local label = parent:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    label:SetPoint("TOPLEFT", x, y)
    if width then Wrap(label, width) end
    label:SetText(COL_MUTED .. text .. COL_RESET)
    return label
end

-- ---------------------------------------------------------------------------
-- cards
-- ---------------------------------------------------------------------------

local function CanSupply(companion)
    return G.SnapshotState("roster") == "ready" and companion and G.HasFlag(companion, "o") and not G.HasFlag(companion, "d")
        and not G.releasing[companion.name]
end

local function SupplyLabel(row)
    return row.name .. (row.kind == "bag" and ("  ·  %.0f ready"):format(row.quantity)
        or row.kind == "craft" and "  ·  Can craft" or "  ·  Not ready")
end

local function EnableButton(button, enabled)
    if enabled then button:Enable() else button:Disable() end
end

local function RefreshSupply()
    local supply = G.supply
    if supplyScope ~= supply then
        supplyScope, supplyChoice, supplyOpen, supplyOffset = supply, nil, false, 0
    end
    local rows, pending = supply and supply.rows, supply and supply.pending
    if not (rows and supplyChoice and rows[supplyChoice.index] == supplyChoice) then supplyChoice = nil end
    if not (rows and #rows > 0) and not pending then supplyOpen = false end

    local anchor
    for i, card in ipairs(cards) do
        local companion = G.companions[i]
        local active = supply and companion and supply.name == companion.name
        EnableButton(card.supplyPicker, active and not pending)
        EnableButton(card.supplyRefresh, active and not pending)
        EnableButton(card.supplyAction, active and supplyChoice and not G.DeliveryPending())
        card.supplyPicker:SetText(active and supplyChoice and SupplyLabel(supplyChoice) or "Choose supply  ▾")
        card.supplyAction:SetText(active and supplyChoice and (supplyChoice.kind == "bag" and "Take supply" or "Craft supply")
            or "Take / Craft")
        local message = "Summon this companion to see supplies."
        if companion and G.releasing[companion.name] then
            message = "Supplies are unavailable while this companion is being released."
        elseif G.HasFlag(companion, "d") then
            message = "Revive this companion to see supplies."
        end
        if active then
            anchor = card.supplyPicker
            message = supply.message or (pending and (pending.kind == "catalog" and "Loading supplies…" or "Preparing trade…"))
                or (rows and (#rows == 0 and "No offerable supplies or supported recipes."
                    or "Stand nearby and out of combat. You still accept the trade."))
                or "Choose a supply to check what this companion can offer or craft."
        end
        card.supplyStatus:SetText(COL_MUTED .. message .. COL_RESET)
    end

    if not (anchor and supplyOpen and rows and #rows > 0) then
        supplyPopup:Hide()
        return
    end
    local visible = math.min(#rows, SUPPLY_VISIBLE)
    supplyOffset = math.min(supplyOffset, #rows - visible)
    for i, button in ipairs(supplyPopup.rows) do
        local row = i <= visible and rows[supplyOffset + i] or nil
        button.row = row
        if row then
            button:SetText(SupplyLabel(row))
            EnableButton(button, row.kind ~= "missing")
            if supplyChoice == row then button:LockHighlight() else button:UnlockHighlight() end
            button:Show()
        else button:Hide() end
    end
    local paged = #rows > visible
    if paged then
        supplyPopup.up:Show()
        supplyPopup.down:Show()
        supplyPopup.range:SetText(("%d–%d of %d"):format(supplyOffset + 1, supplyOffset + visible, #rows))
        EnableButton(supplyPopup.up, supplyOffset > 0)
        EnableButton(supplyPopup.down, supplyOffset + visible < #rows)
    else
        supplyPopup.up:Hide()
        supplyPopup.down:Hide()
        supplyPopup.range:SetText("")
    end
    supplyPopup:SetHeight(visible * 24 + (paged and 36 or 12))
    supplyPopup:ClearAllPoints()
    supplyPopup:SetPoint("TOPLEFT", anchor, "BOTTOMLEFT", 0, -2)
    supplyPopup:Show()
end

local function CreateSupplyPopup()
    supplyPopup = CreateFrame("Frame", nil, frame)
    supplyPopup:SetFrameStrata("FULLSCREEN_DIALOG")
    supplyPopup:SetClampedToScreen(true)
    supplyPopup:SetWidth(392)
    supplyPopup:EnableMouse(true)
    supplyPopup:EnableMouseWheel(true)
    local menuSkin = Chrome("SkinMenuBackground")
    if menuSkin then
        menuSkin(supplyPopup, "bg")
    else
        supplyPopup:SetBackdrop({
            bgFile = "Interface\\Buttons\\WHITE8X8",
            edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border", edgeSize = 10,
        })
        supplyPopup:SetBackdropColor(0.07, 0.07, 0.08, 1)
    end

    local function Scroll(delta)
        supplyOffset = math.max(0, supplyOffset + delta)
        RefreshSupply()
    end
    supplyPopup:SetScript("OnMouseWheel", function(_, delta) Scroll(-delta) end)
    supplyPopup.rows = {}
    for i = 1, SUPPLY_VISIBLE do
        local rowButton = Button(supplyPopup, "", 380, function(self)
            local row = self.row
            if row and row.kind ~= "missing" then
                supplyChoice, supplyOpen = row, false
                RefreshSupply()
            end
        end)
        local text = rowButton:GetFontString()
        text:SetJustifyH("LEFT")
        text:SetWidth(364)
        text:SetHeight(14)
        rowButton:SetPoint("TOPLEFT", 6, -6 - (i - 1) * 24)
        supplyPopup.rows[i] = rowButton
    end
    supplyPopup.up = Button(supplyPopup, "Previous", 76, function() Scroll(-SUPPLY_VISIBLE) end)
    supplyPopup.up:SetPoint("BOTTOMLEFT", 6, 6)
    supplyPopup.down = Button(supplyPopup, "Next", 76, function() Scroll(SUPPLY_VISIBLE) end)
    supplyPopup.down:SetPoint("BOTTOMRIGHT", -6, 6)
    supplyPopup.range = supplyPopup:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    supplyPopup.range:SetPoint("BOTTOM", 0, 11)
    supplyPopup:Hide()
end

local function SyncSupplyScope(companion)
    G.SetSupplyCompanion(frame:IsShown() and not (addPanel and addPanel:IsShown())
        and CanSupply(companion) and companion.name or nil)
    RefreshSupply()
end

G.OnSupplyChanged = function() if supplyPopup then RefreshSupply() end end

function G.CardState(companion)
    local online = G.HasFlag(companion, "o")
    local dead = G.HasFlag(companion, "d")
    local releasing = G.releasing and G.releasing[companion.name]
    local state = {
        online = online, dead = dead, releasing = releasing,
        status = releasing and "Releasing…" or dead and "Dead" or not online and "Dismissed"
            or G.HasFlag(companion, "g") and "In party" or "Summoned",
    }
    if not online or releasing then return state end

    local skills = G.professionSkills[companion.name]
    if skills then
        state.gather, state.craft = skills.gather, skills.craft
    elseif companion.gather then
        local name, current, cap = string.match(companion.gather, "^(.*%S)%s+(%d+)/(%d+)$")
        if name then state.gather = { name = name, current = tonumber(current), cap = tonumber(cap) } end
    end
    state.quests, state.free = companion.quests, companion.free
    return state
end

local function SkillTrack(parent, x, y)
    local track = CreateFrame("Frame", nil, parent)
    track:SetSize(SKILL_WIDTH, 22)
    track:SetPoint("TOPLEFT", x, y)
    track.text = track:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    track.text:SetPoint("TOPLEFT", 0, 0)
    track.background = track:CreateTexture(nil, "BACKGROUND")
    track.background:SetPoint("BOTTOMLEFT", 0, 1)
    track.background:SetSize(SKILL_WIDTH, 3)
    track.background:SetTexture(1, 1, 1, 0.12)
    track.fill = track:CreateTexture(nil, "ARTWORK")
    track.fill:SetPoint("BOTTOMLEFT", 0, 1)
    track.fill:SetHeight(3)
    return track
end

local function UpdateSkill(track, skill, fallback, r, g, b)
    if not skill or skill.cap == 0 then
        track.text:SetText(COL_MUTED .. (skill and (skill.name .. " · Not learned") or fallback) .. COL_RESET)
        track.background:Hide()
        track.fill:Hide()
        return
    end
    track.text:SetText(("%s  %s%d / %d%s"):format(skill.name, COL_MUTED, skill.current, skill.cap, COL_RESET))
    track.background:Show()
    if skill.current == 0 then
        track.fill:Hide()
    else
        track.fill:SetTexture(r, g, b, 0.8)
        track.fill:SetWidth(math.max(1, SKILL_WIDTH * math.min(1, skill.current / skill.cap)))
        track.fill:Show()
    end
end

local function CreateCard()
    local card = CreateFrame("Frame", nil, frame)
    card:SetSize(WIDTH - 2 * PAD, CARD_HEIGHT)

    card.bg = card:CreateTexture(nil, "BACKGROUND")
    card.bg:SetAllPoints()
    card.bg:SetTexture(1, 1, 1, 0.045)

    -- a hairline along the top edge separates stacked cards without boxing each one in
    card.top = card:CreateTexture(nil, "BORDER")
    card.top:SetPoint("TOPLEFT", 0, 0)
    card.top:SetPoint("TOPRIGHT", 0, 0)
    card.top:SetHeight(1)
    card.top:SetTexture(1, 1, 1, 0.06)

    card.stripe = card:CreateTexture(nil, "ARTWORK")
    card.stripe:SetPoint("TOPLEFT", 0, 0)
    card.stripe:SetPoint("BOTTOMLEFT", 0, 0)
    card.stripe:SetWidth(3)

    card.dot = card:CreateTexture(nil, "ARTWORK")
    card.dot:SetSize(8, 8)
    card.dot:SetPoint("TOPLEFT", 16, -17)

    card.name = card:CreateFontString(nil, "OVERLAY", "GameFontNormal")
    card.name:SetPoint("TOPLEFT", 30, -9)

    card.subtitle = card:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    card.subtitle:SetPoint("TOPLEFT", 16, -29)

    card.questsTitle = Label(card, "QUESTS", 16, -57)
    card.professionsTitle = Label(card, "PROFESSIONS", 224, -57)
    card.suppliesTitle = Label(card, "SUPPLIES", 440, -57)
    card.gatherTrack = SkillTrack(card, 224, -77)
    card.craftTrack = SkillTrack(card, 224, -104)
    card.quests = Label(card, "", 16, -77, 192)
    card.bags = Label(card, "", 440, -77, 192)
    card.merchantHint = Label(card, "Stop by a merchant", 440, -132, 192)
    card.merchantHint:Hide()

    -- the whole card responds to the cursor, so the row under it is obviously the one the
    -- buttons belong to
    card:EnableMouse(true)
    card:SetScript("OnEnter", function(self)
        self.bg:SetTexture(1, 1, 1, 0.075)
        if not self.companion then return end
        GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
        GameTooltip:AddLine(self.companion.name, 1, 0.85, 0.4)
        if G.HasFlag(self.companion, "b") then
            GameTooltip:AddLine("You carry a quest this companion has not picked up yet.", 0.75, 0.75, 0.75, true)
        end
        if self.merchantHint:IsShown() then
            GameTooltip:AddLine("Stop beside a merchant and let your companion catch up. If automatic junk selling is enabled, they can sell eligible junk once both of you are out of combat and not trading.", 1, 0.82, 0.4, true)
        end
        GameTooltip:AddLine("Companions craft and offer surplus automatically. Accept the trade when it opens.", 0.75, 0.75, 0.75, true)
        GameTooltip:Show()
    end)
    card:SetScript("OnLeave", function(self)
        self.bg:SetTexture(1, 1, 1, 0.045)
        GameTooltip:Hide()
    end)

    -- The dead companion's one action: near, the body rises at the usual
    -- resurrection penalty; far, its spirit releases at the graveyard and the ghost
    -- walks home on its own, rejoining when it rises.
    card.reviveButton = Button(card, "Revive", 62, function()
        if card.companion then G.Action("revive", card.companion.name) end
    end, "Raise it beside you - or, if you are too far away, release it to the graveyard and let it walk home.")
    card.reviveButton:SetPoint("TOPRIGHT", -16, -14)
    card.reviveButton:GetFontString():SetTextColor(0.5, 1, 0.6)
    card.reviveButton:Hide()

    card.primary = Button(card, "Summon", 84, function()
        if not card.companion then return end
        G.Action(G.HasFlag(card.companion, "o") and "dismiss" or "summon", card.companion.name)
    end, "Call this companion out, or send it away. It stays bound either way.")
    card.primary:SetPoint("TOPRIGHT", -16, -14)

    card.levelButton = Button(card, "Level", 56, function()
        if card.companion then G.Action("level", card.companion.name) end
    end, "Bring its level up to yours.")
    card.levelButton:SetPoint("TOPRIGHT", -106, -14)

    card.mapButton = Button(card, "Map", 44, function()
        ToggleWorldMap()
        if CompanionPins_Request then CompanionPins_Request() end
    end, "Show companion quest markers on the world map.")
    card.mapButton:SetPoint("TOPLEFT", 16, -100)

    card.trainButton = Button(card, "Train", 60, function()
        if card.companion then G.Action("learn", card.companion.name) end
    end, "Teach this companion missing class spells and spend its unspent talents.")
    card.trainButton:SetPoint("TOPLEFT", 224, -132)
    card.professionsButton = Button(card, "Teach skills", 92, function()
        if card.companion then G.Action("professions", card.companion.name) end
    end, "Teach or update this companion's gathering and crafting professions.")
    card.professionsButton:SetPoint("LEFT", card.trainButton, "RIGHT", 6, 0)

    -- The role is recorded intent, nothing more yet: it decides which strategies the module
    -- will hand this companion later, and "auto" means it keeps its hands off entirely.
    card.roleButton = Button(card, "Role: auto", 118, function()
        if not card.companion then return end
        local ORDER = { "auto", "tank", "healer", "dps", "support" }
        local current = (G.Roles and G.Roles[card.companion.name]) or "auto"
        local at = 1
        for i, role in ipairs(ORDER) do if role == current then at = i end end
        local nextRole = ORDER[(at % #ORDER) + 1]
        G.Roles = G.Roles or {}
        G.Roles[card.companion.name] = nextRole
        card.roleButton:SetText("Role: " .. nextRole)
        G.Run(".companionsync role " .. card.companion.name .. " " .. nextRole)
    end, "What this companion is for. Auto leaves it entirely to its own AI.", false)
    card.roleButton:SetPoint("TOPLEFT", 16, -132)

    card.collectButton = Button(card, "Collect delivery", 112, function()
        local companion = card.companion
        if CanSupply(companion) then G.CollectDelivery(companion.name) end
    end, "Stand nearby and out of combat to collect what this companion has ready. You still accept the trade.")
    card.collectButton:SetPoint("TOPLEFT", 440, -100)

    card.supplyPicker = Button(card, "Choose supply  ▾", 300, function()
        local supply = G.supply
        if not (supply and CanSupply(card.companion) and supply.name == card.companion.name) then return end
        if not supply.rows or #supply.rows == 0 then
            supplyOpen, supplyOffset = true, 0
            G.RequestSupplyCatalog()
        else
            supplyOpen = not supplyOpen
            RefreshSupply()
        end
    end, "See this companion's spare items and supported recipes.")
    card.supplyPicker:SetPoint("TOPLEFT", 16, -162)
    card.supplyPicker:GetFontString():SetWidth(284)
    card.supplyPicker:GetFontString():SetHeight(14)
    card.supplyAction = Button(card, "Take / Craft", 118, function()
        if not (G.supply and CanSupply(card.companion) and G.supply.name == card.companion.name) then return end
        G.RequestSupplyItem(supplyChoice)
    end, "Ask for the selected item. Crafting uses carried materials and makes one batch. You accept the trade.", true)
    card.supplyAction:SetPoint("TOPLEFT", 322, -162)
    card.supplyRefresh = Button(card, "Refresh supplies", 126, function()
        if not (G.supply and CanSupply(card.companion) and G.supply.name == card.companion.name) then return end
        supplyOpen, supplyOffset = false, 0
        G.RequestSupplyCatalog()
    end, "Check this companion's current supplies and crafting availability.")
    card.supplyRefresh:SetPoint("TOPLEFT", 446, -162)
    card.supplyStatus = Label(card, "", 16, -189, WIDTH - 4 * PAD)
    card.supplyStatus:SetHeight(22)

    -- Release and untrack used to hide behind a "..." dropdown. A control the player has to
    -- discover is a control that does not get used: the release never reached the server at all.
    -- They are plain buttons now; release is a request for offline retirement.
    card.untrackButton = Button(card, "Untrack", 62, function()
        if card.companion then G.ConfirmUnbind(card.companion.name) end
    end, "Stop following this companion's quests. The bot stays yours.")
    card.untrackButton:SetPoint("BOTTOMRIGHT", -12, -12)

    card.releaseButton = Button(card, "Release", 62, function()
        if card.companion then G.ConfirmRelease(card.companion.name) end
    end, "Retire it. Its slot frees once it has left the world.")
    card.releaseButton:SetPoint("BOTTOMRIGHT", -78, -12)
    card.releaseButton:GetFontString():SetTextColor(1, 0.6, 0.6)

    return card
end

local function LayoutWindow(count)
    if not frame then return end

    local panelOpen = addPanel and addPanel:IsShown()
    local body = panelOpen and (ADD_PANEL_HEIGHT + 6)
        or count > 0 and (ROSTER_HEIGHT + CARD_HEIGHT + 8) or EMPTY_HEIGHT
    frame:SetHeight(HEADER_HEIGHT + body + FOOTER_HEIGHT)

    for index, card in ipairs(cards) do
        local tab = roster[index]
        local companion = index <= count and G.companions[index]
        if not panelOpen and companion then tab:Show() else tab:Hide() end
        if not panelOpen and companion and companion.name == selectedName then
            card:Show()
        else card:Hide() end
    end

    if frame.hint then
        if panelOpen then frame.hint:Hide() else frame.hint:Show() end
    end

    if frame.empty then
        if panelOpen or count > 0 then frame.empty:Hide() else frame.empty:Show() end
    end

    if frame.notice then
        if panelOpen then frame.notice:Hide() end
    end
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

local function PoolState() return G.SnapshotState("pool") end

local function RefreshAddPanel()
    if not addPanel or not addPanel:IsShown() then return end

    local stock = G.StockFor(pick.race, pick.class)

    UIDropDownMenu_SetText(raceDropdown, G.RaceName(pick.race))
    UIDropDownMenu_SetText(classDropdown, G.ClassName(pick.class))
    createButton:SetText("Create " .. G.RaceName(pick.race) .. " " .. G.ClassName(pick.class))

    local state = PoolState()
    EnableButton(createButton, G.ServerAlive() and state ~= "disabled")
    if state == "disabled" then
        stockLabel:SetText(COL_WARN .. "Pool creation is disabled on this server." .. COL_RESET)
    elseif not G.ServerAlive() then
        stockLabel:SetText(COL_BAD .. "Pool stock unavailable: server not answering." .. COL_RESET)
    elseif state == "missing" then
        stockLabel:SetText(COL_MUTED .. "Waiting for pool stock…" .. COL_RESET)
    elseif state == "stale" then
        stockLabel:SetText(COL_WARN .. "Pool stock is stale; the server checks availability on Create." .. COL_RESET)
    elseif state == "empty" then
        stockLabel:SetText(COL_WARN .. "The pool is empty right now." .. COL_RESET)
    elseif stock > 0 then
        stockLabel:SetText(("%s%d free in the pool%s"):format(COL_GOOD, stock, COL_RESET))
    else
        stockLabel:SetText(COL_BAD .. "none free - the server will refuse this combination" .. COL_RESET)
    end
end

local DROPDOWN_TEXT_INSET = 16      -- UIDropDownMenuTemplate puts its text inside a button

local function CreateAddPanel()
    addPanel = CreateFrame("Frame", nil, frame)
    addPanel:SetPoint("TOPLEFT", PAD, -HEADER_HEIGHT)
    addPanel:SetSize(WIDTH - 2 * PAD, ADD_PANEL_HEIGHT)

    -- The panel has fixed content, so it must not be sized by what is left of the window:
    -- anchored to the bottom it got 74 px inside a one-companion frame and every row of
    -- controls landed on top of the next. It keeps its own height and the window grows.
    addPanel:SetScript("OnShow", G.Refresh)
    addPanel:SetScript("OnHide", G.Refresh)
    addPanel:Hide()

    addPanel.bg = addPanel:CreateTexture(nil, "BACKGROUND")
    addPanel.bg:SetAllPoints()
    addPanel.bg:SetTexture(unpack(panelTone))

    addPanel.edge = CreateFrame("Frame", nil, addPanel)
    addPanel.edge:SetAllPoints()
    addPanel.edge:SetBackdrop({
        edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
        edgeSize = 10,
        insets = { left = 1, right = 1, top = 1, bottom = 1 },
    })
    addPanel.edge:SetBackdropBorderColor(1, 1, 1, 0.12)

    addPanel.title = addPanel:CreateFontString(nil, "OVERLAY", "GameFontNormal")
    addPanel.title:SetPoint("TOPLEFT", 18, -14)
    addPanel.title:SetText("New companion")

    addPanel.hint = addPanel:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    addPanel.hint:SetPoint("TOPLEFT", 18, -34)
    Wrap(addPanel.hint, WIDTH - 64)
    addPanel.hint:SetText(COL_MUTED ..
        "A reviewed level-1 body is claimed, named and called to your side." .. COL_RESET)

    -- row 1: race and class
    Label(addPanel, "Race", 20, -62)
    Label(addPanel, "Class", 216, -62)

    raceDropdown = CreateFrame("Frame", "CSYNCRaceDropdown", addPanel, "UIDropDownMenuTemplate")
    raceDropdown:SetPoint("TOPLEFT", 4, -74)
    UIDropDownMenu_SetWidth(raceDropdown, 150)
    UIDropDownMenu_Initialize(raceDropdown, function()
        local known = PoolState() == "ready" or PoolState() == "empty"
        for _, race in ipairs(RaceList()) do
            UIDropDownMenu_AddButton({
                text = race.text .. (not known and "" or race.stock > 0 and ("   " .. COL_MUTED .. race.stock .. COL_RESET)
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

    classDropdown = CreateFrame("Frame", "CSYNCClassDropdown", addPanel, "UIDropDownMenuTemplate")
    classDropdown:SetPoint("TOPLEFT", 200, -74)
    UIDropDownMenu_SetWidth(classDropdown, 150)
    UIDropDownMenu_Initialize(classDropdown, function()
        local known = PoolState() == "ready" or PoolState() == "empty"
        for _, class in ipairs(ClassList(pick.race)) do
            UIDropDownMenu_AddButton({
                text = not known and class.text or class.stock > 0 and (class.text .. "   " .. COL_MUTED .. class.stock .. COL_RESET)
                    or ("|cff5a5a5a" .. class.text .. COL_RESET),
                value = class.id,
                checked = (pick.class == class.id),
                disabled = known and class.stock <= 0,
                func = function()
                    pick.class = class.id
                    RefreshAddPanel()
                end,
            })
        end
    end)

    -- row 2: name
    Label(addPanel, "Name", 20, -132)

    nameBox = CreateFrame("EditBox", "CSYNCNameBox", addPanel)
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
    Wrap(stockLabel, WIDTH - 64)

    serverLine = addPanel:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    serverLine:SetPoint("TOPLEFT", 20, -206)
    serverLine:SetHeight(32)          -- two lines; the actions sit below it
    Wrap(serverLine, WIDTH - 64)
    serverLine:SetText(COL_MUTED .. "the server's answer appears here" .. COL_RESET)

    -- row 4: actions
    createButton = Button(addPanel, "Create", 190, function()
        if G.CreateCompanion(pick.race, pick.class, nameBox:GetText() or "") then
            nameBox:ClearFocus()
        end
    end, "Claim a body from the pool with this race, class and name.")
    createButton:SetPoint("BOTTOMLEFT", 20, 12)

    local cancel = Button(addPanel, "Cancel", 80, function() addPanel:Hide() end)
    cancel:SetPoint("BOTTOMLEFT", 218, 12)

    return addPanel
end

local POLL_SECONDS = 15
local lastPoolRequest, lastRosterRequest

local function RequestPool()
    if not G.ServerAlive() or PoolState() == "disabled" then return end
    local now = time()
    if lastPoolRequest and now - lastPoolRequest < POLL_SECONDS then return end
    if G.Run(".companionsync pool") then lastPoolRequest = now end
end

function G.ShowAddPanel()
    if not addPanel then CreateAddPanel() end

    pick.race = G.PlayerRaces()[1] or 1
    pick.class = 5
    nameBox:SetText(G.SuggestName())
    serverLine:SetText(COL_MUTED .. "the server's answer appears here" .. COL_RESET)

    addPanel.rosterCount = #G.companions
    addPanel:Show()
    RefreshAddPanel()
    RequestPool()
end

function G.OnStockChanged() RefreshAddPanel() end

-- Poll local freshness, not the realm. Normally the roster is pushed on change.
function G.PollState(force)
    if not G.ServerAlive() then return end
    if addPanel and addPanel:IsShown() and (force or PoolState() == "missing" or PoolState() == "stale") then
        RequestPool()
    end
    local now = time()
    local state = G.SnapshotState("roster")
    if not force and (state == "ready" or state == "empty") then return end
    if lastRosterRequest and now - lastRosterRequest < POLL_SECONDS then return end
    if G.Run(".companionsync refresh roster") then lastRosterRequest = now end
end

function G.OnServerMessage(text)
    -- The server answers button presses with plain system lines ("that is not one of your
    -- companions", "the pool has no free body"). They used to appear only on the Add panel,
    -- so a refusal for Summon, Dismiss or Release came back as nothing at all - exactly the
    -- silence this module exists to end.
    if frame and frame.notice then
        frame.notice:SetText(COL_WARN .. text .. COL_RESET)
        frame.notice:Show()
        frame.noticeExpires = time() + 8   -- stale refusals must not read as current state
    end

    if addPanel and addPanel:IsShown() and serverLine then
        serverLine:SetText(COL_WARN .. text .. COL_RESET)
    end
end

-- ---------------------------------------------------------------------------
-- window
-- ---------------------------------------------------------------------------

local function RefreshDelivery()
    if not frame or not frame.delivery then return end
    local order = G.delivery
    local color = order and (order.state == "completed" and COL_GOOD
        or order.state == "failed" and COL_BAD or order.state == "cancelled" and COL_WARN
        or order.state == "unconfirmed" and COL_WARN or COL_MUTED)
    frame.delivery:SetText(order and (color .. order.name .. ": " .. order.message .. COL_RESET) or "")
end

G.OnDeliveryChanged = function()
    if frame and frame:IsShown() then
        G.Refresh()
    elseif G.delivery and G.delivery.wireNonce and not G.DeliveryPending() then
        G.Warn(G.delivery.name .. ": " .. G.delivery.message)
    end
end

local emptyRosterText = "No companions yet.\n\nPress + Add companion, pick a race and a class, then Create.\nFour is the most: a party of five, counting you."
local readinessKey
local function Readiness()
    local live, state = G.ServerAlive(), G.SnapshotState("roster")
    local presence = live and "live" or (G.lastServerLineAt or G.presenceProbeExpired) and "lost" or "waiting"
    return presence, state, presence .. ":" .. state .. ":" .. PoolState()
end

G.OnReadinessChanged = function()
    local _, _, key = Readiness()
    if key == readinessKey then return end
    if frame and frame:IsShown() then G.Refresh() else readinessKey = key end
    if G.RefreshLauncherBadge then G.RefreshLauncherBadge() end
end

function G.Refresh()
    if not frame then return end

    local count = #G.companions

    local presence, rosterState, key = Readiness()
    readinessKey = key
    local stale = rosterState == "stale"
    local rosterMessage = presence == "waiting" and "Waiting for the server…"
        or presence == "lost" and "server not answering"
        or rosterState == "missing" and "Waiting for the roster…"
        or stale and "Roster is stale; waiting for an update…"
    if frame.count then
        local state = rosterMessage and ((presence == "lost" and COL_BAD or COL_WARN) .. rosterMessage .. COL_RESET)
            or next(G.releasing) and (COL_WARN .. "working - releasing…" .. COL_RESET)
            or (COL_GOOD .. "server answering" .. COL_RESET)
        frame.count:SetText(count == 0 and state
            or (COL_MUTED .. count .. (stale and " last seen · " or count == 1 and " companion · " or " companions · ") .. COL_RESET) .. state)
    end
    if frame.empty then
        local hint = G.poolEnabled == false and "No companions yet. Pool creation is disabled on this server." or emptyRosterText
        frame.empty:SetText(COL_MUTED .. (rosterMessage or hint) .. COL_RESET)
    end

    local selected
    for _, entry in ipairs(G.companions) do
        if entry.name == selectedName then selected = entry break end
    end
    selected = selected or G.companions[1]
    selectedName = selected and selected.name or nil

    for i = 1, MAX_CARDS do
        local card, tab = cards[i], roster[i]
        local companion = G.companions[i]
        tab.companion = companion

        if companion then
            card.companion = companion
            tab:SetText(companion.name)
            if companion.name == selectedName then
                tab:LockHighlight()
                tab:GetFontString():SetTextColor(1, 0.82, 0.4)
            else
                tab:UnlockHighlight()
                tab:GetFontString():SetTextColor(0.76, 0.76, 0.78)
            end

            local r, g, b = G.ClassColor(companion.classId)
            local online = G.HasFlag(companion, "o")

            card.name:SetText(companion.name)
            card.name:SetTextColor(online and r or 0.5, online and g or 0.5, online and b or 0.5)
            card.dot:SetTexture(r, g, b, online and 1 or 0.3)
            card.stripe:SetTexture(r, g, b, online and 0.85 or 0.25)

            local state = G.CardState(companion)
            local statusColor = stale and COL_MUTED or state.dead and COL_BAD or state.releasing and COL_WARN
                or state.online and COL_GOOD or COL_MUTED
            local subtitle = statusColor .. (stale and "Last seen: " or "") .. state.status .. COL_RESET
            if state.online and companion.level > 0 then
                subtitle = ("%sLevel %d %s%s  ·  %s"):format(
                    COL_MUTED, companion.level, G.ClassName(companion.classId), COL_RESET, subtitle)
            end
            card.subtitle:SetText(subtitle)

            local lowBags = rosterState == "ready" and state.free ~= nil and state.free < 8
                and not state.dead and G.HasFlag(companion, "g")
            if lowBags then
                card.merchantHint:Show()
            elseif card.merchantHint:IsShown() then
                card.merchantHint:Hide()
                if GameTooltip:IsOwned(card) then GameTooltip:Hide() end
            end
            if state.free ~= nil then
                UpdateSkill(card.gatherTrack, state.gather, "Gathering · Unavailable", 0.46, 0.73, 0.52)
                UpdateSkill(card.craftTrack, state.craft, "Crafting · Unavailable", 0.72, 0.62, 0.37)
                card.gatherTrack:Show()
                card.craftTrack:Show()
                local free = state.free
                local bags = free == 0 and "Bags full" or ("%d bag slots free"):format(free)
                card.quests:SetText(COL_MUTED .. ("%d active quest%s"):format(state.quests, state.quests == 1 and "" or "s") .. COL_RESET)
                card.bags:SetText((lowBags and COL_WARN or COL_MUTED) .. bags .. COL_RESET)
                card.quests:Show()
                card.bags:Show()
            else
                card.gatherTrack:Hide()
                card.craftTrack:Hide()
                card.quests:SetText(COL_MUTED .. (state.releasing and "Returning to the pool…" or "Summon to see quests.") .. COL_RESET)
                card.quests:Show()
                card.bags:SetText(COL_MUTED .. (state.releasing and "Unavailable" or "Summon to collect.") .. COL_RESET)
                card.bags:Show()
            end
            local canCollect = CanSupply(companion) and not G.DeliveryPending()
            EnableButton(card.collectButton, canCollect)
            if canCollect then
                card.collectButton:GetFontString():SetTextColor(1, 0.82, 0.4)
            else
                card.collectButton:GetFontString():SetTextColor(0.53, 0.53, 0.55)
            end
            if online and not state.dead and not state.releasing then
                card.trainButton:Show()
                card.professionsButton:Show()
            else
                card.trainButton:Hide()
                card.professionsButton:Hide()
            end

            if card.roleButton then
                local role = (G.Roles and G.Roles[companion.name]) or "auto"
                card.roleButton:SetText("Role: " .. role)
                local text = card.roleButton:GetFontString()
                if role == "auto" then text:SetTextColor(0.62, 0.62, 0.66)
                else text:SetTextColor(1.0, 0.82, 0.30) end
            end

            card.primary:SetText(online and "Dismiss" or "Summon")
            card.primary:GetFontString():SetTextColor(online and 1 or 0.35, online and 0.82 or 1, 0.35)
            card.primary:ClearAllPoints()
            card.primary:SetPoint("TOPRIGHT", state.dead and -84 or -16, -14)
            if state.releasing then card.primary:Hide() else card.primary:Show() end
            if online and not state.dead and not state.releasing and companion.level > 0 and not G.HasFlag(companion, "s") then
                card.levelButton:Show()
            else card.levelButton:Hide() end
            if state.dead and not state.releasing then card.reviveButton:Show() else card.reviveButton:Hide() end
            if online and not state.releasing then card.mapButton:Show() else card.mapButton:Hide() end
            if state.releasing then
                card.releaseButton:Hide()
                card.untrackButton:Hide()
            else
                card.releaseButton:Show()
                card.untrackButton:Show()
            end
            EnableButton(card.releaseButton, not stale)

        else
            card.companion = nil
        end
    end

    -- The footer line says what the roster cannot: how many slots are left. It is anchored to
    -- the bottom of the frame, so it moves with the height instead of being repositioned.
    if frame.hint then
        local free = MAX_CARDS - count

        if count == 0 then
            frame.hint:SetText("")
        elseif G.poolEnabled == false then
            frame.hint:SetText(COL_MUTED .. "Pool creation is disabled on this server." .. COL_RESET)
        elseif free > 0 then
            frame.hint:SetText(("%s%d of %d companions  ·  %d slot%s free - press + Add companion%s"):format(
                COL_MUTED, count, MAX_CARDS, free, free == 1 and "" or "s", COL_RESET))
        else
            frame.hint:SetText(("%s%d of %d companions - a full group of five with you%s"):format(
                COL_MUTED, count, MAX_CARDS, COL_RESET))
        end
    end

    if addPanel and addPanel:IsShown() and count > addPanel.rosterCount then addPanel:Hide() end
    LayoutWindow(count)
    SyncSupplyScope(selected)
    RefreshAddPanel()
    RefreshDelivery()
end

local function CreateWindow()
    local dragonWindow = Chrome("ApplyNineSlice")
    panelTone = dragonWindow and { 0.1216, 0.1176, 0.1294, 0.98 } or { 0.05, 0.05, 0.06, 0.95 }
    buttonIdle = dragonWindow and { 0.80, 0.72, 0.60, 0.10 } or { 1, 1, 1, 0.07 }
    buttonHover = dragonWindow and { 1.00, 0.85, 0.59, 0.18 } or { 1, 1, 1, 0.16 }
    frame = CreateFrame("Frame", "CompanionSyncFrame", UIParent)
    frame:SetSize(WIDTH, HEADER_HEIGHT + CARD_HEIGHT + FOOTER_HEIGHT)
    frame:SetFrameStrata("DIALOG")
    frame:SetClampedToScreen(true)
    frame:SetMovable(true)
    frame:EnableMouse(true)
    frame:RegisterForDrag("LeftButton")
    frame:SetScript("OnHide", function()
        G.SetSupplyCompanion(nil)
        RefreshSupply()
    end)
    frame:SetScript("OnDragStart", frame.StartMoving)
    frame:SetScript("OnDragStop", function(self)
        self:StopMovingOrSizing()
        local point, _, _, x, y = self:GetPoint()
        CompanionSyncDB.pos = { point = point, x = x, y = y }
    end)

    -- Shadow, then the panel: depth without a visible frame edge, which is what made the old
    -- version read as a dialogue box from a different expansion.
    frame.shadow = frame:CreateTexture(nil, "BACKGROUND")
    frame.shadow:SetPoint("TOPLEFT", -3, 3)
    frame.shadow:SetPoint("BOTTOMRIGHT", 3, -3)
    frame.shadow:SetTexture(0, 0, 0, 0.5)

    frame.bg = frame:CreateTexture(nil, "BACKGROUND")
    frame.bg:SetAllPoints()
    frame.bg:SetTexture(unpack(panelTone))

    if dragonWindow then
        dragonWindow(frame, "metalframe", "BORDER")
    else
        frame.border = CreateFrame("Frame", nil, frame)
        frame.border:SetAllPoints()
        frame.border:SetBackdrop({
            edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
            edgeSize = 12,
            insets = { left = 3, right = 3, top = 3, bottom = 3 },
        })
        frame.border:SetBackdropBorderColor(1, 1, 1, 0.10)
    end

    frame.title = frame:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
    frame.title:SetPoint("TOPLEFT", PAD, -12)
    frame.title:SetText((dragonWindow and "|cffffd100" or "|cffffc94d") .. "Companions|r")

    frame.count = frame:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    frame.count:SetPoint("TOPLEFT", PAD, -34)

    -- The notice used to be a ratchet: the last refusal stayed current-looking forever,
    -- and a click answered nothing until a chat line happened to arrive. A notice now
    -- fades after a few seconds unless a newer one replaced it, and a click writes its
    -- own "asking the server..." first.
    frame.noticeExpires = 0

    frame.divider = frame:CreateTexture(nil, "BORDER")
    frame.divider:SetPoint("TOPLEFT", PAD, -HEADER_HEIGHT + 1)
    frame.divider:SetPoint("TOPRIGHT", -PAD, -HEADER_HEIGHT + 1)
    frame.divider:SetHeight(1)
    frame.divider:SetTexture(1, 1, 1, 0.07)

    local add = Button(frame, "+ Add companion", 124, function()
        if addPanel and addPanel:IsShown() then addPanel:Hide() else G.ShowAddPanel() end
    end, "Create a companion from the pool.", true)
    -- clear of the close button: UIPanelCloseButton is 32 px wide at -6, so anything ending
    -- before -44 leaves the two controls their own space
    add:SetPoint("TOPRIGHT", -46, -14)

    -- Formation presets: spacing is a property of the owner, not of one card, so the controls
    -- sit in the header next to Add companion. Only the distance is ours - combat hands movement
    -- straight back to the bot's own AI, which is why there is no "attack" button here.
    local FORMATIONS = {
        { name = "close",  label = "Close",  hint = "2 yd behind you - arm's length, for handing items over" },
        { name = "tight",  label = "Tight",  hint = "4 yd behind you - close enough to see your back" },
        { name = "loose",  label = "Loose",  hint = "12 yd behind you - room to cast" },
        { name = "spread", label = "Spread", hint = "20 yd behind you - out of each other's way" },
        { name = "off",    label = "Off",    hint = "leave the bot's own follow alone" },
    }
    frame.formation = {}
    for i, preset in ipairs(FORMATIONS) do
        local button = Button(frame, preset.label, 46, function()
            G.Formation = preset.name
            G.Run(".companionsync formation " .. preset.name)
            if G.RefreshFormation then G.RefreshFormation() end
        end, "Formation: companions hold " .. preset.hint, false)
        -- left to right: Close, Tight, Loose, Spread, Off - below Add companion, above the roster
        button:SetPoint("TOPRIGHT", -46 - (#FORMATIONS - i) * 50, -42)
        frame.formation[preset.name] = button
    end

    G.RefreshFormation = function()
        local active = G.Formation or "off"
        for name, button in pairs(frame.formation) do
            local text = button:GetFontString()
            if name == active then text:SetTextColor(1.0, 0.82, 0.30)
            else text:SetTextColor(0.62, 0.62, 0.66) end
        end
    end
    G.RefreshFormation()

    frame.elapsed = 0
    frame:SetScript("OnUpdate", function(self, delta)
        self.elapsed = self.elapsed + delta
        G.ExpireSupply()

        if frame.notice and frame.notice:IsShown() and frame.noticeExpires > 0 and time() > frame.noticeExpires then
            frame.notice:Hide()
            frame.noticeExpires = 0
        end

        if self.elapsed >= POLL_SECONDS then
            self.elapsed = 0
            G.PollState()
        end
    end)

    local closeSkin = Chrome("CreateCloseButton")
    frame.close = closeSkin and closeSkin(frame, { size = 24 })
        or CreateFrame("Button", nil, frame, "UIPanelCloseButton")
    frame.close:SetPoint("TOPRIGHT", -6, -6)

    cards, roster = {}, {}
    for i = 1, MAX_CARDS do
        cards[i] = CreateCard()
        cards[i]:SetPoint("TOPLEFT", PAD, -(HEADER_HEIGHT + ROSTER_HEIGHT))
        local tab = Button(frame, "", 150, function(self)
            if not self.companion then return end
            selectedName = self.companion.name
            frame.notice:Hide()
            G.Refresh()
        end)
        tab:SetPoint("TOPLEFT", PAD + (i - 1) * 156, -HEADER_HEIGHT - 4)
        roster[i] = tab
    end
    CreateSupplyPopup()

    -- Both pinned to the bottom, above the footer, so they travel with the frame's height.
    frame.notice = frame:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    frame.notice:SetPoint("BOTTOMLEFT", PAD, 50)
    Wrap(frame.notice, WIDTH - 2 * PAD)
    frame.notice:Hide()

    frame.hint = frame:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    frame.hint:SetPoint("BOTTOMLEFT", PAD, 36)
    Wrap(frame.hint, WIDTH - 2 * PAD)

    frame.delivery = frame:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    frame.delivery:SetPoint("BOTTOMLEFT", PAD, 65)
    frame.delivery:SetHeight(22)
    Wrap(frame.delivery, WIDTH - 2 * PAD)

    frame.empty = frame:CreateFontString(nil, "OVERLAY", "GameFontNormal")
    frame.empty:SetPoint("TOPLEFT", PAD, -(HEADER_HEIGHT + 8))
    Wrap(frame.empty, WIDTH - 2 * PAD)
    frame.empty:SetSpacing(4)

    frame.footline = frame:CreateTexture(nil, "BORDER")
    frame.footline:SetPoint("BOTTOMLEFT", PAD, FOOTER_HEIGHT)
    frame.footline:SetPoint("BOTTOMRIGHT", -PAD, FOOTER_HEIGHT)
    frame.footline:SetHeight(1)
    frame.footline:SetTexture(1, 1, 1, 0.07)

    local footer = CreateFrame("Frame", nil, frame)
    footer:SetPoint("BOTTOMLEFT", PAD, 10)
    footer:SetPoint("BOTTOMRIGHT", -PAD, 10)
    footer:SetHeight(22)

    local Ack = function()
        if frame.notice then
            frame.notice:SetText(COL_MUTED .. "asking the server..." .. COL_RESET)
            frame.notice:Show()
            frame.noticeExpires = time() + 4
        end
    end

    local summonAll = Button(footer, "Summon missing", 108, function() Ack() G.Action("summon") end,
        "Bring out every companion that is not with you.")
    summonAll:SetPoint("LEFT", 0, 0)

    local dismissAll = Button(footer, "Dismiss all", 84, function() Ack() G.Action("dismiss") end,
        "Send every companion away. They stay bound.")
    dismissAll:SetPoint("LEFT", summonAll, "RIGHT", 6, 0)

    local refresh = Button(footer, "Refresh", 66, function()
        G.PollState(true)
    end, "Refresh companion cards without reloading the map or pool.")
    refresh:SetPoint("LEFT", dismissAll, "RIGHT", 6, 0)

    local quests = Button(footer, "Group quests", 90, function() Ack() G.Run(".companionsync quests") end,
        "Print what each companion is carrying to chat.")
    quests:SetPoint("LEFT", refresh, "RIGHT", 6, 0)

    local catchUp = Button(footer, "Ground quests", 96, function() G.Run(".companionsync catchup") end,
        "Finish a quest the companion cannot gather for itself. Collects only items that lie on the ground.")
    catchUp:SetPoint("LEFT", quests, "RIGHT", 6, 0)

    LayoutWindow(0)
    frame:Hide()

    return frame
end

function G.Toggle()
    if not CompanionSyncDB then InitDB() end
    if not frame then CreateWindow() end

    if frame:IsShown() then
        frame:Hide()
    else
        local pos = CompanionSyncDB.pos
        frame:ClearAllPoints()
        frame:SetPoint(pos.point or "CENTER", UIParent, pos.point or "CENTER", pos.x or 0, pos.y or 60)
        frame:Show()
        G.ExpireRequests()
        G.Refresh()
        G.PollState()
    end
end

G.OnCompanionsChanged = function()
    if frame and frame:IsShown() then G.Refresh() end
    if G.RefreshLauncherBadge then G.RefreshLauncherBadge() end
end

-- ---------------------------------------------------------------------------
-- launcher + startup
-- ---------------------------------------------------------------------------

-- A square plate, not a disc. The round tracking border the first version borrowed is the
-- minimap's own language for "another tracker", and against the clock ring it read as a stray
-- lens. A flat dark plate with a hairline border and the icon inset looks like a button that
-- belongs to an interface, and it still sits in the minimap corner where people look for it.
local launcher

local function RingRadius(anchor)
    if anchor ~= Minimap then return 84 end
    local width, height = anchor:GetWidth(), anchor:GetHeight()
    return math.max(type(width) == "number" and width or 140, type(height) == "number" and height or 140) / 2 + 14
end

local function Finite(n) return type(n) == "number" and n > -math.huge and n < math.huge end

local function LauncherScale(frame)
    local scale = frame:GetEffectiveScale()
    return Finite(scale) and scale > 0 and scale or 1
end

local function PlaceLauncher(saved)
    local anchor = Minimap or UIParent
    local radius = RingRadius(anchor)
    local x, y = -radius, 0
    if type(saved) == "table" and Finite(saved.x) and Finite(saved.y) and not saved.legacy
        and (saved.version == 2 or (saved.version == nil and not (saved.x == 6 and saved.y == -60))) then
        local distance = math.sqrt(saved.x * saved.x + saved.y * saved.y)
        if distance > 0.001 and Finite(distance) then
            local clamp = math.min(1, radius / distance)
            x, y = saved.x * clamp, saved.y * clamp
        end
    end
    if CompanionSyncDB then CompanionSyncDB.launcher = { version = 2, x = x, y = y } end
    if launcher then
        local scale = LauncherScale(anchor) / LauncherScale(launcher)
        launcher:ClearAllPoints()
        launcher:SetPoint("CENTER", anchor, "CENTER", x * scale, y * scale)
    end
end

local function CreateLauncher()
    -- Parent to UIParent and anchor to the minimap: a child of Minimap inherits its draw
    -- order and can end up behind the map itself on clients with a restyled minimap.
    launcher = CreateFrame("Button", "CompanionSyncLauncher", UIParent)
launcher:SetSize(24, 24)
PlaceLauncher(CompanionSyncDB and CompanionSyncDB.launcher)
launcher:SetFrameStrata("HIGH")
launcher:SetBackdrop({
    edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
    edgeSize = 9,
    insets = { left = 2, right = 2, top = 2, bottom = 2 },
})
local BORDER_IDLE = { 0.32, 0.30, 0.27, 1 }
local BORDER_HOVER = { 1, 0.79, 0.30, 1 }
launcher:SetBackdropBorderColor(unpack(BORDER_IDLE))
launcher:SetBackdropColor(0, 0, 0, 0)

launcher.plate = launcher:CreateTexture(nil, "BACKGROUND")
launcher.plate:SetPoint("TOPLEFT", 2, -2)
launcher.plate:SetPoint("BOTTOMRIGHT", -2, 2)
launcher.plate:SetTexture(0.03, 0.03, 0.04, 0.9)

launcher.icon = launcher:CreateTexture(nil, "ARTWORK")
launcher.icon:SetPoint("TOPLEFT", 3, -3)
launcher.icon:SetPoint("BOTTOMRIGHT", -3, 3)
launcher.icon:SetTexture("Interface\\Icons\\Ability_Hunter_BeastCall")
launcher.icon:SetTexCoord(0.07, 0.93, 0.07, 0.93)

-- how many companions are bound, so the corner says something before the window does
launcher.badge = launcher:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
launcher.badge:SetPoint("BOTTOMRIGHT", 3, -3)
launcher.badge:SetJustifyH("RIGHT")
launcher.badge:Hide()

local function RefreshLauncherBadge()
    if not (launcher and launcher.badge) then return end
    local count = G.companions and #G.companions or 0
    if G.SnapshotState("roster") == "ready" and count > 0 then
        launcher.badge:SetText("|cffffd200" .. count .. "|r")
        launcher.badge:Show()
    else
        launcher.badge:Hide()
    end
end

G.RefreshLauncherBadge = RefreshLauncherBadge

launcher:SetMovable(true)
launcher:RegisterForDrag("LeftButton")
launcher:SetClampedToScreen(true)

launcher:SetScript("OnMouseDown", function(self) self.dragged = nil end)
launcher:SetScript("OnDragStart", function(self)
    self.dragged = true
    self:StartMoving()
    GameTooltip:Hide()
end)

launcher:SetScript("OnDragStop", function(self)
    self:StopMovingOrSizing()
    self:SetUserPlaced(false) -- use the versioned save, not WoW's auto-placement

    -- Take the cursor's angle around the minimap: the cursor position is divided by the
    -- MINIMAP's effective scale and measured against the minimap's own centre, so both
    -- numbers live in one coordinate space. The previous version mixed the button's
    -- GetLeft (parent space) with the minimap's GetCenter (the suite may have reparented
    -- and rescaled it) - two different scales produced a different anchor every drag.
    local anchor = Minimap or UIParent
    if not anchor then return end

    local scale = LauncherScale(anchor)
    local cx, cy = GetCursorPosition()
    cx, cy = cx / scale, cy / scale
    local mx, my = anchor:GetCenter()

    if not (Finite(mx) and Finite(my)) then
        PlaceLauncher(CompanionSyncDB and CompanionSyncDB.launcher)
        return
    end
    if not CompanionSyncDB then InitDB() end
    PlaceLauncher({ version = 2, x = cx - mx, y = cy - my })
end)

launcher:SetScript("OnClick", function(self)
    if self.dragged then
        self.dragged = nil
        return
    end
    G.Toggle()
end)
launcher:SetScript("OnEnter", function(self)
    self:SetBackdropBorderColor(unpack(BORDER_HOVER))
    GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
    GameTooltip:AddLine("Companions", 1, 0.79, 0.30)
    GameTooltip:AddLine("Drag to move - click to open", 0.6, 0.6, 0.6)
    local state = G.SnapshotState("roster")
    local count = G.companions and #G.companions or 0
    if state == "ready" then
        GameTooltip:AddLine(("%d bound - click to manage"):format(count), 1, 1, 1)
    elseif state == "stale" then
        GameTooltip:AddLine("Roster is stale - click for status", 1, 1, 1)
    elseif state == "missing" then
        GameTooltip:AddLine("Roster not received - click for status", 1, 1, 1)
    elseif G.poolEnabled == false then
        GameTooltip:AddLine("None bound - pool creation is disabled", 1, 1, 1)
    else
        GameTooltip:AddLine("None yet - click to add one", 1, 1, 1)
    end
    GameTooltip:Show()
end)
launcher:SetScript("OnLeave", function(self)
    self:SetBackdropBorderColor(unpack(BORDER_IDLE))
    GameTooltip:Hide()
end)

    return launcher
end

-- A button that fails to build must not take the rest of the window with it, and it must not
-- vanish silently: the first version died on a bad escape and the only symptom was no button.
local built, failure = pcall(CreateLauncher)
if not built then
    DEFAULT_CHAT_FRAME:AddMessage("|cffff6a6aCompanion Sync|r launcher failed: " .. tostring(failure))
end
local boot = CreateFrame("Frame")
boot:RegisterEvent("ADDON_LOADED")
boot:RegisterEvent("PLAYER_ENTERING_WORLD")
boot:SetScript("OnEvent", function(self, event, arg1)
    if event == "ADDON_LOADED" then
        if arg1 == addonName then
            InitDB()
            PlaceLauncher(CompanionSyncDB.launcher)
        end
        return
    end

    self:UnregisterEvent("PLAYER_ENTERING_WORLD")
    if not CompanionSyncDB then InitDB() end
    PlaceLauncher(CompanionSyncDB.launcher)
    if not frame then CreateWindow() end

    -- One line, once per login, so "is it even loaded?" is answerable from chat instead of
    -- from guesswork: the addon version the client actually read, and whether the minimap
    -- button exists.
    DEFAULT_CHAT_FRAME:AddMessage(("|cffffc94dCompanion Sync|r |cff9d9d9dv%s|r loaded - "
        .. "minimap button %s"):format(G.version,
        launcher and "|cff20ff20ready|r" or "|cffff6a6aMISSING|r"))

    -- Record what was actually built. "Not on the map" has cost us hours of guessing; the
    -- numbers below say whether the button exists, where it sits and what it is drawn over.
    if CompanionSyncDB then
        local s = {
            version = G.version,
            launcher = launcher and "created" or "nil",
            minimap = Minimap and (Minimap:GetWidth() .. "x" .. Minimap:GetHeight()) or "nil",
        }
        if launcher then
            local point, _, relPoint, x, y = launcher:GetPoint(1)
            s.shown = launcher:IsShown() and 1 or 0
            s.width = launcher:GetWidth()
            s.height = launcher:GetHeight()
            s.strata = launcher:GetFrameStrata()
            s.scale = launcher:GetEffectiveScale()
            s.alpha = launcher:GetAlpha()
            s.point = tostring(point) .. "->" .. tostring(relPoint)
            s.x = math.floor((x or 0) * 10) / 10
            s.y = math.floor((y or 0) * 10) / 10
            s.parent = launcher:GetParent() and launcher:GetParent():GetName() or "?"
        end
        CompanionSyncDB.state = s
    end

    local pos = CompanionSyncDB.pos
    frame:ClearAllPoints()
    frame:SetPoint(pos.point or "CENTER", UIParent, pos.point or "CENTER", pos.x or 0, pos.y or 60)
end)


-- /csstate - print what the launcher is, where it is, and what it is anchored to
-- /csreset - restore the launcher's default ring position
SLASH_CSRESET1 = "/csreset"
SLASH_CSRESET2 = "/wlcgreset"
SlashCmdList["CSRESET"] = function()
    if not CompanionSyncDB then InitDB() end
    PlaceLauncher(nil)
    DEFAULT_CHAT_FRAME:AddMessage("|cffffc94dCompanion Sync|r minimap button reset")
end

SLASH_CSSTATE1 = "/csstate"
SLASH_CSSTATE2 = "/wlcgstate"
SlashCmdList["CSSTATE"] = function()
    local s = CompanionSyncDB and CompanionSyncDB.state
    if not s then
        DEFAULT_CHAT_FRAME:AddMessage("|cffffc94dCompanion Sync|r no recorded state - try /reload")
        return
    end
    DEFAULT_CHAT_FRAME:AddMessage(("|cffffc94dCompanion Sync|r launcher=%s minimap=%s parent=%s point=%s %.0f,%.0f shown=%s w=%s strata=%s alpha=%s")
        :format(s.launcher or "?", s.minimap or "?", s.parent or "?", s.point or "?",
                s.x or 0, s.y or 0, tostring(s.shown), tostring(s.width), s.strata or "?", tostring(s.alpha)))
end

-- If the server never says anything, say so once. A window whose buttons all do nothing is
-- worse than one that admits the module is not running - and "no companions yet" and "no
-- module" look identical otherwise.
local probe = CreateFrame("Frame")
probe:RegisterEvent("PLAYER_ENTERING_WORLD")
probe:SetScript("OnEvent", function(self)
    self:UnregisterEvent("PLAYER_ENTERING_WORLD")

    local waited = 0
    self:SetScript("OnUpdate", function(frame, delta)
        if G.ServerAlive() then
            frame:SetScript("OnUpdate", nil)
            return
        end

        waited = waited + delta
        if waited < G.ServerPatience() then return end

        frame:SetScript("OnUpdate", nil)

        G.presenceProbeExpired = true
        G.OnReadinessChanged()

        DEFAULT_CHAT_FRAME:AddMessage("|cffffc94dCompanion Sync|r: the server is not answering - "
            .. "is mod-companion-questsync enabled?")
    end)
end)
