-- Companion Pins - WoW 3.3.5a (Interface 30300).
-- The server publishes authoritative map snapshots; the client only renders them.
local ADDON = "CompanionPins"
local FLIP_Y = false
local PIN_HALF = 13

local CompanionPins = CreateFrame("Frame")
CompanionPins.debug = false

-- read the version from our own TOC, because hand-written version comments drift
do
    local toc = C_AddOns and C_AddOns.GetAddOnMetadata or GetAddOnMetadata
    CompanionPins.version = (toc and toc("CompanionPins", "Version")) or "?"
end
CompanionPins.offsetX = 0
CompanionPins.offsetY = 0
CompanionPins.pins = {}
CompanionPins.frames = {}
CompanionPins.only = false   -- show one companion's pins, or all
CompanionPins.hidden = false

-- shape: "round" (hand-in) or "square" (work to do)
local STYLE = {
    handin = { key = "handin", label = "ready to hand in", glyph = "?", priority = 3,
               ring = { 1.00, 0.80, 0.15 } },
    pending = { key = "pending", label = "work left", glyph = "?", priority = 2,
                ring = { 0.72, 0.74, 0.77 } },
    unavailable = { key = "unavailable", label = "hand-in not available yet", glyph = "?", priority = 2,
                    ring = { 0.72, 0.74, 0.77 } },
    gather = { key = "gather", label = "to gather", glyph = "!", priority = 1,
               ring = { 0.35, 0.95, 0.35 } },
    finish = { key = "finish", label = "to finish", glyph = "!", shape = "square",
               ring = { 0.35, 0.95, 0.35 } },
    other  = { key = "other",  label = "quest",     glyph = "?", shape = "square",
               ring = { 0.40, 0.70, 1.00 } },
}

local WIRE_STYLE = { h = STYLE.handin, p = STYLE.pending, u = STYLE.unavailable, g = STYLE.gather }

-- Canonical character names are UTF-8. Match the final @Name, not a locale's %w.
local function SplitCompanion(name)
    if not name then return nil, nil end
    local quest, companion = name:match("^(.+)@([A-Za-z\128-\255]+)$")
    if quest then return quest, companion end
    return name, nil
end

local function DisplayedMapNames()
    local zone = GetCurrentMapZone and GetCurrentMapZone()
    if zone == 0 then return nil end  -- World/continent maps are not zone coordinates.
    local zoneName
    if zone and zone > 0 and GetCurrentMapContinent and GetMapZones then
        local continent = GetCurrentMapContinent()
        if continent and continent > 0 then
            zoneName = ({ GetMapZones(continent) })[zone]
        end
    end
    local fileName = GetMapInfo and GetMapInfo()
    return zoneName or fileName, fileName
end

local function FoldASCII(text)
    return (text:gsub("[A-Z]", string.lower))
end

local function MapKey(text)
    if not text then return end
    -- DBC display names and map texture keys differ mainly in punctuation/spacing.
    -- Never drop the bytes of a localized name or perform a substring match.
    local key = FoldASCII(text):gsub("[ \t\r\n'%-_%.]", "")
    if key ~= "" then return key end
end

--------------------------------------------------------------------------
-- storage
--------------------------------------------------------------------------

local function SaveDB()
    CompanionPinsDB = CompanionPinsDB or {}
    CompanionPinsDB.offsetX, CompanionPinsDB.offsetY = CompanionPins.offsetX, CompanionPins.offsetY
    CompanionPinsDB.showLegend = CompanionPins.showLegend
    CompanionPinsDB.showLabels = CompanionPins.showLabels
    CompanionPinsDB.hidden = CompanionPins.hidden
end

local function LoadDB()
    CompanionPinsDB = CompanionPinsDB or { pins = {} }
    CompanionPins.showLegend = CompanionPinsDB.showLegend
    if CompanionPins.showLegend == nil then CompanionPins.showLegend = true end
    CompanionPins.showLabels = CompanionPinsDB.showLabels
    if CompanionPins.showLabels == nil then CompanionPins.showLabels = false end
    CompanionPins.hidden = CompanionPinsDB.hidden == true

    CompanionPins.offsetX = CompanionPinsDB.offsetX or 0
    CompanionPins.offsetY = CompanionPinsDB.offsetY or 0
end

-- "Pin sync 3: 43,65,h=Kobold Candles|64,59,g=Gold Dust Exchange" is the server's own
-- answer to "which pins are real". Rebuilding from it is what cleans the map after a
-- desync: anything it does not mention is removed, anything missing is added back.
local lastPayload, lastSnapshotAt, samplePin
local pending, appliedRevision, legacy
local supported, lastRequestAt, manualAt, unavailable
local pinEpoch, tagged, handInAdvertised, handInRequested
local itemsAdvertised, itemsRequested
local attempts = 0
local STALE_AFTER, RETRY_AFTER, ASSEMBLY_TIMEOUT, MAX_ATTEMPTS = 120, 15, 10, 3

