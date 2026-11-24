"""Roles: what a companion is for, recorded per companion and announced to the addon.

Chunk one of the role work: storage and presentation only. These checks exist to keep it
that way until the strategy layer is written deliberately - a role that quietly starts
changing behaviour would be a surprise, not a feature.
"""

import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src" / "mod_companion_questsync.cpp").read_text(encoding="utf-8")
SQL = (ROOT / "sql" / "companion-roles.sql").read_text(encoding="utf-8")
DOC = (ROOT / "docs" / "companion-roles.md").read_text(encoding="utf-8")
UI = (ROOT / "addon" / "CompanionSync" / "UI.lua").read_text(encoding="utf-8")
CORE = (ROOT / "addon" / "CompanionSync" / "Core.lua").read_text(encoding="utf-8")


def slice_from(anchor, length=2400):
    return SOURCE[SOURCE.index(anchor):SOURCE.index(anchor) + length]


class StorageContract(unittest.TestCase):
    def test_the_five_roles_are_the_vocabulary(self):
        self.assertIn('kCompanionRoles[] = { "auto", "tank", "healer", "dps", "support" }', SOURCE)

    def test_migration_adds_the_column_with_auto_as_default(self):
        self.assertIn("ALTER TABLE cq_companion_custody", SQL)
        self.assertIn("ADD COLUMN role VARCHAR(16) NOT NULL DEFAULT 'auto'", SQL)

    def test_a_missing_column_is_reported_not_ignored(self):
        body = slice_from("    bool RoleColumnPresent()")
        self.assertIn("information_schema.columns", body)
        self.assertIn("companion roles stay unavailable", body)
        self.assertIn("LOG_WARN", body)
        self.assertIn("companion-roles.sql", body)

    def test_a_role_write_is_read_back(self):
        body = slice_from("    bool SetCompanionRole(")
        self.assertIn("HasCurrentCustody(ownerGuid, botGuid, false)", body)
        self.assertIn("return CompanionRole(botGuid) == wanted;", body)

    def test_unknown_roles_fall_back_to_auto_instead_of_storing_nonsense(self):
        body = slice_from("    std::string RoleOrAuto(")
        self.assertIn('RoleKnown(role) ? role : std::string("auto")', body)


class StrategyContract(unittest.TestCase):
    """The point of a role: what the companion is allowed to do in a fight."""

    def test_tank_is_allowed_to_taunt(self):
        body = slice_from("    std::vector<RoleStrategyEdit> RoleStrategyEdits(")
        self.assertIn('if (role == "tank")', body)
        self.assertIn('{ "tank", BOT_STATE_COMBAT, true }', body)

    def test_healer_heals_and_drops_tank_and_pull(self):
        body = slice_from("    std::vector<RoleStrategyEdit> RoleStrategyEdits(")
        self.assertIn('{ "heal", BOT_STATE_COMBAT, true }', body)
        self.assertIn('{ "tank", BOT_STATE_COMBAT, false }', body)
        self.assertIn('{ "pull", BOT_STATE_NON_COMBAT, false }', body)

    def test_dps_and_support_cannot_taunt_or_pull(self):
        body = slice_from("    std::vector<RoleStrategyEdit> RoleStrategyEdits(")
        self.assertIn('if (role == "dps" || role == "support")', body)

    def test_auto_touches_nothing(self):
        body = slice_from("    std::vector<RoleStrategyEdit> RoleStrategyEdits(")
        self.assertIn("return {};", body)
        apply_body = slice_from("    std::string ApplyRoleStrategies(Player* bot)")
        self.assertIn('if (role == "auto")', apply_body)

    def test_an_edit_that_did_not_take_is_verified_then_reported(self):
        body = slice_from("    std::string ApplyRoleStrategies(Player* bot)")
        self.assertIn("ai->ChangeStrategy(std::string(edit.add ? \"+\" : \"-\") + edit.name, edit.state);",
                      body)
        self.assertIn("if (ai->HasStrategy(edit.name, edit.state) != edit.add)", body)

    def test_the_role_is_re_asserted_because_playerbots_resets_strategies(self):
        body = slice_from("    void ReconcileRoles(Player* owner)")
        self.assertIn("kRoleReassertSeconds", body)
        self.assertIn("ApplyRoleStrategies(bot)", body)
        self.assertIn("kRoleReassertSeconds = 60", SOURCE)
        self.assertIn("ReconcileRoles(player);", SOURCE)

    def test_role_state_is_purged_with_the_bot_guid(self):
        body = slice_from("void PurgeCompanionState(", 2600)
        self.assertIn("g_roleNotes.erase(botGuid);", body)
        self.assertIn("g_roleApplyAt.erase(botGuid);", body)

    def test_it_only_ever_edits_an_owned_companions_ai(self):
        body = slice_from("    void ReconcileRoles(Player* owner)")
        self.assertIn("IsControlledCompanion(bot, owner)", body)

    def test_a_shortfall_is_named_not_swallowed(self):
        body = slice_from("    static bool HandleRoleCommand")
        self.assertIn("this class has no {} strategy", body)
        self.assertIn('line += ":na";', SOURCE)

    def test_documentation_reflects_that_it_is_live(self):
        self.assertIn("Each role is translated into strategy edits", DOC)
        self.assertIn("no setter", DOC)


