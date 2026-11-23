"""Contracts at the boundaries between companion offers and AzerothCore trade."""
import unittest
from pathlib import Path

SOURCE = (Path(__file__).resolve().parents[1] / 'src/mod_companion_questsync.cpp').read_text(encoding='utf-8')
FILL = SOURCE.split('void ApplyPendingTrades()', 1)[1].split('/// Bring companions', 1)[0]
POLICY = (Path(__file__).resolve().parents[1] / 'src/CuratedDelivery.inc').read_text(encoding='utf-8')
PLAN = POLICY.split(' PlanDelivery(', 1)[1].split('static bool DeliveryStillValid', 1)[0]


class TradeSafetyContract(unittest.TestCase):
    def test_plan_only_records_inventory_values(self):
        self.assertNotIn('SplitItem(', PLAN)
        self.assertIn('item->GetGUID()', PLAN)

    def test_fill_resolves_exact_stack_without_reusing_an_offered_item(self):
        self.assertIn('GetItemByGuid(guid)', FILL)
        self.assertIn('trade->HasItem(tradeItem->GetGUID())', FILL)

    def test_live_offer_checks_exact_slots_and_cancels_during_combat(self):
        self.assertIn('MatchesTradeOffer(bot, trade, pending)', FILL)
        self.assertIn('!CanTradeSurplusNow(bot, owner)', FILL)
        self.assertLess(FILL.index('!CanTradeSurplusNow(bot, owner)'), FILL.index('pending.at < 2'))

    def test_empty_or_invalid_trade_closes_and_automatic_no_op_is_quiet(self):
        self.assertIn('CancelSurplusTrade(bot, owner', FILL)
        self.assertIn('OpenTradeSurplus(bot, owner, /*announce=*/false, /*curated=*/true)', SOURCE)

    def test_sweep_does_not_touch_replacement_window(self):
        self.assertIn('trade->GetOfferToken() != pending.noticeId', FILL)
        token_check = FILL.index('trade->GetOfferToken() != pending.noticeId')
        self.assertLess(token_check, FILL.index('CancelSurplusTrade('))

    def test_interrupted_fill_closes_its_guarded_window(self):
        occupied = FILL.split('if (!empty || trade->GetMoney() || trade->GetSpell())', 1)[1]
        self.assertIn('CancelSurplusTrade(bot, owner', occupied.split('g_pendingTrade.erase(itr)', 1)[0])

    def test_accept_guard_requires_exact_counts_and_preserves_reserve(self):
        self.assertIn('td->SetOfferGuard(noticeId', SOURCE)
        self.assertIn('MatchesTradeOffer(bot, trade, pending)', SOURCE)
        self.assertIn('item->GetCount() != count', SOURCE)
        self.assertIn('bot->GetItemCount(entry, false) < count + rule.reserve', POLICY)

    def test_split_checks_bag_compatibility_and_failure_retains_cooldown(self):
        self.assertIn('ItemCanGoIntoBag(item, bag->GetTemplate())', SOURCE)
        self.assertIn('FindFreeBagPos(bot, item->GetTemplate(), nullptr)', SOURCE)
        cancel = SOURCE.split('static void CancelSurplusTrade(', 1)[1].split('void ApplyPendingTrades()', 1)[0]
        self.assertNotIn('g_lastAutoTrade.erase', cancel)

    def test_core_rejection_leaves_trade_state_for_playerbot_failure_detection(self):
        patch = (Path(__file__).resolve().parents[1] / 'patches/azerothcore-trade-offer-guard.patch').read_text()
        self.assertIn('!my_trade->AcceptsOffer() || !his_trade->AcceptsOffer()', patch)
        self.assertIn('my_trade->SetAccepted(false);', patch)
        self.assertIn('his_trade->SetAccepted(false);', patch)
        self.assertNotIn('+        _player->TradeCancel(', patch)
