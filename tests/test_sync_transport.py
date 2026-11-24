"""Replay CompanionSync system traffic and click the real UI on Lua 5.1."""
import re
import unittest

from lupa.lua51 import LuaRuntime
from lua_sandbox import ADDON, CHAT_SINK, STUB
from test_supply_picker import FRAMES

A = 'Amy,5,1,14,1,ogs,8'
B = 'Bob,5,1,14,1,ogs,8'


class ServerTransportContract(unittest.TestCase):
    def test_server_sends_both_formats_and_keeps_presence_outside_auto_sync(self):
        source = (ADDON.parents[1] / 'src' / 'mod_companion_questsync.cpp').read_text(encoding='utf-8')
        self.assertIn('"Hello " + std::to_string(g_cfg.intervalSeconds)', source)
        self.assertIn('"Sync capabilities 1 {} {}"', source)
        self.assertIn('"{} set {} {}/{}:{}"', source)
        self.assertIn('SendChunkedPayload(owner, "Comp", "Comp sync", payload, entries)', source)
        self.assertIn('SendChunkedPayload(owner, "Pool", "Pool stock", payload, entries)', source)
        self.assertRegex(source, r'SendPoolStock\(Player\* owner, bool force = false\)\s*\{\s*if \(!owner')
        self.assertRegex(source, r'SendCompSync\(Player\* owner, bool force = false\)\s*\{\s*if \(!owner')
        self.assertLess(source.index('SendHeartbeat(player);'), source.index('if (!g_cfg.autoSync)\n            return;'))


