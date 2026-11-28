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
        # Professions survive the trim: one recipe's worth of a vendor reagent, two stacks of a
        # material - and none of the raw gathering stock once no-gather mode retires it.
        self.assertIn('IsCraftVendorReagent(entry) ? CompanionReagentKeep', trim)
        self.assertIn('? 0 : CompanionMatKeep', trim)
        self.assertLess(trim.index('kept < keep'), trim.index('HandleSellItemOpcode'))
        # Nothing the player handed over deliberately, or anything bound, is sold.
        for guard in ('ITEM_CLASS_TRADE_GOODS', 'IsInTrade()', 'IsRefundable()', 'CanReleaseItem(bot, owner, item)'):
            self.assertIn(guard, trim)
        for constant in ('CompanionBagComfort = 16', 'CompanionReagentKeep = 5', 'CompanionMatKeep = 10'):
            self.assertIn(constant, SOURCE)

    def test_level_gear_refreshes_a_kit_that_lags_the_level(self):
        # the definition, not the forward declaration the spec code needs
        refresh = SOURCE.split('uint32 RefreshLevelGear(Player* owner, Player* bot, bool specChanged)', 1)[1].split('/// ', 1)[0]
        # A kit that matches the level is never touched; only a lagging one is re-geared.
        self.assertIn('!specChanged && pieces && average + CompanionGearLag >= level', refresh)
        self.assertIn('CompanionGearLag = 4', SOURCE)
        # The factory needs inventory room for the swap and a real level to dress for.
        self.assertIn('bot->GetFreeInventorySpace() < CompanionBagReserve', refresh)
        self.assertIn('level < 5', refresh)
        # Incremental: upgrade slots only, never a bare re-roll over the whole character.
        self.assertIn('InitEquipment(/*incremental=*/true)', refresh)
        self.assertIn('CompanionQuestSync.LevelGear', SOURCE)
        # Re-gearing happens on claim and on the periodic pass, anchored on the call sites
        # themselves - counting the name also counted the definition.
        self.assertIn('RefreshLevelGear(master, fresh);', SOURCE)
        periodic = SOURCE.split('SellManagedJunk(owner, bot);', 1)[1]
        self.assertIn('RefreshLevelGear(owner, bot);', periodic)

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
        self.assertLess(trim.index('MailboxNearby(bot)'), trim.index('MailHome(owner, bot, mailbox, letter)'))
        # Several letters are fine; a single-letter-or-nothing rule stranded every full bag.
        self.assertIn('kMailAttachments = 12', trim)
        self.assertLess(trim.index('last = now;'), trim.index('MailHome(owner, bot, mailbox, letter)'))

    def test_a_companion_hired_high_is_taught_its_level(self):
        catch_up = SOURCE.split('void EnsureLevelProgress(', 1)[1].split('/// ', 1)[0]
        # Nothing a trainer sells before five, and the sync is idempotent per level.
        self.assertIn('level < 5', catch_up)
        self.assertIn('if (g_taughtLevel[botGuid] == level)', catch_up)
        # The latch lands after the teaching: a pass that could not teach must retry.
        self.assertLess(catch_up.index('TeachProfessions(bot, owner)'),
                        catch_up.index('g_taughtLevel[botGuid] = level;'))
        # Spells and talents come from the trainer sync; professions from the pair sync.
        self.assertIn('TeachTrainerContent(bot, owner)', catch_up)
        self.assertIn('TeachProfessions(bot, owner)', catch_up)
        # It runs at claim and on the periodic pass, not only on a level-up hook.
        self.assertGreaterEqual(SOURCE.count('EnsureLevelProgress('), 3)

    def test_a_ready_hand_in_is_pinned_even_when_the_giver_is_far(self):
        pins = SOURCE.split('std::string BuildPinLine(', 1)[1]
        self.assertIn('if (giver || handInStates || (ready && versioned))', pins)
        self.assertLess(pins.index('if (giver || handInStates || (ready && versioned))'),
                        pins.index("char const style = giver ? 'h' : ready ? 'u' : 'p';"))

    def test_the_lead_me_whisper_says_where_to_go(self):
        block = SOURCE.split('if (!giver && g_cfg.handInMode == 2', 1)[1].split('continue;', 1)[0]
        self.assertIn('ZoneName(spot.zone)', block)
        self.assertIn('MapCoords(spot.x, spot.y, spot.zone)', block)
        self.assertIn('" (" + coords + ")"', block)

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
        sell = SOURCE.split('uint32 SellManagedJunk(', 1)[1].split('/// Sell the *spare* trade goods', 1)[0]
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

    def test_pooled_guids_do_not_inherit_level_latches(self):
        purge = SOURCE.split('void PurgeCompanionState(', 1)[1].split('bool g_custodyReady', 1)[0]
        # Bot guids are pooled: an inherited latch means a fresh companion silently skips
        # its spell, talent and profession catch-up, or waits a level to be re-geared.
        for latch in ('g_taughtLevel.erase(botGuid)', 'g_gearLevel.erase(botGuid)',
                      'g_trimAt.erase(botGuid)', 'g_lastAutoProduce.erase(botGuid)',
                      'g_deadNoticeAt.erase(botGuid)', 'EraseByBot(g_missingCache, botGuid)'):
            self.assertIn(latch, purge)

    def test_restock_and_trim_share_one_recipe_budget(self):
        restock = SOURCE.split('void RestockNearVendor(', 1)[1].split('/// The highest', 1)[0]
        self.assertIn('uint32 const want = CompanionReagentKeep;', restock)
        self.assertNotIn('uint32 const want = 5;', restock)

    def test_a_loose_empty_bag_is_spare_and_a_full_one_is_not(self):
        trim = SOURCE.split('uint32 SellBagSurplus(', 1)[1].split('/// ', 1)[0]
        # Containers are refused by CanReleaseItem, so this branch has to run before it.
        self.assertLess(trim.index('ITEM_CLASS_CONTAINER && proto->SubClass == ITEM_SUBCLASS_CONTAINER'),
                        trim.index('if (!CanReleaseItem(bot, owner, item))'))
        self.assertIn('bag->IsEmpty()', trim)
        self.assertIn('Bag* bag = item->ToBag();', trim)

    def test_trim_sales_reach_the_session_tally(self):
        trim = SOURCE.split('uint32 SellBagSurplus(', 1)[1].split('/// ', 1)[0]
        self.assertIn('itr->second.sold += sold + gearSold;', trim)
        self.assertIn('itr->second.mailed += posted;', trim)


