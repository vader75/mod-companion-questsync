"""Exercise the selected companion and scoped buttons with the real addon Lua."""
import unittest

from lupa import LuaRuntime
from lua_sandbox import ADDON, STUB


class CompanionLayout(unittest.TestCase):
    def setUp(self):
        self.lua = LuaRuntime(unpack_returned_tuples=True)
        self.lua.execute(STUB)
        self.lua.execute('''
            clock = 100
            time = function() return clock end
            ToggleWorldMap = function() end
            GameTooltip.SetOwner = function(self, owner) self.owner, self.lines = owner, {} end
            GameTooltip.IsOwned = function(self, owner) return rawget(self, 'owner') == owner end
            GameTooltip.AddLine = function(self, text) self.lines[#self.lines+1] = text end
            GameTooltip.Hide = function(self) self.owner, self.lines = nil, {} end
            function TooltipText() return table.concat(rawget(GameTooltip, 'lines') or {}, '\\n') end
            local create = CreateFrame
            frames, requests = {}, {}
            CreateFrame = function(kind, name, parent, template)
                local f = create(kind, name, parent, template)
                f.kind, f.parent, f.shown, f.enabled = kind, parent, true, true
                f.SetText = function(self, text) self.text = text end
                local fontString = f.CreateFontString
                f.CreateFontString = function(self, ...)
                    local label = fontString(self, ...)
                    self.labels = rawget(self, 'labels') or {}
                    self.labels[#self.labels+1] = label
                    label.SetText = function(self, text) self.text = text end
                    label.Show = function(self) self.shown = true end
                    label.Hide = function(self) self.shown = false end
                    label.IsShown = function(self) return rawget(self, 'shown') ~= false end
                    return label
                end
                f.Show = function(self) self.shown = true end
                f.Hide = function(self) self.shown = false end
                f.IsShown = function(self) return self.shown end
                f.SetEnabled = function(self, enabled) self.enabled = enabled end
                f.Enable = function(self) self.enabled = true end
                f.Disable = function(self) self.enabled = false end
                f.SetScript = function(self, event, fn)
                    _FRAME_SCRIPTS[event] = fn
                    self[event] = fn
                end
                frames[#frames+1] = f
                return f
            end
            function VisibleButton(text)
                for _, f in ipairs(frames) do
                    if f.kind == 'Button' and rawget(f, 'text') == text then
                        local parent = f
                        while type(parent) == 'table' and rawget(parent, 'shown') do
                            parent = rawget(parent, 'parent')
                        end
                        if type(parent) ~= 'table' or rawget(parent, 'shown') ~= false then
                            return f
                        end
                    end
                end
            end
            function VisibleLabel(text)
                for _, f in ipairs(frames) do
                    local parent = f
                    while type(parent) == 'table' and rawget(parent, 'shown') do
                        parent = rawget(parent, 'parent')
                    end
                    if type(parent) ~= 'table' or rawget(parent, 'shown') ~= false then
                        for _, label in ipairs(rawget(f, 'labels') or {}) do
                            local value = rawget(label, 'text')
                            if rawget(label, 'shown') ~= false and value and value:find(text, 1, true) then
                                return f
                            end
                        end
                    end
                end
                return false
            end
        ''')
        self.g = self.lua.table()
        globals_ = self.lua.globals()
        self.lua.execute((ADDON / 'Core.lua').read_text(encoding='utf-8'), 'CompanionSync', self.g)
        self.send = self.lua.eval('_FRAME_SCRIPTS.OnEvent')
        self.lua.execute((ADDON / 'UI.lua').read_text(encoding='utf-8'), 'CompanionSync', self.g)
        self.g.Run = self.lua.eval('function(command) requests[#requests+1] = command end')
        self.visible = globals_.VisibleButton
        self.requests = globals_.requests
        self.g.Toggle()

    def roster(self, text):
        self.send(None, 'CHAT_MSG_SYSTEM', 'CSYNC|Comp sync ' + text)
        self.g.Refresh()

    def click(self, label):
        button = self.visible(label)
        self.assertIsNotNone(button, label)
        self.assertTrue(button.enabled, label)
        button.OnClick(button)

    def test_action_follows_the_selected_name_even_when_roster_reorders(self):
        self.roster('2: Mirdan,7,11,14,8,ogs,10|Drodan,5,1,14,1,ogs,8')
        self.click('Drodan')
        self.click('Collect delivery')
        self.assertEqual(self.requests[len(self.requests)], '.companionsync delivery collect Drodan')
        self.roster('2: Drodan,5,1,14,1,ogs,8|Mirdan,7,11,14,8,ogs,10')
        self.click('Train')
        self.assertEqual(self.requests[len(self.requests)], '.companionsync learn Drodan')
        self.roster('1: Mirdan,7,11,14,8,ogs,10')
        self.click('Collect delivery')
        self.assertEqual(self.requests[len(self.requests)], '.companionsync delivery collect Mirdan')

    def test_dismissed_companion_can_be_summoned_but_cannot_deliver(self):
        self.roster('2: Mirdan,7,11,14,8,ogs,10|Drodan,0,0,0,0,-,0')
        self.click('Drodan')
        self.assertFalse(self.visible('Collect delivery').enabled)
        self.click('Summon')
        self.assertEqual(self.requests[len(self.requests)], '.companionsync summon Drodan')

    def test_delivery_accepts_the_existing_bound_name(self):
        self.roster('1: Élise,5,1,14,1,ogs,8')
        self.click('Collect delivery')
        self.assertEqual(self.requests[len(self.requests)], '.companionsync delivery collect Élise')

    def test_low_bags_suggest_a_merchant_stop_until_space_recovers(self):
        visible = self.lua.globals().VisibleLabel
        self.roster('1: Mirdan,7,11,14,8,ogs,8')
        self.assertFalse(visible('Stop by a merchant'))
        count = len(self.requests)
        self.roster('1: Mirdan,7,11,14,8,ogs,7')
        self.assertTrue(visible('7 bag slots free'))
        self.assertTrue(visible('Stop by a merchant'))
        self.assertEqual(len(self.requests), count)
        self.roster('1: Mirdan,7,11,14,8,ogs,12')
        self.assertFalse(visible('Stop by a merchant'))

    def test_merchant_cue_does_not_use_stale_or_unavailable_companions(self):
        visible = self.lua.globals().VisibleLabel
        self.roster('1: Mirdan,7,11,14,8,ogs,0')
        self.assertTrue(visible('Bags full'))
        self.assertTrue(visible('Stop by a merchant'))
        self.lua.globals().clock = 10000
        self.g.Refresh()
        self.assertFalse(visible('Stop by a merchant'))
        for flags in ('ogsd', 'os', '-'):
            self.roster(f'1: Mirdan,7,11,14,8,{flags},0')
            self.assertFalse(visible('Stop by a merchant'), flags)

    def test_open_tooltip_drops_merchant_advice_when_bag_space_recovers(self):
        self.roster('1: Mirdan,7,11,14,8,ogs,0')
        globals_ = self.lua.globals()
        card = globals_.VisibleLabel('Stop by a merchant')
        card.OnEnter(card)
        self.assertIn('Stop beside a merchant', globals_.TooltipText())
        self.roster('1: Mirdan,7,11,14,8,ogs,12')
        self.assertNotIn('Stop beside a merchant', globals_.TooltipText())


if __name__ == '__main__':
    unittest.main()
