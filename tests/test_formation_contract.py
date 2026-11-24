"""Formation contract: the presets, the spacing, and the hand-back to the bot's own AI.

Source-text checks, like the rest of this suite: they pin the *decisions* (which distance,
which angle formula, when to keep hands off) rather than the shape of the code.
"""

import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src" / "mod_companion_questsync.cpp").read_text(encoding="utf-8")
CONF = (ROOT / "conf" / "mod_companion_questsync.conf.dist").read_text(encoding="utf-8")
DOC = (ROOT / "docs" / "companion-formation.md").read_text(encoding="utf-8")
UI = (ROOT / "addon" / "CompanionSync" / "UI.lua").read_text(encoding="utf-8")
CORE = (ROOT / "addon" / "CompanionSync" / "Core.lua").read_text(encoding="utf-8")


def slice_from(anchor, length=2600):
    start = SOURCE.index(anchor)
    return SOURCE[start:start + length]


class PresetContract(unittest.TestCase):
    def test_presets_are_the_documented_distances(self):
        self.assertIn("constexpr float FormationTightSpacing  = 4.0f;", SOURCE)
        self.assertIn("constexpr float FormationLooseSpacing  = 12.0f;", SOURCE)
        self.assertIn("constexpr float FormationSpreadSpacing = 20.0f;", SOURCE)

    def test_unknown_preset_falls_back_to_leaving_the_ai_alone(self):
        body = slice_from("    float FormationSpacing(std::string const& preset)")
        self.assertIn('return 0.0f;', body)

    def test_config_option_is_read_and_defaults_off(self):
        self.assertIn('"CompanionQuestSync.Formation", "off"', SOURCE)
        self.assertIn("CompanionQuestSync.Formation = off", CONF)
        self.assertIn("`Formation` | `off`", (ROOT / "README.md").read_text(encoding="utf-8"))


class SpacingContract(unittest.TestCase):
    def test_angle_mirrors_the_playerbots_follow_formula(self):
        body = slice_from("    void ApplyFormation(Player* owner)")
        self.assertIn("1.75f * float(i + 1) / total", body)
        self.assertIn("0.125f", body)
        self.assertIn("M_PI", body)

    def test_distance_is_the_only_thing_managed_and_it_is_the_preset(self):
        body = slice_from("    void ApplyFormation(Player* owner)")
        self.assertIn("MoveFollow(owner, spacing, angle)", body)
        self.assertNotIn("MovePoint(", body)

    def test_combat_hands_movement_back_to_the_bot(self):
        body = slice_from("    void ApplyFormation(Player* owner)")
        self.assertIn("bot->IsInCombat()", body)
        # and the module must not keep asking while it fights
        self.assertLess(body.index("IsInCombat()"), body.index("MoveFollow"))

    def test_other_reasons_to_leave_movement_alone(self):
        body = slice_from("    void ApplyFormation(Player* owner)")
        for guard in ("IsInWorld()", "IsAlive()", "GetMapId() != owner->GetMapId()",
                      "IsInFlight()", "GetVehicle()"):
            self.assertIn(guard, body)

    def test_it_does_not_restart_the_generator_on_every_tick(self):
        body = slice_from("    void ApplyFormation(Player* owner)")
        self.assertIn("std::fabs(bot->GetDistance(owner) - spacing) <= 2.5f", body)

    def test_ring_index_is_stable_across_resummons(self):
        body = slice_from("    void ApplyFormation(Player* owner)")
        self.assertIn("std::sort(bots.begin(), bots.end())", body)

    def test_tick_calls_it_for_every_owner(self):
        tick = slice_from("SendHeartbeat(player);", 200)
        self.assertIn("ApplyFormation(player);", tick)


class CommandContract(unittest.TestCase):
    def test_command_is_registered_for_players(self):
        self.assertIn('{ "formation", HandleFormationCommand, SEC_PLAYER, Console::No },', SOURCE)

    def test_command_rejects_a_typo_instead_of_silently_ignoring_it(self):
        body = slice_from("    static bool HandleFormationCommand")
        self.assertIn("is not one of off, tight, loose, spread", body)

    def test_command_applies_immediately_and_tells_the_addon(self):
        body = slice_from("    static bool HandleFormationCommand")
        self.assertIn("g_formation[player->GetGUID().GetCounter()] = wanted;", body)
        self.assertIn("ApplyFormation(player);", body)
        self.assertIn('SendProtocolLine(player, "Formation " + wanted);', body)

    def test_login_announces_the_current_preset(self):
        self.assertIn('SendProtocolLine(owner, "Formation " + FormationFor(owner));', SOURCE)

    def test_logout_forgets_the_preset(self):
        body = slice_from("void OnPlayerLogout", 3200)
        self.assertIn("g_formation.erase(ownerGuid);", body)


class AddonContract(unittest.TestCase):
    def test_header_offers_the_four_presets(self):
        for preset in ('"tight"', '"loose"', '"spread"', '"off"'):
            self.assertIn("name = " + preset, UI)

    def test_buttons_send_the_command(self):
        self.assertIn('G.Run(".companionsync formation " .. preset.name)', UI)

    def test_active_preset_is_highlighted_from_server_state(self):
        self.assertIn("G.RefreshFormation = function()", UI)
        self.assertIn('local active = G.Formation or "off"', UI)

    def test_formation_line_is_parsed(self):
        self.assertIn('text:match("^Formation (%a+)$")', CORE)
        self.assertIn("G.Formation = formation", CORE)


class DocumentationContract(unittest.TestCase):
    def test_the_doc_says_combat_belongs_to_the_bot(self):
        self.assertIn("Combat is the bot's", DOC)

    def test_the_doc_keeps_pitch_in_radius_out_of_scope(self):
        self.assertIn("WowLegends.BotPitchIn.Radius", DOC)
        self.assertIn("untouched", DOC)


if __name__ == "__main__":
    unittest.main()
