"""The strategic layer: a translation table, a threat lever, pets, and one place to look.

Every rule here exists because a bug showed the old shape was wrong:

- the role table was class-blind, so a druid stayed a bear;
- `mine` whispered a target every two seconds when the AI already persists it;
- "it cannot taunt" was mistaken for "it cannot pull aggro";
- hunter and voidwalker pets taunted whatever the role said;
- every diagnosis needed playerbots' source open.
"""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src" / "mod_companion_questsync.cpp").read_text(encoding="utf-8")
UI = (ROOT / "addon" / "CompanionSync" / "UI.lua").read_text(encoding="utf-8")
CORE = (ROOT / "addon" / "CompanionSync" / "Core.lua").read_text(encoding="utf-8")
REVIEW = (ROOT / "docs" / "companion-behaviour-review.md").read_text(encoding="utf-8")
ROLES = (ROOT / "docs" / "companion-roles.md").read_text(encoding="utf-8")


def slice_from(anchor, length=2600):
    at = SOURCE.index(anchor)
    return SOURCE[at:at + length]


class TranslationTable(unittest.TestCase):
    def test_class_rules_are_data_not_branches(self):
        body = slice_from("    ClassRule constexpr kClassRules[] = {", 900)
        self.assertIn('{ CLASS_DRUID, "dps",     "bear", BOT_STATE_COMBAT, false, 0 },', body)
        self.assertIn('{ CLASS_DRUID, "tank",    "bear", BOT_STATE_COMBAT, true,  5487 }', body)
        self.assertIn('{ CLASS_DRUID, "dps",     "cat",  BOT_STATE_COMBAT, true,  768  }', body)

    def test_a_rule_with_a_spell_requirement_is_checked(self):
        body = slice_from("    std::vector<RoleStrategyEdit> RoleStrategyEdits(", 2000)
        self.assertIn("bot->HasSpell(rule.needsSpell)", body)
        self.assertIn("bot->getClass() == rule.klass", body)

    def test_quiet_is_a_threat_edit_for_dps_and_support_only(self):
        body = slice_from("    std::vector<RoleStrategyEdit> RoleStrategyEdits(", 2000)
        self.assertIn('edits.push_back({ "threat", BOT_STATE_COMBAT, true });', body)
        self.assertIn('if (quiet && (role == "dps" || role == "support"))', body)


class TargetPin(unittest.TestCase):
    def test_the_pin_is_event_driven_not_polled(self):
        body = slice_from("    void ApplyEngagement(Player* owner, Player* bot)", 6000)
        self.assertIn("uint32& pinned = g_pinnedTarget[botGuid];", body)
        self.assertIn("if (wantTarget != pinned)", body)
        self.assertNotIn('HandleCommand(CHAT_MSG_WHISPER, "attack"', body)

    def test_the_pin_uses_the_ai_input_not_a_swing(self):
        body = slice_from("    void ApplyEngagement(Player* owner, Player* bot)", 6000)
        self.assertIn('ai->DoSpecificAction("attack my target");', body)

    def test_leaving_mine_stops_pinning(self):
        body = slice_from("    void ApplyEngagement(Player* owner, Player* bot)", 6000)
        self.assertIn("g_pinnedTarget.erase(botGuid)", body)


class Pets(unittest.TestCase):
    def test_taunt_abilities_are_named(self):
        body = slice_from("    bool IsTauntAbility(uint32 spellId)", 600)
        for ability in ("Growl", "Torment", "Suffering", "Anguish"):
            self.assertIn('"%s"' % ability, body)

    def test_only_tanking_keeps_its_taunt_and_only_we_turn_it_back_on(self):
        body = slice_from("    void ReconcilePetTaunt(Player* bot)", 1800)
        self.assertIn('if (role == "tank" || role == "auto")', body)
        self.assertIn("for (uint32 const spellId : off)", body)      # restore what we took
        self.assertIn("pet->ToggleAutocast(info, false);", body)
        self.assertIn("off.push_back(spellId);", body)

    def test_it_runs_with_the_slow_reconcile_not_the_fast_tick(self):
        body = slice_from("    void ReconcileRoles(Player* owner)", 2200)
        self.assertIn("ReconcilePetTaunt(bot);", body)


