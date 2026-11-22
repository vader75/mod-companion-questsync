-- Companion Pins - WoW 3.3.5a (Interface 30300).
-- The server publishes authoritative map snapshots; the client only renders them.
local ADDON = "CompanionPins"
local FLIP_Y = false

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

-- shape: "round" (hand-in) or "square" (work to do)
local STYLE = {
    handin = { key = "handin", label = "hand in",  glyph = "?", shape = "round",
               ring = { 1.00, 0.80, 0.15 } },
    gather = { key = "gather", label = "to gather", glyph = "!", shape = "square",
               ring = { 0.35, 0.95, 0.35 } },
    finish = { key = "finish", label = "to finish", glyph = "!", shape = "square",
               ring = { 0.35, 0.95, 0.35 } },
    other  = { key = "other",  label = "quest",     glyph = "?", shape = "square",
               ring = { 0.40, 0.70, 1.00 } },
}

-- A pin may name the companion it belongs to ("Kobold Candles@Beta") once there is
-- more than one. Splitting on '@' now means the wire format can grow without the
-- client misreading a pinned quest literally called "Kobold Candles@Beta".
local function SplitCompanion(name)
    if not name then return nil, nil end
    local quest, companion = string.match(name, "^(.-)@([%w]+)$")
    if quest and quest ~= "" then return quest, companion end
    return name, nil
end

local function CurrentZoneName()
    if GetZoneText then return GetZoneText() end
    return ""
end

--------------------------------------------------------------------------
-- storage
--------------------------------------------------------------------------

local function SaveDB()
    CompanionPinsDB = CompanionPinsDB or {}
    CompanionPinsDB.offsetX, CompanionPinsDB.offsetY = CompanionPins.offsetX, CompanionPins.offsetY
    CompanionPinsDB.showLegend = CompanionPins.showLegend
    CompanionPinsDB.showLabels = CompanionPins.showLabels
end

local function LoadDB()
    CompanionPinsDB = CompanionPinsDB or { pins = {} }
    CompanionPins.showLegend = CompanionPinsDB.showLegend
    if CompanionPins.showLegend == nil then CompanionPins.showLegend = true end
    CompanionPins.showLabels = CompanionPinsDB.showLabels
    if CompanionPins.showLabels == nil then CompanionPins.showLabels = false end

    CompanionPins.offsetX = CompanionPinsDB.offsetX or 0
    CompanionPins.offsetY = CompanionPinsDB.offsetY or 0
end

-- "Pin sync 3: 43,65,h=Kobold Candles|64,59,g=Gold Dust Exchange" is the server's own
-- answer to "which pins are real". Rebuilding from it is what cleans the map after a
-- desync: anything it does not mention is removed, anything missing is added back.
local lastPayload, lastSnapshotAt
local pending, appliedRevision, legacy
local lastHelloAt, lastRequestAt