def gear_body(signature: str) -> str:
    """The body of a uint32-returning helper, without relying on doc-comment placement."""
    rest = SOURCE.split(signature, 1)[1]
    end = rest.find(chr(10) + '    uint32 ')
    return rest[:end] if end != -1 else rest


class DisplacedGearGoesToTheVendor(unittest.TestCase):
    """A re-gear must not fill the bags with the pieces it took off.

    playerbots destroys the item it replaces in a slot, so an ordinary upgrade leaves the bags
    alone - but an offhand put down for a two-hander comes back off the arm into the bags, and a
    companion flipping tank/dps sheds one per flip. Those pieces are sold at the next vendor,
    never destroyed, and only pieces this module's own pass displaced are ever marked.
    """

    def test_the_pass_remembers_what_it_was_wearing(self):
        body = gear_body('uint32 RefreshLevelGear(Player* owner, Player* bot, bool specChanged)')
        self.assertIn('std::set<uint64> const wornBefore = WornGuids(bot);', body)
        # the snapshot is taken before the factory runs, or it proves nothing
        self.assertLess(body.index('WornGuids(bot)'), body.index('factory.InitEquipment'))
        worn = fn_body('std::set<uint64> WornGuids(Player* bot)')
        self.assertIn('worn.insert(item->GetGUID().GetRawValue());', worn)

    def test_what_came_off_is_marked_not_deleted(self):
        body = gear_body('uint32 RefreshLevelGear(Player* owner, Player* bot, bool specChanged)')
        self.assertIn('MarkKitDisplaced(bot, wornBefore)', body)
        self.assertNotIn('bot->DestroyItem', body)          # the vendor's job, not the bin's
        mark = fn_body('uint32 MarkKitDisplaced(Player* bot, std::set<uint64> const& wornBefore)')
        self.assertIn('g_staleGear[botGuid].insert(raw).second', mark)
        self.assertIn('item->IsEquipped()', mark)           # still worn is not displaced

    def test_the_vendor_takes_them_whatever_their_quality(self):
        body = gear_body('uint32 SellManagedJunk(Player* owner, Player* bot)')
        self.assertIn('bool const displaced = IsStaleGear(botGuid, item);', body)
        # quality and item-usage tests only guard the ordinary junk path
        guard = body[body.index('if (!displaced)'):]
        self.assertIn('ITEM_USAGE_VENDOR', guard)
        self.assertIn('g_staleGear[botGuid].erase(guid.GetRawValue());', body)

    def test_the_mark_does_not_outlive_the_body(self):
        self.assertIn('g_staleGear.erase(botGuid);', SOURCE)

    def test_a_pass_that_changed_nothing_says_so(self):
        body = gear_body('uint32 RefreshLevelGear(Player* owner, Player* bot, bool specChanged)')
        self.assertIn('no slot needed changing', body)


