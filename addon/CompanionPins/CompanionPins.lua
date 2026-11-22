-- Companion Pins - 3.3.5a (Interface 30300)
--
-- The server cannot draw a quest-area pin for a quest that is not in your own log,
-- and cannot colour one at all. The companion already says where it needs to go:
--   "... I will gather them as we go (map 64, 59)."
--   "... I will hand it in next time we are near (map 43, 65)."
-- This addon turns those whispers into signposts.
--
-- v0.3  shape carries the meaning, not just colour: a round badge for a hand-in,
--       a square badge with a corner flag for work still to do, and both sit on a
--       black outline plate so nothing can be mistaken for map art.

local ADDON = "CompanionPins"
-- Pins are deleted, never recoloured, once they are this old. /cpins life changes it.
-- A pin exists exactly as long as the server says it does. No lifetimes, no saved
-- pin sets, no whisper parsing: the server's payload is the whole truth, and this
-- addon is a renderer. Every "leftover pin" bug here came from the client guessing
-- at state it could not see.
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
CompanionPins.only = nil   -- show one companion's pins, or all

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
local function SyncPins(payload)
    local fresh = {}
    for entry in string.gmatch(payload or "", "[^|]+") do
        -- grammar: x,y,h=Title@Companion[,z=ZoneName][ +N]
        local x, y, style, name, zone = string.match(entry, "^%s*(%d+)%s*,%s*(%d+)%s*,%s*([hg])%s*=%s*(.+),z=([^+|]+)")
        if x and y then
            name = string.match(name, "^[^,]+") or name
            -- A merged entry reads "TitleA@Drodan + TitleB@Drodan + TitleA@Ilyrin + ...":
            -- one tile can carry two quests for two companions, so a companion appears in
            -- several segments and a name appears twice. Collect DISTINCT titles and
            -- DISTINCT owners, in order - "The Collector + Wanted: Hogger" over
            -- "Drodan + Ilyrin", not four names with two of them doubled.
            local titles, ownerList, seenTitle, seenOwner = {}, {}, {}, {}
            for segment in string.gmatch(name, "[^+]+") do
                local q, c = SplitCompanion((segment:gsub("^%s+", ""):gsub("%s+$", "")))
                if q and q ~= "" and not seenTitle[q] then
                    seenTitle[q] = true
                    titles[#titles + 1] = q
                end
                if c and c ~= "" and not seenOwner[c] then
                    seenOwner[c] = true
                    ownerList[#ownerList + 1] = c
                    CompanionPins.companions = CompanionPins.companions or {}
                    CompanionPins.companions[c] = true
                end
            end
            local title = table.concat(titles, " + ")
            table.insert(fresh, {
                x = tonumber(x), y = tonumber(y),
                style = (style == "h") and STYLE.handin or STYLE.gather,
                quest = (title ~= "" and title) or "companion quest",
                companion = ownerList[1],
                owners = table.concat(ownerList, " + "),
                at = time(),
                -- the zone is the SERVER's word for where the pin lives; the player's
                -- own location at receive time was never the pin's location
                zone = zone and (zone:gsub("%s+$", "")) or nil,
            })
        end
    end

    local removed = #CompanionPins.pins - #fresh
    CompanionPins.pins = fresh
    SaveDB()

    local ok, err = pcall(function() CompanionPins:Redraw() end)
    if not ok then
        DEFAULT_CHAT_FRAME:AddMessage("|cffff6060CompanionPins|r draw failed: " .. tostring(err))
    end

    if CompanionPins.debug then
        DEFAULT_CHAT_FRAME:AddMessage(string.format(
            "|cff00ccffCompanionPins|r synced: %d pin(s), %d removed", #fresh, removed > 0 and removed or 0))
    end
end

local function RemovePin(pin)
    for i, p in ipairs(CompanionPins.pins) do
        if p == pin then table.remove(CompanionPins.pins, i); break end
    end
    SaveDB(); CompanionPins:Redraw()
end

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

    -- a soft offset copy of the glyph, so it holds up over bright parchment without a
    -- plate behind it (the OUTLINE font flag alone thins out at small sizes)
    f.shadow = f:CreateFontString(nil, "BACKGROUND")
    f.shadow:SetFont(STANDARD_TEXT_FONT, 25, "OUTLINE")
    f.shadow:SetPoint("CENTER", f, "CENTER", 1, -1)
    f.shadow:SetTextColor(0, 0, 0, 0.9)

    f.glyph = f:CreateFontString(nil, "OVERLAY")
    f.glyph:SetFont(STANDARD_TEXT_FONT, 25, "OUTLINE")
    f.glyph:SetPoint("CENTER", f, "CENTER", 0, 0)

    -- label to the right of the marker: the terrain under it stays visible, unlike a
    -- black bar sitting on the map
    f.label = f:CreateFontString(nil, "OVERLAY")
    f.label:SetFont(STANDARD_TEXT_FONT, 11, "OUTLINE")
    f.label:SetPoint("LEFT", f, "RIGHT", 4, 0)
    f.label:SetJustifyH("LEFT")

    -- who owes this: its own line under the quest, so ownership never fights the title
    -- for characters and never gets truncated into "@Dr..."
    f.owner = f:CreateFontString(nil, "OVERLAY")
    f.owner:SetFont(STANDARD_TEXT_FONT, 10, "OUTLINE")
    f.owner:SetPoint("TOPLEFT", f.label, "BOTTOMLEFT", 0, -1)
    f.owner:SetJustifyH("LEFT")

    f:SetScript("OnEnter", function(self)
        GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
        local pin = self.pin
        if pin then
            GameTooltip:AddLine(pin.quest or "companion quest", 1, 0.92, 0.6)
            if pin.owners and pin.owners ~= "" then
                for owner in string.gmatch(pin.owners, "[^+]+") do
                    GameTooltip:AddLine("owed by" .. owner, 0.8, 0.8, 0.8)
                end
            end
            GameTooltip:AddLine(string.format("%s - %s", pin.style.label,
                (pin.zone and pin.zone ~= "" and pin.zone or "here")), 0.85, 0.85, 0.85)
            GameTooltip:AddLine(string.format("map %d, %d", pin.x, pin.y), 0.6, 0.6, 0.6)
            GameTooltip:AddLine("right-click to remove", 0.5, 0.5, 0.5)
        end
        GameTooltip:Show()
    end)
    f:SetScript("OnLeave", function() GameTooltip:Hide() end)
    f:SetScript("OnClick", function(self, button)
        if button == "RightButton" and self.pin then RemovePin(self.pin) end
    end)

    CompanionPins.frames[index] = f
    return f
end

-- legend: what a marker means, bottom right of the map
--------------------------------------------------------------------------

-- Three lines of text on a soft dark plate. The map is parchment and the markers are
-- glyphs, so a legend that is a box with a border fights the map the same way a plated
-- pin did. Small, dim, and it only appears while there is something to explain.
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

    f:SetBackdrop({
        bgFile = "Interface\\Tooltips\\UI-Tooltip-Background",
        edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
        tile = true, tileSize = 16, edgeSize = 10,
        insets = { left = 3, right = 3, top = 3, bottom = 3 },
    })
    f:SetBackdropColor(0, 0, 0, 0.35)
    f:SetBackdropBorderColor(1, 1, 1, 0.10)

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
-- The module sends its pins as system messages prefixed CSYNC, not whispers: a whisper
-- needs a live speaker, and the map has to work when no companion is out. This addon kept
-- listening on the whisper channel after that change, so it silently received nothing and
-- its pins were whatever an older session had left in saved variables - an old title sitting
-- on another quest's area.
CompanionPins:RegisterEvent("CHAT_MSG_WHISPER")
CompanionPins:RegisterEvent("CHAT_MSG_MONSTER_WHISPER")
CompanionPins:RegisterEvent("CHAT_MSG_SYSTEM")
CompanionPins:RegisterEvent("WORLD_MAP_UPDATE")

CompanionPins:SetScript("OnEvent", function(self, event, arg1, arg2)
    if event == "PLAYER_LOGIN" then
        LoadDB()
        DEFAULT_CHAT_FRAME:AddMessage("|cff00ccffCompanion Pins|r v" .. CompanionPins.version ..
            " loaded - " .. #self.pins .. " pin(s). /cpins for help.")

    elseif event == "CHAT_MSG_WHISPER" or event == "CHAT_MSG_MONSTER_WHISPER"
        or event == "CHAT_MSG_SYSTEM" then
        if self.debug then
            DEFAULT_CHAT_FRAME:AddMessage("|cff00ccffCompanionPins|r whisper: " .. tostring(arg1))
        end
        -- "Finished 'Kobold Candles' - that pin can go." arrives when the companion
        -- actually hands a quest in; the marker has done its job by then.
        -- the server greys these lines so they do not shout in chat; strip the
        -- colour codes before trying to read anything out of them
        local text = string.gsub(arg1 or "", "|c%x%x%x%x%x%x%x%x", "")
        text = string.gsub(text, "|r", "")

        -- system messages carry every line the server says; only ours are prefixed
        if event == "CHAT_MSG_SYSTEM" then
            if string.sub(text, 1, 6) ~= "CSYNC|" then return end
            text = string.sub(text, 7)
        end

        -- "Finished 'X' - that pin can go." is spoken the moment a companion hands a
        -- quest in, which can be most of a payload window ahead of the next "Pin sync".
        -- Remove that quest's marker on the spot: the next payload is still the whole
        -- truth and re-syncs everything, this just refuses to show a cleared pin for a
        -- minute. The pin's title is the server-truncated form, so match it as a
        -- prefix of the full quest name the notice carries.
        local finished = string.match(text, "Finished%s+'(.+)'%s+-%s+that pin can go")
        if finished then
            local done = string.lower(finished)
            local before = #self.pins
            local kept = {}
            for _, pin in ipairs(self.pins) do
                local q = string.gsub(string.lower(pin.quest or ""), "%s*%.%.$", "")
                if q == "" or string.find(done, q, 1, true) then
                    kept[#kept + 1] = pin
                end
            end
            if #kept < before then
                self.pins = kept
                SaveDB()
                local ok, err = pcall(function() self:Redraw() end)
                if not ok then
                    DEFAULT_CHAT_FRAME:AddMessage("|cffff6060CompanionPins|r draw failed: " .. tostring(err))
                end
                if self.debug then
                    DEFAULT_CHAT_FRAME:AddMessage("|cff00ccffCompanionPins|r cleared a finished quest's pin: " .. finished)
                end
            end
            return
        end

        -- "Pin sync 1/3: ..." arrives in parts when the companion carries more quests
        -- than one whisper can hold. Hold them until the set is complete, then rebuild
        -- once - applying a part on its own would wipe the pins named in the others.
        local part, total, piece = string.match(text, "^Pin sync (%d+)/(%d+):%s*(.*)$")
        if part then
            CompanionPins.syncParts = CompanionPins.syncParts or {}

            -- Abandon a torn set. If a part never arrives (rate limit, relog) the held
            -- parts would sit there and pair with a later sync, leaving the map showing
            -- half of one set and half of another. Ten seconds is longer than a
            -- three-part burst ever takes.
            local now = time()
            for i = #CompanionPins.syncParts, 1, -1 do
                if now - (CompanionPins.syncParts[i].at or 0) > 10 then
                    table.remove(CompanionPins.syncParts, i)
                end
            end

            CompanionPins.syncParts[tonumber(part)] = { text = piece, at = now }

            -- '#' on a sparse table is undefined in Lua: parts arriving as 1 and 3
            -- reported a length of 3, the set was declared complete with part 2 still
            -- missing, and SyncPins then deleted the valid pins the missing part named.
            -- Completeness is checked explicitly, key by key.
            local complete = true
            for i = 1, tonumber(total) do
                if not CompanionPins.syncParts[i] then complete = false break end
            end

            if complete then
                local joined = {}
                for i = 1, tonumber(total) do
                    joined[i] = CompanionPins.syncParts[i] and CompanionPins.syncParts[i].text or ""
                end
                CompanionPins.syncParts = nil
                SyncPins(table.concat(joined, "|"))
            end
            return
        end

        local syncCount, payload = string.match(text, "^Pin sync (%d+):%s*(.*)$")
        if syncCount then
            SyncPins(payload)
            return
        end

        -- "Finished 'X'" is narrative now: the next payload already knows the quest is
        -- gone, and a pin system with one source of truth does not need a second way
        -- to remove things.
    elseif event == "WORLD_MAP_UPDATE" then
        self:Redraw()
    end
end)

local zoneWatch = CreateFrame("Frame")
zoneWatch:RegisterEvent("ZONE_CHANGED")
zoneWatch:RegisterEvent("ZONE_CHANGED_NEW_AREA")
zoneWatch:SetScript("OnEvent", function()
    if WorldMapFrame and WorldMapFrame:IsShown() then CompanionPins:Redraw() end
end)



function CompanionPins:Redraw()
    if not WorldMapDetailFrame or not WorldMapDetailFrame:IsShown() then return end

    local w, h = WorldMapDetailFrame:GetWidth(), WorldMapDetailFrame:GetHeight()
    local shown = 0
    local placed = {}
    -- The zone filter must fail OPEN: if the client API is missing or misbehaves,
    -- every pin draws (the pre-filter behaviour) rather than none.
    local mapName = nil
    if GetMapInfo then
        local ok, value = pcall(GetMapInfo)
        if ok and value then mapName = tostring(value) end
    end


    for i, pin in ipairs(self.pins) do
        -- "/cpins only <name>" narrows the map to one companion; the others are hidden
        -- but kept, so clearing the filter brings them straight back.
        -- A pin belongs to the zone it was made in; drawing it on every map and every
        -- continent read as "a pin over the wrong town", and the tooltip's zone line was
        -- the only hint. The zone stamp now decides where the pin shows. While a pin is
        -- drawn it keeps its data, so opening the right zone's map brings it back.
        local hidden = CompanionPins.only and pin.companion
                       and CompanionPins.only ~= string.lower(pin.companion)
        if not hidden and CompanionPins.only and not pin.companion then
            hidden = true    -- the filter narrows to companions; untagged pins are not a match
        end
        -- Zone names disagree between the DBC ("Elwynn") and the client's map title
        -- ("Elwynn Forest"); an exact compare hid every pin on every map. A name that
        -- CONTAINS the other is the same place.
        if not hidden and pin.zone and mapName then
            local pz, mz = string.lower(pin.zone), string.lower(mapName)
            if not string.find(mz, pz, 1, true) and not string.find(pz, mz, 1, true) then
                hidden = true
            end
        end

        if self.debug then
            DEFAULT_CHAT_FRAME:AddMessage(string.format(
                "|cff00ccffCompanionPins|r pin '%s' zone '%s' vs map '%s' -> %s",
                pin.quest or "?", pin.zone or "-", mapName or "-",
                hidden and "hidden" or "drawn"))
        end

        if hidden then
            if CompanionPins.frames[i] then CompanionPins.frames[i]:Hide() end
            shown = shown + 0
        else
        -- Reuse the frame at this index: creating a new one per redraw orphaned the old
        -- one, still parented to the map and still visible, so every zoom left another
        -- marker behind that nothing could hide.
        local f = CompanionPins.frames[i] or NewPin(i)
        f.pin = pin

        local px = ((pin.x + CompanionPins.offsetX) / 100) * w
        local py = (FLIP_Y and (1 - (pin.y + CompanionPins.offsetY) / 100)
                           or ((pin.y + CompanionPins.offsetY) / 100)) * h
        f:ClearAllPoints()
        f:SetPoint("CENTER", WorldMapDetailFrame, "TOPLEFT", px, -py)
        f:Show()

        local style = pin.style or STYLE.other
        local r, g, b = style.ring[1], style.ring[2], style.ring[3]

        f.glyph:SetText(style.glyph)
        f.glyph:SetTextColor(r, g, b)
        f.shadow:SetText(style.glyph)

        -- Glyphs are the map's language, not labels: eight quests with persistent text
        -- carpeted the map and the rows of nudged labels collided anyway. The glyph's
        -- colour already says the state and its spot says where; the hover tooltip
        -- carries the full detail (title, owed by, zone, coords). Persistent labels are
        -- an opt-in via /cpins labels, and the nudge runs only in that mode.
        if CompanionPins.showLabels then
            local text = pin.quest or "companion quest"
            if #text > 22 then text = string.sub(text, 1, 20) .. ".." end
            f.label:SetText(text)
            f.label:SetTextColor(r, g, b)

            -- the companions that owe it, on their own muted line - every name whole
            if pin.owners and pin.owners ~= "" then
                f.owner:SetText("|cff9a9a9a" .. pin.owners .. "|r")
            else
                f.owner:SetText("")
            end

            -- Nudge the label down a row when it would land on one already placed: two
            -- Goldshire hand-ins are four percent apart, invisible at map scale.
            local width = f.label:GetStringWidth()
            local row = 0
            for attempt = 0, 3 do
                row = attempt
                local clash = false
                local myCentreX = px + 16 + width / 2
                for _, other in ipairs(placed) do
                    if math.abs(myCentreX - other.x) * 2 < (width + other.w)
                       and math.abs(py - (other.y)) < 13 then
                        clash = true
                        break
                    end
                end
                if not clash then break end
            end

            f.label:SetPoint("LEFT", f, "RIGHT", 4, -row * 13)
            -- the clash test compares this pin's GLYPH centre against the other's LABEL
            -- centre; both sides now use the label centre, so a near-miss cannot pass
            table.insert(placed, { x = px + 16 + width / 2, y = py - row * 13, w = width, label = true })
        else
            f.label:SetText("")
            f.owner:SetText("")
            f.label:SetPoint("LEFT", f, "RIGHT", 4, 0)
        end

        end   -- /cpins only
        shown = shown + 1
    end

    for i = #self.pins + 1, #self.frames do
        self.frames[i]:Hide()
        self.frames[i].pin = nil
    end

    local lg = BuildLegend()

    if CompanionPins.showLegend == false or #self.pins == 0 then
        lg:Hide()
    else
        lg:Show()
    end
end

--------------------------------------------------------------------------
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
        SaveDB(); CompanionPins:Redraw()
        DEFAULT_CHAT_FRAME:AddMessage("|cff00ccffCompanion Pins|r cleared.")

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
        DEFAULT_CHAT_FRAME:AddMessage("  /wlpins list | clear | test | debug | flip")
        DEFAULT_CHAT_FRAME:AddMessage("  /wlpins offset X Y - nudge the glyph if it sits off the road")
        DEFAULT_CHAT_FRAME:AddMessage("  /wlpins only <name> | all - one companion's glyphs or everyone's")
    end
end
