"""Replay real CSYNC system lines and click the real picker on Lua 5.1."""
import re
import unittest

from lupa.lua51 import LuaRuntime
from lua_sandbox import ADDON, CHAT_SINK, STUB


FRAMES = r'''
clock = 100
GetTime = function() return clock end
time = GetTime
frames, requests, filters = {}, {}, {}
ChatFrame_AddMessageEventFilter = function(event, fn) filters[event] = fn end
SendChatMessage = function(text) requests[#requests + 1] = text end
local function Visible(self)
    local node = self
    while type(node) == 'table' do
        if rawget(node, 'shown') == false then return false end
        node = rawget(node, 'parent')
    end
    return true
end
local create = CreateFrame
CreateFrame = function(kind, name, parent, template)
    local f = create(kind, name, parent, template)
    f.kind, f.parent, f.shown, f.enabled = kind, parent, true, true
    f.SetText = function(self, text) self.text = text end
    local function SetShown(self, shown)
        if self.shown == shown then return end
        self.shown = shown
        local callback = rawget(self, shown and 'OnShow' or 'OnHide')
        if callback then callback(self) end
    end
    f.Show = function(self) SetShown(self, true) end
    f.Hide = function(self) SetShown(self, false) end
    f.IsShown = function(self) return self.shown end
    f.IsVisible = Visible
    f.Enable = function(self) self.enabled = true end
    f.Disable = function(self) self.enabled = false end
    f.SetScript = function(self, event, fn) self[event] = fn end
    f.CreateFontString = function(self) return CreateFrame('FontString', nil, self) end
    frames[#frames + 1] = f
    return f
end
function VisibleWithText(kind, text, prefix)
    for _, f in ipairs(frames) do
        local label = rawget(f, 'text')
        if f.kind == kind and label and Visible(f) and
            (label == text or (prefix and label:find(text, 1, true))) then return f end
    end
end
'''


