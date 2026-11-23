"""Integration contracts for exact companion trades and their supply catalog."""
import unittest
from pathlib import Path

SRC = Path(__file__).resolve().parents[1] / 'src'
WORLD = (SRC / 'mod_companion_questsync.cpp').read_text(encoding='utf-8')
POLICY = (SRC / 'CuratedDelivery.inc').read_text(encoding='utf-8')
PICKER = (SRC / 'SupplyPicker.inc').read_text(encoding='utf-8') if (SRC / 'SupplyPicker.inc').exists() else ''


class SpecificDelivery(unittest.TestCase):
    def test_catalog_counts_the_exact_offer_not_all_owned_items(self):
        self.assertIn('Supply row {} {} {} {} {}:', PICKER)
        self.assertIn('Supply end {} {}', PICKER)
        self.assertIn('PlanDelivery(bot, owner, false, false, { entry })', PICKER)

    def test_selected_entry_is_preserved_before_fill_and_on_accept(self):
        self.assertIn('false, pending.supply)', WORLD)
        self.assertIn('pending.supply = supply', WORLD)
        self.assertIn('item->GetEntry() != supply.entry', POLICY)
        self.assertIn('DeliveryRuleFor(bot, owner, item, pending.curated, pending.supply)', POLICY)

    def test_craft_uses_held_materials_and_preserves_preexisting_product(self):
        body = PICKER.split('static void HandleSupplyAction(', 1)[-1].split('static bool HandleNamedSupply(', 1)[0]
        self.assertIn('SupplyCraftReady(bot, owner, plan)', body)
        self.assertIn('~TRIGGERED_IGNORE_POWER_AND_REAGENT_COST', body)
        self.assertIn('supply.keep = bot->GetItemCount(entry)', body)
        self.assertIn('after - supply.keep < batch', body)
        self.assertIn('supply.fresh = batch', body)
        self.assertNotIn('StoreNewItem(', body)
        self.assertIn('SupplyCraftRule(supply.keep, supply.fresh)', POLICY)

    def test_action_uses_the_owners_live_binding_and_guarded_trade(self):
        handler = PICKER.split('static bool HandleNamedSupply(', 1)[1]
        self.assertIn('FindCompanions(ownerGuid, bound)', handler)
        self.assertIn('candidate->GetName() == tokens.back()', handler)
        self.assertIn('HasSupplyAuthority(candidate, owner, true)', handler)
        self.assertIn('#ifndef AC_TRADE_OFFER_GUARD', handler)
        action = PICKER.split('static void HandleSupplyAction(', 1)[1].split('#endif', 1)[0]
        self.assertLess(action.index('CanTradeSurplusNow'), action.index('bot->CastSpell'))
        self.assertIn('OpenTradeSurplus(bot, owner, /*announce=*/true, /*curated=*/false, supply, opened, nonce)', action)

    def test_named_trade_checks_live_authority_at_acceptance(self):
        self.assertIn('HasSupplyAuthority(source, target, true)', WORLD)
        control = WORLD.split('static bool IsControlledCompanion(Player* bot, Player* owner)\n', 1)[1].split('static bool HasSupplyAuthority(', 1)[0]
        self.assertIn('GetSession()->IsBot()', control)
        self.assertIn('CancelBoundDelivery(botGuid, owner);', WORLD)
        self.assertIn('CancelBoundDelivery(botGuid, player);', WORLD)

    def test_craft_preflights_one_exact_source_before_cast(self):
        action = PICKER.split('static void HandleSupplyAction(', 1)[1].split('#endif', 1)[0]
        for precondition in ('PreflightSupplyCraft(', 'InitiateDeliveryTrade('):
            self.assertIn(precondition, action)
            self.assertLess(action.index(precondition), action.index('bot->CastSpell'))
        self.assertIn('canCreateExtraItems(', PICKER)
        self.assertIn('supply.sourcePos', POLICY)

    def test_binding_requires_custody_and_supply_also_requires_the_players_own_bot_manager(self):
        binder = WORLD.split('static bool HandleBindCommand(', 1)[1].split('static bool HandleUnbindCommand(', 1)[0]
        self.assertLess(binder.index('ResolveOwnedCompanion(ownerGuid'), binder.index('SetCompanionTracking(ownerGuid'))
        self.assertNotIn('INSERT INTO', binder)
        verifier = WORLD.split('static bool HasSupplyAuthority(Player* bot, Player* owner, bool authoritative)\n', 1)[1].split('return true;', 1)[0]
        self.assertIn('IsControlledCompanion(bot, owner)', verifier)
        self.assertIn('HasCurrentCustody(ownerGuid, botGuid, true)', verifier)
        self.assertIn('manager->GetPlayerBot(bot->GetGUID()) == bot', WORLD)
