-- WL Companion Pins - 3.3.5a (Interface 30300)  v0.3
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

local ADDON = "WLPins"
local PIN_LIFETIME = 30 * 60
local FLIP_Y = false

local WLPins = CreateFrame("Frame")
WLPins.debug = false
WLPins.offsetX = 0
WLPins.offsetY = 0
WLPins.pins = {}
WLPins.frames = {}

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

local function ParseCoords(text)
    if type(text) ~= "string" then return nil end
    local x, y = string.match(text, "map%s+(%d+)%s*,%s*(%d+)")
    if not x or not y then return nil end
    x, y = tonumber(x), tonumber(y)
    if not x or not y or x < 0 or x > 100 or y < 0 or y > 100 then return nil end
    return x, y
end

local function ParseQuestName(text)
    if type(text) ~= "string" then return nil end
    local name = string.match(text, "'([^']+)'")      -- 'Kobold Candles'
    if name then return name end
    return string.match(text, "\"([^\"]+)\"")
end

local function Classify(text)
    local lower = string.lower(text or "")
    if string.find(lower, "my own copy") or string.find(lower, "hand it in") then return STYLE.handin end
    if string.find(lower, "i still owe") then return STYLE.gather end
    if string.find(lower, "to finish") then return STYLE.finish end
    return STYLE.other
end

local function CurrentZoneName()
    if GetZoneText then return GetZoneText() end
    return ""
end

--------------------------------------------------------------------------
-- storage
--------------------------------------------------------------------------

local function SaveDB()
    WLPinsDB = WLPinsDB or {}
    WLPinsDB.offsetX, WLPinsDB.offsetY = WLPins.offsetX, WLPins.offsetY
    WLPinsDB.pins = {}
    for i, pin in ipairs(WLPins.pins) do
        WLPinsDB.pins[i] = { x = pin.x, y = pin.y, at = pin.at, quest = pin.quest,
                             zone = pin.zone, style = pin.style.key }
    end
end

local function LoadDB()
    WLPinsDB = WLPinsDB or { pins = {} }
    WLPins.offsetX = WLPinsDB.offsetX or 0
    WLPins.offsetY = WLPinsDB.offsetY or 0
    for _, saved in ipairs(WLPinsDB.pins or {}) do
        table.insert(WLPins.pins, {
            x = saved.x, y = saved.y, at = saved.at, quest = saved.quest,
            zone = saved.zone, style = STYLE[saved.style or "other"] or STYLE.other,
        })
    end
end

local function AddPin(x, y, style, quest)
    for _, pin in ipairs(WLPins.pins) do
        if math.abs(pin.x - x) < 1.5 and math.abs(pin.y - y) < 1.5 then
            pin.at, pin.style, pin.quest, pin.zone = time(), style, quest, CurrentZoneName()
            SaveDB(); WLPins:Redraw()
            return
        end
    end
    table.insert(WLPins.pins, { x = x, y = y, at = time(), style = style,
                                quest = quest, zone = CurrentZoneName() })
    SaveDB(); WLPins:Redraw()
end

