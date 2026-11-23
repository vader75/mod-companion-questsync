"""Keep companion hand-in feedback separate from native player quest status."""
import unittest
from pathlib import Path

SOURCE = (Path(__file__).resolve().parents[1] / 'src/mod_companion_questsync.cpp').read_text(encoding='utf-8')


class HandInContract(unittest.TestCase):
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
