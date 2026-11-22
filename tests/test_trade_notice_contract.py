"""Regression checks for trade whispers at their game-engine boundaries."""
import unittest
from pathlib import Path

SOURCE = (Path(__file__).resolve().parents[1] / "src/mod_companion_questsync.cpp").read_text(encoding="utf-8")


class TradeNoticeContract(unittest.TestCase):
    def test_offer_notice_is_tied_to_one_trade(self):
        apply = SOURCE.split('void ApplyPendingTrades()', 1)[1].split('/// Bring companions', 1)[0]
        start_notice = apply.split('if (offered)', 1)[1]
        self.assertRegex(start_notice, r'SayToOwner\([^;]+pending\.noticeId\);')
        self.assertLess(start_notice.index('pending.applied = true'), start_notice.index('SayToOwner'))
        self.assertIn('pending.noticeId = ++g_nextTradeNoticeId;', SOURCE)

    def test_closed_or_replaced_trade_is_discarded_before_queue_selection(self):
        flush = SOURCE.split('void FlushWhisperQueue(', 1)[1].split('auto chosen = queue.begin()', 1)[0]
        self.assertIn('!IsLiveTradeNotice(speaker, owner, entry->tradeNoticeId)', flush)
        check = SOURCE.split('bool IsLiveTradeNotice(', 1)[1].split('void FlushWhisperQueue', 1)[0]
        for guard in ('noticeId != noticeId', '->second.applied', 'GetTradeData()', 'GetTrader() != owner', 'GetTrader() != bot'):
            self.assertIn(guard, check)

    def test_replacement_window_must_still_contain_the_original_offer(self):
        check = SOURCE.split('bool IsLiveTradeNotice(', 1)[1].split('void FlushWhisperQueue', 1)[0]
        self.assertIn('pending->second.offeredItems', check)
        self.assertIn('GetItem(slot)', check)
        self.assertIn('item->GetGUID() != guid', check)
        self.assertIn('pending.offeredItems.emplace_back(TradeSlots(slot), tradeItem->GetGUID());', SOURCE)

    def test_vital_notices_keep_priority_over_live_offers(self):
        flush = SOURCE.split('void FlushWhisperQueue(', 1)[1].split('void Flush', 1)[0]
        self.assertLess(flush.index('if (hasVital)'), flush.index('if (liveTrade != queue.end())'))

    def test_live_offer_outranks_ambient_queue_without_bypassing_rate_limit(self):
        flush = SOURCE.split('void FlushWhisperQueue(', 1)[1].split('void Flush', 1)[0]
        self.assertIn('if (liveTrade != queue.end())', flush)
        self.assertLess(flush.index('Timing::WhisperGap'), flush.index('if (liveTrade != queue.end())'))
        self.assertIn('chosen = liveTrade;', flush)
        enqueue = SOURCE.split('auto& queue = g_whisperQueue[ownerGuid];', 1)[1].split('return false;', 1)[0]
        self.assertIn('vital || tradeNoticeId || queue.size()', enqueue)