local function Notice(text)
    DEFAULT_CHAT_FRAME:AddMessage("|cff00ccffCompanion Pins|r: " .. text)
end

local function MapOpen()
    return WorldMapFrame and WorldMapFrame:IsShown()
end

local function Fresh(now)
    return lastSnapshotAt and now - lastSnapshotAt < STALE_AFTER
end

-- Keep the server's uint64 epoch as a decimal string: Lua 5.1 numbers can round it.
local function AcceptEpoch(epoch)
    if #epoch > 20 or epoch:sub(1, 1) == "0" then return false end
    if pinEpoch == epoch then return true end
    if pinEpoch and (#epoch < #pinEpoch or (#epoch == #pinEpoch and epoch < pinEpoch)) then return false end
    pinEpoch = epoch
    pending, appliedRevision, legacy = nil, nil, nil
    lastSnapshotAt = nil
    attempts, supported, unavailable = 0, true, nil
    return true
end

local function DecodeMissingItems(wire)
    if not wire or #wire > 6 * 119 then return nil end
    local items = {}
    for field in wire:gmatch("[^;]+") do
        if #items == 6 then break end
        local have, required, hex = field:match("^(%d+)/(%d+)/(%x+)$")
        have, required = tonumber(have), tonumber(required)
        if have and required and have < required and required <= 4294967295 and #hex <= 96 and #hex % 2 == 0 then
            local name = hex:gsub("%x%x", function(byte) return string.char(tonumber(byte, 16)) end)
            if not name:find("[%z\1-\31\127]") then
                items[#items + 1] = "Needs " .. (required - have) .. " × " .. name:gsub("|", "||")
                    .. " (" .. have .. "/" .. required .. ")"
            end
        end
    end
    return items
end

local function SyncPins(payload)
    lastSnapshotAt = time()
    samplePin = nil
    attempts, supported, unavailable = 0, true, nil
    if manualAt then
        manualAt = nil
        Notice("Map pins refreshed.")
    end
    if lastPayload == payload then
        CompanionPins:Redraw()
        return
    end
    lastPayload = payload
    local fresh, companions = {}, {}
    for entry in string.gmatch(payload, "[^|]+") do
        local x, y, style, name, zone = entry:match("^(%d+%.?%d*),(%d+%.?%d*),([hpug])=(.+),z=(.-)%s*$")
        if x and tonumber(x) <= 100 and tonumber(y) <= 100 and zone ~= "" then
            local pin = { x = tonumber(x), y = tonumber(y), zone = zone,
                style = WIRE_STYLE[style], details = {} }
            -- Server joins titles with the literal separator ' + '. Keep the quest,
            -- companion and progress together instead of flattening independent lists.
            for raw in (name .. " + "):gmatch("(.-) %+ ") do
                local segment = raw:gsub("^%s+", ""):gsub("%s+$", "")
                local items
                local suffix = segment:find("~i=", 1, true)
                if suffix then
                    if style == "p" then items = DecodeMissingItems(segment:sub(suffix + 3)) end
                    segment = segment:sub(1, suffix - 1)
                end
                local owed = segment:match(":([%d/;]+)$")
                local quest, companion = SplitCompanion(owed and segment:gsub(":[%d/;]+$", "") or segment)
                if quest and quest ~= "" then
                    pin.details[#pin.details + 1] = { quest = quest, companion = companion, owed = owed,
                        style = pin.style, items = items }
                    if companion then companions[companion] = true end
                end
            end
            pin.quest = pin.details[1] and pin.details[1].quest or "Companion quest"
            pin.companion = pin.details[1] and pin.details[1].companion
            fresh[#fresh + 1] = pin
        end
    end
    CompanionPins.pins, CompanionPins.companions = fresh, companions
    CompanionPins:Redraw()
end

local function CompleteParts(buffer)
    for i = 1, buffer.total do
        if buffer.parts[i] == nil then return false end
    end
    return true
end

local function RequestPins(force, opening)
    if not MapOpen() then
        if force then Notice("Open the world map before refreshing.") end
        return
    end
    local now = time()
    if opening then attempts = 0 end
    if lastRequestAt and now - lastRequestAt < RETRY_AFTER then
        if force then Notice("Please wait a few seconds before refreshing again.") end
        return
    end
    local upgrade = pinEpoch and handInAdvertised and (not handInRequested or (itemsAdvertised and not itemsRequested))
    if not force then
        if not supported or unavailable or CompanionPins.hidden or attempts >= MAX_ATTEMPTS then return end
        if pending and now - pending.at < ASSEMBLY_TIMEOUT then return end
        if not upgrade and not pending and (not pinEpoch or tagged) and Fresh(now)
            and (appliedRevision or lastRequestAt) then return end
    else
        attempts, manualAt = 0, now
        Notice(supported and "Requesting map pins…" or "Checking whether this server supports map pins…")
    end
    attempts, lastRequestAt = attempts + 1, now
    local command = ".companionsync pins v2"
    if pinEpoch then
        command = command .. " epoch"
        if handInAdvertised then
            handInRequested = true
            command = command .. " handin"
            if itemsAdvertised then
                itemsRequested = true
                command = command .. " items"
            end
        end
    end
    SendChatMessage(command, "SAY")
end

CompanionPins_Request = RequestPins

--------------------------------------------------------------------------
-- drawing
--------------------------------------------------------------------------

-- No textures at all: the marker is a glyph.
--
-- A coloured "?" or "!" with an outline is the client's own language for quests, it
-- needs no art assets, and it cannot degenerate into a black square the way a
-- plate-and-ring badge does at map scale. Colour says whose business it is, the glyph
-- says what to do, the label says which quest - and nothing is boxed.
local function HidePinTooltip(self)
    if GameTooltip:IsOwned(self) then GameTooltip:Hide() end
end

local function NewPin(index)
    local f = CreateFrame("Button", ADDON .. "Pin" .. index, WorldMapDetailFrame)
    f:SetSize(2 * PIN_HALF, 2 * PIN_HALF)
    f:EnableMouse(false)

    -- A UI suite skinning the map can hand out frame levels above the pin's own: sit
    -- clearly on top of the map's scroll level, or the cursor never reaches the frame.
    if WorldMapDetailFrame and WorldMapDetailFrame.GetFrameLevel then
        f:SetFrameLevel((WorldMapDetailFrame:GetFrameLevel() or 0) + 10)
    end

    -- a soft offset copy of the glyph, so it holds up over bright parchment without a
    -- plate behind it (the OUTLINE font flag alone thins out at small sizes)
    f.shadow = f:CreateFontString(nil, "BACKGROUND")
    f.shadow:SetFont(STANDARD_TEXT_FONT, 19, "OUTLINE")
    f.shadow:SetPoint("CENTER", f, "CENTER", 1, -1)
    f.shadow:SetTextColor(0, 0, 0, 0.9)

    f.glyph = f:CreateFontString(nil, "OVERLAY")
    f.glyph:SetFont(STANDARD_TEXT_FONT, 19, "OUTLINE")
    f.glyph:SetPoint("CENTER", f, "CENTER", 0, 0)

    -- label to the right of the marker: the terrain under it stays visible, unlike a
    -- black bar sitting on the map
    f.label = f:CreateFontString(nil, "OVERLAY")
    f.label:SetFont(STANDARD_TEXT_FONT, 11, "OUTLINE")
    f.label:SetPoint("LEFT", f, "RIGHT", 4, 0)
    f.label:SetJustifyH("LEFT")

    f.count = f:CreateFontString(nil, "OVERLAY")
    f.count:SetFont(STANDARD_TEXT_FONT, 10, "OUTLINE")
    f.count:SetPoint("BOTTOMLEFT", f.glyph, "BOTTOMRIGHT", 0, 2)
    f.count:SetTextColor(0.97, 0.92, 0.79)

    f:SetScript("OnEnter", function(self)
        GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
        local pin = self.pin
        if pin then
            GameTooltip:AddLine(pin.zone or "Companion quests", 1, 0.92, 0.6)
            local titles, order = {}, {}
            for _, row in ipairs(pin.details) do
                local title = row.style.glyph .. " " .. row.quest
                if not titles[title] then
                    titles[title] = {}
                    order[#order + 1] = title
                end
                local detail = row.companion or "Companion"
                if row.style.glyph == "?" then detail = detail .. " — " .. row.style.label end
                if row.owed then detail = detail .. "  " .. row.owed:gsub(";", "  ·  ") end
                titles[title][#titles[title] + 1] = { text = detail, items = row.items }
            end
            for _, title in ipairs(order) do
                GameTooltip:AddLine(title, 1, 0.92, 0.6, true)
                for _, detail in ipairs(titles[title]) do
                    GameTooltip:AddLine("  " .. detail.text, 0.85, 0.85, 0.85, true)
                    for _, item in ipairs(detail.items or {}) do
                        GameTooltip:AddLine("    " .. item, 0.85, 0.85, 0.85, true)
                    end
                end
            end
        end
        GameTooltip:Show()
    end)
    f:SetScript("OnLeave", HidePinTooltip)
    f:SetScript("OnHide", HidePinTooltip)
    CompanionPins.frames[index] = f
    return f
end

-- legend: what a marker means, bottom right of the map
--------------------------------------------------------------------------

-- A small outlined-text legend, without a plate or border.
local LEGEND = {
    { style = STYLE.gather, text = "work left" },
    { style = STYLE.handin, text = "ready to hand in" },
    { style = STYLE.pending, text = "not ready to hand in" },
}

local legend, status, overlayAnchor

local function BuildLegend()
    if legend then return legend end

    local f = CreateFrame("Frame", ADDON .. "Legend", WorldMapFrame)
    f:SetSize(132, 16 + #LEGEND * 15)
    f:SetFrameLevel(WorldMapFrame:GetFrameLevel() + 12)
    f:EnableMouse(false)

    for index, row in ipairs(LEGEND) do
        local glyph = f:CreateFontString(nil, "OVERLAY")
        glyph:SetFont(STANDARD_TEXT_FONT, 14, "OUTLINE")
        glyph:SetPoint("TOPLEFT", f, "TOPLEFT", 8, -5 - (index - 1) * 15)
        glyph:SetText(row.style.glyph)
        glyph:SetTextColor(row.style.ring[1], row.style.ring[2], row.style.ring[3])

        local label = f:CreateFontString(nil, "OVERLAY")
        label:SetFont(STANDARD_TEXT_FONT, 11, "OUTLINE")
        label:SetPoint("LEFT", glyph, "RIGHT", 7, 0)
        label:SetText(row.text)
        label:SetTextColor(0.85, 0.85, 0.85)
    end

    f:Hide()
    legend = f
    return f
end

local function BuildStatus()
    if status then return end
    status = CreateFrame("Frame", ADDON .. "Status", WorldMapFrame)
    status:SetSize(350, 20)
    status:SetFrameLevel(WorldMapFrame:GetFrameLevel() + 12)
    status:EnableMouse(false)
    status.text = status:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    status.text:SetFont(STANDARD_TEXT_FONT, 11, "OUTLINE")
    status.text:SetPoint("CENTER")
    status.text:SetTextColor(1, 0.88, 0.62)
end

-- DragonUI only reparents the canvas while zoomed. Detect the real clip from its
-- scroll-child ancestry, without calling DragonUI or changing any Blizzard frames.
local function MapViewport(detail)
    local child = detail
    while child and child ~= WorldMapFrame and child.GetParent do
        local parent = child:GetParent()
        if parent and parent.GetObjectType and parent:GetObjectType() == "ScrollFrame"
            and parent:GetScrollChild() == child then return parent end
        child = parent
    end
    return detail
end

function CompanionPins:UpdateViewport(force)
    local detail = WorldMapDetailFrame
    if not detail or not detail:IsVisible() or not status then return end
    local viewport = MapViewport(detail)
    if viewport ~= overlayAnchor then
        overlayAnchor = viewport
        legend:ClearAllPoints()
        legend:SetPoint("BOTTOMRIGHT", viewport, "BOTTOMRIGHT", -12, 36)
        status:ClearAllPoints()
        status:SetPoint("BOTTOM", viewport, "BOTTOM", 0, 24)
    end
    local scale, viewScale = detail:GetEffectiveScale(), viewport:GetEffectiveScale()
    local dl, dt = detail:GetLeft(), detail:GetTop()
    local vl, vr, vb, vt = viewport:GetLeft(), viewport:GetRight(), viewport:GetBottom(), viewport:GetTop()
    local left, right, top, bottom
    if dl and dt and vl and vr and vb and vt and scale > 0 then
        local ratio = viewScale / scale
        left, right = vl * ratio - dl, vr * ratio - dl
        top, bottom = dt - vt * ratio, dt - vb * ratio
    end
    if not force and self.viewLeft == left and self.viewRight == right
        and self.viewTop == top and self.viewBottom == bottom then return end
    self.viewLeft, self.viewRight, self.viewTop, self.viewBottom = left, right, top, bottom
    for i = 1, self.drawnPins do
        local f = self.frames[i]
        local x, y = f.mapX, f.mapY
        local inside = left and x + PIN_HALF > left and x - PIN_HALF < right
            and y + PIN_HALF > top and y - PIN_HALF < bottom
        if inside then
            f:SetHitRectInsets(math.max(0, left - x + PIN_HALF), math.max(0, x + PIN_HALF - right),
                math.max(0, top - y + PIN_HALF), math.max(0, y + PIN_HALF - bottom))
            f:Show()
        else
            f:Hide()
        end
        f:EnableMouse(not not inside)
    end
end

function CompanionPins:UpdateStatus()
    if not status or not self.drawWidth then return end
    local now = time()
    local fresh = not not (self.drawingSample or (Fresh(now) and not unavailable))
    local message
    if self.drawingSample then
        message = "Sample pin — /cpins clear"
    elseif unavailable then
        message = "Companion pins are disabled on this server."
    elseif not fresh and lastPayload ~= nil then
        message = "Stale companion pins — /cpins refresh"
    elseif not fresh then
        local noReply = (not supported or attempts >= MAX_ATTEMPTS) and lastRequestAt and now - lastRequestAt >= RETRY_AFTER
        message = noReply and "No pin snapshot received — /cpins refresh" or "Waiting for companion pins…"
    elseif #self.pins == 0 then
        message = "No companion pins in this snapshot."
    elseif self.drawnPins == 0 then
        message = self.only and "No pins for " .. self.only .. " on this map." or "No companion pins on this map."
    end
    if fresh ~= self.lastFresh or self.drawnPins ~= self.lastDrawn then
        self.lastFresh, self.lastDrawn = fresh, self.drawnPins
        for i = 1, self.drawnPins do self.frames[i]:SetAlpha(fresh and 1 or 0.55) end
    end
    if self.hidden or not message then
        status:Hide()
    else
        status.text:SetText(message)
        status:Show()
    end
end

--------------------------------------------------------------------------
-- events
--------------------------------------------------------------------------

CompanionPins:RegisterEvent("PLAYER_LOGIN")
CompanionPins:RegisterEvent("CHAT_MSG_SYSTEM")
CompanionPins:RegisterEvent("WORLD_MAP_UPDATE")

if ChatFrame_AddMessageEventFilter then
    ChatFrame_AddMessageEventFilter("CHAT_MSG_SYSTEM", function(_, _, message)
        return type(message) == "string" and (message:sub(1, 14) == "CSYNC|Pin set "
            or message:sub(1, 15) == "CSYNC|Pin sync ")
    end)
end

CompanionPins:SetScript("OnEvent", function(self, event, message)
    if event == "PLAYER_LOGIN" then
        LoadDB()
        if WorldMapFrame then
            WorldMapFrame:HookScript("OnShow", function()
                self:Redraw()
                RequestPins(false, true)
            end)
        end
        return
    end
    if event == "WORLD_MAP_UPDATE" then
        self:Redraw()
        RequestPins()
        return
    end
    if event ~= "CHAT_MSG_SYSTEM" or type(message) ~= "string" or message:sub(1, 6) ~= "CSYNC|" then return end

    local text = message:sub(7)
    if self.debug and (text:find("^Pin set ") or text:find("^Pin sync ")) then
        DEFAULT_CHAT_FRAME:AddMessage("Companion Pins: " .. text)
    end
    if text:match("^Hello%s+%d+$") then
        supported = true
        if not lastRequestAt then RequestPins() end
        return
    end
    if text == "Pin set unavailable" then
        unavailable = true
        pending, legacy = nil, nil
        if manualAt then
            manualAt = nil
            Notice("Map pins are disabled on this server.")
        end
        self:Redraw()
        return
    end

    if text == "Pin set handin 1" then
        handInAdvertised = true
        return
    end
    if text == "Pin set items 1" then
        itemsAdvertised = true
        return
    end
    local session = text:match("^Pin set session (%d+)$")
    if session then
        if AcceptEpoch(session) then self:Redraw() end
        return
    end
    local epoch, revision, part, total, piece = text:match("^Pin set (%d+):(%d+) (%d+)/(%d+):(.*)$")
    if not epoch and not tagged then
        revision, part, total, piece = text:match("^Pin set (%d+) (%d+)/(%d+):(.*)$")
    end
    if revision then
        revision, part, total = tonumber(revision), tonumber(part), tonumber(total)
        if revision < 1 or revision > 4294967295 or total < 1 or total > 512 or part < 1 or part > total or #piece > 150 then return end
        if epoch then
            if not AcceptEpoch(epoch) then return end
            if not tagged then pending, appliedRevision, legacy = nil, nil, nil end
            tagged = true
        end
        if appliedRevision and revision <= appliedRevision then return end
        local now = time()
        if pending and revision < pending.revision then return end
        if pending and now - pending.at >= ASSEMBLY_TIMEOUT then
            if revision == pending.revision then return end
            pending = nil
        end
        if not pending or pending.revision ~= revision then
            pending = { revision = revision, total = total, at = now, parts = {} }
        elseif pending.total ~= total then
            return
        end
        pending.parts[part] = piece
        if CompleteParts(pending) then
            appliedRevision = revision
            local payload = table.concat(pending.parts, "")
            pending, legacy = nil, nil
            SyncPins(payload)
        end
        return
    end
    if pinEpoch or appliedRevision then return end  -- never roll a v2 map back to an unversioned set

    part, total, piece = text:match("^Pin sync (%d+)/(%d+):%s*(.*)$")
    if part then
        part, total = tonumber(part), tonumber(total)
        if total < 1 or total > 512 or part < 1 or part > total then return end
        local now = time()
        if part == 1 then legacy = { total = total, at = now, parts = {} } end
        if not legacy or legacy.total ~= total or now - legacy.at >= ASSEMBLY_TIMEOUT then return end
        legacy.parts[part] = piece
        if CompleteParts(legacy) then
            SyncPins(table.concat(legacy.parts, "|"))
            legacy = nil
        end
        return
    end
    local single = text:match("^Pin sync %d+:%s*(.*)$")
    if single then
        legacy = nil
        SyncPins(single)
    end
end)

local watchElapsed, viewElapsed = 0, 0
CompanionPins:SetScript("OnUpdate", function(self, elapsed)
    local open = MapOpen()
    if open and WorldMapDetailFrame and WorldMapDetailFrame:IsVisible() then
        viewElapsed = viewElapsed + elapsed
        if viewElapsed >= 0.05 then
            viewElapsed = 0
            local w, h = WorldMapDetailFrame:GetWidth(), WorldMapDetailFrame:GetHeight()
            if w ~= self.drawWidth or h ~= self.drawHeight then self:Redraw() else self:UpdateViewport() end
        end
    elseif status and self.drawWidth then
        self.drawWidth, overlayAnchor = nil, nil
        status:Hide()
        legend:Hide()
    end
    watchElapsed = watchElapsed + elapsed
    if watchElapsed < 1 then return end
    watchElapsed = 0
    if manualAt and time() - manualAt >= RETRY_AFTER then
        manualAt = nil
        Notice("No pin snapshot received. This server may not support map pins or may have them disabled.")
    end
    if open then
        RequestPins()
        self:UpdateStatus()
    end
end)

local zoneWatch = CreateFrame("Frame")
zoneWatch:RegisterEvent("ZONE_CHANGED")
zoneWatch:RegisterEvent("ZONE_CHANGED_NEW_AREA")
zoneWatch:SetScript("OnEvent", function()
    if WorldMapFrame and WorldMapFrame:IsShown() then CompanionPins:Redraw() end
end)



-- Keep a hand-in at its NPC and group nearby work spots around a single glyph.
-- A stable ordering keeps the same anchor as filters and progress change.
function CompanionPins:VisiblePins(mapName, width, height, fileName)
    local candidates = {}
    local map, alternate = MapKey(mapName), MapKey(fileName)
    for _, pin in ipairs(self.pins) do
        local zone = MapKey(pin.zone)
        if zone and (zone == map or zone == alternate) then
            local details = {}
            for _, row in ipairs(pin.details) do
                if not self.only or (row.companion and FoldASCII(row.companion) == FoldASCII(self.only)) then
                    details[#details + 1] = row
                end
            end
            if #details > 0 then
                candidates[#candidates + 1] = { x = pin.x, y = pin.y, zone = pin.zone, style = pin.style,
                    details = details, count = 1 }
            end
        end
    end
    table.sort(candidates, function(a, b)
        if a.style.priority ~= b.style.priority then return a.style.priority > b.style.priority end
        if a.zone ~= b.zone then return a.zone < b.zone end
        if a.x ~= b.x then return a.x < b.x end
        if a.y ~= b.y then return a.y < b.y end
        if a.style.key ~= b.style.key then return a.style.key < b.style.key end
        return a.details[1].quest < b.details[1].quest
    end)
    local clusters = {}
    for _, pin in ipairs(candidates) do
        local cluster, nearest = nil, 18 * 18
        for _, entry in ipairs(clusters) do
            if entry.zone == pin.zone then
                local dx, dy = (entry.x - pin.x) * width / 100, (entry.y - pin.y) * height / 100
                if dx == 0 and dy == 0 then
                    cluster = entry
                    break
                end
                local distance = dx * dx + dy * dy
                if pin.style == STYLE.gather and (entry.style == STYLE.gather or entry.style == STYLE.handin)
                    and (distance < nearest or (not cluster and distance == nearest)) then
                    cluster, nearest = entry, distance
                end
            end
        end
        if cluster then
            cluster.count = cluster.count + 1
            for _, row in ipairs(pin.details) do
                local duplicate = false
                for _, prior in ipairs(cluster.details) do
                    if prior.quest == row.quest and prior.companion == row.companion and prior.style == row.style then
                        duplicate = true
                        break
                    end
                end
                if not duplicate then cluster.details[#cluster.details + 1] = row end
            end
        else
            clusters[#clusters + 1] = pin
        end
    end
    return clusters
end

-- Keep map coordinates intact. Nudge only the rendered button when a hand-in
-- would share a hit box, preferring a vertical step so labels can stay readable.
local function PlacePin(pin, x, y, w, h, placed)
    local step = 2 * PIN_HALF
    local function fits(px, py)
        for _, prior in ipairs(placed) do
            if (pin.style.glyph == "?" or prior.pin.style.glyph == "?")
                and math.abs(prior.mapX - px) < step and math.abs(prior.mapY - py) < step then
                return false
            end
        end
        return true
    end
    local function bounded(px, py)
        return math.max(PIN_HALF, math.min(w - PIN_HALF, px)), math.max(PIN_HALF, math.min(h - PIN_HALF, py))
    end
    x, y = bounded(x, y)
    if fits(x, y) then return x, y end
    for ring = 1, math.ceil(math.max(w, h) / step) do
        local bestX, bestY, bestDistance
        local function try(dx, dy)
            local px, py = bounded(x + dx * step, y + dy * step)
            local distance = (px - x)^2 + (py - y)^2
            if (not bestDistance or distance < bestDistance) and fits(px, py) then
                bestX, bestY, bestDistance = px, py, distance
            end
        end
        try(0, ring)
        try(0, -ring)
        for offset = 1, ring do
            try(offset, ring)
            try(-offset, ring)
            try(offset, -ring)
            try(-offset, -ring)
        end
        for offset = 1 - ring, ring - 1 do
            try(ring, offset)
            try(-ring, offset)
        end
        if bestDistance then return bestX, bestY end
    end
    return x, y -- a canvas saturated by buttons has no free full-sized slot
end

function CompanionPins:Redraw()
    if not MapOpen() or not WorldMapDetailFrame or not WorldMapDetailFrame:IsVisible() then return end
    local w, h = WorldMapDetailFrame:GetWidth(), WorldMapDetailFrame:GetHeight()
    local mapName, fileName = DisplayedMapNames()
    local pins = {}
    self.drawingSample = false
    if not self.hidden and w >= 2 * PIN_HALF and h >= 2 * PIN_HALF then
        if samplePin and mapName and MapKey(samplePin.zone) == MapKey(mapName) then
            pins, self.drawingSample = {samplePin}, true
        else
            pins = self:VisiblePins(mapName, w, h, fileName)
        end
    end
    local placed = {}
    for i, pin in ipairs(pins) do
        local f = self.frames[i] or NewPin(i)
        -- This button can be reused for new state or a different location.
        HidePinTooltip(f)
        f.pin = pin
        local px = (pin.x + self.offsetX) / 100 * w
        local py = (pin.y + self.offsetY) / 100
        f.mapX, f.mapY = PlacePin(pin, px, (FLIP_Y and (1 - py) or py) * h, w, h, placed)
        placed[i] = f
        f:ClearAllPoints()
        f:SetPoint("CENTER", WorldMapDetailFrame, "TOPLEFT", f.mapX, -f.mapY)
        local style = pin.style
        f.glyph:SetText(style.glyph)
        f.glyph:SetTextColor(style.ring[1], style.ring[2], style.ring[3])
        f.shadow:SetText(style.glyph)
        local count = math.max(pin.count, #pin.details)
        f.count:SetText(count > 1 and tostring(count) or "")
        local label = pin.details[1].quest
        if #label > 22 then
            local cut = 21
            while cut > 1 and label:byte(cut) >= 128 and label:byte(cut) < 192 do cut = cut - 1 end
            label = label:sub(1, cut - 1) .. ".."
        end
        local flipLabel = f.mapX > w / 2
        f.label:ClearAllPoints()
        f.label:SetPoint(flipLabel and "RIGHT" or "LEFT", f, flipLabel and "LEFT" or "RIGHT", flipLabel and -4 or 4, 0)
        f.label:SetJustifyH(flipLabel and "RIGHT" or "LEFT")
        f.label:SetWidth(math.max(1, (flipLabel and f.mapX or w - f.mapX) - PIN_HALF - 4))
        f.label:SetText(self.showLabels and label or "")
        f.label:SetTextColor(style.ring[1], style.ring[2], style.ring[3])
    end
    for i = #pins + 1, #self.frames do
        self.frames[i]:Hide()
        self.frames[i]:EnableMouse(false)
        self.frames[i].pin = nil
    end
    self.drawnPins, self.drawWidth, self.drawHeight = #pins, w, h
    local lg = BuildLegend()
    if self.showLegend == false or #pins == 0 then lg:Hide() else lg:Show() end
    BuildStatus()
    self:UpdateViewport(true)
    self:UpdateStatus()
end

--------------------------------------------------------------------------
-- commands
--------------------------------------------------------------------------

-- /cpins is the name; /wlpins stays registered because hands remember it
SLASH_WLPINS1 = "/cpins"
SLASH_WLPINS2 = "/wlpins"
SlashCmdList["WLPINS"] = function(msg)
    local raw = msg or ""
    msg = FoldASCII(raw)

    if msg == "clear" then
        if pending then appliedRevision = pending.revision end
        CompanionPins.pins, CompanionPins.companions = {}, {}
        lastPayload, lastSnapshotAt, pending, legacy, attempts = nil, nil, nil, nil, 0
        samplePin = nil
        CompanionPins:Redraw()
        DEFAULT_CHAT_FRAME:AddMessage("|cff00ccffCompanion Pins|r cleared.")

    elseif msg == "refresh" then
        RequestPins(true)

    elseif msg == "show" or msg == "hide" then
        CompanionPins.hidden = msg == "hide"
        SaveDB()
        CompanionPins:Redraw()
        if not CompanionPins.hidden then RequestPins(false, true) end
        Notice(CompanionPins.hidden and "Map overlay hidden." or "Map overlay shown.")

    elseif msg == "labels" then
        CompanionPins.showLabels = not CompanionPins.showLabels
        SaveDB()
        CompanionPins:Redraw()
        DEFAULT_CHAT_FRAME:AddMessage("|cff00ccffCompanion Pins|r persistent labels " ..
            (CompanionPins.showLabels and "|cff20ff20on|r - hover a marker for detail" or
            "off|r - the tooltip on hover carries the detail") .. ".")

    elseif msg == "legend" then
        CompanionPins.showLegend = not CompanionPins.showLegend
        SaveDB()
        CompanionPins:Redraw()
        DEFAULT_CHAT_FRAME:AddMessage("|cff00ccffCompanion Pins|r legend " ..
            (CompanionPins.showLegend and "on" or "off") .. ".")

    elseif msg == "flip" then
        FLIP_Y = not FLIP_Y
        CompanionPins:Redraw()
        DEFAULT_CHAT_FRAME:AddMessage("|cff00ccffCompanion Pins|r vertical flip " ..
            (FLIP_Y and "on" or "off") .. ".")

    elseif string.find(msg, "^offset") then
        local ox, oy = string.match(msg, "^offset%s+(-?%d+)%s*(-?%d+)")
        if ox and oy then
            CompanionPins.offsetX, CompanionPins.offsetY = tonumber(ox), tonumber(oy)
            SaveDB(); CompanionPins:Redraw()
            DEFAULT_CHAT_FRAME:AddMessage(string.format(
                "|cff00ccffCompanionPins|r calibration offset %d, %d.", CompanionPins.offsetX, CompanionPins.offsetY))
        else
            DEFAULT_CHAT_FRAME:AddMessage(string.format(
                "|cff00ccffCompanionPins|r offset is %d, %d - change it with /wlpins offset -7 -3",
                CompanionPins.offsetX, CompanionPins.offsetY))
        end

    elseif string.find(msg, "^only") then
        local name = raw:match("^%S+%s+(%S+)%s*$")
        if name then
            CompanionPins.only = name
            CompanionPins:Redraw()
            DEFAULT_CHAT_FRAME:AddMessage("|cff00ccffCompanion Pins|r showing only " .. name ..
                " - pins from other zones are hidden until you open their map.")
        else
            DEFAULT_CHAT_FRAME:AddMessage("|cff00ccffCompanion Pins|r showing only " ..
                (CompanionPins.only or "everyone") .. " - /wlpins only <name>, /wlpins all")
        end

    elseif msg == "all" then
        CompanionPins.only = nil
        CompanionPins:Redraw()
        DEFAULT_CHAT_FRAME:AddMessage("|cff00ccffCompanion Pins|r showing every companion.")

    elseif msg == "test" then
        local mapName = MapOpen() and DisplayedMapNames()
        if mapName then
            samplePin = { x = 48, y = 52, zone = mapName, count = 1, style = STYLE.handin,
                details = { { quest = "TEST PIN", style = STYLE.handin } } }
            CompanionPins:Redraw()
            Notice("Sample pin at 48, 52 on this zone map.")
        else
            Notice("Open a zone map before drawing a sample pin.")
        end

    elseif msg == "debug" then
        CompanionPins.debug = not CompanionPins.debug
        DEFAULT_CHAT_FRAME:AddMessage("|cff00ccffCompanionPins|r whisper logging " ..
            (CompanionPins.debug and "on" or "off") .. ".")

    elseif msg == "list" then
        if #CompanionPins.pins == 0 then
            DEFAULT_CHAT_FRAME:AddMessage("|cff00ccffCompanion Pins|r: no pins.")
            return
        end
        for _, pin in ipairs(CompanionPins.pins) do
            DEFAULT_CHAT_FRAME:AddMessage(string.format("|cff00ccffCompanionPins|r %d, %d - %s (%s)",
                pin.x, pin.y, pin.quest or "?", pin.style.label))
        end

    else
        DEFAULT_CHAT_FRAME:AddMessage("|cff00ccffCompanion Pins|r - a renderer for what the server says")
        DEFAULT_CHAT_FRAME:AddMessage("  |cffffcc33round gold ?|r  ready to hand in, at the giver")
        DEFAULT_CHAT_FRAME:AddMessage("  |cff59f259square green !|r  area where it still has work")
        DEFAULT_CHAT_FRAME:AddMessage("  |cff9a9a9a/cpins legend|r  show or hide the key on the map")
        DEFAULT_CHAT_FRAME:AddMessage("  /cpins show | hide | refresh | list | clear | test | debug | flip")
        DEFAULT_CHAT_FRAME:AddMessage("  /wlpins offset X Y - nudge the glyph if it sits off the road")
        DEFAULT_CHAT_FRAME:AddMessage("  /wlpins only <name> | all - one companion's glyphs or everyone's")
    end
end
