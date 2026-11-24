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

    def test_full_slot_roster_is_reported_as_its_own_cause(self):
        create = section('bool CreateCompanion(', 'bool OwnerHasQuestCompanionLacks(')
        self.assertIn('CompanionSlotCount(', create)
        self.assertIn('companion slots are in use', create)
        # The post-commit message must not blame the slot cap it no longer owns.
        self.assertNotIn('all ownership slots are reserved', create)

    def test_release_retires_the_body_once_the_logout_is_observed(self):
        poll = section('void CheckPendingReleases()', 'bool BeginRelease(')
        self.assertIn('FinalizeRetirementInline(', poll)
        self.assertIn('retired', poll)
        # The timeout branch must still leave the row for offline maintenance.
        self.assertIn('remains quarantined', poll)
        final = section('bool FinalizeRetirementInline(', 'void CheckPendingReleases()')
        self.assertIn("x.state = 'retired'", final)
        self.assertIn('DELETE FROM cq_bindings', final)
        self.assertIn('c.online = 0', final)
        self.assertIn('DirectCommitTransaction', final)
        # A retired body is a tombstone: never reset, rename or delete it.
        self.assertNotIn('DELETE FROM characters', final)
        self.assertNotIn('ResetPoolBody', final)

    def test_create_is_atomic_and_never_resets_an_unclaimed_body(self):
        create = section('bool CreateCompanion(', 'bool OwnerHasQuestCompanionLacks(')
        self.assertIn('DirectCommitTransaction', create)
        self.assertIn('HasCurrentCustody', create)
        self.assertIn('FreeCustodyClause', create)
        self.assertNotIn('ResetPoolBody', create)
        self.assertNotIn('RenameBody', create)

    def test_only_a_successful_new_claim_arms_the_lifetime_starter_grant(self):
        create = section('bool CreateCompanion(', 'bool OwnerHasQuestCompanionLacks(')
        self.assertIn("x.starter_bag_attempt = COALESCE(x.starter_bag_attempt, '')", create)
        self.assertIn('g_starterBagsReady ?', create)
        self.assertNotIn('if (!g_starterBagsReady)', create)
        self.assertIn('+ starterAssignment +', create)

    def test_starter_bags_require_proof_of_consumption_before_equip(self):
        starter = section('bool HandleStarterBags(', 'void EnsureBags(')
        consume = section('bool ConsumeStarterBags(', 'bool HandleStarterBags(')
        self.assertIn("x.starter_bag_attempt = ''", consume)
        self.assertIn('x.starter_bag_attempt = \'{}\'', consume)
        self.assertIn('IsControlledCompanion(bot, owner)', starter)
        self.assertLess(starter.index('ConsumeStarterBags('), starter.index('EquipEmptyBagSlot('))

    def test_native_policy_registered_at_script_load_and_vetoes_by_body_not_tracking(self):
        register = SRC.split('void Addmod_companion_questsyncScripts()', 1)[1]
        self.assertIn('RegisterInitializationVeto(DeferNativeBotInitialization)', register)
        veto = section('bool DeferNativeBotInitialization(', 'bool HasSingleColumnUniqueKey(')
        self.assertIn('bot_guid = {}', veto)
        self.assertIn("state IN ('owned', 'retiring')", veto)
        self.assertNotIn('tracking', veto)
        self.assertIn('!row ||', veto)

    def test_native_factory_guards_full_reset_and_bags_but_exposes_empty_slot_equip(self):
        patch_path = Path(__file__).resolve().parents[1] / 'patches/mod-playerbots-companion-initialization.patch'
        self.assertTrue(patch_path.is_file(), 'missing portable Playerbots patch')
        patch = patch_path.read_text()
        additions = '\n'.join(line[1:] for line in patch.splitlines() if line.startswith('+'))
        self.assertGreaterEqual(additions.count('ShouldDeferInitialization(bot->GetGUID().GetCounter())'), 2)
        self.assertIn('bool PlayerbotFactory::EquipEmptyBagSlot(', additions)
        self.assertIn('dest == expected', additions)
        self.assertIn('CanEquipUnseenItem(slot, dest, item, false)', additions)

    def test_upkeep_cannot_refill_portable_holes(self):
        upkeep = section('void EnsureBags(', '/// Put clothes on')
        self.assertIn('g_cfg.extraBagItem == kStarterBagItem', upkeep)
        self.assertLess(upkeep.index('HandleStarterBags(owner, bot)'), upkeep.index('g_cfg.extraBags'))

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
