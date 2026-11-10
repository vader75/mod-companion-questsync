-- WL Companion Pins - 3.3.5a (Interface 30300)
--
-- The server cannot tint a quest-area pin and cannot draw one for a quest that is
-- not in your own log. What it CAN do is tell you where the companion needs to go,
-- and it already does that in its whisper: "... about 456 yards from me, map 82, 34."
--
-- This addon reads those whispers and puts a coloured pin at those map coordinates,
-- so the companion's errands show up on your world map as their own markers:
--
--   gold pin   the companion still owes its own copy of a quest you already finished
--   blue pin   a quest you are both working on, and the giver is that way
--   red pin    a pin older than 30 minutes (stale, click to remove)
--
-- No libraries, no server changes. Install, log in, and the companion's whispers
-- start drawing pins. Right-click a pin to remove it, or /wlpins clear.

local ADDON = "WLPins"
local PIN_LIFETIME = 30 * 60          -- seconds a pin stays useful
local FLIP_Y = false                  -- set true with /wlpins flip if pins land mirrored

local WLPins = CreateFrame("Frame")
WLPins.pins = {}                      -- [{x=, y=, colour=, label=, at=, zone=}]
WLPins.frames = {}

--------------------------------------------------------------------------
-- parsing
--------------------------------------------------------------------------

-- "... map 82, 34." / "(map 82, 34)" - digits, comma, digits, not part of a bigger number
local function ParseCoords(text)
    if type(text) ~= "string" then return nil end
    local x, y = string.match(text, "map%s+(%d+)%s*,%s*(%d+)")
    if not x or not y then return nil end
    x, y = tonumber(x), tonumber(y)
    if not x or not y or x < 0 or x > 100 or y < 0 or y > 100 then return nil end
    return x, y
end

local function Classify(text)
    local lower = string.lower(text or "")
    if string.find(lower, "my own copy") then
        return { r = 1.00, g = 0.82, b = 0.16 }, "companion turn-in"
    end
    if string.find(lower, "still owe") or string.find(lower, "help each other") then
        return { r = 0.20, g = 0.90, b = 0.30 }, "quest to finish"
    end
    return { r = 0.25, g = 0.60, b = 1.00 }, "turn-in"
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
    WLPinsDB.pins = {}
    for i, pin in ipairs(WLPins.pins) do
        WLPinsDB.pins[i] = { x = pin.x, y = pin.y, at = pin.at,
                             label = pin.label, zone = pin.zone,
                             r = pin.colour.r, g = pin.colour.g, b = pin.colour.b }
    end
end

local function LoadDB()
    WLPinsDB = WLPinsDB or { pins = {} }
    for _, saved in ipairs(WLPinsDB.pins or {}) do
        table.insert(WLPins.pins, {
            x = saved.x, y = saved.y, at = saved.at, label = saved.label, zone = saved.zone,
            colour = { r = saved.r or 1, g = saved.g or 0.82, b = saved.b or 0.16 },
        })
    end
end

local function AddPin(x, y, colour, label)
    -- one pin per spot: refresh instead of stacking
    for _, pin in ipairs(WLPins.pins) do
        if math.abs(pin.x - x) < 1.5 and math.abs(pin.y - y) < 1.5 then
            pin.at, pin.colour, pin.label, pin.zone = time(), colour, label, CurrentZoneName()
            SaveDB(); WLPins:Redraw()
            return
        end
    end

    table.insert(WLPins.pins, { x = x, y = y, at = time(), colour = colour,
                                label = label, zone = CurrentZoneName() })
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

