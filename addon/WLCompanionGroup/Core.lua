-- WLCompanionGroup/Core.lua
--
-- Data, names and commands. Everything the server tells us arrives on the whisper
-- channel, because a 3.3.5 client cannot ask the server anything.
--
--   Comp sync 2: Healy,5,1,32,4,ogs,110|Soak,9,2,32,2,o,105
--                name,class,race,level,quests,flags,free slots
--
--   Pool stock 2: 1-5=12|3-2=9 ...
--                race-class=free bodies, so the picker can grey out what the pool
--                cannot serve instead of failing after the player has chosen.
--
-- Flags: o in world, g in your party, s same level as you, b behind (you carry a quest
-- it has not got).

local addonName, G = ...

G.version = "2.0.0"
G.companions = {}
G.stock = {}            -- ["race-class"] = free bodies
G.syncParts = {}
G.syncExpected = 0
G.stockParts = {}
G.stockExpected = 0

G.RACES = {
    [1] = "Human", [2] = "Orc", [3] = "Dwarf", [4] = "Night Elf", [5] = "Undead",
    [6] = "Tauren", [7] = "Gnome", [8] = "Troll", [10] = "Blood Elf", [11] = "Draenei",
}

G.CLASSES = {
    [1] = "Warrior", [2] = "Paladin", [3] = "Hunter", [4] = "Rogue", [5] = "Priest",
    [6] = "Death Knight", [7] = "Shaman", [8] = "Mage", [9] = "Warlock", [11] = "Druid",
}

G.CLASS_TOKEN = {
    [1] = "WARRIOR", [2] = "PALADIN", [3] = "HUNTER", [4] = "ROGUE", [5] = "PRIEST",
    [6] = "DEATHKNIGHT", [7] = "SHAMAN", [8] = "MAGE", [9] = "WARLOCK", [11] = "DRUID",
}

local ALLIANCE_RACES = { 1, 3, 4, 7, 11 }
local HORDE_RACES    = { 2, 5, 6, 8, 10 }
local GREY = { r = 0.62, g = 0.62, b = 0.62 }

-- suggestions for the name box, so creating a companion needs no typing at all
local NAME_PARTS_A = { "Bran", "Kel", "Vor", "Mir", "Thal", "Ari", "Dro", "Sel", "Gor", "Ily" }
local NAME_PARTS_B = { "dan", "rin", "gar", "wyn", "thas", "nor", "mir", "vek", "lin", "ros" }

function G.RaceName(id) return G.RACES[id] or "Unknown" end
function G.ClassName(id) return G.CLASSES[id] or "Unknown" end

function G.ClassColor(classId)
    local token = G.CLASS_TOKEN[classId]
    local col = token and RAID_CLASS_COLORS and RAID_CLASS_COLORS[token]
    if col then return col.r, col.g, col.b end
    return GREY.r, GREY.g, GREY.b
end

function G.PlayerRaces()
    local faction = UnitFactionGroup("player")
    if faction == "Horde" then return HORDE_RACES end
    return ALLIANCE_RACES
end