-- "Pin sync 3: 43,65,h=Kobold Candles|64,59,g=Gold Dust Exchange" is the server's own
-- answer to "which pins are real". Rebuilding from it is what cleans the map after a
-- desync: anything it does not mention is removed, anything missing is added back.
local function SyncPins(payload)
    local fresh = {}
    for entry in string.gmatch(payload or "", "[^|]+") do
        local x, y, style, name = string.match(entry, "^%s*(%d+)%s*,%s*(%d+)%s*,%s*([hg])%s*=%s*(.+)$")
        if x and y then
            local key = string.lower(name)
            local kept = nil
            for _, pin in ipairs(WLPins.pins) do
                if string.lower(pin.quest or "") == key then kept = pin.at end
            end
            table.insert(fresh, {
                x = tonumber(x), y = tonumber(y),
                style = (style == "h") and STYLE.handin or STYLE.gather,
                quest = name, at = kept or time(), zone = CurrentZoneName(),
            })
        end
    end

    local removed = #WLPins.pins - #fresh
    WLPins.pins = fresh
    SaveDB(); WLPins:Redraw()

    if WLPins.debug then
        DEFAULT_CHAT_FRAME:AddMessage(string.format(
            "|cff00ccffWLPins|r synced: %d pin(s), %d removed", #fresh, removed > 0 and removed or 0))
    end
end

local function RemoveByQuest(name)
    if not name then return end
    local target = string.lower(name)
    for i = #WLPins.pins, 1, -1 do
        local quest = string.lower(WLPins.pins[i].quest or "")
        if quest == target then table.remove(WLPins.pins, i) end
    end
    SaveDB(); WLPins:Redraw()
end

local function RemovePin(pin)
    for i, p in ipairs(WLPins.pins) do
        if p == pin then table.remove(WLPins.pins, i); break end
    end
    SaveDB(); WLPins:Redraw()
end

--------------------------------------------------------------------------
-- drawing
--------------------------------------------------------------------------

-- Built-in textures only: the round badge uses the quickslot ring, the square one
-- is a plate built from WHITE8X8. Every badge gets a black outline underlay so it
-- reads against parchment, snow and dark forest alike.
local RING_ROUND = "Interface\\Buttons\\UI-Quickslot2"
local WHITE      = "Interface\\Buttons\\WHITE8X8"

local function NewPin(index)
    local f = CreateFrame("Button", ADDON .. "Pin" .. index, WorldMapDetailFrame)
    f:SetSize(34, 34)

    f.outline = f:CreateTexture(nil, "BACKGROUND")
    f.outline:SetPoint("CENTER")
    f.outline:SetSize(36, 36)
    f.outline:SetTexture(WHITE)
    f.outline:SetVertexColor(0, 0, 0, 0.95)      -- black outline plate

    f.shape = f:CreateTexture(nil, "BACKGROUND")
    f.shape:SetPoint("CENTER")
    f.shape:SetSize(30, 30)
    f.shape:SetTexture(RING_ROUND)                -- ring texture doubles as the face

    f.plate = f:CreateTexture(nil, "ARTWORK")
    f.plate:SetPoint("CENTER")
    f.plate:SetSize(22, 22)
    f.plate:SetTexture(WHITE)
    f.plate:SetVertexColor(0.05, 0.04, 0.03, 0.95)  -- dark face keeps text legible

    f.glyph = CreateFrame("Frame", nil, f)
    f.glyph.text = f.glyph:CreateFontString(nil, "OVERLAY")
    f.glyph.text:SetFont(STANDARD_TEXT_FONT, 17, "OUTLINE")
    f.glyph.text:SetPoint("CENTER", f, "CENTER", 0, 0)

    f.tab = CreateFrame("Frame", nil, f)
    f.tab:SetPoint("TOP", f, "BOTTOM", 0, -3)
    f.tab:SetHeight(17)
    f.tab.outline = f.tab:CreateTexture(nil, "BACKGROUND")
    f.tab.outline:SetAllPoints(f.tab)
    f.tab.outline:SetTexture(WHITE)
    f.tab.outline:SetVertexColor(0, 0, 0, 0.95)
    f.tab.bg = f.tab:CreateTexture(nil, "BORDER")
    f.tab.bg:SetPoint("TOPLEFT", 2, -2)
    f.tab.bg:SetPoint("BOTTOMRIGHT", -2, 2)
    f.tab.bg:SetTexture(WHITE)
    f.tab.bg:SetVertexColor(0.07, 0.06, 0.05, 0.95)
    f.tab.text = f.tab:CreateFontString(nil, "OVERLAY")
    f.tab.text:SetFont(STANDARD_TEXT_FONT, 12, "OUTLINE")
    f.tab.text:SetPoint("CENTER", f.tab, "CENTER", 0, 0)

    f:SetScript("OnEnter", function(self)
        GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
        local pin = self.pin
        if pin then
            GameTooltip:AddLine(pin.quest or "companion quest", 1, 0.92, 0.6)
            GameTooltip:AddLine(string.format("%s - %s", pin.style.label,
                (pin.zone and pin.zone ~= "" and pin.zone or "here")), 0.85, 0.85, 0.85)
            GameTooltip:AddLine(string.format("%d, %d", pin.x, pin.y), 0.6, 0.6, 0.6)
            GameTooltip:AddLine("right-click to remove", 0.5, 0.5, 0.5)
        end
        GameTooltip:Show()
    end)
    f:SetScript("OnLeave", function() GameTooltip:Hide() end)
    f:SetScript("OnClick", function(self, button)
        if button == "RightButton" and self.pin then RemovePin(self.pin) end
    end)

    WLPins.frames[index] = f
    return f
end

function WLPins:Redraw()
    if not WorldMapDetailFrame or not WorldMapDetailFrame:IsShown() then return end

    local w, h = WorldMapDetailFrame:GetWidth(), WorldMapDetailFrame:GetHeight()
    local now = time()
    local shown = 0

    for i, pin in ipairs(self.pins) do
        local f = NewPin(i)
        f.pin = pin

        local px = ((pin.x + WLPins.offsetX) / 100) * w
        local py = (FLIP_Y and (1 - (pin.y + WLPins.offsetY) / 100)
                           or ((pin.y + WLPins.offsetY) / 100)) * h
        f:ClearAllPoints()
        f:SetPoint("CENTER", WorldMapDetailFrame, "TOPLEFT", px, -py)
        f:Show()

        local style = pin.style or STYLE.other
        local stale = (now - (pin.at or 0)) > PIN_LIFETIME
        local r, g, b = style.ring[1], style.ring[2], style.ring[3]
        if stale then r, g, b = 0.75, 0.35, 0.30 end

        -- round badge for a hand-in, square plate for work left to do
        if style.shape == "round" then
            f.shape:SetTexture(RING_ROUND)
            f.shape:SetSize(30, 30)
            f.shape:SetVertexColor(r, g, b, 1)
            f.plate:SetSize(20, 20)
            f.plate:Show()
        else
            f.shape:SetTexture(WHITE)
            f.shape:SetSize(30, 30)
            f.shape:SetVertexColor(r, g, b, 1)
            f.plate:SetSize(24, 24)
            f.plate:Show()
        end

        f.glyph.text:SetText(style.glyph)
        f.glyph.text:SetTextColor(r, g, b)

        local text = pin.quest or "companion quest"
        if #text > 24 then text = string.sub(text, 1, 22) .. "..." end
        f.tab.text:SetText(text .. "  |cffb0a898(" .. style.label .. ")|r")
        f.tab:SetWidth(math.max(46, f.tab.text:GetStringWidth() + 14))

        shown = shown + 1
    end

    for i = shown + 1, #self.frames do
        self.frames[i]:Hide()
    end
end

--------------------------------------------------------------------------
-- events
--------------------------------------------------------------------------

WLPins:RegisterEvent("PLAYER_LOGIN")
WLPins:RegisterEvent("CHAT_MSG_WHISPER")
WLPins:RegisterEvent("CHAT_MSG_MONSTER_WHISPER")
WLPins:RegisterEvent("WORLD_MAP_UPDATE")

WLPins:SetScript("OnEvent", function(self, event, arg1)
    if event == "PLAYER_LOGIN" then
        LoadDB()
        DEFAULT_CHAT_FRAME:AddMessage("|cff00ccffWL Companion Pins|r v0.3 loaded - " ..
            #self.pins .. " pin(s). /wlpins for help.")

    elseif event == "CHAT_MSG_WHISPER" or event == "CHAT_MSG_MONSTER_WHISPER" then
        if self.debug then
            DEFAULT_CHAT_FRAME:AddMessage("|cff00ccffWLPins|r whisper: " .. tostring(arg1))
        end
        -- "Finished 'Kobold Candles' - that pin can go." arrives when the companion
        -- actually hands a quest in; the marker has done its job by then.
        -- the server greys these lines so they do not shout in chat; strip the
        -- colour codes before trying to read anything out of them
        local text = string.gsub(arg1 or "", "|c%x%x%x%x%x%x%x%x", "")
        text = string.gsub(text, "|r", "")

        local syncCount, payload = string.match(text, "^Pin sync (%d+):%s*(.*)$")
        if syncCount then
            SyncPins(payload)
            return
        end

        local done = string.match(text, "Finished '([^']+)'")
        if done then
            RemoveByQuest(done)
            if self.debug then
                DEFAULT_CHAT_FRAME:AddMessage("|cff00ccffWLPins|r pin cleared: " .. done)
            end
            return
        end

        local x, y = ParseCoords(text)
        if x and y then
            AddPin(x, y, Classify(text), ParseQuestName(text) or "companion quest")
        end

    elseif event == "WORLD_MAP_UPDATE" then
        self:Redraw()
    end
end)

local zoneWatch = CreateFrame("Frame")
zoneWatch:RegisterEvent("ZONE_CHANGED")
zoneWatch:RegisterEvent("ZONE_CHANGED_NEW_AREA")
zoneWatch:SetScript("OnEvent", function()
    if WorldMapFrame and WorldMapFrame:IsShown() then WLPins:Redraw() end
end)

--------------------------------------------------------------------------
-- commands
--------------------------------------------------------------------------

SLASH_WLPINS1 = "/wlpins"
SlashCmdList["WLPINS"] = function(msg)
    msg = string.lower(msg or "")

    if msg == "clear" then
        WLPins.pins = {}
        SaveDB(); WLPins:Redraw()
        DEFAULT_CHAT_FRAME:AddMessage("|cff00ccffWL Companion Pins|r cleared.")

    elseif msg == "flip" then
        FLIP_Y = not FLIP_Y
        WLPins:Redraw()
        DEFAULT_CHAT_FRAME:AddMessage("|cff00ccffWL Companion Pins|r vertical flip " ..
            (FLIP_Y and "on" or "off") .. ".")

    elseif string.find(msg, "^offset") then
        local ox, oy = string.match(msg, "^offset%s+(-?%d+)%s*(-?%d+)")
        if ox and oy then
            WLPins.offsetX, WLPins.offsetY = tonumber(ox), tonumber(oy)
            SaveDB(); WLPins:Redraw()
            DEFAULT_CHAT_FRAME:AddMessage(string.format(
                "|cff00ccffWLPins|r calibration offset %d, %d.", WLPins.offsetX, WLPins.offsetY))
        else
            DEFAULT_CHAT_FRAME:AddMessage(string.format(
                "|cff00ccffWLPins|r offset is %d, %d - change it with /wlpins offset -7 -3",
                WLPins.offsetX, WLPins.offsetY))
        end

    elseif msg == "test" then
        AddPin(48, 52, STYLE.handin, "TEST PIN")
        DEFAULT_CHAT_FRAME:AddMessage("|cff00ccffWLPins|r test badge at 48, 52 - press M.")

    elseif msg == "debug" then
        WLPins.debug = not WLPins.debug
        DEFAULT_CHAT_FRAME:AddMessage("|cff00ccffWLPins|r whisper logging " ..
            (WLPins.debug and "on" or "off") .. ".")

    elseif msg == "list" then
        if #WLPins.pins == 0 then
            DEFAULT_CHAT_FRAME:AddMessage("|cff00ccffWL Companion Pins|r: no pins.")
            return
        end
        for _, pin in ipairs(WLPins.pins) do
            DEFAULT_CHAT_FRAME:AddMessage(string.format("|cff00ccffWLPins|r %d, %d - %s (%s)",
                pin.x, pin.y, pin.quest or "?", pin.style.label))
        end

    else
        DEFAULT_CHAT_FRAME:AddMessage("|cff00ccffWL Companion Pins|r - signposts from the companion's whispers")
        DEFAULT_CHAT_FRAME:AddMessage("  |cffffcc33round gold ?|r  hand in a copy the companion owes")
        DEFAULT_CHAT_FRAME:AddMessage("  |cff59f259square green !|r  area where it still has work")
        DEFAULT_CHAT_FRAME:AddMessage("  |cff66b3ffsquare blue ?|r  a turn-in only it can do")
        DEFAULT_CHAT_FRAME:AddMessage("  /wlpins list | clear | test | debug | flip")
        DEFAULT_CHAT_FRAME:AddMessage("  /wlpins offset X Y - nudge every pin if they sit off the road")
        DEFAULT_CHAT_FRAME:AddMessage("  right-click a pin to remove it")
    end
end