local function PinFrame(index)
    local f = WLPins.frames[index]
    if f then return f end

    f = CreateFrame("Button", ADDON .. "Pin" .. index, WorldMapDetailFrame)
    f:SetSize(16, 16)

    f.glow = f:CreateTexture(nil, "BACKGROUND")
    f.glow:SetAllPoints(f)
    f.glow:SetTexture("Interface\\Buttons\\WHITE8X8")
    f.glow:SetVertexColor(1, 1, 1, 0.35)

    f.dot = f:CreateTexture(nil, "ARTWORK")
    f.dot:SetPoint("CENTER")
    f.dot:SetSize(12, 12)
    f.dot:SetTexture("Interface\\Buttons\\WHITE8X8")

    f.ring = f:CreateTexture(nil, "OVERLAY")
    f.ring:SetPoint("CENTER")
    f.ring:SetSize(16, 16)
    f.ring:SetTexture("Interface\\Buttons\\WHITE8X8")
    f.ring:SetVertexColor(0, 0, 0, 0.55)

    f.label = f:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    f.label:SetPoint("BOTTOM", f, "TOP", 0, 2)
    f.label:SetTextColor(1, 0.92, 0.65)

    f:SetScript("OnEnter", function(self)
        GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
        local pin = self.pin
        if pin then
            GameTooltip:AddLine(pin.label or "companion", 1, 0.9, 0.6)
            GameTooltip:AddLine(string.format("%d, %d%s", pin.x, pin.y,
                pin.zone and pin.zone ~= "" and ("  " .. pin.zone) or ""), 0.9, 0.9, 0.9)
            GameTooltip:AddLine("right-click to remove", 0.6, 0.6, 0.6)
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
        local stale = (now - (pin.at or 0)) > PIN_LIFETIME
        local f = PinFrame(i)
        f.pin = pin

        local px = (pin.x / 100) * w
        local py = (FLIP_Y and (1 - pin.y / 100) or (pin.y / 100)) * h
        f:ClearAllPoints()
        f:SetPoint("CENTER", WorldMapDetailFrame, "TOPLEFT", px, -py)
        f:Show()

        local c = pin.colour or { r = 1, g = 0.82, b = 0.16 }
        if stale then
            f.dot:SetVertexColor(1, 0.25, 0.20, 0.75)
        else
            f.dot:SetVertexColor(c.r, c.g, c.b, 1)
        end
        f.label:SetText(pin.label or "")
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

WLPins:SetScript("OnEvent", function(self, event, arg1, arg2)
    if event == "PLAYER_LOGIN" then
        LoadDB()
        DEFAULT_CHAT_FRAME:AddMessage("|cff00ccffWL Companion Pins|r loaded - " ..
            #self.pins .. " pin(s). /wlpins for help.")

    elseif event == "CHAT_MSG_WHISPER" or event == "CHAT_MSG_MONSTER_WHISPER" then
        local x, y = ParseCoords(arg1)
        if x and y then
            local colour, label = Classify(arg1)
            AddPin(x, y, colour, label)
        end

    elseif event == "WORLD_MAP_UPDATE" then
        self:Redraw()
    end
end)

-- the map frame is rebuilt whenever the zone changes; redraw after it settles
local hooked = CreateFrame("Frame")
hooked:RegisterEvent("ZONE_CHANGED")
hooked:RegisterEvent("ZONE_CHANGED_NEW_AREA")
hooked:SetScript("OnEvent", function()
    if WorldMapFrame and WorldMapFrame:IsShown() then WLPins:Redraw() end
end)

--------------------------------------------------------------------------
-- slash command
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
            (FLIP_Y and "on" or "off") .. " (pins move to the other side of the map).")

    elseif msg == "list" then
        if #WLPins.pins == 0 then
            DEFAULT_CHAT_FRAME:AddMessage("|cff00ccffWL Companion Pins|r: no pins.")
            return
        end
        for _, pin in ipairs(WLPins.pins) do
            DEFAULT_CHAT_FRAME:AddMessage(string.format("|cff00ccffWLPins|r %d, %d - %s",
                pin.x, pin.y, pin.label or "?"))
        end

    else
        DEFAULT_CHAT_FRAME:AddMessage("|cff00ccffWL Companion Pins|r - pins from the companion's whispers")
        DEFAULT_CHAT_FRAME:AddMessage("  /wlpins list  - what is pinned")
        DEFAULT_CHAT_FRAME:AddMessage("  /wlpins clear - remove every pin")
        DEFAULT_CHAT_FRAME:AddMessage("  /wlpins flip  - pins mirrored top/bottom? flip them")
        DEFAULT_CHAT_FRAME:AddMessage("  right-click a pin to remove it")
    end
end
