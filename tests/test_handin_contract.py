"""Keep companion hand-in feedback separate from native player quest status."""
import unittest
from pathlib import Path

SOURCE = (Path(__file__).resolve().parents[1] / 'src/mod_companion_questsync.cpp').read_text(encoding='utf-8')


class HandInContract(unittest.TestCase):
    def test_no_second_hand_in_for_a_body_rejected_by_the_sync_resolver(self):
        sweep = SOURCE.split('SyncReport SyncCompanion(Player* owner)', 1)[1].split('aggregate.added += one.added;', 1)[0]
        self.assertNotIn('DeliverHandIns(owner, bot)', sweep)
        sync = SOURCE.split('SyncReport SyncCompanionFor(', 1)[1].split('bool CreateCompanion(', 1)[0]
        self.assertLess(sync.index('Player* bot = ResolveCompanion('), sync.index('DeliverHandIns(owner, bot)'))
        self.assertIn('if (report.completed)\n            DeliverHandIns(owner, bot);', sync)

    def test_reward_path_requires_personal_control_and_current_tracked_custody(self):
        turnin = SOURCE.split('void DeliverHandIns(', 1)[1].split('bool CanManageBags(', 1)[0]
        self.assertIn('if (!IsControlledCompanion(bot, owner)', turnin.split('for (uint16 slot', 1)[0])
        check = 'if (!IsControlledCompanion(bot, owner) || !HasCurrentCustody(ownerGuid, botGuid, true))'
        before_native_reward = turnin[:turnin.index('HandleQuestgiverChooseRewardOpcode')].rsplit('prodAt = nowTs;', 1)[1]
        self.assertIn(check + '\n                return;', before_native_reward)

    def test_missing_or_uninteractable_ender_is_only_a_navigation_hint(self):
        turnin = SOURCE.split('void DeliverHandIns(', 1)[1].split('bool CanManageBags(', 1)[0]
        missing = turnin.split('if (!giver)', 1)[1].split('continue;', 1)[0]
        self.assertIn('FindQuestEnder(bot, questId)', turnin)
        self.assertIn('"no-interactable-ender"', missing)
        self.assertNotIn('StepBesidePoint(', turnin)
        self.assertNotIn('AssistNearbyHandIns(', SOURCE)

    def test_module_hand_ins_use_only_the_native_chosen_giver_path(self):
        turnin = SOURCE.split('void DeliverHandIns(', 1)[1].split('bool CanManageBags(', 1)[0]
        self.assertIn('g_cfg.handInMode == 0)\n            return;', turnin)
        self.assertIn('HandleQuestgiverChooseRewardOpcode(request)', turnin)
        self.assertNotIn('HandleCommand(CHAT_MSG_WHISPER, "r", owner)', turnin)

    def test_companion_does_not_send_native_questgiver_status(self):
        self.assertNotIn('->SendQuestGiverStatus(', SOURCE)

    def test_addon_pin_checks_that_the_companion_has_its_turnin_items(self):
        pins = SOURCE.split('std::string BuildPinLine(', 1)[1].split('void SendPinSync(', 1)[0]
        self.assertIn('bool const ready =', pins)
        self.assertIn('DescribeMissingItemsUncached(bot, quest).empty()', pins)
        self.assertIn('if (ready)', pins)

    def test_reward_refusal_is_logged_at_the_existing_attempt_cadence(self):
        turnin = SOURCE.split('void DeliverHandIns(', 1)[1].split('bot->RewardQuest(', 1)[0]
        self.assertIn('LogHandInWait(owner, bot, quest, spot, dist, "reward-check")', turnin)
        self.assertLess(turnin.index('Timing::RewardProd'), turnin.index('"reward-check"'))

    def test_queued_arrival_whisper_does_not_claim_reward_and_expires_with_quest(self):
        report = SOURCE.split('void ReportPosition(', 1)[1].split('if (dist > 60.0f)', 1)[0]
        self.assertNotIn('handing it in now', report)
        self.assertIn('questId, true);', report)
