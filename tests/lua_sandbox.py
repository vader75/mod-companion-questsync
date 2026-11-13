"""Run the addon's Lua outside the client, against a stub WoW API.

The client hides addon errors well enough that "the button is not there" cost hours.
This loads Core.lua then UI.lua exactly as the client does - same `...` arguments, same
shared G table - and reports the first runtime error with its line number.

    python tests/lua_sandbox.py
"""
from __future__ import annotations

import sys
from pathlib import Path

from lupa import LuaRuntime

ADDON = Path(__file__).resolve().parent.parent / "addon" / "CompanionSync"

STUB = r"""
local function make_stub()
    local stub = {}
    local mt = {
        __index = function(t, k)
            if k == "IsShown" then return function() return false end end
            return function(...) return make_stub() end
        end,
        __newindex = function(t, k, v) rawset(t, k, v) end,
        __call = function(self, ...) return make_stub() end,
        __tostring = function() return "stub" end,
    }
    setmetatable(stub, mt)
    return stub
end

unpack = table.unpack
UIParent = make_stub()
Minimap = make_stub()
DEFAULT_CHAT_FRAME = make_stub()
GameTooltip = make_stub()
SlashCmdList = {}
StaticPopupDialogs = {}
StaticPopup_Show = function() return make_stub() end
UIParent.GetName = function() return "UIParent" end
Minimap.GetWidth = function() return 140 end
Minimap.GetHeight = function() return 140 end
GameFontNormal = make_stub()
GameFontNormalSmall = make_stub()
GameFontHighlightSmall = make_stub()
UIDropDownMenu_SetWidth = function() end
UIDropDownMenu_Initialize = function() end
UIDropDownMenu_AddButton = function() end
UIDropDownMenu_SetText = function() end
CreateFrame = function(kind, name, parent, template)
    local frame = make_stub()
    if name then _G[name] = frame end   -- the client publishes named frames as globals
    return frame
end
GetLocale = function() return "enUS" end
UnitName = function() return "Pilot" end
UnitClass = function() return "Paladin", "PALADIN" end
GetRealmName = function() return "WoW Legends - Local" end
"""

CHAT_SINK = r"""
MESSAGES = {}
local real = DEFAULT_CHAT_FRAME
DEFAULT_CHAT_FRAME = {
    AddMessage = function(_, text) MESSAGES[#MESSAGES + 1] = tostring(text) end,
}
"""


def main() -> int:
    addon_name = "CompanionSync"
    lua = LuaRuntime(unpack_returned_tuples=True)
    lua.execute(STUB)
    lua.execute(CHAT_SINK)

    # the shared addon table every file in the TOC receives as the second vararg
    g = lua.table()
    lua.globals().G_SHARED = g

    for name in ("Core.lua", "UI.lua"):
        source = (ADDON / name).read_text(encoding="utf-8")
        try:
            lua.execute(source, addon_name, g)
            print(f"PASS  {name} loaded")
        except Exception as exc:  # noqa: BLE001 - any Lua error is the point
            print(f"FAIL  {name}")
            print(f"      {exc}")
            return 1

    # what the file registered, measured rather than assumed
    report = lua.eval("""function()
        local out = {}
        out.launcher = CompanionSyncLauncher and "created" or "nil"
        out.slash = SlashCmdList["COMPANIONSYNC"] and "registered" or "missing"
        out.toggle = (G_SHARED and G_SHARED.Toggle) and "defined" or "missing"
        out.messages = #MESSAGES
        return out
    end""")
    r = report()
    print(f"launcher={r['launcher']} slash={r['slash']} toggle={r['toggle']} chat_lines={r['messages']}")
    if r["launcher"] == "created" and r["slash"] == "registered":
        print("PASS  the addon would build its button and answer /csync")
        return 0
    print("FAIL  the addon loads but does not finish registering")
    return 1


if __name__ == "__main__":
    sys.exit(main())