class Observability(unittest.TestCase):
    def test_inspect_reports_the_setting_the_ledger_and_the_pitfalls(self):
        body = slice_from("    static bool HandleInspectCommand(", 7600)
        self.assertIn("StrategySnapshot(ai)", body)
        self.assertIn("LedgerSnapshot(botGuid)", body)
        self.assertIn("g_engageNotes.find(botGuid)", body)
        self.assertIn("g_petTauntOff.find(botGuid)", body)

    def test_inspect_and_quiet_are_commands(self):
        self.assertIn('{ "inspect",   HandleInspectCommand,', SOURCE)
        self.assertIn('{ "quiet",     HandleQuietCommand,', SOURCE)

    def test_quiet_is_stored_and_read_back(self):
        body = slice_from("    bool SetCompanionQuiet(", 700)
        self.assertIn("return CompanionQuiet(botGuid) == quiet;", body)
        sql = (ROOT / "sql" / "companion-quiet.sql").read_text(encoding="utf-8")
        self.assertIn("ADD COLUMN quiet TINYINT UNSIGNED NOT NULL DEFAULT 0", sql)


class GapClosers(unittest.TestCase):
    """A dps warrior must not charge: the charge is a trigger on the class strategy, not a lever."""

    def test_charge_is_known_to_playerbots_as_a_spell_not_a_strategy(self):
        body = slice_from("    GapCloser constexpr kGapClosers[] = {", 400)
        for rank in ("100", "6178", "11578"):
            self.assertIn(rank, body)
        self.assertIn('"Charge"', body)

    def test_only_tanking_and_auto_keep_it(self):
        body = slice_from("    /// How long a gap closer is muted for.", 2600)
        self.assertIn('bool const shouldMute = (role != "tank" && role != "auto");', body)

    def test_it_is_taken_away_and_verified(self):
        body = slice_from("    /// How long a gap closer is muted for.", 2600)
        # a cooldown, not the spellbook: removing the spell leaves the charge action possible
        # and the warrior never walks in
        self.assertIn("bot->AddSpellCooldown(closer.spell, 0, now + kGapCloserMuteMs);", body)
        self.assertNotIn("removeSpell", body)
        self.assertIn("if (bot->GetSpellCooldownDelay(closer.spell) < kGapCloserMuteMs / 2)", body)

    def test_it_comes_back_the_same_way(self):
        body = slice_from("    /// How long a gap closer is muted for.", 2600)
        self.assertIn("bot->RemoveSpellCooldown(closer.spell, true);", body)

    def test_it_runs_with_the_role_and_on_the_slow_pass(self):
        self.assertIn("ReconcileRoleSpells(bot, role);", SOURCE)
        self.assertIn("ReconcileRoleSpells(bot, CompanionRole(", SOURCE)

    def test_it_is_behind_a_config_switch(self):
        self.assertIn('GetOption<bool>("CompanionQuestSync.SuppressGapClosers", true)', SOURCE)

    def test_inspect_names_the_formation_and_what_it_competes_with(self):
        body = slice_from("    static bool HandleInspectCommand(", 7600)
        self.assertIn("std::string const formation = EffectiveFormation(player, bot);", body)
        self.assertIn("FormationLeash(formation)", body)
        self.assertIn('GetOption<uint32>("WowLegends.BotPitchIn.Radius", 20)', body)
        self.assertIn("formation: off (distance left to playerbots)", body)

    def test_inspect_notices_a_charge_missing_from_the_spellbook(self):
        body = slice_from("    static bool HandleInspectCommand(", 7400)
        self.assertIn("missing from the spellbook (an older ", body)
        self.assertIn("bot->HasSpell(closer.spell)", body)

    def test_inspect_says_what_was_withheld(self):
        body = slice_from("    static bool HandleInspectCommand(", 7600)
        self.assertIn("gap closers withheld", body)


class Addon(unittest.TestCase):
    def test_the_card_offers_quiet(self):
        self.assertIn("card.quietButton", UI)
        self.assertIn('G.Run(".companionsync quiet " .. card.companion.name', UI)

    def test_the_flag_is_parsed(self):
        self.assertIn('if flags:find(":q") then G.Quiet[name] = true end', CORE)


class Documentation(unittest.TestCase):
    def test_the_review_records_what_landed(self):
        self.assertIn("event-driven", REVIEW.lower())
        self.assertIn("inspect", REVIEW.lower())

    def test_the_role_doc_explains_quiet(self):
        self.assertIn("Quiet", ROLES)


if __name__ == "__main__":
    unittest.main()
