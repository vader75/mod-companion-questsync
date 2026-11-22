"""Exercise CompanionPins wire and map data against a stub 3.3.5 client."""
import unittest
from pathlib import Path

from lupa.lua51 import LuaRuntime
from lua_sandbox import STUB

PINS = Path(__file__).resolve().parents[1] / 'addon/CompanionPins/CompanionPins.lua'


class CompanionPinsContract(unittest.TestCase):
    def setUp(self):
        self.lua = LuaRuntime(unpack_returned_tuples=True)
        self.lua.execute(STUB)
        self.lua.execute('''
            now = 100
            time = function() return now end
            local create = CreateFrame
            frames = {}
            requests = {}
            WorldMapFrame = { IsShown = function() return false end }
            SendChatMessage = function(command) requests[#requests + 1] = command end
            CreateFrame = function(...)
                local frame = create(...)
                frames[#frames + 1] = frame
                frame.SetScript = function(self, event, handler) self['script_' .. event] = handler end
                return frame
            end
        ''')
        self.lua.execute(PINS.read_text(encoding='utf-8'))
        self.frame = self.lua.globals().frames[1]

    def send(self, payload, event='CHAT_MSG_SYSTEM'):
        self.frame.script_OnEvent(self.frame, event, payload)

    def test_torn_or_old_snapshot_does_not_replace_complete_pins(self):
        self.send('CSYNC|Pin set 10 1/1:48,52,h=Old@Mirdan,z=Westfall')
        self.assertEqual(self.frame.pins[1].quest, 'Old')
        self.send('CSYNC|Pin set 11 1/2:50,56,g=Unfinished')
        self.send('CSYNC|Pin set 12 2/2:iryn:2/8,z=Westfall')
        self.assertEqual(self.frame.pins[1].quest, 'Old')
        self.send('CSYNC|Pin set 12 1/2:50,56,g=New@Il')
        self.assertEqual(self.frame.pins[1].quest, 'New')
        self.send('CSYNC|Pin set 11 2/2:@Gordan,z=Westfall')
        self.assertEqual(self.frame.pins[1].quest, 'New')
        self.send('CSYNC|Pin set 13 1/1:')
        self.assertEqual(len(self.frame.pins), 0)

    def test_only_system_snapshots_change_pins(self):
        self.send('CSYNC|Pin sync 2: 42,50,h=Finished Quest@Mirdan,z=Westfall|45,51,g=Other@Ilyrin,z=Westfall')
        self.send("Finished 'Finished Quest' - that pin can go.", 'CHAT_MSG_WHISPER')
        self.send('CSYNC|Pin set 99 1/1:', 'CHAT_MSG_WHISPER')
        self.assertEqual(len(self.frame.pins), 2)

    def test_shared_pin_keeps_quest_owner_progress_associations(self):
        self.send('CSYNC|Pin sync 1: 45,51,g=Stew@Mirdan:1/3 + Stew@Ilyrin:2/3 + Other@Ilyrin:4/7,z=Westfall')
        rows = self.frame.pins[1].details
        self.assertEqual([(rows[i].quest, rows[i].companion, rows[i].owed) for i in range(1, 4)],
                         [('Stew', 'Mirdan', '1/3'), ('Stew', 'Ilyrin', '2/3'), ('Other', 'Ilyrin', '4/7')])
        self.frame.only = 'ilyrin'
        accessible = self.frame.VisiblePins(self.frame, 'Westfall', 900, 700)
        self.assertEqual(len(accessible), 1)
        self.assertEqual(len(accessible[1].details), 2)

    def test_map_requests_only_after_server_hello_and_recovers_once(self):
        self.send('CSYNC|Hello 30')
        globals_ = self.lua.globals()
        self.assertEqual(len(globals_.requests), 0)
        self.lua.execute('WorldMapFrame.IsShown = function() return true end')
        globals_.CompanionPins_Request()
        globals_.CompanionPins_Request()
        self.assertEqual(len(globals_.requests), 1)
        self.assertEqual(globals_.requests[1], '.companionsync pins v2')
        self.send('CSYNC|Pin set 2 1/1:')
        self.lua.execute('now = 500')
        self.send('CSYNC|Hello 30')
        self.assertEqual(len(globals_.requests), 1)
        globals_.CompanionPins_Request(True)
        self.assertEqual(len(globals_.requests), 2)

    def test_explicit_refresh_recovers_without_a_recent_hello(self):
        globals_ = self.lua.globals()
        self.lua.execute('WorldMapFrame.IsShown = function() return true end')
        globals_.CompanionPins_Request()
        self.assertEqual(len(globals_.requests), 0)
        globals_.CompanionPins_Request(True)
        self.assertEqual(len(globals_.requests), 1)
        self.send('CSYNC|Hello 30')
        self.lua.execute('now = 300')
        globals_.CompanionPins_Request(True)
        self.assertEqual(len(globals_.requests), 2)

    def test_reopening_an_old_map_requests_a_recovery_snapshot(self):
        globals_ = self.lua.globals()
        self.lua.execute('WorldMapFrame.IsShown = function() return true end')
        self.send('CSYNC|Hello 30')
        self.send('CSYNC|Pin set 1 1/1:')
        self.lua.execute('now = 300')
        self.send('CSYNC|Hello 30')
        self.assertEqual(len(globals_.requests), 1)
        globals_.CompanionPins_Request(False, True)
        self.assertEqual(len(globals_.requests), 2)

    def test_nearby_markers_cluster_with_hand_in_at_npc(self):
        self.send('CSYNC|Pin sync 3: 45,51,g=Stew@Mirdan:1/3,z=Westfall|46,52,h=Return@Ilyrin,z=Westfall|48,53,g=Far@Mirdan,z=Elwynn')
        visible = self.frame.VisiblePins(self.frame, 'Westfall', 900, 700)
        self.assertEqual(len(visible), 1)
        self.assertEqual((visible[1].x, visible[1].y, visible[1].count), (46, 52, 2))
        self.assertEqual(visible[1].style.key, 'handin')


if __name__ == '__main__':
    unittest.main()
