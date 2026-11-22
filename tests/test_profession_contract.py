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
        self.assertIn("CanStoreNewItem", planner)

    def test_auto_produce_blocks_combat_independently_of_sync_configuration(self):
        self.assertRegex(SOURCE, r"if \(g_cfg\.autoProduce && !bot->IsInCombat\(\) && !owner->IsInCombat\(\)")

    def test_missing_materials_are_not_treated_as_vendor_stock(self):
        producer = section("static std::vector<std::string> ProduceForCompanion(", "/// A free bag position")
        self.assertRegex(producer, r"if \(!IsCraftVendorReagent\(item\)\)")
        self.assertNotIn("IsGatherableReagent", producer)


if __name__ == "__main__":
    unittest.main()
