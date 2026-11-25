"""Distance contract: the module names a formation, playerbots does the walking.

The module used to drive `MoveFollow` onto a ring of its own, out of combat only. playerbots
ships nine formations that already move bots with combat priority and persist per bot, so that
ring was a second, weaker spacing system: it could not position anybody during a fight, which is
when positioning matters. These checks pin the replacement - a table from preset and role to
formation name, movement left to the bot, and the same number doing double duty as the leash.
"""

import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


def read(relative):
    """Source text with one line ending, so expectations do not depend on a CRLF checkout."""
    return (ROOT / relative).read_text(encoding="utf-8").replace(chr(13) + chr(10), chr(10))


SOURCE = read("src/mod_companion_questsync.cpp")
CONF = read("conf/mod_companion_questsync.conf.dist")
DOC = read("docs/companion-formation.md")
UI = read("addon/CompanionSync/UI.lua")
CORE = read("addon/CompanionSync/Core.lua")
README = read("README.md")


DEF_ANCHOR = ("    std::string EffectiveFormation(Player* owner, Player* bot)" + chr(10) + "    {")


def slice_from(anchor, length=2600):
    start = SOURCE.index(anchor)
    return SOURCE[start:start + length]


class PresetContract(unittest.TestCase):
    def test_presets_are_formation_names(self):
        body = slice_from("    std::string FormationForPreset(", 500)
        for preset, formation in (("close", "near"), ("tight", "queue"), ("loose", "circle"),
                                  ("spread", "far"), ("lead", "shield")):
            self.assertIn('if (preset == "%s")' % preset, body)
            self.assertIn('return "%s";' % formation, body)

    def test_the_spacing_maths_is_gone(self):
        for gone in ("FormationSpacing(", "FormationTightSpacing", "FormationCloseSpacing"):
            self.assertNotIn(gone, SOURCE)

    def test_config_option_defaults_to_the_role(self):
        self.assertIn('GetOption<std::string>("CompanionQuestSync.Formation", "auto")', SOURCE)
        self.assertIn("CompanionQuestSync.Formation = auto", CONF)
        self.assertIn("`Formation` | `auto`", README)

    def test_off_still_means_hands_off(self):
        body = slice_from(DEF_ANCHOR, 3400)
        self.assertIn('if (preset == "off")', body)
        self.assertIn("return std::string();", body)


class RoleDistanceContract(unittest.TestCase):
    def body(self):
        return slice_from(DEF_ANCHOR, 3400)

    def test_a_tank_leads(self):
        body = self.body()
        self.assertIn('if (role == "tank")', body)
        self.assertIn('shape = "shield";', body)

    def test_a_healer_keeps_its_distance(self):
        body = self.body()
        self.assertIn('if (role == "healer")', body)
        self.assertIn('shape = "far";', body)

    def test_support_keeps_a_ring(self):
        body = self.body()
        self.assertIn('if (role == "support")', body)
        self.assertIn('shape = "circle";', body)

    def test_dps_depends_on_whether_it_is_melee(self):
        self.assertIn('shape = PlayerbotAI::IsMelee(bot) ? "melee" : "far";', self.body())

    def test_auto_falls_back_to_playerbots_own_shape(self):
        self.assertIn('shape = "chaos";', self.body())

    def test_a_companion_that_refuses_the_fight_refuses_to_stand_in_it(self):
        body = self.body()
        self.assertIn('if (owner->IsInCombat())', body)
        self.assertIn('if (policy == "never")', body)
        self.assertIn('return "far";', body)

    def test_an_open_trade_window_beats_every_preset(self):
        self.assertIn('if (bot->GetTradeData() || OwnerIsInteracting(owner))', self.body())