function G.SuggestName()
    for _ = 1, 40 do
        local name = NAME_PARTS_A[math.random(#NAME_PARTS_A)] .. NAME_PARTS_B[math.random(#NAME_PARTS_B)]
        if not G.NameTaken(name) then return name end
    end
    return "Companion"
end

--- Names already used by anyone in our list, so a suggestion is never a duplicate.
function G.NameTaken(name)
    for _, companion in ipairs(G.companions) do
        if string.lower(companion.name) == string.lower(name) then return true end
    end
    return false
end

function G.ValidName(name)
    return type(name) == "string" and #name >= 2 and #name <= 12 and name:match("^%a+$") ~= nil
end

function G.HasFlag(companion, flag)
    return companion and companion.flags and companion.flags:find(flag, 1, true) ~= nil
end

function G.StockFor(raceId, classId)
    return G.stock[tostring(raceId) .. "-" .. tostring(classId)] or 0
end

-- ---------------------------------------------------------------------------
-- parsing
-- ---------------------------------------------------------------------------

local function StripColors(text)
    text = text or ""
    text = text:gsub("|c%x%x%x%x%x%x%x%x", "")
    text = text:gsub("|r", "")
    return text
end

function G.ParseCompPayload(payload)
    local rows = {}

    for entry in string.gmatch(payload or "", "[^|]+") do
        local name, classId, raceId, level, quests, flags, free =
            string.match(entry, "^([^,]+),([^,]*),([^,]*),([^,]*),([^,]*),([^,]*),([^,]*)$")

        if name and name ~= "" then
            rows[#rows + 1] = {
                name = name,
                classId = tonumber(classId) or 0,
                raceId = tonumber(raceId) or 0,
                level = tonumber(level) or 0,
                quests = tonumber(quests) or 0,
                flags = flags or "-",
                free = tonumber(free) or 0,
            }
        end
    end

    table.sort(rows, function(a, b)
        local ao, bo = G.HasFlag(a, "o"), G.HasFlag(b, "o")
        if ao ~= bo then return ao end
        return a.name < b.name
    end)

    return rows
end

function G.ParseStockPayload(payload)
    local stock = {}

    for entry in string.gmatch(payload or "", "[^|]+") do
        local key, count = string.match(entry, "^(%d+%-%d+)=(%d+)$")
        if key then stock[key] = tonumber(count) or 0 end
    end

    return stock
end

function G.SetCompanions(rows)
    G.companions = rows or {}
    if G.OnCompanionsChanged then G.OnCompanionsChanged() end
end

function G.SetStock(stock)
    G.stock = stock or {}
    if G.OnStockChanged then G.OnStockChanged() end
end

-- ---------------------------------------------------------------------------
-- whisper protocol
-- ---------------------------------------------------------------------------

local listener = CreateFrame("Frame")
listener:RegisterEvent("CHAT_MSG_WHISPER")
listener:RegisterEvent("CHAT_MSG_MONSTER_WHISPER")

-- Parts of a chunked payload are held until the set is complete, then joined. Sharing
-- this for both payloads keeps one place to fix if the server ever changes the grammar.
local function Accumulate(parts, expected, index, text)
    parts[index] = text

    for i = 1, expected do
        if not parts[i] then return false end
    end

    return true
end

local function Join(parts, expected)
    local ordered = {}
    for i = 1, expected do ordered[#ordered + 1] = parts[i] end
    return table.concat(ordered, "|")
end

listener:SetScript("OnEvent", function(_, _, message)
    local text = StripColors(message)

    -- companions
    local part, total, body = string.match(text, "^Comp%s+sync%s+(%d+)/(%d+):%s*(.*)$")
    if part then
        G.syncExpected = tonumber(total)
        if Accumulate(G.syncParts, G.syncExpected, tonumber(part), body) then
            local joined = Join(G.syncParts, G.syncExpected)
            G.syncParts = {}
            G.SetCompanions(G.ParseCompPayload(joined))
        end
        return
    end

    local single = string.match(text, "^Comp%s+sync%s+%d+:%s*(.*)$")
    if single then
        G.SetCompanions(G.ParseCompPayload(single))
        return
    end

    -- pool stock
    local sPart, sTotal, sBody = string.match(text, "^Pool%s+stock%s+(%d+)/(%d+):%s*(.*)$")
    if sPart then
        G.stockExpected = tonumber(sTotal)
        if Accumulate(G.stockParts, G.stockExpected, tonumber(sPart), sBody) then
            local joined = Join(G.stockParts, G.stockExpected)
            G.stockParts = {}
            G.SetStock(G.ParseStockPayload(joined))
        end
        return
    end

    local sSingle = string.match(text, "^Pool%s+stock%s+%d+:%s*(.*)$")
    if sSingle then
        G.SetStock(G.ParseStockPayload(sSingle))
    end
end)

-- ---------------------------------------------------------------------------
-- commands
-- ---------------------------------------------------------------------------

function G.Run(command)
    if not command or command == "" then return end
    SendChatMessage(command, "SAY")
end

function G.Action(verb, name)
    if name and name ~= "" then
        G.Run(".companionsync " .. verb .. " " .. name)
    else
        G.Run(".companionsync " .. verb)
    end
end

function G.CreateCompanion(raceId, classId, name)
    if not G.ValidName(name) then
        G.Warn("a name is 2 to 12 letters")
        return false
    end

    if G.StockFor(raceId, classId) <= 0 then
        G.Warn("the pool has no free " .. G.RaceName(raceId) .. " " .. G.ClassName(classId) .. " body")
        return false
    end

    G.Run(".companionsync create " .. raceId .. " " .. classId .. " " .. name)
    return true
end

function G.Warn(text)
    DEFAULT_CHAT_FRAME:AddMessage("|cffffc94dCompanions|r: " .. (text or ""))
end

-- confirmations -----------------------------------------------------------------
local function Confirm(key, text, onAccept)
    StaticPopupDialogs[key] = {
        text = text,
        button1 = "Yes",
        button2 = "Cancel",
        OnAccept = function(self, data) if onAccept then onAccept(data) end end,
        timeout = 0,
        whileDead = true,
        hideOnEscape = true,
        preferredIndex = 3,
    }
    return key
end

Confirm("WLCG_UNBIND",
    "Stop tracking %s?\n\nThe bot stays yours and keeps questing - it just stops following your quest log.",
    function(data) if data and data.name then G.Action("unbind", data.name) end end)

Confirm("WLCG_RELEASE",
    "Release %s for good?\n\nThe body is wiped, renamed and returned to the pool. You cannot get it back.",
    function(data) if data and data.name then G.Action("release", data.name) end end)

function G.ConfirmUnbind(name)
    local dialog = StaticPopup_Show("WLCG_UNBIND", name)
    if dialog then dialog.data = { name = name } end
end

function G.ConfirmRelease(name)
    local dialog = StaticPopup_Show("WLCG_RELEASE", name)
    if dialog then dialog.data = { name = name } end
end

-- ---------------------------------------------------------------------------
-- slash (for power users; nothing in the window needs it)
-- ---------------------------------------------------------------------------

SLASH_WLCOMPANIONGROUP1 = "/wlcg"
SLASH_WLCOMPANIONGROUP2 = "/companions"

SlashCmdList["WLCOMPANIONGROUP"] = function(input)
    local command = string.lower(string.match(input or "", "^(%S*)") or "")

    if command == "" or command == "wlcg" or command == "show" then
        G.Toggle()
    elseif command == "test" then
        G.SelfTest()
    elseif command == "quests" then
        G.Run(".companionsync quests")
    else
        DEFAULT_CHAT_FRAME:AddMessage("|cffffc94dWL Companion Group|r: click everything in /wlcg. "
            .. "Commands: /wlcg test for a parser check, /wlcg quests for the quest log view.")
    end
end

-- ---------------------------------------------------------------------------
-- self test
-- ---------------------------------------------------------------------------

function G.SelfTest()
    local rows = G.ParseCompPayload("Healy,5,1,32,4,ogs,110|Soak,9,2,32,2,o,105|Ghost,0,0,0,0,-,0")
    local stock = G.ParseStockPayload("1-5=12|2-9=0|3-2=4")

    local ok = #rows == 3 and rows[1].name == "Healy" and rows[3].name == "Ghost"
    ok = ok and G.HasFlag(rows[1], "g") and not G.HasFlag(rows[3], "o")
    ok = ok and rows[1].raceId == 1 and rows[1].level == 32 and rows[3].quests == 0
    ok = ok and stock["1-5"] == 12 and stock["2-9"] == 0 and stock["3-2"] == 4

    DEFAULT_CHAT_FRAME:AddMessage(("|cffffc94dCompanions|r self test: %s (%d rows, 3 stock entries)"):format(
        ok and "|cff20ff20pass|r" or "|cffff4040FAIL|r", #rows))

    return ok
end
