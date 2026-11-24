"""Contracts for the managed bag, delivery, and guarded trade paths."""
import unittest
from pathlib import Path

SOURCE = (Path(__file__).resolve().parents[1] / 'src/mod_companion_questsync.cpp').read_text(encoding='utf-8')
POLICY = (Path(__file__).resolve().parents[1] / 'src/CuratedDelivery.inc').read_text(encoding='utf-8')


class ManagedInventory(unittest.TestCase):
    def test_automatic_vendor_restock_preserves_free_slots(self):
        restock = SOURCE.split('void RestockNearVendor(', 1)[1].split('/// The highest', 1)[0]
        self.assertIn('newSlots > 0', restock)
        self.assertLess(restock.index('CanStoreNewItem'), restock.index('newSlots > 0'))
        self.assertLess(restock.index('CompanionBagReserve + newSlots'), restock.index('StoreNewItem(dest'))
        self.assertIn('!bot->GetItemByPos(slot.pos)', restock)

    def test_bag_pressure_sells_surplus_above_the_recipe_budget(self):
        trim = SOURCE.split('uint32 SellBagSurplus(', 1)[1].split('/// ', 1)[0]
        # A healthy companion is never sold from: the trim only runs below the comfort line.
        self.assertLess(trim.index('GetFreeInventorySpace() >= CompanionBagComfort'), trim.index('HandleSellItemOpcode'))
        # Professions survive the trim: one recipe's worth of a vendor reagent, two stacks of a material.
        self.assertIn('IsCraftVendorReagent(entry) ? CompanionReagentKeep : CompanionMatKeep', trim)
        self.assertLess(trim.index('kept < keep'), trim.index('HandleSellItemOpcode'))
        # Nothing the player handed over deliberately, or anything bound, is sold.
        for guard in ('ITEM_CLASS_TRADE_GOODS', 'IsInTrade()', 'IsRefundable()', 'CanReleaseItem(bot, owner, item)'):
            self.assertIn(guard, trim)
        for constant in ('CompanionBagComfort = 16', 'CompanionReagentKeep = 5', 'CompanionMatKeep = 10'):
            self.assertIn(constant, SOURCE)

    def test_level_gear_refreshes_a_kit_that_lags_the_level(self):
        refresh = SOURCE.split('uint32 RefreshLevelGear(', 1)[1].split('/// ', 1)[0]
        # A kit that matches the level is never touched; only a lagging one is re-geared.
        self.assertIn('average + CompanionGearLag >= level', refresh)
        self.assertIn('CompanionGearLag = 4', SOURCE)
        # The factory needs inventory room for the swap and a real level to dress for.
        self.assertIn('bot->GetFreeInventorySpace() < CompanionBagReserve', refresh)
        self.assertIn('level < 5', refresh)
        # Incremental: upgrade slots only, never a bare re-roll over the whole character.
        self.assertIn('InitEquipment(/*incremental=*/true)', refresh)
        self.assertIn('CompanionQuestSync.LevelGear', SOURCE)
        # Re-gearing happens on claim and on the periodic pass, not only when asked.
        self.assertGreaterEqual(SOURCE.count('RefreshLevelGear('), 3)

    def test_outgrown_gear_is_sold_instead_of_staying_in_the_bag(self):
        trim = SOURCE.split('uint32 SellBagSurplus(', 1)[1].split('/// ', 1)[0]
        # Playerbots auto-stores the piece it replaces, so the bag would keep every old set.
        for guard in ('ITEM_CLASS_WEAPON', 'ITEM_CLASS_ARMOR',
                      'RequiredLevel) + 2 >= level', 'Quality > ITEM_QUALITY_UNCOMMON'):
            self.assertIn(guard, trim)
        # A profession tool is never outgrown: the companion still needs its pick and knife.
        self.assertIn('ProfessionToolItems(bot->getClass())', trim)
        self.assertIn('tools.count(item->GetEntry())', trim)

    def test_consumables_and_loose_bags_are_capped_like_a_player_would(self):
        trim = SOURCE.split('uint32 SellBagSurplus(', 1)[1].split('/// ', 1)[0]
        self.assertIn('CompanionConsumableStacks = 2', SOURCE)
        self.assertIn('keptStacks++ >= CompanionConsumableStacks', trim)
        self.assertIn('ITEM_CLASS_CONTAINER && proto->SubClass == ITEM_SUBCLASS_CONTAINER', trim)

    def test_restock_stops_buying_into_a_tight_pack(self):
        restock = SOURCE.split('void RestockNearVendor(', 1)[1].split('/// The highest', 1)[0]
        self.assertIn('GetFreeInventorySpace() < CompanionBagComfort', restock)
        self.assertLess(restock.index('GetFreeInventorySpace() < CompanionBagComfort'),
                        restock.index('StoreNewItem(dest'))

    def test_a_tight_pack_without_a_merchant_posts_spare_home(self):
        trim = SOURCE.split('uint32 SellBagSurplus(', 1)[1].split('/// ', 1)[0]
        self.assertIn('MailboxNearby(bot)', trim)
        self.assertLess(trim.index('MailboxNearby(bot)'), trim.index('MailHome(owner, bot, mailbox, attachments)'))
        self.assertIn('attachments.size() > 3', trim)   # one letter's worth at a time

    def test_a_companion_hired_high_is_taught_its_level(self):
        catch_up = SOURCE.split('void EnsureLevelProgress(', 1)[1].split('/// ', 1)[0]
        # Nothing a trainer sells before five, and the sync is idempotent per level.
        self.assertIn('level < 5', catch_up)
        self.assertIn('g_taughtLevel[bot->GetGUID().GetCounter()]', catch_up)
        self.assertIn('taught == level', catch_up)
        # Spells and talents come from the trainer sync; professions from the pair sync.
        self.assertIn('TeachTrainerContent(bot, owner)', catch_up)
        self.assertIn('TeachProfessions(bot, owner)', catch_up)
        # It runs at claim and on the periodic pass, not only on a level-up hook.
        self.assertGreaterEqual(SOURCE.count('EnsureLevelProgress('), 3)

    def test_corpse_loot_reserve_preserves_stacks_skinning_and_protected_items(self):
        patch = (Path(__file__).resolve().parents[1] /
                 'patches/azerothcore-companion-loot-reserve.patch').read_text(encoding='utf-8')
        self.assertLess(patch.index('CanStoreNewItem'), patch.index('VetoNewStack(this,'))
        self.assertLess(patch.index('VetoNewStack(this,'), patch.index('StoreNewItem(dest, item->itemid, true'))
        for guard in ('!GetItemByPos(slot.pos)', 'loot->loot_type == LOOT_CORPSE',
                      '!source->GetCreatureTemplate()->SkinLootId', '!qitem && !conditem && !item->rollWinnerGUID'):
            self.assertIn(guard, patch)
        reserve = SOURCE.split('bool ReserveCompanionLootSlot(', 1)[1].split('\n#endif', 1)[0]
        for guard in ('GetFreeInventorySpace() > CompanionBagReserve',
                      'proto->SubClass != ITEM_SUBCLASS_JUNK', '!= ITEM_USAGE_VENDOR',
                      'CanReleaseTemplate(bot, owner, proto)', '"always loot list"',
                      'IsControlledCompanion(bot, owner)', 'HasCurrentCustody('):
            self.assertIn(guard, reserve)

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
        # rsplit: the file declares MailHome above its use, and the definition is the last one.
        mail = SOURCE.rsplit('uint32 MailHome(', 1)[1].split('ObjectGuid MailboxNearby(', 1)[0]
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