class SyncTransport(unittest.TestCase):
    def setUp(self):
        self.load()

    def load(self, extra=''):
        self.lua = LuaRuntime(unpack_returned_tuples=True)
        self.assertEqual(self.lua.eval('_VERSION'), 'Lua 5.1')
        self.lua.execute(STUB.replace('unpack = table.unpack', '') + CHAT_SINK + FRAMES)
        self.lua.execute('UnitFactionGroup = function() return "Alliance" end\n' + extra)
        self.env = self.lua.globals()
        self.g = self.lua.table()
        for name in ('Core.lua', 'UI.lua'):
            self.lua.execute((ADDON / name).read_text(encoding='utf-8'), 'CompanionSync', self.g)
        self.event = self.env.frames[1].OnEvent

    def send(self, body, event='CHAT_MSG_SYSTEM', prefix='CSYNC|'):
        self.event(None, event, prefix + body)

    def names(self):
        return [self.g.companions[i].name for i in range(1, len(self.g.companions) + 1)]

    def visible(self, text, kind='FontString'):
        return self.env.VisibleWithText(kind, text, True)

    def click(self, text):
        button = self.visible(text, 'Button')
        self.assertIsNotNone(button, text)
        self.assertTrue(button.enabled, text)
        button.OnClick(button)

    def advance(self, seconds):
        self.env.clock += seconds
        listener = self.env.frames[1]
        listener.OnUpdate(listener, seconds)
        frame = self.env.CompanionSyncFrame
        if frame is not None:
            frame.OnUpdate(frame, seconds)

    def test_roster_pool_and_presence_require_raw_prefixed_system_provenance(self):
        for event, prefix in [('CHAT_MSG_SYSTEM', ''), ('CHAT_MSG_WHISPER', 'CSYNC|'),
                              ('CHAT_MSG_MONSTER_WHISPER', 'CSYNC|'), ('CHAT_MSG_SYSTEM', '|cff112233CSYNC|')]:
            self.send('Comp sync 1: ' + A, event, prefix)
            self.send('Pool stock 1: 1-5=4', event, prefix)
            self.send('Hello 30', event, prefix)
        self.assertEqual(self.names(), [])
        self.assertEqual(self.g.StockFor(1, 5), 0)
        self.assertFalse(self.g.ServerAlive())
        self.send('Companion quest sync: unrelated notice', prefix='')
        self.assertFalse(self.g.ServerAlive())

    def test_revision_joins_out_of_order_and_never_mixes_legacy_or_older_parts(self):
        self.send('Comp set 10 2/2:' + A[8:])
        self.send('Comp sync 1: ' + B)
        self.send('Comp set 9 1/2:' + B[:8])
        self.assertEqual(self.names(), [])
        self.send('Comp set 10 1/2:' + A[:8])
        self.assertEqual(self.names(), ['Amy'])
        self.send('Comp set 10 1/1:' + B)
        self.assertEqual(self.names(), ['Amy'])
        self.send('Pool set 10 2/2:=4')
        self.send('Pool set 10 1/2:1-5')
        self.assertEqual(self.g.StockFor(1, 5), 4)
        self.send('Comp set 11 1/1:')
        self.send('Pool set 11 1/1:')
        self.assertEqual(self.names(), [])
        self.assertEqual(self.g.StockFor(1, 5), 0)

    def test_bad_headers_and_conflicting_duplicates_are_rejected_without_publication(self):
        self.send('Comp set 1 1/1:' + A)
        for header in ('0 1/1', '2 0/1', '2 2/1', '2 1/0', '2 1/33', '9007199254740992 1/1'):
            self.send('Comp set ' + header + ':' + B)
        self.send('Comp set 2 1/2:' + B[:8])
        self.send('Comp set 2 2/3:' + B[8:])
        self.send('Comp set 2 2/2:' + B[8:])
        self.send('Comp set 3 1/2:' + B[:8])
        self.send('Comp set 3 1/2:' + A[:8])
        self.send('Comp set 3 2/2:' + B[8:])
        self.send('Comp set 4 1/1:' + 'x' * 193)
        self.send('Comp set 5 1/1:Broken')
        self.send('Comp set 6 1/1:' + B + '|' + B)
        self.assertEqual(self.names(), ['Amy'])
        self.send('Comp set 7 1/2:' + B[:8])
        self.send('Comp set 7 1/2:' + B[:8])
        self.send('Comp set 7 2/2:' + B[8:])
        self.assertEqual(self.names(), ['Bob'])

    def test_new_revision_supersedes_incomplete_set_and_deadline_is_not_sliding(self):
        self.send('Comp set 1 1/2:' + A[:8])
        self.send('Comp set 2 1/2:' + B[:8])
        self.send('Comp set 1 2/2:' + A[8:])
        self.advance(7)
        self.send('Comp set 2 1/2:' + B[:8])
        self.advance(2)
        self.send('Comp set 2 2/2:' + B[8:])
        self.send('Comp set 2 1/2:' + B[:8])
        self.assertEqual(self.names(), [])
        self.send('Comp set 3 1/1:' + B)
        self.assertEqual(self.names(), ['Bob'])

    def test_legacy_requires_order_start_count_and_live_buffer(self):
        self.send('Comp sync 2/2: ' + B)
        self.send('Comp sync 1/2: ' + A)
        self.assertEqual(self.names(), [])
        self.advance(9)
        self.send('Comp sync 2/2: ' + B)
        self.send('Comp sync 2: ' + A)
        self.assertEqual(self.names(), [])
        self.send('Comp sync 1/3: ' + A)
        self.send('Comp sync 3/3: ' + B)
        self.send('Comp sync 2/3: ' + B)
        self.assertEqual(self.names(), [])
        self.send('Comp sync 1/2: ' + A)
        self.send('Comp sync 2/2: ' + B)
        self.assertEqual(self.names(), ['Amy', 'Bob'])
        self.send('Comp sync 0: ')
        self.assertEqual(self.names(), [])

    def test_ui_distinguishes_heartbeat_missing_empty_stale_and_disabled_pool(self):
        self.g.Toggle()
        self.assertIsNotNone(self.visible('Waiting for the server'))
        self.send('Hello 15')
        self.assertIsNotNone(self.visible('Waiting for the roster'))
        self.advance(59)
        self.assertTrue(self.g.ServerAlive())
        self.send('Sync capabilities 1 60 0')
        self.send('Comp set 1 1/1:')
        self.assertIsNotNone(self.visible('No companions yet'))
        self.click('+ Add companion')
        self.assertIsNotNone(self.visible('Pool creation is disabled'))
        self.advance(76)
        self.assertIsNotNone(self.visible('server not answering'))
        self.send('Hello 15')
        self.assertIsNotNone(self.visible('Roster is stale'))
        self.assertIsNone(self.visible('No companions yet'))

    def test_pool_missing_empty_and_stale_not_conflated(self):
        self.send('Hello 30')
        self.g.Toggle()
        self.click('+ Add companion')
        self.assertIsNotNone(self.visible('Waiting for pool stock'))
        self.send('Pool stock 0:')
        self.assertIsNotNone(self.visible('The pool is empty'))
        self.advance(76)
        self.send('Hello 30')
        self.assertIsNotNone(self.visible('Pool stock is stale'))

    def test_pool_buffer_is_independent_and_accepts_all_32_parts(self):
        self.send('Pool set 1 1/1:1-5=3')
        self.send('Comp set 1 1/2:' + A[:8])
        for part in range(32, 0, -1):
            row = f'{part}-5={part}'
            self.send(f'Pool set 2 {part}/32:' + ('|' if part != 1 else '') + row)
        self.send('Comp set 1 2/2:' + A[8:])
        self.assertEqual(self.names(), ['Amy'])
        self.assertEqual(self.g.StockFor(32, 5), 32)
        self.send('Pool set 3 1/2:1-5=42')
        self.send('Pool set 3 2/2:')
        self.send('Pool set 3 2/2:|2-5=42')
        self.send('Pool set 4 1/1:1-5=42|1-5=43')
        self.send('Pool set 5 1/1:1-5=4294967296')
        self.assertEqual(self.g.StockFor(1, 5), 1)

    def test_unknown_or_malformed_protocol_cannot_refresh_presence(self):
        self.send('Hello 0')
        self.send('Sync capabilities 2 60 1')
        self.send('Sync capabilities 1 60 2')
        self.send('Comp set 1 1/2:' + A)
        self.send('Pool stock 1:abc')
        self.assertFalse(self.g.ServerAlive())
        self.assertIsNone(self.g.poolEnabled)
        self.send('Hello 40')
        self.advance(80)
        self.assertTrue(self.g.ServerAlive())
        self.advance(16)
        self.assertFalse(self.g.ServerAlive())

    def test_release_surfaces_quarantine_without_claiming_pool_completion(self):
        self.send('Comp sync 1: ' + A)
        self.g.Toggle()
        self.assertTrue(self.g.Action('release', 'Amy'))
        self.send('Amy is quarantined for offline maintenance. The body and name remain reserved until staff review.', prefix='')
        self.assertIsNone(self.g.releasing.Amy)
        self.assertIsNotNone(self.visible('remain reserved until staff review'))

    def test_release_reports_the_slot_freeing_itself(self):
        self.send('Comp sync 1: ' + A)
        self.g.Toggle()
        self.assertTrue(self.g.Action('release', 'Amy'))
        self.send('Companion quest sync: Amy is retiring. Its slot frees once it has left the world.', prefix='')
        self.assertIsNone(self.g.releasing.Amy)
        self.assertIsNotNone(self.visible('slot frees once it has left the world'))
        self.send('Amy has retired and its slot is free again. Ask me for a new companion whenever you are ready.', prefix='')
        self.assertIsNotNone(self.visible('has retired and its slot is free again'))

    def test_release_is_not_queued_without_send_and_expires_without_roster(self):
        self.assertFalse(self.g.Action('release', 'Amy'))
        self.assertIsNone(self.g.releasing.Amy)
        self.send('Comp sync 2: ' + A + '|' + B)
        self.g.Toggle()
        self.assertTrue(self.g.Action('release', 'Amy'))
        self.assertFalse(self.g.Action('release', 'Amy'))
        self.assertTrue(self.g.Action('release', 'Bob'))
        self.send('Companion quest sync: Amy could not be released - retry.', prefix='')
        self.assertIsNone(self.g.releasing.Amy)
        self.assertIsNotNone(self.g.releasing.Bob)
        self.advance(26)
        self.assertIsNone(self.g.releasing.Bob)
        self.assertIsNotNone(self.visible('Release not confirmed'))

    def test_release_completion_clears_only_the_removed_companion(self):
        self.send('Comp sync 2: ' + A + '|' + B)
        self.assertTrue(self.g.Action('release', 'Amy'))
        self.assertTrue(self.g.Action('release', 'Bob'))
        self.send('Comp sync 1: ' + B)
        self.assertIsNone(self.g.releasing.Amy)
        self.assertIsNotNone(self.g.releasing.Bob)

    def collect(self):
        self.click('Collect delivery')
        command = self.env.requests[len(self.env.requests)]
        match = re.fullmatch(r'\.companionsync delivery collect Amy (\d+)', command)
        self.assertIsNotNone(match, command)
        return int(match[1])

    def test_collect_progress_survives_selection_and_closed_window_until_terminal_reply(self):
        self.send('Sync capabilities 1 60 1')
        self.send('Delivery receipts 1')
        self.send('Comp set 1 1/1:' + A + '|' + B)
        self.g.Toggle()
        nonce = self.collect()
        self.send(f'Delivery progress {nonce + 1} failed:Other order')
        self.send(f'Delivery progress {nonce} completed:Spoof', 'CHAT_MSG_WHISPER')
        self.send(f'Delivery progress {nonce} requested:Checking order')
        self.click('Bob')
        self.send(f'Delivery progress {nonce} ready:Trade opened; accept to finish')
        self.send(f'Delivery progress {nonce} requested:Out of order')
        self.assertIsNotNone(self.visible('Amy: Trade opened; accept to finish'))
        self.g.Toggle()
        self.g.Toggle()
        self.assertIsNotNone(self.visible('Amy: Trade opened; accept to finish'))
        self.g.Toggle()
        self.send(f'Delivery progress {nonce} completed:Items delivered')
        self.send(f'Delivery progress {nonce} cancelled:Late cancellation')
        self.assertEqual(len(self.env.MESSAGES), 1)
        self.assertIn('Amy: Items delivered', self.env.MESSAGES[1])
        self.g.Toggle()
        self.assertIsNotNone(self.visible('Amy: Items delivered'))

    def test_collect_timeout_has_unknown_outcome_and_late_reply_does_not_change_it(self):
        self.send('Sync capabilities 1 60 1')
        self.send('Delivery receipts 1')
        self.send('Comp set 1 1/1:' + A)
        self.g.Toggle()
        nonce = self.collect()
        self.advance(16)
        self.assertIsNotNone(self.visible('No delivery reply'))
        self.send(f'Delivery progress {nonce} completed:Too late')
        self.assertIsNone(self.visible('Too late'))

    def test_ready_delivery_times_out_as_unconfirmed_with_the_window_hidden(self):
        self.send('Sync capabilities 1 60 1')
        self.send('Delivery receipts 1')
        self.send('Comp set 1 1/1:' + A)
        self.g.Toggle()
        nonce = self.collect()
        self.send(f'Delivery progress {nonce} ready:Trade ready')
        self.g.Toggle()
        self.advance(119)
        self.send(f'Delivery progress {nonce} ready:Duplicate must not extend the deadline')
        self.advance(2)
        self.assertEqual(self.g.delivery.state, 'unconfirmed')
        self.g.Toggle()
        self.assertIsNotNone(self.visible('No final confirmation'))

    def test_named_take_and_craft_progress_are_not_discarded_on_roster_switch(self):
        for verb, kind, quantity, terminal in [('Take', 'bag', 2, 'cancelled'), ('Craft', 'craft', 0, 'failed')]:
            with self.subTest(verb=verb):
                self.load()
                self.send('Sync capabilities 1 60 1')
                self.send('Delivery receipts 1')
                self.send('Comp set 1 1/1:' + A + '|' + B)
                self.g.Toggle()
                self.click('Choose supply')
                catalog = self.env.requests[len(self.env.requests)].split()[-2]
                self.send(f'Supply row {catalog} 1 929 {kind} {quantity}:Healing Potion')
                self.send(f'Supply end {catalog} 1')
                self.click('Healing Potion')
                self.click(verb + ' supply')
                action = self.env.requests[len(self.env.requests)].split()[-3]
                self.send(f'Delivery progress {action} requested:Checking the requested supply')
                self.click('Bob')
                self.assertFalse(self.visible('Collect delivery', 'Button').enabled)
                self.send(f'Delivery progress {action} unknown:Do not show this')
                self.send(f'Delivery progress {action} {terminal}:Order ended without delivery')
                self.assertIsNotNone(self.visible('Amy: Order ended without delivery'))
                self.assertTrue(self.visible('Collect delivery', 'Button').enabled)

    def test_legacy_collect_still_sends_old_command_and_does_not_claim_success(self):
        self.send('Comp sync 1: ' + A)
        self.g.Toggle()
        self.click('Collect delivery')
        self.assertEqual(self.env.requests[len(self.env.requests)], '.companionsync delivery collect Amy')
        self.assertIsNotNone(self.visible('See chat and the trade window'))

    def test_snapshot_protocol_does_not_imply_receipt_support(self):
        for capabilities in (None, 'Sync capabilities 1 60 1'):
            with self.subTest(capabilities=capabilities):
                self.load()
                if capabilities:
                    self.send(capabilities)
                self.send('Comp set 1 1/1:' + A)
                self.send('Delivery receipts 2')
                self.send('Delivery receipts 1', 'CHAT_MSG_WHISPER')
                self.g.Toggle()
                self.click('Collect delivery')
                self.assertEqual(self.env.requests[len(self.env.requests)], '.companionsync delivery collect Amy')
                self.assertEqual(self.g.delivery.state, 'legacy')

    def test_save_timeout_is_a_terminal_unconfirmed_receipt(self):
        self.send('Delivery receipts 1')
        self.send('Comp set 1 1/1:' + A)
        self.g.Toggle()
        nonce = self.collect()
        self.send(f'Delivery progress {nonce} ready:Review the trade')
        self.send(f'Delivery progress {nonce} unconfirmed:Check your bags before retrying')
        self.assertEqual(self.g.delivery.state, 'unconfirmed')
        self.assertFalse(self.g.DeliveryPending())

    def test_named_supply_without_receipt_support_uses_legacy_supply_result(self):
        self.send('Sync capabilities 1 60 1')
        self.send('Comp set 1 1/1:' + A)
        self.g.Toggle()
        self.click('Choose supply')
        catalog = self.env.requests[len(self.env.requests)].split()[-2]
        self.send(f'Supply row {catalog} 1 929 bag 2:Healing Potion')
        self.send(f'Supply end {catalog} 1')
        self.click('Healing Potion')
        self.click('Take supply')
        action = self.env.requests[len(self.env.requests)].split()[-3]
        self.assertIsNone(self.g.delivery)
        self.send(f'Supply result {action}:Preparing the trade')
        self.assertIsNotNone(self.visible('Preparing the trade'))
        self.assertIsNone(self.g.delivery)

    def test_launcher_preserves_old_right_side_ring_and_versions_new_drag(self):
        self.load('''
            CompanionSyncDB = { launcher = { x = 84, y = 0 } }
            Minimap.GetCenter = function() return 100, 100 end
            Minimap.GetEffectiveScale = function() return 1 end
            GetCursorPosition = function() return 100, 200 end
            UIParent.GetEffectiveScale = function() return 1 end
        ''')
        launcher = self.env.CompanionSyncLauncher
        self.assertEqual(self.env.CompanionSyncDB.launcher.version, 2)
        self.assertEqual(self.env.CompanionSyncDB.launcher.x, 84)
        launcher.OnDragStop(launcher)
        self.assertEqual(self.env.CompanionSyncDB.launcher.version, 2)
        self.assertEqual(self.env.CompanionSyncDB.launcher.y, 84)
        self.env.SlashCmdList.CSRESET()
        self.assertEqual(self.env.CompanionSyncDB.launcher.x, -84)


    def test_launcher_restores_saved_north_position_with_a_scaled_minimap(self):
        self.load('''
            CompanionSyncDB = { launcher = { x = 0, y = 84 } }
            Minimap.GetEffectiveScale = function() return 2 end
            local create = CreateFrame
            CreateFrame = function(kind, name, ...)
                local frame = create(kind, name, ...)
                if name == 'CompanionSyncLauncher' then
                    frame.GetEffectiveScale = function() return 1 end
                    frame.SetPoint = function(self, point, anchor, relative, x, y) self.y = y end
                end
                return frame
            end
        ''')
        self.assertEqual(self.env.CompanionSyncDB.launcher.y, 84)
        self.assertEqual(self.env.CompanionSyncLauncher.y, 168)


if __name__ == '__main__':
    unittest.main()
