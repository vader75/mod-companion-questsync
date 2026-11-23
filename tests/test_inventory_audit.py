"""Regression contracts for companion inventory and trade outcomes."""
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WORLD = (ROOT / 'src/mod_companion_questsync.cpp').read_text(encoding='utf-8')
POLICY = (ROOT / 'src/CuratedDelivery.inc').read_text(encoding='utf-8')
SUPPLY = (ROOT / 'src/SupplyPicker.inc').read_text(encoding='utf-8')


def section(source, start, end):
    return source.split(start, 1)[1].split(end, 1)[0]


class InventoryAudit(unittest.TestCase):
    def test_every_batch_uses_same_quest_safe_reagent_counts_as_named_craft(self):
        produce = section(WORLD, 'ProduceForCompanion(Player*', '/// A free bag position')
        self.assertIn('CraftReagentRequirements(plan.spell)', produce)
        self.assertIn('UsableCraftReagentCount(bot, owner, item)', produce.split('for (uint8 batch', 1)[1])
        self.assertIn('UsableCraftReagentCount(bot, owner, reagent)', SUPPLY)
        self.assertIn('CanReleaseTemplate(bot, owner,', section(WORLD, 'static uint32 UsableCraftReagentCount(', 'static std::vector<std::string> ProduceForCompanion'))

    def test_mail_only_defers_to_an_applied_offer_and_ignores_expired_parcel_latch(self):
        mail = section(WORLD, 'void TryCuratedMail(', '/// Credit')
        self.assertNotIn('CanOfferTownDelivery', mail)
        self.assertIn('g_lastAutoTrade.find(ownerGuid)', mail)
        self.assertIn('mail->expire_time >= mailNow', mail)
        applied = section(WORLD, 'pending.applied = true;', 'the record stays')
        self.assertIn('g_lastAutoTrade[pending.ownerGuid]', applied)
        self.assertIn('RecoverableDeliveryMail(mail, ownerGuid)', POLICY)
        self.assertIn('!mail->IsReturnedMail()', POLICY)

    def test_trade_selects_from_a_stable_order_and_keeps_whole_and_split_candidates_separate(self):
        plan = section(POLICY, ' PlanDelivery(', 'static bool DeliveryStillValid')
        self.assertIn('std::sort(candidates.begin(), candidates.end()', plan)
        self.assertIn('splitOffer.second > wholeQuantity', plan)
        self.assertLess(plan.index('std::sort(candidates'), plan.index('for (Item* item : candidates)'))

    def test_repair_checks_eligible_repair_npc_and_normal_repair_handler(self):
        repair = section(WORLD, 'static bool RepairNearVendor(', 'void RestockNearVendor')
        self.assertIn('GetNPCIfCanInteractWith(c->GetGUID(), UNIT_NPC_FLAG_REPAIR)', repair)
        self.assertIn('HandleRepairItemOpcode(request)', repair)
        self.assertNotIn('1.0f', repair)

    def test_disabled_cleanup_checks_exact_guard_and_does_not_guard_accept_while_disabled(self):
        cleanup = section(WORLD, 'static void CancelPendingDeliveries()', '/// Fill after')
        self.assertIn('CancelPendingDelivery', cleanup)
        single = section(WORLD, 'static void CancelPendingDelivery(', 'static void CancelBoundDelivery(')
        self.assertIn('trade->GetOfferToken() == pending.noticeId', single)
        self.assertIn('CancelPendingDeliveries();', section(WORLD, 'void Refresh(bool reload)', 'void OnPlayerLogout'))
        self.assertIn('return g_cfg.enable && g_tableReady &&', section(WORLD, 'td->SetOfferGuard(noticeId', 'return uint32(pending.offer.size())'))

    def test_completion_requires_core_database_receipt_not_window_disappearance(self):
        receipts = section(WORLD, 'static void ApplyDeliveryReceipts()', 'static void CancelPendingDelivery(')
        self.assertIn('receipt.saved.get()', receipts)
        self.assertIn('committed && receipt.transferred', receipts)
        self.assertIn('"completed"', receipts)
        trade = section(WORLD, 'void ApplyPendingTrades()', '/// Bring companions')
        self.assertIn('"cancelled"', trade)
        self.assertNotIn('"completed"', trade)
        patch = (ROOT / 'patches/azerothcore-trade-offer-commit.patch').read_text(encoding='utf-8')
        self.assertIn('AsyncCommitTransaction(trans).m_future.share()', patch)
        self.assertIn('trader->GetItemCount(entry) == counts.second', patch)
        self.assertLess(patch.index('AsyncCommitTransaction'), patch.index('TRADE_STATUS_TRADE_COMPLETE'))

    def test_actions_and_collect_use_nonce_progress(self):
        self.assertIn('Delivery progress {} {}:{}', WORLD)
        self.assertIn('SendDeliveryProgress(owner, nonce, "requested"', SUPPLY)
        self.assertIn('SendDeliveryProgress(owner, pending.nonce, "ready"', WORLD)
        collect = section(SUPPLY, 'static bool HandleModernCollect(', 'static bool HandleNamedSupply(')
        self.assertIn('ParseSupplyNumber(tokens[2], nonce)', collect)
        self.assertIn('HasSupplyAuthority', collect)
        self.assertIn('SendDeliveryProgress(owner, nonce, "failed"', collect)
        self.assertIn('ResolveCompanion(player, text.substr(8), bound)', WORLD)

    def test_native_guarded_gift_accept_requires_human_opcode_and_full_inventory_delta(self):
        patch = (ROOT / 'patches/azerothcore-trade-offer-commit.patch').read_text(encoding='utf-8')
        accept = section(patch, '+    bool const guardedOffer', '         _player->SaveInventoryAndGoldToDB(trans);')
        self.assertIn('if (guardedOffer && IsBot())\n+        return;', accept)
        self.assertIn('if (guardedOffer)', accept)  # verify legacy guarded offers too
        self.assertIn('info.Status = transferred ? TRADE_STATUS_TRADE_COMPLETE : TRADE_STATUS_TRADE_CANCELED;', patch)
        self.assertIn('AsyncCommitTransaction(trans).m_future.share()', patch)

    def test_receipts_are_advertised_only_if_the_core_provides_them(self):
        hello = section(WORLD, 'void SendSyncHello(Player* owner)', 'void SendHeartbeat(')
        self.assertIn('Sync capabilities 1 {} {}', hello)
        self.assertIn('#ifdef AC_TRADE_OFFER_COMMIT\n        SendProtocolLine(owner, "Delivery receipts 1");\n#endif', hello)


if __name__ == '__main__':
    unittest.main()
