"""Exercise CompanionPins wire and map data against a stub 3.3.5 client."""
import unittest
from pathlib import Path

from lupa.lua51 import LuaRuntime
from lua_sandbox import CHAT_SINK, STUB

PINS = Path(__file__).resolve().parents[1] / 'addon/CompanionPins/CompanionPins.lua'


class CompanionPinsContract(unittest.TestCase):
    def setUp(self):
        self.lua = LuaRuntime(unpack_returned_tuples=True)
        self.lua.execute(STUB)
        self.lua.execute(CHAT_SINK)
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

    def test_epoch_resets_revision_and_rejects_late_packets(self):
        self.send('CSYNC|Pin set 99 1/1:48,52,h=Old@Mirdan,z=Westfall')
        self.send('CSYNC|Pin set session 1000')
        self.send('CSYNC|Pin set 1000:100 1/1:48,52,h=First@Mirdan,z=Westfall')
        self.send('CSYNC|Pin set 1000:101 1/2:48,52,h=Interrupted')
        self.send('CSYNC|Pin set 1001:1 2/2:rdan,z=Westfall')
        self.send('CSYNC|Pin set 1000:101 2/2:@Mirdan,z=Westfall')
        self.send('CSYNC|Pin set session 1000')
        self.send('CSYNC|Pin set 102 1/1:48,52,h=Untagged@Mirdan,z=Westfall')
        self.send('CSYNC|Pin set 1001:1 1/2:48,52,h=New@Mi')
        self.assertEqual(self.frame.pins[1].quest, 'New')
        self.send('CSYNC|Pin set 1000:102 1/1:')
        self.assertEqual(self.frame.pins[1].quest, 'New')

    def test_epoch_request_requires_server_advertisement(self):
        self.lua.execute('WorldMapFrame.IsShown = function() return true end')
        self.send('CSYNC|Hello 30')
        requests = self.lua.globals().requests
        self.assertEqual(requests[1], '.companionsync pins v2')
        self.send('CSYNC|Pin set session 1000')
        self.send('CSYNC|Pin set 1 1/1:')  # compatible bootstrap snapshot
        self.lua.execute('now = 115')
        self.lua.globals().CompanionPins_Request()
        self.assertEqual(requests[2], '.companionsync pins v2 epoch')

    def test_epochs_larger_than_lua_integer_precision_can_differ_by_one(self):
        self.send('CSYNC|Pin set 10000000000000000:10 1/1:48,52,h=Old,z=Westfall')
        self.send('CSYNC|Pin set 10000000000000001:1 1/1:48,52,h=New,z=Westfall')
        self.assertEqual(self.frame.pins[1].quest, 'New')

    def test_pending_hand_in_is_requested_only_after_capability_advertisement(self):
        self.lua.execute('WorldMapFrame.IsShown = function() return true end')
        self.send('CSYNC|Hello 30')
        requests = self.lua.globals().requests
        self.assertEqual(requests[1], '.companionsync pins v2')
        self.send('CSYNC|Pin set handin 1')
        self.send('CSYNC|Pin set session 1000')
        self.send('CSYNC|Pin set 1 1/1:')
        self.lua.globals().now = 115
        self.frame.script_OnUpdate(self.frame, 1)
        self.assertEqual(requests[2], '.companionsync pins v2 epoch handin')

    def test_same_ender_retains_each_companions_state_and_recomputes_filtered_style(self):
        self.stub_map()
        g = self.lua.globals()
        self.lua.execute('''
            tooltip = {}
            GameTooltip.AddLine = function(_, text) tooltip[#tooltip + 1] = text end
        ''')
        self.send('CSYNC|Pin set 1 1/1:45.1250,51.1250,h=Stew@Mirdan,z=The Barrens|'
                  '45.1250,51.1250,p=Stew@Ilyrin:1/3,z=The Barrens|'
                  '45.1250,51.1250,u=Stew@Gordan,z=The Barrens')
        pin = g.CompanionPinsPin1
        self.assertEqual(self.frame.drawnPins, 1)
        self.assertEqual((pin.pin.style.key, pin.glyph.value), ('handin', '?'))
        pin.script_OnEnter(pin)
        lines = '\n'.join(g.tooltip[i] for i in range(1, len(g.tooltip) + 1))
        self.assertIn('Mirdan — ready to hand in', lines)
        self.assertIn('Ilyrin — work left  1/3', lines)
        self.assertIn('Gordan — hand-in not available yet', lines)
        g.SlashCmdList.WLPINS('only Gordan')
        self.assertEqual((pin.pin.style.key, pin.glyph.value, len(pin.pin.details)), ('unavailable', '?', 1))
        self.assertEqual(pin.pin.details[1].companion, 'Gordan')

    def test_repaint_dismisses_a_stale_hand_in_tooltip(self):
        self.stub_map()
        g = self.lua.globals()
        self.send('CSYNC|Pin set 1 1/1:45,51,h=Stew@Mirdan,z=The Barrens')
        pin, tip = g.CompanionPinsPin1, g.GameTooltip
        pin.script_OnEnter(pin)
        self.assertIn('ready to hand in', tip.lines[3])
        self.send('CSYNC|Pin set 2 1/1:45,51,u=Stew@Mirdan,z=The Barrens')
        self.assertEqual(pin.pin.style.key, 'unavailable')
        self.assertFalse(tip.IsOwned(tip, pin))
        pin.script_OnEnter(pin)
        self.assertIn('hand-in not available yet', tip.lines[3])

    def test_repaint_does_not_dismiss_another_frames_tooltip(self):
        self.stub_map()
        g = self.lua.globals()
        self.send('CSYNC|Pin set 1 1/1:45,51,h=Stew@Mirdan,z=The Barrens')
        tip = g.GameTooltip
        tip.SetOwner(tip, g.WorldMapFrame)
        tip.AddLine(tip, 'Another tooltip')
        tip.Show(tip)
        self.send('CSYNC|Pin set 2 1/1:45,51,u=Stew@Mirdan,z=The Barrens')
        self.send('CSYNC|Pin set 3 1/1:')
        self.assertTrue(tip.shown and tip.IsOwned(tip, g.WorldMapFrame))
        self.assertEqual(tip.lines[1], 'Another tooltip')

    def test_close_but_distinct_hand_in_locations_do_not_cluster(self):
        self.send('CSYNC|Pin set 1 1/1:45,51,h=Stew@Mirdan,z=Westfall|'
                  '45.25,51,p=Stew@Ilyrin,z=Westfall|46,51,u=Stew@Gordan,z=Westfall|'
                  '45.25,51,g=Stew@Ilyrin:1/3,z=Westfall')
        visible = self.frame.VisiblePins(self.frame, 'Westfall', 900, 700)
        self.assertEqual([(visible[i].x, visible[i].style.key) for i in range(1, len(visible) + 1)],
                         [(45, 'handin'), (45.25, 'pending'), (46, 'unavailable')])
        self.assertEqual(len(self.frame.VisiblePins(self.frame, 'Elwynn', 900, 700)), 0)
        self.send('CSYNC|Pin set 2 1/1:')
        self.assertEqual(len(self.frame.VisiblePins(self.frame, 'Westfall', 900, 700)), 0)

    def test_distinct_clamped_hand_ins_have_disjoint_mouse_hit_boxes(self):
        for zoomed in (False, True):
            with self.subTest(zoomed=zoomed):
                self.stub_map(zoomed=zoomed)
                g = self.lua.globals()
                self.send(f'CSYNC|Pin set {1 + zoomed} 1/1:0.25,51,h=Stew@Mirdan,z=The Barrens|'
                          '0.5,51,p=Stew@Ilyrin,z=The Barrens')
                rects = []
                for index, (x, companion) in enumerate(((0.25, 'Mirdan'), (0.5, 'Ilyrin')), 1):
                    marker = g[f'CompanionPinsPin{index}']
                    self.assertEqual((marker.pin.x, marker.pin.details[1].companion), (x, companion))
                    self.assertTrue(marker.shown and marker.mouse)
                    scale = g.WorldMapDetailFrame.GetEffectiveScale()
                    cx, cy = marker.point[4], -marker.point[5]
                    half_width, half_height = marker.width / 2, marker.height / 2
                    left, right, top, bottom = ((cx - half_width + marker.insets[1]) * scale,
                                                (cx + half_width - marker.insets[2]) * scale,
                                                (cy - half_height + marker.insets[3]) * scale,
                                                (cy + half_height - marker.insets[4]) * scale)
                    self.assertLess(left, right)
                    self.assertLess(top, bottom)
                    rects.append((left, right, top, bottom))
                a, b = rects
                self.assertTrue(a[1] <= b[0] or b[1] <= a[0] or a[3] <= b[2] or b[3] <= a[2], rects)

    def test_muted_hand_in_does_not_swallow_nearby_green_work(self):
        self.send('CSYNC|Pin set 1 1/1:45.5,51,p=Stew@Mirdan,z=Westfall|'
                  '45,51,g=Stew@Mirdan:1/3,z=Westfall')
        visible = self.frame.VisiblePins(self.frame, 'Westfall', 900, 700)
        self.assertEqual([(visible[i].x, visible[i].style.glyph) for i in range(1, len(visible) + 1)],
                         [(45.5, '?'), (45, '!')])

    def test_sample_pin_does_not_acknowledge_manual_refresh_or_mutate_snapshot(self):
        self.stub_map()
        g = self.lua.globals()
        g.SlashCmdList.WLPINS('refresh')
        g.SlashCmdList.WLPINS('test')
        self.assertEqual(len(self.frame.pins), 0)
        self.assertEqual(len(g.MESSAGES), 2)
        self.assertIn('Sample', g.CompanionPinsStatus.text.value)
        self.send('CSYNC|Pin set 1 1/1:')
        self.assertFalse(g.CompanionPinsPin1.shown)
        self.assertIn('refreshed', g.MESSAGES[3])

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

    def test_still_open_map_recovers_after_total_loss_with_bounded_attempts(self):
        self.lua.execute('WorldMapFrame.IsShown = function() return true end')
        self.send('CSYNC|Hello 30')
        self.send('CSYNC|Pin set 1 1/1:45,50,g=Old@Mirdan,z=Westfall')
        requests = self.lua.globals().requests
        for now, count in [(219, 1), (220, 2), (233, 2), (235, 3), (250, 4), (265, 4), (350, 4)]:
            self.lua.globals().now = now
            self.assertIsNotNone(self.frame.script_OnUpdate)
            self.frame.script_OnUpdate(self.frame, 1)
            self.assertEqual(len(requests), count, f'at {now}')
        self.assertEqual(self.frame.pins[1].quest, 'Old')
        self.send('CSYNC|Pin set 2 1/1:')
        self.lua.globals().now = 470
        self.frame.script_OnUpdate(self.frame, 1)
        self.assertEqual(len(requests), 5)

    def test_clear_keeps_watermark_and_can_rebuild_identical_payload(self):
        self.send('CSYNC|Pin set 7 1/1:45,50,g=Find It@Mirdan,z=Westfall')
        self.lua.globals().SlashCmdList.WLPINS('clear')
        self.send('CSYNC|Pin set 7 1/1:45,50,g=Find It@Mirdan,z=Westfall')
        self.assertEqual(len(self.frame.pins), 0)
        self.send('CSYNC|Pin set 8 1/1:45,50,g=Find It@Mirdan,z=Westfall')
        self.assertEqual(self.frame.pins[1].quest, 'Find It')

    def test_manual_refresh_explains_closed_map_and_reports_no_reply(self):
        g = self.lua.globals()
        g.SlashCmdList.WLPINS('refresh')
        self.assertEqual(len(g.requests), 0)
        self.assertIn('Open the world map', g.MESSAGES[1])
        self.lua.execute('WorldMapFrame.IsShown = function() return true end')
        g.SlashCmdList.WLPINS('refresh')
        self.assertIn('support', g.MESSAGES[2])
        self.lua.globals().now = 115
        self.frame.script_OnUpdate(self.frame, 1)
        self.assertIn('not support', g.MESSAGES[3])

    def test_disabled_server_does_not_auto_retry_and_tells_manual_requester(self):
        self.lua.execute('WorldMapFrame.IsShown = function() return true end')
        g = self.lua.globals()
        g.SlashCmdList.WLPINS('refresh')
        self.send('CSYNC|Pin set unavailable')
        self.assertIn('disabled', g.MESSAGES[2])
        g.now = 115
        self.send('CSYNC|Hello 30')
        self.frame.script_OnUpdate(self.frame, 1)
        self.assertEqual(len(g.requests), 1)

    def test_nearby_markers_cluster_with_hand_in_at_npc(self):
        self.send('CSYNC|Pin sync 3: 45,51,g=Stew@Mirdan:1/3,z=Westfall|46,52,h=Return@Ilyrin,z=Westfall|48,53,g=Far@Mirdan,z=Elwynn')
        visible = self.frame.VisiblePins(self.frame, 'Westfall', 900, 700)
        self.assertEqual(len(visible), 1)
        self.assertEqual((visible[1].x, visible[1].y, visible[1].count), (46, 52, 2))
        self.assertEqual(visible[1].style.key, 'handin')

    def stub_map(self, zoomed=False):
        self.lua.globals().zoomed = zoomed
        self.lua.execute('''
            WorldMapFrame.IsShown = function() return true end
            WorldMapFrame.HookScript = function() end
            WorldMapFrame.GetFrameLevel = function() return 1 end
            GameTooltip.SetOwner = function(self, owner) self.owner, self.lines = owner, {} end
            GameTooltip.IsOwned = function(self, owner) return self.owner == owner end
            GameTooltip.AddLine = function(self, line) self.lines[#self.lines + 1] = line end
            GameTooltip.Show = function(self) self.shown = true end
            GameTooltip.Hide = function(self) self.shown, self.owner = false, nil end
            local function rect(frame, left, right, bottom, top, scale)
                frame.GetLeft = function() return left end
                frame.GetRight = function() return right end
                frame.GetBottom = function() return bottom end
                frame.GetTop = function() return top end
                frame.GetEffectiveScale = function() return scale end
            end
            local parent, clip = {}, { GetObjectType = function() return 'ScrollFrame' end }
            parent.GetParent = function() return clip end
            clip.GetScrollChild = function() return parent end
            rect(clip, 20, 120, 40, 180, 1)
            local detail = CreateFrame('Frame')
            rect(detail, 0, 100, 0, 100, zoomed and 2 or 1)
            detail.GetParent = function() return zoomed and parent or WorldMapFrame end
            detail.GetWidth = function() return 100 end
            detail.GetHeight = function() return 100 end
            detail.GetFrameLevel = function() return 3 end
            detail.IsVisible = function() return true end
            WorldMapDetailFrame = detail
            local create = CreateFrame
            function CreateFrame(...)
                local frame = create(...)
                frame.Show = function(self) self.shown = true end
                frame.Hide = function(self)
                    local wasShown = self.shown
                    self.shown = false
                    if wasShown and self.script_OnHide then self:script_OnHide() end
                end
                frame.SetAlpha = function(self, alpha) self.alpha = alpha end
                frame.EnableMouse = function(self, enabled) self.mouse = enabled end
                frame.SetPoint = function(self, ...) self.point = {...} end
                frame.SetSize = function(self, w, h) self.width, self.height = w, h end
                frame.SetHitRectInsets = function(self, ...) self.insets = {...} end
                local font = frame.CreateFontString
                frame.CreateFontString = function(self, ...)
                    local text = font(self, ...)
                    text.SetText = function(region, value) region.value = value end
                    return text
                end
                return frame
            end
            GetMapInfo = function() return 'Barrens' end
            GetCurrentMapZone = function() return 1 end
            GetCurrentMapContinent = function() return 1 end
            GetMapZones = function() return 'The Barrens', 'Other' end
        ''')

    def test_map_status_distinguishes_waiting_stale_empty_and_hidden(self):
        self.stub_map()
        g = self.lua.globals()
        self.send(None, 'PLAYER_LOGIN')
        self.send(None, 'WORLD_MAP_UPDATE')
        self.assertIn('Waiting', g.CompanionPinsStatus.text.value)
        self.send('CSYNC|Pin set 1 1/1:45,50,g=Find It@Mirdan,z=The Barrens')
        self.assertTrue(g.CompanionPinsPin1.shown)
        self.assertFalse(g.CompanionPinsStatus.shown)
        g.now = 220
        self.frame.script_OnUpdate(self.frame, 1)
        self.assertIn('Stale', g.CompanionPinsStatus.text.value)
        self.assertLess(g.CompanionPinsPin1.alpha, 1)
        self.send('CSYNC|Pin set 2 1/1:')
        self.assertFalse(g.CompanionPinsPin1.shown)
        self.assertIn('No companion pins', g.CompanionPinsStatus.text.value)
        g.SlashCmdList.WLPINS('hide')
        self.assertFalse(g.CompanionPinsStatus.shown)
        self.assertTrue(g.CompanionPinsDB.hidden)
        g.SlashCmdList.WLPINS('show')
        self.assertTrue(g.CompanionPinsStatus.shown)

    def test_viewport_clips_hit_rect_and_tracks_pan_without_moving_pin(self):
        self.stub_map(zoomed=True)
        g = self.lua.globals()
        self.send('CSYNC|Pin set 1 1/1:0,50,g=Edge,z=The Barrens|95,50,g=Far,z=The Barrens')
        self.assertTrue(g.CompanionPinsPin1.mouse)
        self.assertEqual(g.CompanionPinsPin1.point[4], 13)
        self.assertEqual(g.CompanionPinsPin1.insets[1], 10)
        self.assertFalse(g.CompanionPinsPin2.mouse)
        self.lua.execute('WorldMapDetailFrame.GetLeft = function() return -40 end')
        self.frame.script_OnUpdate(self.frame, 0.1)
        self.assertFalse(g.CompanionPinsPin1.mouse)
        self.assertTrue(g.CompanionPinsPin2.mouse)
        self.assertEqual(g.CompanionPinsPin1.point[4], 13)

    def test_utf8_companion_names_survive_and_can_be_filtered(self):
        name = 'Éléonôrezāël'
        self.send(f'CSYNC|Pin set 1 1/1:45,51,g=Stew@{name}:2/3,z=Westfall')
        row = self.frame.pins[1].details[1]
        self.assertEqual((row.quest, row.companion, row.owed), ('Stew', name, '2/3'))
        self.lua.globals().SlashCmdList.WLPINS(f'ONLY {name}')
        self.assertEqual(len(self.frame.VisiblePins(self.frame, 'west fall', 900, 700)), 1)

    def test_unknown_broad_or_similar_map_names_do_not_match(self):
        self.send('CSYNC|Pin sync 2: 40,50,g=One,z=Westfall|60,50,g=Two,z=龍骨荒野')
        for wrong in (None, '', ' ', 'Azeroth', 'West', 'Westfall Beach', '龍骨', '其他區域'):
            with self.subTest(map=wrong):
                self.assertEqual(len(self.frame.VisiblePins(self.frame, wrong, 900, 700)), 0)
        self.assertEqual(len(self.frame.VisiblePins(self.frame, '龍骨荒野', 900, 700)), 1)


if __name__ == '__main__':
    unittest.main()