class SupplyPicker(unittest.TestCase):
    def setUp(self):
        self.lua = LuaRuntime(unpack_returned_tuples=True)
        self.assertEqual(self.lua.eval('_VERSION'), 'Lua 5.1')
        self.lua.execute(STUB.replace('unpack = table.unpack', ''))
        self.lua.execute(CHAT_SINK)
        self.lua.execute(FRAMES)
        self.g = self.lua.table()
        self.env = self.lua.globals()
        self.lua.execute((ADDON / 'Core.lua').read_text(encoding='utf-8'), 'CompanionSync', self.g)
        self.send = self.env.frames[1].OnEvent
        self.lua.execute((ADDON / 'UI.lua').read_text(encoding='utf-8'), 'CompanionSync', self.g)
        self.deliver('Comp sync 2: Élise,5,1,14,1,ogs,8|Drodan,5,1,14,1,ogs,8')
        self.g.Toggle()

    def deliver(self, text, event='CHAT_MSG_SYSTEM'):
        self.send(None, event, 'CSYNC|' + text)

    def button(self, text, partial=False):
        return self.env.VisibleWithText('Button', text, partial)

    def status(self, text):
        return self.env.VisibleWithText('FontString', text, True) is not None

    def click(self, text, partial=False):
        button = self.button(text, partial)
        self.assertIsNotNone(button, text)
        self.assertTrue(button.enabled, text)
        button.OnClick(button)

    def advance(self, seconds):
        self.env.clock += seconds
        frame = self.env.CompanionSyncFrame
        frame.OnUpdate(frame, seconds)

    def request(self, verb, name='Élise', item=None):
        command = self.env.requests[len(self.env.requests)]
        suffix = (' ' + str(item) if item is not None else '') + ' ' + name
        match = re.fullmatch(r'\.companionsync delivery ' + verb + r' ([0-9]+)' + re.escape(suffix), command or '')
        self.assertIsNotNone(match, command)
        nonce = int(match[1])
        self.assertTrue(0 < nonce <= 0xffffffff, nonce)
        return nonce

    def catalog(self, name='Élise'):
        self.click('Choose supply', partial=True)
        return self.request('catalog', name)

    def test_completed_catalog_offers_bag_item_and_scopes_result_inline(self):
        catalog = self.catalog()
        line = f'Supply row {catalog} 1 929 bag 4:Healing Potion'
        self.deliver(line)
        self.assertIsNone(self.button('Healing Potion', partial=True))
        self.deliver(f'Supply end {catalog} 1')
        self.click('Healing Potion', partial=True)
        self.advance(2)
        self.click('Take supply')
        action = self.request('take', item=929)
        self.assertNotEqual(catalog, action)
        self.deliver(f'Supply result {action}:Trade opened: accept when ready.')
        self.assertTrue(self.status('Trade opened: accept when ready.'))
        self.assertTrue(self.env.filters['CHAT_MSG_SYSTEM'](None, None, 'CSYNC|' + line))
        self.assertEqual(len(self.env.MESSAGES), 0)

    def test_craft_uses_a_supported_recipe_but_not_an_unready_recipe(self):
        catalog = self.catalog()
        self.deliver(f'Supply row {catalog} 2 3385 missing 0:Lesser Mana Potion')
        self.deliver(f'Supply row {catalog} 1 118 craft 0:Potion de soins: mineure')
        self.deliver(f'Supply row {catalog} 1 999 craft 0:Duplicate index')
        self.deliver(f'Supply end {catalog} 2')
        self.assertFalse(self.button('Lesser Mana Potion  ·  Not ready').enabled)
        self.assertIsNone(self.button('Duplicate index', partial=True))
        self.assertFalse(self.button('Take / Craft').enabled)
        self.click('Potion de soins: mineure  ·  Can craft')
        self.click('Craft supply')
        self.request('make', item=118)
        self.assertFalse(self.button('Refresh supplies').enabled)
        self.assertEqual(len(self.env.requests), 2)

    def test_only_current_system_reply_can_complete_the_catalog(self):
        old = self.catalog()
        self.click('Drodan')
        self.advance(2)
        current = self.catalog('Drodan')
        self.deliver(f'Supply result {old}:Old reply')
        self.deliver(f'Supply row {old} 1 928 bag 5:Old item')
        self.deliver(f'Supply end {old} 1')
        self.deliver(f'Supply result {current}:Spoofed', 'CHAT_MSG_WHISPER')
        self.deliver(f'Supply row {current} 1 929 bag 4:Spoofed item', 'CHAT_MSG_WHISPER')
        self.deliver(f'Supply end {current} 0', 'CHAT_MSG_WHISPER')
        self.assertTrue(self.status('Loading supplies'))
        self.assertIsNone(self.button('Old item', partial=True))
        self.deliver(f'Supply end {current} 0')
        self.assertTrue(self.status('No offerable supplies or supported recipes.'))
        self.assertIsNone(self.button('Spoofed item', partial=True))

    def test_matching_catalog_error_exits_loading(self):
        catalog = self.catalog()
        self.deliver(f'Supply result {catalog}:The companion is busy.')
        self.assertTrue(self.status('The companion is busy.'))
        self.assertTrue(self.button('Refresh supplies').enabled)
        self.deliver(f'Supply end {catalog} 0')
        self.assertTrue(self.status('The companion is busy.'))

    def test_incomplete_or_overlimit_catalog_cannot_publish_rows(self):
        catalog = self.catalog()
        self.deliver(f'Supply row {catalog} 2 929 bag 4:Out of range')
        self.deliver(f'Supply end {catalog} 1')
        self.assertTrue(self.status('Incomplete supply catalog'))
        self.deliver(f'Supply row {catalog} 1 929 bag 4:Too late')
        self.deliver(f'Supply end {catalog} 2')
        self.assertTrue(self.status('Incomplete supply catalog'))
        self.advance(2)
        self.click('Refresh supplies')
        catalog = self.request('catalog')
        for i in range(1, 66):
            self.deliver(f'Supply row {catalog} {i} {1000 + i} bag 1:Item {i:02}')
        self.deliver(f'Supply end {catalog} 65')
        self.assertTrue(self.status('Incomplete supply catalog'))
        self.assertFalse(self.button('Take / Craft').enabled)

    def test_maximum_size_catalog_can_be_browsed_to_its_last_row(self):
        catalog = self.catalog()
        for i in range(1, 65):
            self.deliver(f'Supply row {catalog} {i} {1000 + i} bag 1:Item {i:02}')
        self.deliver(f'Supply end {catalog} 64')
        self.assertIsNone(self.button('Item 64', partial=True))
        for _ in range(7):
            self.click('Next')
        self.assertIsNone(self.button('Item 01', partial=True))
        self.assertFalse(self.button('Next').enabled)
        self.click('Item 64  ·  1 ready')
        self.advance(2)
        self.click('Take supply')
        self.request('take', item=1064)

    def test_old_server_usage_is_hidden_and_catalog_expires(self):
        nonce = self.catalog()
        usage = 'Usage: .companionsync delivery [preview|collect <name>|request <item ID>|clear]'
        self.assertTrue(self.env.filters['CHAT_MSG_SYSTEM'](None, None, usage))
        self.send(None, 'CHAT_MSG_SYSTEM', usage)
        self.advance(9)
        self.assertFalse(self.env.filters['CHAT_MSG_SYSTEM'](None, None, usage))
        self.assertTrue(self.status('No supply catalog reply'))
        self.deliver(f'Supply end {nonce} 0')
        self.assertTrue(self.status('No supply catalog reply'))
        self.assertTrue(self.button('Refresh supplies').enabled)

    def test_action_reply_is_dropped_after_roster_switch_and_return(self):
        nonce = self.catalog()
        self.deliver(f'Supply row {nonce} 1 929 bag 4:Healing Potion')
        self.deliver(f'Supply end {nonce} 1')
        self.click('Healing Potion', partial=True)
        self.advance(2)
        self.click('Take supply')
        action = self.request('take', item=929)
        self.click('Drodan')
        self.click('Élise')
        self.deliver(f'Supply result {action}:Late trade reply.')
        self.assertFalse(self.status('Late trade reply.'))
        self.assertIsNone(self.button('Take supply'))

    def test_catalog_replies_do_not_reopen_the_popup_after_window_close(self):
        nonce = self.catalog()
        self.g.Toggle()
        self.deliver(f'Supply row {nonce} 1 929 bag 4:Healing Potion')
        self.deliver(f'Supply end {nonce} 1')
        self.g.Toggle()
        self.assertIsNone(self.button('Healing Potion', partial=True))
        self.advance(2)
        self.assertNotEqual(nonce, self.catalog())

    def test_rapid_catalog_clicks_do_not_send_a_burst_across_companions(self):
        self.catalog()
        self.assertFalse(self.button('Choose supply', partial=True).enabled)
        self.click('Drodan')
        self.click('Choose supply', partial=True)
        self.assertEqual(len(self.env.requests), 1)
        self.advance(1.5)
        self.click('Choose supply', partial=True)
        self.assertEqual(len(self.env.requests), 1)
        self.advance(0.5)
        self.catalog('Drodan')
        self.assertEqual(len(self.env.requests), 2)

    def test_dismissal_clears_the_visible_popup_and_disables_supply_actions(self):
        nonce = self.catalog()
        self.deliver(f'Supply row {nonce} 1 929 bag 4:Healing Potion')
        self.deliver(f'Supply end {nonce} 1')
        self.assertIsNotNone(self.button('Healing Potion', partial=True))
        self.deliver('Comp sync 2: Élise,5,1,14,1,-,0|Drodan,5,1,14,1,ogs,8')
        self.assertIsNone(self.button('Healing Potion', partial=True))
        self.assertFalse(self.button('Choose supply', partial=True).enabled)
        self.assertFalse(self.button('Collect delivery').enabled)


if __name__ == '__main__':
    unittest.main()