local function SyncPins(payload)
    lastSnapshotAt = time()
    if lastPayload == payload then return end
    lastPayload = payload
    local fresh, companions = {}, {}
    for entry in string.gmatch(payload, "[^|]+") do
        local x, y, style, name, zone = entry:match("^(%d+),(%d+),([hg])=(.+),z=(.-)%s*$")
        if x and tonumber(x) <= 100 and tonumber(y) <= 100 and zone ~= "" then
            local pin = { x = tonumber(x), y = tonumber(y), zone = zone,
                style = style == "h" and STYLE.handin or STYLE.gather, details = {} }
            -- Server joins titles with the literal separator ' + '. Keep the quest,
            -- companion and progress together instead of flattening independent lists.
            for raw in (name .. " + "):gmatch("(.-) %+ ") do
                local segment = raw:gsub("^%s+", ""):gsub("%s+$", "")
                local owed = segment:match(":([%d/;]+)$")
                local quest, companion = SplitCompanion(owed and segment:gsub(":[%d/;]+$", "") or segment)
                if quest and quest ~= "" then
                    pin.details[#pin.details + 1] = { quest = quest, companion = companion, owed = owed }
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
    if not WorldMapFrame or not WorldMapFrame:IsShown() then return end
    local now = time()
    if not force then
        if not lastHelloAt or now - lastHelloAt > 120 then return end
        local fresh = lastSnapshotAt and now - lastSnapshotAt < 120
        if appliedRevision and lastSnapshotAt and not pending and (not opening or fresh) then return end
        if not pending and lastRequestAt and fresh then return end
    end
    if lastRequestAt and now - lastRequestAt < 15 then return end
    lastRequestAt = now
    SendChatMessage(".companionsync pins v2", "SAY")
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
local function NewPin(index)
    local f = CreateFrame("Button", ADDON .. "Pin" .. index, WorldMapDetailFrame)
    f:SetSize(26, 26)

    -- Without this the OnEnter/OnLeave/OnClick scripts never fire: hover was silently
    -- dead since the pin frames existed, and it only became visible when the labels
    -- went and hover was the sole detail surface.
    f:EnableMouse(true)

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
                local title = (row.style == STYLE.handin and "? " or "! ") .. row.quest
                if not titles[title] then
                    titles[title] = {}
                    order[#order + 1] = title
                end
                local detail = row.companion or "Companion"
                if row.owed then detail = detail .. "  " .. row.owed:gsub(";", "  ·  ") end
                titles[title][#titles[title] + 1] = detail
            end
            for _, title in ipairs(order) do
                GameTooltip:AddLine(title, 1, 0.92, 0.6, true)
                for _, detail in ipairs(titles[title]) do
                    GameTooltip:AddLine("  " .. detail, 0.85, 0.85, 0.85, true)
                end
            end
        end
        GameTooltip:Show()
    end)
    f:SetScript("OnLeave", function() GameTooltip:Hide() end)
    CompanionPins.frames[index] = f
    return f
end

-- legend: what a marker means, bottom right of the map
--------------------------------------------------------------------------

-- A small outlined-text legend, without a plate or border.
local LEGEND = {
    { style = STYLE.gather, text = "work left" },
    { style = STYLE.handin, text = "ready to hand in" },
}

local legend

local function BuildLegend()
    if legend then return legend end

    local f = CreateFrame("Frame", ADDON .. "Legend", WorldMapDetailFrame)
    f:SetSize(118, 16 + #LEGEND * 15)
    f:SetPoint("BOTTOMRIGHT", WorldMapDetailFrame, "BOTTOMRIGHT", -12, 46)
    f:SetFrameStrata("MEDIUM")

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
        lastHelloAt = time()
        RequestPins()
        return
    end

    local revision, part, total, piece = text:match("^Pin set (%d+) (%d+)/(%d+):(.*)$")
    if revision then
        revision, part, total = tonumber(revision), tonumber(part), tonumber(total)
        if total < 1 or total > 512 or part < 1 or part > total or #piece > 150 then return end
        if appliedRevision and revision <= appliedRevision then return end
        local now = time()
        if pending and revision < pending.revision then return end
        if pending and now - pending.at > 10 then
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
            SyncPins(table.concat(pending.parts, ""))
            pending, legacy = nil, nil
        end
        return
    end
    if appliedRevision then return end  -- never roll a v2 map back to an unversioned set

    part, total, piece = text:match("^Pin sync (%d+)/(%d+):%s*(.*)$")
    if part then
        part, total = tonumber(part), tonumber(total)
        if total < 1 or total > 512 or part < 1 or part > total then return end
        local now = time()
        if part == 1 then legacy = { total = total, at = now, parts = {} } end
        if not legacy or legacy.total ~= total or now - legacy.at > 10 then return end
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

local zoneWatch = CreateFrame("Frame")
zoneWatch:RegisterEvent("ZONE_CHANGED")
zoneWatch:RegisterEvent("ZONE_CHANGED_NEW_AREA")
zoneWatch:SetScript("OnEvent", function()
    if WorldMapFrame and WorldMapFrame:IsShown() then CompanionPins:Redraw() end
end)



-- Keep a hand-in at its NPC and group nearby work spots around a single glyph.
-- A stable ordering keeps the same anchor as filters and progress change.
function CompanionPins:VisiblePins(mapName, width, height)
    local candidates = {}
    local map = mapName and mapName:lower():gsub("[^%w]", "")
    for _, pin in ipairs(self.pins) do
        local zone = pin.zone and pin.zone:lower():gsub("[^%w]", "")
        if not map or not zone or map:find(zone, 1, true) or zone:find(map, 1, true) then
            local details = {}
            for _, row in ipairs(pin.details) do
                if not self.only or (row.companion and row.companion:lower() == self.only) then
                    details[#details + 1] = { quest = row.quest, companion = row.companion,
                        owed = row.owed, style = pin.style }
                end
            end
            if #details > 0 then
                candidates[#candidates + 1] = { x = pin.x, y = pin.y, zone = pin.zone, style = pin.style,
                    details = details, count = 1 }
            end
        end
    end
    table.sort(candidates, function(a, b)
        if a.style ~= b.style then return a.style == STYLE.handin end
        if a.zone ~= b.zone then return a.zone < b.zone end
        if a.x ~= b.x then return a.x < b.x end
        if a.y ~= b.y then return a.y < b.y end
        return a.details[1].quest < b.details[1].quest
    end)
    local clusters = {}
    for _, pin in ipairs(candidates) do
        local cluster
        for _, entry in ipairs(clusters) do
            local dx, dy = (entry.x - pin.x) * width / 100, (entry.y - pin.y) * height / 100
            local bothHandIns = pin.style == STYLE.handin and entry.style == STYLE.handin
            if entry.zone == pin.zone and dx * dx + dy * dy <= 18 * 18 and (not bothHandIns or (dx == 0 and dy == 0)) then
                cluster = entry
                break
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

function CompanionPins:Redraw()
    if not WorldMapDetailFrame or not WorldMapDetailFrame:IsVisible() then return end
    local w, h = WorldMapDetailFrame:GetWidth(), WorldMapDetailFrame:GetHeight()
    local mapName = GetMapInfo and GetMapInfo()
    local pins = self:VisiblePins(mapName, w, h)
    for i, pin in ipairs(pins) do
        local f = self.frames[i] or NewPin(i)
        f.pin = pin
        local px = (pin.x + self.offsetX) / 100 * w
        local py = (pin.y + self.offsetY) / 100
        f:ClearAllPoints()
        f:SetPoint("CENTER", WorldMapDetailFrame, "TOPLEFT", px, -(FLIP_Y and (1 - py) or py) * h)
        local style = pin.style
        f.glyph:SetText(style.glyph)
        f.glyph:SetTextColor(style.ring[1], style.ring[2], style.ring[3])
        f.shadow:SetText(style.glyph)
        local count = math.max(pin.count, #pin.details)
        f.count:SetText(count > 1 and tostring(count) or "")
        local label = pin.details[1].quest
        if #label > 22 then label = label:sub(1, 20) .. ".." end
        f.label:SetText(self.showLabels and label or "")
        f.label:SetTextColor(style.ring[1], style.ring[2], style.ring[3])
        f:Show()
    end
    for i = #pins + 1, #self.frames do
        self.frames[i]:Hide()
        self.frames[i].pin = nil
    end
    local lg = BuildLegend()
    if self.showLegend == false or #pins == 0 then lg:Hide() else lg:Show() end
end

--------------------------------------------------------------------------
-- commands
--------------------------------------------------------------------------

-- /cpins is the name; /wlpins stays registered because hands remember it
SLASH_WLPINS1 = "/cpins"
SLASH_WLPINS2 = "/wlpins"
SlashCmdList["WLPINS"] = function(msg)
    msg = string.lower(msg or "")

    if msg == "clear" then
        CompanionPins.pins = {}
        lastPayload, lastSnapshotAt = nil, nil
        CompanionPins:Redraw()
        DEFAULT_CHAT_FRAME:AddMessage("|cff00ccffCompanion Pins|r cleared.")

    elseif msg == "refresh" then
        RequestPins(true)

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
        local name = string.match(msg, "^only%s+(%S+)")
        if name then
            CompanionPins.only = string.lower(name)
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
        SyncPins("48,52,h=TEST PIN,z=" .. (CurrentZoneName and CurrentZoneName() or ""))
        DEFAULT_CHAT_FRAME:AddMessage("|cff00ccffCompanionPins|r test badge at 48, 52 - press M.")

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
        DEFAULT_CHAT_FRAME:AddMessage("  /cpins refresh | list | clear | test | debug | flip")
        DEFAULT_CHAT_FRAME:AddMessage("  /wlpins offset X Y - nudge the glyph if it sits off the road")
        DEFAULT_CHAT_FRAME:AddMessage("  /wlpins only <name> | all - one companion's glyphs or everyone's")
    end
end
