"""Exercise the real Lua protocol handler and card state without a WoW client."""
import unittest

from lupa import LuaRuntime
from lua_sandbox import ADDON, STUB


class CompanionCards(unittest.TestCase):
    def setUp(self):
        self.lua = LuaRuntime(unpack_returned_tuples=True)
        self.lua.execute(STUB)
        self.lua.execute('''
            time = function() return 100 end
            filters = {}
            ChatFrame_AddMessageEventFilter = function(event, filter) filters[event] = filter end
        ''')
        self.g = self.lua.table()
        self.lua.execute((ADDON / 'Core.lua').read_text(encoding='utf-8'), 'CompanionSync', self.g)
        self.message = self.lua.eval('_FRAME_SCRIPTS.OnEvent')
        self.lua.execute((ADDON / 'UI.lua').read_text(encoding='utf-8'), 'CompanionSync', self.g)

    def send(self, body):
        self.message(None, 'CHAT_MSG_SYSTEM', 'CSYNC|' + body)

    def test_legacy_roster_has_gathering_but_does_not_invent_crafting(self):
        self.send('Comp sync 1: Mirdan,7,11,14,8,ogs,10,y=Skinning 53/150')
        card = self.g.CardState(self.g.companions[1])
        self.assertEqual(card.status, 'In party')
        self.assertEqual(card.gather.current, 53)
        self.assertIsNone(card.craft)

    def test_skill_snapshot_overrides_legacy_cap_and_tracks_both_skills(self):
        self.send('Comp sync 1: Mirdan,7,11,14,8,ogs,10,y=Skinning 53/150')
        self.send('Skill sync: Mirdan,393,53,75,165,48,150')
        card = self.g.CardState(self.g.companions[1])
        self.assertEqual((card.gather.name, card.gather.cap), ('Skinning', 75))
        self.assertEqual((card.craft.name, card.craft.current, card.craft.cap), ('Leatherworking', 48, 150))
        self.send('Skill sync: Mirdan,393,53,75,165,0,0')
        self.assertEqual(self.g.CardState(self.g.companions[1]).craft.cap, 0)

    def test_dismissed_cards_do_not_display_zero_stats_or_stale_skills(self):
        self.send('Skill sync: Mirdan,393,53,75,165,48,150')
        self.send('Comp sync 1: Mirdan,0,0,0,0,-,0')
        card = self.g.CardState(self.g.companions[1])
        self.assertEqual(card.status, 'Dismissed')
        self.assertIsNone(card.gather)
        self.assertIsNone(card.craft)
        self.assertIsNone(card.free)
        self.send('Comp sync 1: Mirdan,7,11,14,8,ogd,10')
        card = self.g.CardState(self.g.companions[1])
        self.assertEqual(card.status, 'Dead')
        self.assertIsNone(card.craft)

    def test_skill_snapshot_accepts_only_prefixed_system_messages_and_can_clear(self):
        self.send('Comp sync 1: Mirdan,7,11,14,8,ogs,10,y=Skinning 53/150')
        self.send('Skill sync: Mirdan,393,53,75,165,48,150')
        self.message(None, 'CHAT_MSG_WHISPER', 'Skill sync:')
        self.message(None, 'CHAT_MSG_SYSTEM', 'CSYNCxSkill sync:')
        self.assertEqual(self.g.CardState(self.g.companions[1]).craft.current, 48)
        self.send('Skill sync:')
        card = self.g.CardState(self.g.companions[1])
        self.assertIsNone(card.craft)
        self.assertEqual(card.gather.current, 53)

    def test_system_chat_filter_preserves_normal_player_messages(self):
        self.send('Comp sync 1: Mirdan,7,11,14,8,ogs,10')
        filter_system = self.lua.globals().filters.CHAT_MSG_SYSTEM
        self.assertFalse(filter_system(None, 'CHAT_MSG_SYSTEM', 'Vendor items repaired.'))
        self.assertTrue(filter_system(None, 'CHAT_MSG_SYSTEM', 'Mirdan is now level 15.'))
        self.assertTrue(filter_system(None, 'CHAT_MSG_SYSTEM', 'CSYNC|Skill sync:'))

    def test_window_opens_with_summoned_dismissed_and_dead_cards(self):
        self.send('Comp sync 3: Mirdan,7,11,14,8,ogs,10|Drodan,0,0,0,0,-,0|Gordan,3,3,14,3,od,0')
        self.send('Skill sync: Mirdan,393,53,75,165,48,150')
        self.g.Run = self.lua.eval('function() end')
        self.g.Toggle()
        self.g.Refresh()

    def test_malformed_row_does_not_poison_other_companions(self):
        self.send('Comp sync 2: Mirdan,7,11,14,8,ogs,10|Drodan,5,1,14,8,ogs,10')
        self.send('Skill sync: Mirdan,393,garbage,75,165,48,150|Drodan,182,150,150,171,70,150')
        self.assertIsNone(self.g.CardState(self.g.companions[1]).craft)
        self.assertEqual(self.g.CardState(self.g.companions[2]).craft.name, 'Alchemy')


if __name__ == '__main__':
    unittest.main()
