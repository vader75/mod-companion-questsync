-- CompanionSync/Core.lua
--
-- Data, names and commands. Addon replies arrive on the CSYNC system channel.
--
--   Comp sync 2: Healy,5,1,32,4,ogs,110|Soak,9,2,32,2,o,105|Nia,7,3,12,1,og,8,y=Skinning 50/150
--                name,class,race,level,quests,flags,free slots[,y=gather cur/max]
--
--   Pool stock 2: 1-5=12|3-2=9 ...
--                race-class=free bodies, so the picker can grey out what the pool
--                cannot serve instead of failing after the player has chosen.
-- Modern snapshots use Comp set / Pool set <revision> <part>/<total>:<raw slice>.
-- Sync capabilities 1 <heartbeat seconds> <pool enabled 0/1> describes snapshots only.
-- Delivery receipts 1 separately advertises confirmed inventory delivery.
--
-- Flags: o in world, g in your party, s same level as you, b behind (you carry a quest
-- it has not got).

local addonName, G = ...

-- The TOC is the only place the version is written down. Three hand-written copies had
-- already drifted (this line said 2.0.0, the toc 1.3.0, the login line 1.0.5), and the one
-- the client actually read was the toc - so that is the one to report.
local function TocVersion()
    local get = (C_AddOns and C_AddOns.GetAddOnMetadata) or GetAddOnMetadata

    if not get or not addonName then
        return "?"
    end

    return get(addonName, "Version") or "?"
end

