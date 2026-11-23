"""Contracts for the managed bag, delivery, and guarded trade paths."""
import unittest
from pathlib import Path

SOURCE = (Path(__file__).resolve().parents[1] / 'src/mod_companion_questsync.cpp').read_text(encoding='utf-8')
POLICY = (Path(__file__).resolve().parents[1] / 'src/CuratedDelivery.inc').read_text(encoding='utf-8')


class ManagedInventory(unittest.TestCase):
    def test_quest_bag_rescue_only_sells_confirmed_junk(self):
        tidy = SOURCE.split('uint32 TidyInventory(Player* owner, Player* bot)\n', 1)[1].split('/// Resolve the owner', 1)[0]
        self.assertIn('SellManagedJunk(owner, bot)', tidy)
        self.assertNotIn('DestroyItemCount', tidy)
        self.assertNotIn('MailHome(', tidy)
        self.assertNotIn('"s vendor"', SOURCE)
        sell = SOURCE.split('uint32 SellManagedJunk(', 1)[1].split('void TryEquipUpgrades(', 1)[0]
        self.assertIn('proto->Quality != ITEM_QUALITY_POOR', sell)
        self.assertIn('!= ITEM_USAGE_VENDOR', sell)
        self.assertIn('HandleSellItemOpcode(request)', sell)

    def test_automatic_and_manual_trade_revalidate_the_same_delivery_plan(self):
        self.assertIn('PlanDelivery(bot, owner, pending.curated, false, pending.supply)', SOURCE)
        self.assertIn('DeliveryStillValid(bot, trade->GetTrader(), pending)', SOURCE)
        self.assertIn('CanOfferTownDelivery(owner, bot)', SOURCE)

    def test_automatic_mail_is_curated_and_owner_throttled(self):
        self.assertIn('g_ownerDeliveryMailAt[ownerGuid]', SOURCE)
        self.assertIn('item->CanBeTraded(true)', SOURCE)
        self.assertIn('MailboxNearby(bot)', SOURCE)
        mail = SOURCE.split('void TryCuratedMail(', 1)[1].split('/// Credit', 1)[0]
        self.assertIn('PlanDelivery(bot, owner, true, /*mail=*/true)', mail)
        self.assertIn('IsDeliveryMail(mail, ownerGuid)', mail)
        self.assertIn('mail->sender == ownerGuid && mail->receiver == ownerGuid', POLICY)

    def test_mail_uses_online_recipient_and_never_returns_to_pool_character(self):
        self.assertIn('draft.SendMailTo(trans, MailReceiver(owner)', SOURCE)
        self.assertIn('MailSender(MAIL_NORMAL, owner->GetGUID().GetCounter())', SOURCE)

    def test_mail_checks_realm_policy_before_detaching_any_item(self):
        mail = SOURCE.split('uint32 MailHome(', 1)[1].split('ObjectGuid MailboxNearby(', 1)[0]
        self.assertLess(mail.index('OnPlayerCanSendMail(bot, owner->GetGUID()'), mail.index('MoveItemFromInventory('))

    def test_trade_checks_combined_recipient_capacity_and_exact_splits(self):
        self.assertIn('owner->CanStoreItems(incoming, int(offers.size() + 1)', POLICY)
        self.assertIn('owner->CanStoreItem(NULL_BAG, NULL_SLOT, dest, entry, spare)', POLICY)

    def test_delivery_preserves_the_companions_own_upgrade(self):
        self.assertIn('GetValue<ItemUsage>("item upgrade", key)', POLICY)
        self.assertIn('ITEM_USAGE_EQUIP || usage == ITEM_USAGE_REPLACE || usage == ITEM_USAGE_BAD_EQUIP', POLICY)

    def test_automatic_offer_rechecks_town_stop_and_expires(self):
        self.assertIn('!IsTownDeliveryStop(owner, bot)', SOURCE)
        self.assertIn('IsTownDeliveryStop(target, source)', SOURCE)
        self.assertGreaterEqual(SOURCE.count('Timing::TownOfferTimeout'), 2)

    def test_delivery_protects_quest_items_and_counts_unclaimed_mail(self):
        self.assertIn('!QuestNeedsItem(bot, proto->ItemId)', POLICY)
        self.assertIn('!QuestNeedsItem(owner, proto->ItemId)', POLICY)
        self.assertIn('owner->GetMItem(attachment.item_guid)', POLICY)

    def test_only_requested_products_can_be_automatic_supplies(self):
        curated = POLICY.split('if (curated)', 1)[1].split('for (uint32 reagent', 1)[0]
        self.assertIn('!desired->second.count(entry)', curated)
        self.assertIn('ITEM_CLASS_CONSUMABLE', curated)