def fn_body(signature: str, budget: int = 4000) -> str:
    """A window over a helper, wide enough for the assertions without a fragile end anchor."""
    return SOURCE.split(signature, 1)[1][:budget]


class TheRoleDecidesTheShapeOfTheKit(unittest.TestCase):
    """Score cannot see shape. A tank with a two-hander has given up its shield, and the factory
    will happily hand it one because the two-hander scores highest for the spec.

    Only the roles that imply a shape get one: tank on a class that can hold a shield, and healer
    whose hands are full of a melee two-hander. Damage and support are left to best score.
    """

    def test_only_shape_roles_are_asked_for_anything(self):
        need = fn_body('KitNeed KitNeedFor(uint8 cls, std::string const& role)')
        self.assertIn('cls == CLASS_WARRIOR || cls == CLASS_PALADIN || cls == CLASS_SHAMAN', need)
        self.assertIn('return canShield ? KitNeed::Shield : KitNeed::None;', need)
        self.assertIn('role == "healer"', need)
        self.assertIn('return KitNeed::CasterWeapon;', need)
        # a druid tank is a bear and a death knight has no shield slot: no requirement
        self.assertNotIn('CLASS_DRUID', need)
        self.assertNotIn('CLASS_DEATH_KNIGHT', need)
        # and nothing is asked of damage or support
        for role in ('dps', 'support'):
            self.assertNotIn('role == "%s"' % role, need)

    def test_a_staff_is_not_a_melee_two_hander(self):
        body = fn_body('bool RepairKitShape(Player* bot, std::string const& role,')
        melee = body[body.index('bool const meleeTwoHand'):]
        melee = melee[:melee.index(';')]
        for weapon in ('AXE2', 'MACE2', 'SWORD2', 'POLEARM'):
            self.assertIn('ITEM_SUBCLASS_WEAPON_' + weapon, melee)
        self.assertNotIn('STAFF', melee)          # a caster's two-hander is exactly right
        self.assertIn('ITEM_SUBCLASS_WEAPON_STAFF ? "staff"', SOURCE)

    def test_the_hands_change_without_the_bags_filling(self):
        body = fn_body('bool EquipKitPiece(Player* bot, uint8 slot, uint32 itemId,')
        self.assertIn('/*swap=*/true', body)
        refit = fn_body('bool RepairKitShape(Player* bot, std::string const& role,')
        self.assertIn('bot->GetFreeInventorySpace() < 1', refit)   # one slot for the piece it swaps out
        # the sweep that marks displaced pieces runs after the refit, so swapped pieces are vendored
        pass_ = fn_body('uint32 RefreshLevelGear(Player* owner, Player* bot, bool specChanged)', 8000)
        self.assertLess(pass_.index('RepairKitShape(bot'), pass_.index('MarkKitDisplaced(bot, wornBefore)'))

    def test_the_refit_runs_after_the_factory_that_it_corrects(self):
        pass_ = fn_body('uint32 RefreshLevelGear(Player* owner, Player* bot, bool specChanged)', 8000)
        self.assertLess(pass_.index('factory.InitEquipment'), pass_.index('RepairKitShape(bot'))

    def test_candidates_are_scanned_once_per_piece_and_level_band(self):
        body = fn_body('uint32 FindKitItem(Player* bot, KitPiece piece)')
        self.assertIn('uint32 const key = uint32(piece) * 100 + level / 5;', body)
        self.assertIn('GetItemTemplateStoreFast()', body)
        self.assertIn('bot->CanUseItem(proto) == EQUIP_ERR_OK', body)

    def test_inspect_says_what_the_hands_are(self):
        self.assertIn('"  kit: {} ', SOURCE)
        self.assertIn('KitShapeText(bot)', SOURCE)