class ClampContract(unittest.TestCase):
    """Distance serves the interaction out of a fight, and the role in one."""

    def body(self):
        return slice_from(DEF_ANCHOR, 3400)

    def test_an_interaction_on_either_side_pulls_everyone_in(self):
        self.assertIn("if (owner->GetTradeData())", SOURCE)
        self.assertIn('if (bot->GetTradeData() || OwnerIsInteracting(owner))', self.body())

    def test_a_gossip_menu_counts_as_an_interaction(self):
        self.assertIn("owner->PlayerTalkClass->GetGossipMenu().GetMenuItemCount() > 0", SOURCE)

    def test_standing_still_gathers_one_step_tighter(self):
        self.assertIn("if (!owner->IsInCombat() && OwnerStandingStill(owner))", self.body())
        self.assertIn("return OneStepTighter(shape);", self.body())

    def test_one_step_tighter_reads_the_way_the_shapes_do(self):
        body = slice_from("    std::string OneStepTighter(std::string const& shape)", 500)
        self.assertIn('return "circle";', body)
        self.assertIn('return "queue";', body)
        self.assertIn('return "near";', body)

    def test_the_gather_waits_a_moment(self):
        self.assertIn("kIdleGatherSeconds = 3", SOURCE)

    def test_idle_is_measured_not_polled(self):
        body = slice_from("    bool OwnerStandingStill(Player* owner)", 1200)
        self.assertIn("dx * dx + dy * dy + dz * dz > 1.0f", body)
        self.assertIn("g_ownerIdle", body)

    def test_the_tracker_is_dropped_with_the_player(self):
        self.assertIn("g_ownerIdle.erase(ownerGuid);", SOURCE)


class MovementContract(unittest.TestCase):
    def test_the_module_no_longer_walks_anybody(self):
        body = slice_from("    void ApplyFormation(Player* owner)", 2400)
        self.assertNotIn("MoveFollow", body)
        self.assertNotIn("GetMotionMaster", body)

    def test_it_sets_the_formation_through_the_bots_own_verb(self):
        body = slice_from("    void ApplyFormation(Player* owner)", 2400)
        self.assertIn('ai->HandleCommand(CHAT_MSG_WHISPER, "formation " + target, owner);', body)

    def test_it_speaks_only_on_change(self):
        body = slice_from("    void ApplyFormation(Player* owner)", 2400)
        self.assertIn("applied->second == target", body)
        self.assertIn("g_formationApplied[botGuid] = target;", body)

    def test_tick_calls_it_for_every_owner(self):
        tick = slice_from("SendHeartbeat(player);", 200)
        self.assertIn("ApplyFormation(player);", tick)


class LeashContract(unittest.TestCase):
    def test_the_leash_is_measured_per_formation(self):
        body = slice_from("    /// How far, roughly, a formation lets a companion work", 900)
        self.assertIn("return 25.0f;", body)
        self.assertIn('if (formation == "melee" || formation == "shield" || formation == "arrow")', body)

    def test_the_pin_is_gated_by_it(self):
        body = slice_from("    void ApplyEngagement(Player* owner, Player* bot)", 6200)
        self.assertIn("owner->GetDistance(bot) > FormationLeash(EffectiveFormation(owner, bot)) + 5.0f", body)


class CommandContract(unittest.TestCase):
    def test_command_is_registered_for_players(self):
        self.assertIn('{ "formation", HandleFormationCommand, SEC_PLAYER, Console::No },', SOURCE)

    def test_command_rejects_a_typo_instead_of_silently_ignoring_it(self):
        body = slice_from("    static bool HandleFormationCommand")
        self.assertIn("is not one of auto, close, tight, loose", body)
        self.assertIn("spread, lead, off", body)

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
    def test_header_offers_auto_and_lead(self):
        for preset in ('"auto"', '"lead"', '"close"', '"tight"', '"loose"', '"spread"', '"off"'):
            self.assertIn("name = " + preset, UI)

    def test_buttons_send_the_command(self):
        self.assertIn('G.Run(".companionsync formation " .. preset.name)', UI)

    def test_active_preset_is_highlighted_from_server_state(self):
        self.assertIn("G.RefreshFormation = function()", UI)
        self.assertIn('local active = G.Formation or "auto"', UI)

    def test_formation_line_is_parsed(self):
        self.assertIn('text:match("^Formation (%a+)$")', CORE)
        self.assertIn("G.Formation = formation", CORE)


class DocumentationContract(unittest.TestCase):
    def test_the_doc_says_movement_belongs_to_the_bot(self):
        self.assertIn("playerbots", DOC)
        self.assertIn("formation", DOC)
        self.assertIn("adopt", DOC.lower())

    def test_the_doc_keeps_pitch_in_radius_out_of_scope(self):
        self.assertIn("WowLegends.BotPitchIn.Radius", DOC)
        self.assertIn("untouched", DOC)


if __name__ == "__main__":
    unittest.main()