G.version = TocVersion()
G.companions = {}
G.stock = {}            -- ["race-class"] = free bodies
G.releasing = {}

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
        -- Lua patterns have no optional groups: the y= field is a second, anchored
        -- match, and the plain seven-field form stands in when it does not fit.
        local name, classId, raceId, level, quests, flags, free, gather =
            string.match(entry, "^([^,]+),([^,]*),([^,]*),([^,]*),([^,]*),([^,]*),([^,]*),y=(.+)$")
        if not name then
            name, classId, raceId, level, quests, flags, free =
                string.match(entry, "^([^,]+),([^,]*),([^,]*),([^,]*),([^,]*),([^,]*),([^,]*)$")
        end

        if name and name ~= "" then
            local gatherName, gatherCur, gatherMax
            if gather then
                gatherName, gatherCur, gatherMax = string.match(gather, "^(.*%S)%s+(%d+)/(%d+)$")
            end
            rows[#rows + 1] = {
                name = name,
                classId = tonumber(classId) or 0,
                raceId = tonumber(raceId) or 0,
                level = tonumber(level) or 0,
                quests = tonumber(quests) or 0,
                flags = flags or "-",
                free = tonumber(free) or 0,
                gather = gatherName and (gatherName .. " " .. gatherCur .. "/" .. gatherMax) or nil,
            }
        end
    end

    -- Sticky ordering: cards re-sorted every poll could move under the mouse, so the
    -- Release button belonged to a different companion fifteen seconds later. A name
    -- keeps the slot it first held; only a NEW member takes a fresh slot.
    G.cardOrder = G.cardOrder or {}
    for _, row in ipairs(rows) do
        if not G.cardOrder[row.name] then
            local highest = 0
            for _, slot in pairs(G.cardOrder) do highest = math.max(highest, slot) end
            G.cardOrder[row.name] = highest + 1
        end
    end
    for name in pairs(G.cardOrder) do
        local still = false
        for _, row in ipairs(rows) do
            if row.name == name then still = true break end
        end
        if not still then G.cardOrder[name] = nil end
    end

    table.sort(rows, function(a, b)
        return (G.cardOrder[a.name] or 99) < (G.cardOrder[b.name] or 99)
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

local skillNames = {
    [164] = "Blacksmithing", [165] = "Leatherworking", [171] = "Alchemy",
    [182] = "Herbalism", [186] = "Mining", [393] = "Skinning",
}

G.professionSkills = {}

local function ParseSkillSnapshot(payload)
    local skills = {}
    for entry in string.gmatch(payload, "[^|]+") do
        local name, gather, gCur, gMax, craft, cCur, cMax =
            string.match(entry, "^([^,]+),(%d+),(%d+),(%d+),(%d+),(%d+),(%d+)$")
        if name and skillNames[tonumber(gather)] and skillNames[tonumber(craft)] then
            skills[name] = {
                gather = { name = skillNames[tonumber(gather)], current = tonumber(gCur), cap = tonumber(gMax) },
                craft = { name = skillNames[tonumber(craft)], current = tonumber(cCur), cap = tonumber(cMax) },
            }
        end
    end
    return skills
end

function G.SetCompanions(rows)
    G.companions = rows or {}
    G.lastRosterAt = time()
    local active = {}
    for _, row in ipairs(G.companions) do
        if G.HasFlag(row, "o") then active[row.name] = true end
    end
    for name in pairs(G.professionSkills) do
        if not active[name] then
            G.professionSkills[name] = nil
            G.lastSkillPayload = nil
        end
    end

    -- Removing a retired companion from the active roster does not return it to stock.
    local present = {}
    for _, row in ipairs(G.companions) do present[row.name] = true end
    for name in pairs(G.releasing) do
        if not present[name] then G.releasing[name] = nil end
    end

    if G.OnCompanionsChanged then G.OnCompanionsChanged() end
end

function G.SetStock(stock)
    G.stock = stock or {}
    G.lastStockAt = time()
    if G.OnStockChanged then G.OnStockChanged() end
end

-- ---------------------------------------------------------------------------
-- system protocol
-- ---------------------------------------------------------------------------

local listener = CreateFrame("Frame")
listener:RegisterEvent("CHAT_MSG_SYSTEM")

local function Clock() return GetTime and GetTime() or time() end

local function LegacyHeartbeat()
    local pass = G.serverInterval or 30
    return math.ceil(60 / pass) * pass  -- older servers only heartbeat on a sync pass
end

function G.ServerPatience() return (G.heartbeatInterval or LegacyHeartbeat()) + 15 end

function G.ServerAlive()
    return G.lastServerLineAt ~= nil and time() - G.lastServerLineAt <= G.ServerPatience()
end

function G.SnapshotState(kind)
    if kind == "pool" and G.poolEnabled == false then return "disabled" end
    local stamp = kind == "pool" and G.lastStockAt or kind == "roster" and G.lastRosterAt
    if not stamp then return "missing" end
    if not G.ServerAlive() or time() - stamp > G.ServerPatience() then return "stale" end
    return next(kind == "pool" and G.stock or G.companions) and "ready" or "empty"
end

local function ProtocolActivity()
    G.lastServerLineAt = time()
    if G.OnReadinessChanged then G.OnReadinessChanged() end
end

local SUPPLY_CAP = 64
local UINT32_MAX = 4294967295
local SUPPLY_TIMEOUT = 8
local SUPPLY_CATALOG_COOLDOWN, SUPPLY_ACTION_COOLDOWN = 2, 1
local lastSupplyNonce, lastSupplyCatalog, lastSupplyAction

local function NextNonce()
    lastSupplyNonce = (lastSupplyNonce or time() % 2147483647) % UINT32_MAX + 1
    return lastSupplyNonce
end

local DELIVERY_REPLY_TIMEOUT, DELIVERY_TRADE_TIMEOUT = 15, 120
local deliveryRank = { requested = 1, ready = 2, completed = 3, cancelled = 3, failed = 3, unconfirmed = 3 }

function G.DeliveryPending() return G.delivery ~= nil and G.delivery.deadline ~= nil end

local function DeliveryChanged()
    if G.OnDeliveryChanged then G.OnDeliveryChanged() end
end

local function ExpireDelivery(now)
    local order = G.delivery
    if order and order.deadline and now >= order.deadline then
        order.message = order.state == "ready" and "No final confirmation. Check your bags and the trade window."
            or "No delivery reply. Check chat and the trade window before retrying."
        order.state, order.deadline = "unconfirmed", nil
        DeliveryChanged()
    end
end

local function HandleDeliveryLine(text)
    ExpireDelivery(Clock())
    local nonce, state, message = text:match("^Delivery progress (%d+) (%a+):(.+)$")
    local order = G.delivery
    local rank = deliveryRank[state]
    if not (order and order.deadline and nonce == order.wireNonce and rank and rank > order.rank) then return end
    order.state, order.message, order.rank = state, message, rank
    if rank == 3 then order.deadline = nil
    elseif state == "ready" then order.deadline = Clock() + DELIVERY_TRADE_TIMEOUT end
    DeliveryChanged()
end

local function SupplyChanged()
    if G.OnSupplyChanged then G.OnSupplyChanged() end
end

function G.SetSupplyCompanion(name)
    if (G.supply and G.supply.name) ~= name then
        G.supply = name and { name = name } or nil
    end
end

function G.ExpireSupply()
    local supply = G.supply
    if supply and supply.pending and GetTime() >= supply.pending.deadline then
        local catalog = supply.pending.kind == "catalog"
        supply.pending = nil
        supply.message = catalog and "No supply catalog reply. The server may not support it; try refreshing."
            or "No delivery reply. Check for an open trade before trying again."
        SupplyChanged()
    end
end

local function HandleSupplyLine(text)
    G.ExpireSupply()
    local supply = G.supply
    local pending = supply and supply.pending
    if not pending then return end

    local nonce, result = text:match("^Supply result (%d+):(.+)$")
    if nonce then
        if tonumber(nonce) == pending.nonce then
            supply.pending, supply.message = nil, result
            SupplyChanged()
        end
        return
    end
    if pending.kind ~= "catalog" then return end

    local count
    nonce, count = text:match("^Supply end (%d+) (%d+)$")
    if nonce then
        if tonumber(nonce) ~= pending.nonce then return end
        count = tonumber(count)
        supply.pending = nil
        -- Only a contiguous, 1-based set publishes, including the empty set.
        if count <= SUPPLY_CAP and count == pending.count then
            for i = 1, count do
                if not pending.rows[i] then count = nil break end
            end
            if count then supply.rows = pending.rows end
        end
        if not supply.rows then supply.message = "Incomplete supply catalog. Try refreshing." end
        SupplyChanged()
        return
    end

    local index, itemId, kind, quantity, name
    nonce, index, itemId, kind, quantity, name = text:match("^Supply row (%d+) (%d+) (%d+) (%a+) (%d+):(.+)$")
    if tonumber(nonce) ~= pending.nonce then return end
    index, itemId, quantity = tonumber(index), tonumber(itemId), tonumber(quantity)
    if index < 1 or index > SUPPLY_CAP or pending.rows[index] or itemId < 1 or itemId > UINT32_MAX then return end
    if not (kind == "bag" and quantity > 0 and quantity <= UINT32_MAX
        or (kind == "craft" or kind == "missing") and quantity == 0) then return end

    pending.rows[index] = { index = index, itemId = itemId, kind = kind, quantity = quantity, name = name }
    pending.count = pending.count + 1
end

-- CSYNC uses system messages so it works even with no summoned companion, and
-- is hidden from the chat frame so the player only sees it in the window.
local function IsProtocolLine(text)
    return type(text) == "string" and string.sub(text, 1, 6) == "CSYNC|"
end

if ChatFrame_AddMessageEventFilter then
    -- The companions' AI narrates its quest menu at every giver the owner opens -
    -- "Quest Rewarded: ...", "Quest Available: ...", "Cannot talk to quest giver" at an
    -- innkeeper with no quests at all. Every fact in those lines already reaches the
    -- player through this module's own prefixed whispers and the payload; the narration
    -- is the echo that made one hand-in read as three events. The echo is filtered;
    -- the module's own lines all begin "Name: " and pass untouched.
    local narrationEchoes = {
        "Quest Rewarded: ", "Quest Available: ", "Quest Incompleted: ",
        "Already on ", "Quest completed ", "Cannot talk to quest giver",
    }

    -- The core announces a bot's own milestones - achievements, level-ups, duel wins -
    -- as ordinary system messages, and with companions in the party they read like
    -- noise aimed at the player. The roster is known here: a system line that begins
    -- with a companion's name is the world talking about a bot, not to the player,
    -- and it already reaches the window through the module's own payloads.
    local function IsAboutCompanion(message)
        local text = message or ""
        if not string.find(text, "has earned the achievement", 1, true) and
           not string.find(text, "is now level", 1, true) then
            return false
        end
        for _, companion in ipairs(G.companions) do
            local name = companion.name
            if name and name ~= "" then
                -- the core announces a bot's milestones as "[Name] has earned ..." or
                -- plain "Name ..." - either way the world talking about a bot
                if string.sub(text, 1, #name + 3) == "[" .. name .. "] " or
                   string.sub(text, 1, #name + 1) == name .. " " then
                    return true
                end
            end
        end
        return false
    end
    ChatFrame_AddMessageEventFilter("CHAT_MSG_SYSTEM", function(_, _, message)
        if IsProtocolLine(message) then return true end
        -- Pre-picker servers print usage instead of sending a catalog reply. Hide
        -- that exact legacy fallback only while a picker command could have caused it.
        if lastSupplyCatalog and GetTime() - lastSupplyCatalog < SUPPLY_TIMEOUT
            and message == "Usage: .companionsync delivery [preview|collect <name>|request <item ID>|clear]" then return true end
        if IsAboutCompanion(message) then return true end
        return false
    end)
    ChatFrame_AddMessageEventFilter("CHAT_MSG_WHISPER", function(_, _, message)
        for _, echo in ipairs(narrationEchoes) do
            if string.sub(message or "", 1, #echo) == echo then return true end
        end
        return false
    end)
end

local SNAPSHOT_PARTS, SNAPSHOT_BYTES, SNAPSHOT_TIMEOUT = 32, 192, 8
local streams = { Comp = {}, Pool = {} }

local function UInt(text, max)
    local n = text and tonumber(text)
    return n and n <= max and n or nil
end

local function ValidRows(label, payload, expected)
    if payload:sub(1, 1) == "|" or payload:sub(-1) == "|" or payload:find("||", 1, true) then return false end
    local count, seen = 0, {}
    for row in payload:gmatch("[^|]+") do
        local key
        if label == "Comp" then
            local name, class, race, level, quests, flags, free, suffix =
                row:match("^([^,%c]+),(%d+),(%d+),(%d+),(%d+),([a-z%-]+),(%d+)(.*)$")
            if not (name and UInt(class, 255) and UInt(race, 255) and UInt(level, 255)
                and UInt(quests, UINT32_MAX) and UInt(free, UINT32_MAX)
                and (suffix == "" or suffix:match("^,y=[^,%c]+ %d+/%d+$"))) then return false end
            key = name:lower()
        else
            local race, class, stock = row:match("^(%d+)%-(%d+)=(%d+)$")
            local r, c = UInt(race, 255), UInt(class, 255)
            if not (r and r > 0 and c and c > 0 and UInt(stock, UINT32_MAX)
                and race == tostring(r) and class == tostring(c)) then return false end
            key = race .. "-" .. class
        end
        if seen[key] then return false end
        seen[key], count = true, count + 1
    end
    return not expected or count == expected
end

local function Publish(label, payload, expected)
    if not ValidRows(label, payload, expected) then return end
    G.lastServerLineAt = time()
    if label == "Comp" then G.SetCompanions(G.ParseCompPayload(payload))
    else G.SetStock(G.ParseStockPayload(payload)) end
    if G.OnReadinessChanged then G.OnReadinessChanged() end
end

local function HandleSnapshot(label, text)
    local stream = streams[label]
    local now = Clock()
    if stream.pending and now >= stream.pending.deadline then stream.pending = nil end

    local rev, part, total, body = text:match("^set (%d+) (%d+)/(%d+):(.*)$")
    if rev then
        rev, part, total = UInt(rev, 9007199254740991), UInt(part, SNAPSHOT_PARTS), UInt(total, SNAPSHOT_PARTS)
        if not (rev and rev > 0 and part and part > 0 and total and part <= total) then return end
        if stream.revision and rev < stream.revision then return end
        if not stream.revision or rev > stream.revision then
            stream.revision = rev
            stream.pending = { parts = {}, total = total, count = 0, deadline = now + SNAPSHOT_TIMEOUT }
        end
        local pending = stream.pending
        if not pending then return end
        local previous = pending.parts[part]
        if total ~= pending.total or #body > SNAPSHOT_BYTES or (body == "" and total > 1)
            or (previous and previous ~= body) then
            stream.pending = nil  -- never retry a poisoned revision
        elseif not previous then
            pending.parts[part], pending.count = body, pending.count + 1
            if pending.count == total then
                stream.pending = nil
                Publish(label, table.concat(pending.parts))  -- modern chunks are byte slices
            end
        end
        return
    end

    -- Unversioned replies: require contiguous, in-order parts with a stable total.
    if stream.revision or G.snapshotV1 then return end
    local legacy = label == "Comp" and "sync" or "stock"
    part, total, body = text:match("^" .. legacy .. " (%d+)/(%d+):%s*(.*)$")
    if part then
        part, total = UInt(part, SNAPSHOT_PARTS), UInt(total, SNAPSHOT_PARTS)
        if not (part and part > 0 and total and part <= total and total > 1
            and #body > 0 and #body <= SNAPSHOT_BYTES) then stream.pending = nil return end
        if part == 1 then
            stream.pending = { parts = { body }, total = total, count = 1, deadline = now + SNAPSHOT_TIMEOUT }
            return
        end
        local pending = stream.pending
        if not pending or total ~= pending.total or part ~= pending.count + 1 then stream.pending = nil return end
        pending.parts[part], pending.count = body, part
        if part == total then
            stream.pending = nil
            Publish(label, table.concat(pending.parts, "|"))
        end
        return
    end
    local count
    count, body = text:match("^" .. legacy .. " (%d+):%s*(.*)$")
    stream.pending = nil
    count = UInt(count, SNAPSHOT_PARTS * SNAPSHOT_BYTES)
    if count and #body <= SNAPSHOT_BYTES then Publish(label, body, count) end
end

function G.ExpireRequests()
    local now = Clock()
    for _, stream in pairs(streams) do
        if stream.pending and now >= stream.pending.deadline then stream.pending = nil end
    end
    ExpireDelivery(now)
    local expired
    for name, deadline in pairs(G.releasing) do
        if now >= deadline then
            G.releasing[name] = nil
            expired = expired or {}
            expired[#expired + 1] = name
        end
    end
    if expired then
        table.sort(expired)
        if G.OnServerMessage then G.OnServerMessage("Release not confirmed: " .. table.concat(expired, ", ") .. ". Refresh before retrying.") end
        if G.OnCompanionsChanged then G.OnCompanionsChanged() end
    end
end

-- Keep timeouts running while the companion window is closed.
local elapsed = 0
listener:SetScript("OnUpdate", function(_, delta)
    elapsed = elapsed + delta
    if elapsed < 0.5 then return end
    elapsed = 0
    G.ExpireRequests()
    if G.OnReadinessChanged then G.OnReadinessChanged() end
end)

listener:SetScript("OnEvent", function(_, event, message)
    if event ~= "CHAT_MSG_SYSTEM" then return end
    if not IsProtocolLine(message) then
        local system = StripColors(message)
        local quarantine = system:match("^(.+) is quarantined for offline maintenance%. The body and name remain reserved until staff review%.$")
            or system:match("^(.+): logout observed%. The body stays reserved for offline maintenance%.$")
            or system:match("^(.+): logout was not confirmed before timeout%. The body stays reserved for offline maintenance%.$")
            or system:match("^(.+) has retired and its slot is free again%.")
        local plain = system:match("^Companion quest sync:%s*(.+)$") or quarantine and system
        if not plain then return end
        local name = plain:match("^(.+) could not be released %- ")
            or plain:match("^(.+) has gone back to the pool%.")
            or plain:match("^(.+) is not one of your companions%.$")
            or plain:match("^(.+) is retiring%.")
            or quarantine
        if name and G.releasing[name] then
            G.releasing[name] = nil
            if G.OnCompanionsChanged then G.OnCompanionsChanged() end
        end
        G.lastServerMessage = plain
        if G.OnServerMessage then G.OnServerMessage(plain) end
        return
    end
    local text = message:sub(7)
    local label, payload = text:match("^(%a+) (.*)$")
    if streams[label] then HandleSnapshot(label, payload) return end
    if label == "Supply" then HandleSupplyLine(StripColors(text)) return end
    if label == "Delivery" then
        if text == "Delivery receipts 1" then
            G.deliveryReceiptsV1 = true
        else
            HandleDeliveryLine(StripColors(text))
        end
        return
    end

    local hello = UInt(text:match("^Hello (%d+)$"), UINT32_MAX)
    if hello and hello > 0 then
        G.serverInterval = hello
        ProtocolActivity()
        return
    end
    local roles = text:match("^Roles (.*)$")
    if roles then
        G.Roles, G.RoleNotes, G.Engage = {}, {}, {}
        for pair in roles:gmatch("[^|]+") do
            local name, rest = pair:match("^(.-)=(.*)$")
            if name then
                -- name=role[ /policy][:na]; ":na" means this class cannot honour part of the
                -- role, and the card says so instead of pretending the role took
                local role, policy, flag = rest:match("^(%a+)/?(%a*)(.*)$")
                G.Roles[name] = role
                if policy ~= "" then G.Engage[name] = policy end
                if flag == ":na" then G.RoleNotes[name] = true end
            end
        end
        ProtocolActivity()
        if G.OnCompanionsChanged then G.OnCompanionsChanged() end
        return
    end
    local formation = text:match("^Formation (%a+)$")
    if formation then
        G.Formation = formation
        ProtocolActivity()
        if G.RefreshFormation then G.RefreshFormation() end
        return
    end
    local h, pool = text:match("^Sync capabilities 1 (%d+) ([01])$")
    h = UInt(h, UINT32_MAX)
    if h and h > 0 then
        if not G.snapshotV1 then
            for _, stream in pairs(streams) do
                if not stream.revision then stream.pending = nil end
            end
        end
        G.snapshotV1, G.heartbeatInterval, G.poolEnabled = true, h, pool == "1"
        ProtocolActivity()
        return
    end
    local skills = text:match("^Skill sync:%s*(.*)$")
    if skills then
        if skills ~= G.lastSkillPayload then
            G.lastSkillPayload = skills
            G.professionSkills = ParseSkillSnapshot(skills)
            if G.OnCompanionsChanged then G.OnCompanionsChanged() end
        end
        return
    end

end)

-- ---------------------------------------------------------------------------
-- commands
-- ---------------------------------------------------------------------------

function G.Run(command)
    if not command or command == "" then return false end

    -- A dot-command rides chat and is intercepted by the server. With the module absent
    -- there is nothing to intercept it, and the text would simply be said aloud to everyone
    -- nearby - so stay quiet while the module has not spoken inside its patience window.
    if not G.ServerAlive() then
        if G.OnServerMessage then
            G.OnServerMessage("mod-companion-questsync is not answering - is it enabled?")
        end
        return false
    end

    SendChatMessage(command, "SAY")
    return true
end

function G.Action(verb, name)
    if verb == "release" then
        G.ExpireRequests()
        if not name or name == "" or G.releasing[name] or G.SnapshotState("roster") ~= "ready"
            or not G.NameTaken(name) then return false end
        if not G.Run(".companionsync release " .. name) then return false end
        G.releasing[name] = Clock() + 25
        if G.OnCompanionsChanged then G.OnCompanionsChanged() end
        return true
    end
    return G.Run(".companionsync " .. verb .. (name and name ~= "" and " " .. name or ""))
end

local function StartDelivery(name, nonce)
    G.delivery = {
        name = name, rank = 0, wireNonce = ("%.0f"):format(nonce), deadline = Clock() + DELIVERY_REPLY_TIMEOUT,
        state = "waiting", message = "Request sent; waiting for a delivery reply…",
    }
    DeliveryChanged()
end

function G.CollectDelivery(name)
    ExpireDelivery(Clock())
    if not name or name == "" or G.DeliveryPending() then return false end
    local command = ".companionsync delivery collect " .. name
    if G.deliveryReceiptsV1 then
        local nonce = NextNonce()
        if not G.Run(command .. (" %.0f"):format(nonce)) then return false end
        StartDelivery(name, nonce)
    else
        if not G.Run(command) then return false end
        G.delivery = { name = name, state = "legacy", message = "See chat and the trade window for the result (legacy server)." }
        DeliveryChanged()
    end
    return true
end

local function RequestSupply(kind, row)
    local supply = G.supply
    if not supply or supply.pending then return false end

    if not G.ServerAlive() then
        supply.message = "The server isn't answering."
        SupplyChanged()
        return false
    end
    local now = GetTime()
    -- Match the separate server buckets: opening a catalog must not delay
    -- the first item action, but switching cards must not bypass either limit.
    local catalog = kind == "catalog"
    ExpireDelivery(now)
    if not catalog and G.DeliveryPending() then return false end
    local last, gap = lastSupplyAction, SUPPLY_ACTION_COOLDOWN
    if catalog then last, gap = lastSupplyCatalog, SUPPLY_CATALOG_COOLDOWN end
    if last and now - last < gap then
        supply.message = "Wait a moment, then try again."
        SupplyChanged()
        return false
    end

    -- Lua doubles hold all uint32 values exactly. Use decimal text for the wire.
    local nonce = NextNonce()
    local args = kind .. (" %.0f"):format(nonce)
    if row then args = args .. (" %.0f"):format(row.itemId) end
    if not G.Action("delivery " .. args, supply.name) then return false end
    if catalog then lastSupplyCatalog = now else lastSupplyAction = now end
    supply.rows, supply.message = nil, nil
    if not catalog and G.deliveryReceiptsV1 then
        StartDelivery(supply.name, nonce)
    else
        local pending = { nonce = nonce, kind = kind, deadline = now + SUPPLY_TIMEOUT }
        if catalog then pending.rows, pending.count = {}, 0 end
        supply.pending = pending
    end
    SupplyChanged()
    return true
end

function G.RequestSupplyCatalog()
    return RequestSupply("catalog")
end

function G.RequestSupplyItem(row)
    local supply = G.supply
    if not (row and supply and supply.rows and supply.rows[row.index] == row) then return false end
    if row.kind == "bag" then return RequestSupply("take", row) end
    if row.kind == "craft" then return RequestSupply("make", row) end
    return false
end

--- Ask the server for a companion.
---
--- The panel does NOT refuse on an empty stock table. Stock arrives asynchronously, and a
--- click that is swallowed locally because a payload was late looks exactly like a broken
--- button - which is what it was. The server is the only authority on what the pool has, and
--- it answers with a reason that is shown in chat and in the panel.
function G.CreateCompanion(raceId, classId, name)
    name = (name or ""):gsub("^%s+", ""):gsub("%s+$", "")

    if not G.ValidName(name) then
        G.Warn("a name is 2 to 12 letters, nothing else")
        return false
    end

    if G.poolEnabled == false then return false end
    return G.Run(".companionsync create " .. raceId .. " " .. classId .. " " .. name)
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

Confirm("CSYNC_UNBIND",
    "Stop tracking %s?\n\nThe bot stays yours and keeps questing - it just stops following your quest log.",
    function(data) if data and data.name then G.Action("unbind", data.name) end end)

Confirm("CSYNC_RELEASE",
    "Retire %s?\n\nIt leaves your roster and its slot frees once it has left the world.",
    function(data) if data and data.name then G.Action("release", data.name) end end)

function G.ConfirmUnbind(name)
    local dialog = StaticPopup_Show("CSYNC_UNBIND", name)
    if dialog then dialog.data = { name = name } end
end

function G.ConfirmRelease(name)
    local dialog = StaticPopup_Show("CSYNC_RELEASE", name)
    if dialog then dialog.data = { name = name } end
end

-- ---------------------------------------------------------------------------
-- slash (for power users; nothing in the window needs it)
-- ---------------------------------------------------------------------------

-- /csync is the name; /wlcg and /companions stay registered because hands remember them
SLASH_COMPANIONSYNC1 = "/csync"
SLASH_COMPANIONSYNC2 = "/wlcg"
SLASH_COMPANIONSYNC3 = "/companions"

SlashCmdList["COMPANIONSYNC"] = function(input)
    local command = string.lower(string.match(input or "", "^(%S*)") or "")

    if command == "" or command == "wlcg" or command == "show" then
        G.Toggle()
    elseif command == "test" then
        G.SelfTest()
    elseif command == "quests" then
        G.Run(".companionsync quests")
    else
        DEFAULT_CHAT_FRAME:AddMessage("|cffffc94dCompanion Sync|r: click everything in /csync. "
            .. "Commands: /csync test for a parser check, /csync quests for the quest log view.")
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