class AWorldUpgradeIsRespected(unittest.TestCase):
    """playerbots equips a looted item the moment it scores better - which is wanted, and which
    happens outside our gear pass.

    So the shape is watched on the slow pass too (throttled), a refit prefers what the companion
    already carries over anything new, and the piece a world drop beat is marked for the vendor
    instead of rotting in the bags.
    """

    def test_the_shape_is_watched_between_gear_passes(self):
        self.assertIn("WatchKitShape(owner, bot);", SOURCE)
        body = fn_body('void WatchKitShape(Player* owner, Player* bot)')
        self.assertIn("if (last && now - last < 60)", body)
        self.assertIn("RefitRoleHands(bot, role)", body)
        refit = fn_body('void RefitRoleHands(Player* bot, std::string const& role)')
        self.assertIn("KitNeedFor(bot->getClass(), role) == KitNeed::None", refit)
        # and it fits the same shape the pass does
        self.assertIn("RepairKitShape(bot, role)", refit)

    def test_a_refit_prefers_what_it_already_carries(self):
        body = fn_body('bool FitKitPiece(Player* bot, uint8 slot, KitPiece piece,')
        self.assertIn("FindCarriedKitPiece(bot, piece)", body)
        self.assertLess(body.index("FindCarriedKitPiece"), body.index("FindKitItem"))
        carried = fn_body('Item* FindCarriedKitPiece(Player* bot, KitPiece piece)')
        self.assertIn("item->IsEquipped()", carried)          # never "finds" what is already worn
        self.assertIn("bot->CanUseItem(proto)", carried)

    def test_a_world_upgrade_leaves_a_cast_off_that_goes_to_the_vendor(self):
        body = fn_body('void MarkSupersededGear(Player* owner, Player* bot)')
        self.assertIn("ProfessionToolItems(bot->getClass())", body)     # tools are not cast-offs
        self.assertIn("CanReleaseItem(bot, owner, item)", body)         # curated and bound are not either
        self.assertIn("worn->GetTemplate()->ItemLevel <= proto->ItemLevel", body)
        self.assertIn("SlotForInventoryType(proto->InventoryType)", body)
        self.assertIn("g_staleGear[botGuid].insert(item->GetGUID().GetRawValue());", body)

    def test_the_slot_map_covers_the_hands(self):
        body = fn_body('int SlotForInventoryType(uint8 type)')
        self.assertIn("case INVTYPE_SHIELD: case INVTYPE_HOLDABLE: case INVTYPE_WEAPONOFFHAND: return EQUIPMENT_SLOT_OFFHAND;", body)
        self.assertIn("case INVTYPE_WEAPON: case INVTYPE_2HWEAPON", body)
        self.assertIn("default: return -1;", body)
