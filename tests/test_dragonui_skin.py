"""Exercise both CompanionSync skin paths in WoW's Lua version."""
import unittest

from lupa.lua51 import LuaRuntime
from lua_sandbox import ADDON, STUB


class DragonUISkin(unittest.TestCase):
    def load(self, dragon=''):
        lua = LuaRuntime(unpack_returned_tuples=True)
        lua.execute('local unpack51 = unpack\n' + STUB + '\nunpack = unpack51')
        lua.execute('time = function() return 100 end')
        g = lua.table()
        for file in ('Core.lua', 'UI.lua'):
            lua.execute((ADDON / file).read_text(encoding='utf-8'), 'CompanionSync', g)
        lua.execute(dragon)  # Also cover DragonUI finishing load after these scripts.
        g.Toggle()
        return lua

    def test_uses_installed_dragon_chrome_for_window_actions_and_picker(self):
        lua = self.load('''
            calls = { primary = 0 }
            DragonUI = { ForeverUI = {
                ApplyNineSlice = function(frame, layout) calls.border = layout end,
                SkinButton = function(button) calls.primary = calls.primary + 1 end,
                CreateCloseButton = function(parent) calls.close = true; return CreateFrame('Button', nil, parent) end,
                SkinMenuBackground = function(frame, style) calls.menu = style end,
            } }
        ''')
        calls = lua.globals().calls
        self.assertEqual(calls.border, 'metalframe')
        self.assertTrue(calls.close)
        self.assertEqual(calls.menu, 'bg')
        self.assertGreater(calls.primary, 0)

    def test_standalone_and_older_dragonui_still_build(self):
        for dragon in ('', 'DragonUI = { ForeverUI = {} }'):
            with self.subTest(dragon=dragon):
                lua = self.load(dragon)
                self.assertIsNotNone(lua.globals().CompanionSyncFrame)
                self.assertIsNotNone(lua.globals().CompanionSyncLauncher)
