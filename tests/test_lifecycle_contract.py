"""Source wiring checks for the realm-only lifecycle entry points (no live DB)."""
from pathlib import Path
import unittest

SRC = (Path(__file__).resolve().parents[1] / 'src/mod_companion_questsync.cpp').read_text(encoding='utf-8')


def section(start, stop):
    return SRC.split(start, 1)[1].split(stop, 1)[0]


class Lifecycle(unittest.TestCase):
    def test_tracking_is_not_ownership_and_history_is_not_authority(self):
        bind = section('static bool HandleBindCommand', 'static bool HandleUnbindCommand')
        unbind = section('static bool HandleUnbindCommand', 'static uint8 ParseRaceToken')
        roster = section('bool FindCompanions(', 'bool FindCompanionGuid(')
        self.assertIn('SetCompanionTracking(ownerGuid, botGuid, true)', bind)
        self.assertIn('SetCompanionTracking(ownerGuid, botGuid, false)', unbind)
        self.assertNotIn('DELETE FROM', unbind)
        self.assertNotIn('released_at', roster)
        self.assertNotIn('DropBindingRow', roster)
        self.assertIn('CurrentCustodyClause', roster)

    def test_create_is_atomic_and_never_resets_an_unclaimed_body(self):
        create = section('bool CreateCompanion(', 'bool OwnerHasQuestCompanionLacks(')
        self.assertIn('DirectCommitTransaction', create)
        self.assertIn('HasCurrentCustody', create)
        self.assertIn('FreeCustodyClause', create)
        self.assertNotIn('ResetPoolBody', create)
        self.assertNotIn('RenameBody', create)

    def test_release_reserves_before_logout_and_checks_deadline(self):
        begin = section('bool BeginRelease(', 'SyncReport SyncCompanion(')
        check = section('void CheckPendingReleases()', 'bool BeginRelease(')
        self.assertLess(begin.index('MarkCompanionRetiring'), begin.index('"remove "'))
        self.assertIn('HasCurrentCustody', begin)
        self.assertIn('IsCompanionLoginPending', begin)
        self.assertIn('g_pendingSummon.count', begin)
        self.assertIn('if (busy && now < pending.deadline)', check)
        self.assertNotIn('ResetPoolBody', check)
        self.assertNotIn('RenameBody', check)

    def test_integrity_does_not_repool_from_release_history(self):
        integrity = section('IntegrityReport RunIntegrity(', 'void SweepIntegrity(')
        self.assertNotIn('RenameBody', integrity)
        self.assertNotIn('DELETE gm FROM group_member', integrity)
        self.assertNotIn('DELETE FROM {} WHERE owner_guid', integrity)

    def test_base_schema_does_not_import_or_drop_unreviewed_bindings(self):
        sql = (Path(__file__).resolve().parents[1] / 'sql/companion-questsync.sql').read_text()
        self.assertNotIn('DROP TABLE', sql)
        self.assertNotIn('INSERT IGNORE', sql)

    def test_addon_does_not_promise_live_recycling(self):
        addon = Path(__file__).resolve().parents[1] / 'addon/CompanionSync'
        core = (addon / 'Core.lua').read_text()
        ui = (addon / 'UI.lua').read_text()
        self.assertNotIn('wiped, renamed and returned to the pool', core)
        self.assertNotIn('Wipe it and give the body back', ui)
        self.assertIn('offline maintenance', core)

    def test_quest_hooks_do_not_mutate_a_differently_controlled_session(self):
        abandon = section('void OnPlayerQuestAbandon(', 'void OnPlayerQuestAccept(')
        accept = section('void OnPlayerQuestAccept(', 'void Addmod_companion_questsyncScripts()')
        self.assertLess(abandon.index('IsControlledCompanion(bot, player)'), abandon.index('AbandonCompanionQuest(bot, slot, questId)'))
        self.assertLess(accept.index('IsControlledCompanion(bot, player)'), accept.index('SyncQuestToBot(player, bot'))
