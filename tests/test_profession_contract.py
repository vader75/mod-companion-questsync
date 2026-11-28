"""Regression checks for the companion producer's game-engine integration."""
import re
import unittest
from pathlib import Path

SOURCE = (Path(__file__).resolve().parents[1] / "src/mod_companion_questsync.cpp").read_text(encoding="utf-8")


def section(start, end):
    return SOURCE.split(start, 1)[1].split(end, 1)[0]


class ProfessionContract(unittest.TestCase):
    def test_production_checks_attained_skill_and_known_recipe(self):
        producer = section("static std::vector<std::string> ProduceForCompanion(", "/// A free bag position")
        planner = section("static std::vector<CraftPlan> PlanCrafts(", "/// A free bag position") if "static std::vector<CraftPlan> PlanCrafts(" in SOURCE else producer
        self.assertIn("GetPureSkillValue", planner)
        self.assertIn("HasSpell", planner)
        self.assertRegex(SOURCE, r"\{\s*3447,\s*929,\s*110\s*\}")  # Healing Potion
        self.assertRegex(SOURCE, r"\{\s*2165,\s*2313,\s*100\s*\}")  # Medium Armor Kit

    def test_producer_pays_the_core_reagent_cost(self):
        producer = section("static std::vector<std::string> ProduceForCompanion(", "/// A free bag position")
        self.assertIn("~TRIGGERED_IGNORE_POWER_AND_REAGENT_COST", producer)
        self.assertNotRegex(producer, r"CastSpell\([^;]+, true\)")

    def test_manual_produce_checks_trade_and_emits_one_report(self):
        handler = section("static bool HandleProduceCommand(", "/// .companionsync learn")
        self.assertIn("GetTradeData()", handler.split("ProduceForCompanion", 1)[0])
        self.assertNotIn("SayToOwner(", handler)
        self.assertNotRegex(handler, r"for \([^\n]+ProduceForCompanion")

    def test_every_batch_checks_output_space_before_consuming_reagents(self):
        producer = section("static std::vector<std::string> ProduceForCompanion(", "/// A free bag position")
        batch = producer.split("for (uint8 batch", 1)[1].split("bot->CastSpell", 1)[0]
        self.assertIn("CanStoreCraftOutput(bot, plan)", batch)
        self.assertIn("CanStoreNewItem", section("static bool CanStoreCraftOutput(", "static std::vector<CraftPlan> PlanCrafts("))

    def test_plan_checks_vendor_bag_space_before_choosing_a_recipe(self):
        planner = section("static std::vector<CraftPlan> PlanCrafts(", "static std::vector<std::string> ProduceForCompanion(")
        self.assertIn("CanStoreCraftOutput(bot, candidate)", planner)
        # the bag-space check lives in the grantable helper the planner consults
        self.assertIn("CraftReagentGrantable(bot, item, need - have)", planner)
        self.assertIn("CanStoreNewItem", section("static bool CraftReagentGrantable(", "// Prefer the best eligible recipe"))

    def test_auto_produce_blocks_combat_independently_of_sync_configuration(self):
        self.assertIn('if (g_cfg.autoProduce && CanManageBags(owner, bot))', SOURCE)
        gate = section('bool CanManageBags(', 'uint32 SellManagedJunk(')
        for actor in ('owner', 'bot'):
            self.assertIn(f'!{actor}->IsInCombat()', gate)
            self.assertIn(f'!{actor}->GetTradeData()', gate)

    def test_missing_materials_are_not_treated_as_vendor_stock(self):
        # the scoping lives in the shared granter the quiet craft and orders use
        granter = section("static bool GrantCraftReagents(", "/// The quiet craft (and the manual Produce report)")
        self.assertRegex(granter, r"IsCraftVendorReagent\(item\) && g_cfg\.professionNoGather < 2")
        self.assertNotIn("IsGatherableReagent", granter)

    def test_no_gather_mode_is_a_three_way_gate(self):
        # off / log / act, exactly like the other staged behaviour gates
        self.assertIn('GetOption<uint32>("CompanionQuestSync.ProfessionNoGather", 0)', SOURCE)
        self.assertIn("if (g_cfg.professionNoGather > 2)", SOURCE)
        teach = section("std::string TeachProfessions(", "/// Distance: presets")
        # act: the skill is set to the cap it can already hold, and the mode takes
        # the gather strategy off; log: the same decision spoken, nothing touched
        self.assertIn("bot->SetSkill(skill, 0, uint16(cap), uint16(cap))", teach)
        self.assertIn('ai->ChangeStrategy("-gather", BOT_STATE_NON_COMBAT)', teach)
        self.assertIn("ProfessionNoGather = 1: logging only", teach)
        # the keep-trying guarantees that only make sense for gatherers stand down
        tools = section("static void EnsureProfessionTools(", "#endif")
        self.assertRegex(tools, r"professionNoGather >= 2\)\s*\n\s*return")
        catchup = section("static void CatchUpGatherSkill(", "/// A gathering profession is only as good as its tool")
        self.assertRegex(catchup, r"professionNoGather >= 2\)\s*\n\s*return")

    def test_gather_strategy_follows_the_mode_every_pass(self):
        # playerbots drops strategies on spec/level changes, so the mode is
        # re-asserted in the keep-trying slot, not only at teach time
        slot = section("EnsureProfessionTools(bot);", "TeachCraftRecipes(bot, ClassProfessionPair(bot->getClass()).second);")
        self.assertIn('ai->HasStrategy("gather", BOT_STATE_NON_COMBAT)', slot)
        self.assertIn('wantGather ? "+gather" : "-gather"', slot)

    def test_quiet_craft_cadence_and_budget_are_configured(self):
        self.assertIn('GetOption<uint32>("CompanionQuestSync.ProduceIntervalSeconds", 180)', SOURCE)
        self.assertIn('GetOption<uint32>("CompanionQuestSync.ProduceMaxPerInterval", 4)', SOURCE)
        self.assertRegex(SOURCE, r"now - lastAuto >= g_cfg\.produceInterval")
        self.assertIn("ProduceForCompanion(bot, owner, g_cfg.produceMaxItems)", SOURCE)
        producer = section("static std::vector<std::string> ProduceForCompanion(", "/// A free bag position")
        self.assertRegex(producer, r"if \(maxItems && made >= maxItems\)")
        # the manual button is an explicit ask: no cap
        manual = section("static bool HandleProduceCommand(", "/// .companionsync learn")
        self.assertIn("ProduceForCompanion(bot, player)", manual)

    def test_combat_first_orders_the_lanes(self):
        planner = section("static std::vector<CraftPlan> PlanCrafts(", "static std::vector<std::string> ProduceForCompanion(")
        self.assertIn("if (g_cfg.produceCombatFirst)", planner)
        self.assertIn("std::stable_sort(plans.begin(), plans.end()", planner)
        priority = section("static uint8 CraftLanePriority(", "// Is a reagent the craft may be supplied")
        self.assertIn("SKILL_ALCHEMY", priority)          # potions first
        self.assertIn("SKILL_BLACKSMITHING || skill == SKILL_LEATHERWORKING", priority)  # then stones and kits
        # smelted stock is the fallback rank
        self.assertRegex(priority, r"return 2;")

    def test_no_gather_mode_sells_dead_gathering_stock(self):
        # raw herbs/ores/hides keep nothing once the gathering half is retired:
        # the tidy pass's budget hands them to the vendor (or mail) lane
        self.assertIn("static bool IsGatheredStock(uint32 item)", SOURCE)
        tidy = section("/// Sell the *spare* trade goods when the bags are tight.", "/// Wear upgrades even when the bags are not full.")
        self.assertIn("professionNoGather >= 2 && IsGatheredStock(entry) ? 0 : CompanionMatKeep", tidy)
        # the producer's grant path is unchanged by this: grants stay scoped to the craft
        producer = section("static std::vector<std::string> ProduceForCompanion(", "/// A free bag position")
        self.assertNotIn("IsGatheredStock", producer)

    def test_supplies_stand_in_for_the_shopping_trip(self):
        # the catalog treats a grantable shortfall as ready - and the on-demand order
        # grants the missing reagents before its craft, the same as the quiet path
        self.assertIn("static bool CraftReagentsReady(Player* bot, Player* owner, SpellInfo const* spell)", SOURCE)
        self.assertIn("static bool GrantCraftReagents(Player* bot, Player* owner, SpellInfo const* spell)", SOURCE)
        # one grant implementation, two callers: the quiet craft here, the on-demand
        # order in the supply picker (asserted below)
        self.assertEqual(SOURCE.count("GrantCraftReagents(bot, owner, plan.spell)"), 1)
        producer = section("static std::vector<std::string> ProduceForCompanion(", "/// A free bag position")
        self.assertIn("GrantCraftReagents(bot, owner, plan.spell);", producer)
        self.assertNotIn("bot->SendNewItem(bought, give, true, false, true)", producer)  # the loop lives in the helper
        # the scoping stays: explicit shop supplies at any mode, all reagents at mode 2
        helper = section("static bool GrantCraftReagents(", "/// The quiet craft (and the manual Produce report)")
        self.assertIn("IsCraftVendorReagent(item) && g_cfg.professionNoGather < 2", helper)
        # the supply picker: readiness is grant-aware, and the grant lands before the check
        picker = (Path(__file__).resolve().parents[1] / "src/SupplyPicker.inc").read_text(encoding="utf-8")
        self.assertIn("if (!CraftReagentsReady(bot, owner, plan.spell))", picker)
        grant = picker.find("GrantCraftReagents(bot, owner, plan.spell);")
        ready = picker.find("if (!SupplyCraftReady(bot, owner, plan)")
        self.assertGreater(grant, 0)
        self.assertLess(grant, ready)


if __name__ == "__main__":
    unittest.main()