class CommandContract(unittest.TestCase):
    def test_command_is_registered_for_players(self):
        self.assertIn('{ "role",      HandleRoleCommand,      SEC_PLAYER, Console::No },', SOURCE)

    def test_partial_arguments_print_the_current_roles(self):
        body = slice_from("    static bool HandleRoleCommand")
        self.assertIn("RoleLine(ownerGuid)", body)

    def test_a_typo_is_named_back_at_the_player(self):
        body = slice_from("    static bool HandleRoleCommand")
        self.assertIn("is not one of auto, tank, healer, dps, support", body)

    def test_role_is_set_only_for_an_owned_companion(self):
        body = slice_from("    static bool HandleRoleCommand")
        self.assertIn("ResolveOwnedCompanion(ownerGuid, a[0])", body)
        self.assertIn("you have no companion called", body)

    def test_change_is_echoed_to_the_addon(self):
        body = slice_from("    static bool HandleRoleCommand", 4000)
        self.assertIn('SendProtocolLine(player, "Roles " + RoleLine(ownerGuid));', body)


class SyncContract(unittest.TestCase):
    def test_login_announces_roles(self):
        self.assertIn('SendProtocolLine(owner, "Roles " + roles);', SOURCE)

    def test_line_is_name_equals_role_pairs(self):
        body = slice_from("    std::string RoleLine(")
        self.assertIn('line += "|";', body)
        self.assertIn('name + "=" + CompanionRole(botGuid)', body)

    def test_a_name_containing_a_separator_is_skipped(self):
        body = slice_from("    std::string RoleLine(")
        self.assertIn("name.find('|') != std::string::npos", body)
        self.assertIn("name.find('=') != std::string::npos", body)


class AddonContract(unittest.TestCase):
    def test_roles_line_is_parsed(self):
        self.assertIn('text:match("^Roles (.*)$")', CORE)
        self.assertIn('pair:match("^(.-)=(%a+)(.*)$")', CORE)

    def test_each_card_can_set_its_own_role(self):
        self.assertIn("card.roleButton", UI)
        self.assertIn('G.Run(".companionsync role " .. card.companion.name .. " " .. nextRole)', UI)

    def test_the_button_shows_the_stored_role_not_the_last_click(self):
        self.assertIn('local role = (G.Roles and G.Roles[companion.name]) or "auto"', UI)

    def test_a_shortfall_is_marked_on_the_card(self):
        self.assertIn('G.RoleNotes[name] = true', CORE)
        self.assertIn('(note and " !" or "")', UI)

    def test_the_cycle_covers_every_role(self):
        self.assertIn('local ORDER = { "auto", "tank", "healer", "dps", "support" }', UI)


if __name__ == "__main__":
    unittest.main()
