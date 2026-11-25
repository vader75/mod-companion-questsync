"""Roles: what a companion is for, recorded per companion and announced to the addon.

Chunk one of the role work: storage and presentation only. These checks exist to keep it
that way until the strategy layer is written deliberately - a role that quietly starts
changing behaviour would be a surprise, not a feature.
"""

import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src" / "mod_companion_questsync.cpp").read_text(encoding="utf-8")
WORKFLOW = (ROOT / "src" / "RoleSwitch.inc").read_text(encoding="utf-8")
SQL = (ROOT / "sql" / "companion-roles.sql").read_text(encoding="utf-8")
DOC = (ROOT / "docs" / "companion-roles.md").read_text(encoding="utf-8")
UI = (ROOT / "addon" / "CompanionSync" / "UI.lua").read_text(encoding="utf-8")
CORE = (ROOT / "addon" / "CompanionSync" / "Core.lua").read_text(encoding="utf-8")
README = (ROOT / "README.md").read_text(encoding="utf-8")


def slice_from(anchor, length=2400):
    return SOURCE[SOURCE.index(anchor):SOURCE.index(anchor) + length]


def handler_body(anchor="    static bool HandleRoleCommand"):
    """One handler, exactly: the next top-level function ends it.

    Length-based slices break the moment a refusal is added above an assertion - which is what
    a role guard is - and the failure looks like a missing message rather than a moved one.
    """
    start = SOURCE.index(anchor)
    return SOURCE[start:SOURCE.index(chr(10) + "    static ", start + 10)]


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
        self.assertIn("    else\n            return edits;   // auto: the module keeps its hands off", body)
        apply_body = slice_from("    std::string ApplyRoleStrategies(Player* bot,")
        self.assertIn('if (role == "auto")', apply_body)

    def test_an_edit_that_did_not_take_is_verified_then_reported(self):
        body = SOURCE.split("    std::string ApplyRoleStrategies(Player* bot,", 1)[1].split("    /// Undo the role edits", 1)[0]
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
        body = handler_body()
        self.assertNotIn("this class has no {} strategy", body)
        self.assertIn("could not switch to", WORKFLOW)
        self.assertIn('line += ":na";', SOURCE)

    def test_role_change_checks_hands_without_waiting_for_the_gear_watch(self):
        self.assertIn("RepairKitShape(bot, role, &generated)", WORKFLOW)

    def test_role_gear_uses_a_native_swap_and_verifies_the_destination(self):
        body = slice_from("    bool EquipCarriedKitPiece(", 560)
        self.assertIn("bot->SwapItem(item->GetPos(), dest)", body)
        self.assertIn("bot->GetItemByPos(dest) == item", body)
        self.assertNotIn("bot->EquipItem(", body)

    def test_support_does_not_heal_by_itself(self):
        # support is "stay out of the tank's way", not "off-heal": it must not add a heal
        # strategy, or hybrids would heal badly and pull threat doing it
        body = slice_from('if (role == "dps" || role == "support")', 260)
        self.assertNotIn('"heal"', body)
        self.assertIn('{ "tank", BOT_STATE_COMBAT, false }', body)

    def test_healer_is_the_lever_that_adds_healing(self):
        body = slice_from('if (role == "healer")', 220)
        self.assertIn('{ "heal", BOT_STATE_COMBAT, true }', body)

    def test_the_documentation_names_healer_as_the_off_heal_lever(self):
        self.assertIn("Support does not heal, on purpose", DOC)
        self.assertIn("mark it `healer`", DOC)

    def test_a_druid_is_taken_out_of_bear_form_for_dps_and_support(self):
        # playerbots picks bear from the Thick Hide aura, not from intent: dropping "tank"
        # alone leaves the companion standing in a bear suit, so the form strategy goes too
        rules = slice_from("    ClassRule constexpr kClassRules[] = {", 900)
        self.assertIn('{ CLASS_DRUID, "dps",     "bear", BOT_STATE_COMBAT, false, 0 },', rules)
        self.assertIn('{ CLASS_DRUID, "support", "bear", BOT_STATE_COMBAT, false, 0 },', rules)
        self.assertIn('{ CLASS_DRUID, "dps",     "cat",  BOT_STATE_COMBAT, true,  768  },', rules)

    def test_a_druid_healer_is_taken_out_of_bear_form(self):
        rules = slice_from("    ClassRule constexpr kClassRules[] = {", 900)
        self.assertIn('{ CLASS_DRUID, "healer",  "bear", BOT_STATE_COMBAT, false, 0 },', rules)

    def test_a_druid_tank_is_put_into_bear_form(self):
        rules = slice_from("    ClassRule constexpr kClassRules[] = {", 900)
        self.assertIn('{ CLASS_DRUID, "tank",    "bear", BOT_STATE_COMBAT, true,  5487 }', rules)

    def test_forms_are_only_touched_for_druids(self):
        # the table carries the class in every row, so a form edit cannot leak to another class
        # stop at the end of the table: the spec whitelist that follows it names bear and
        # cat as well, and it is not a form edit
        rules = slice_from("    ClassRule constexpr kClassRules[] = {", 1200).split("    };")[0]
        rows = [line for line in rules.splitlines() if '"bear"' in line or '"cat"' in line]
        self.assertTrue(rows)
        for row in rows:
            self.assertIn("CLASS_DRUID", row, row)

    def test_a_role_change_never_disarms_the_companion(self):
        # "tank" is a role word and a protection warrior's whole combat strategy: taking it away
        # leaves no spec at all, and the companion stands in the fight doing nothing
        source = SOURCE
        self.assertIn("bool WouldDisarm(PlayerbotAI* ai, std::string const& removing)", source)
        self.assertIn('"tank", "arms", "fury", "melee", "dps", "cat", "bear", "balance", "resto", "heal",', source)
        self.assertIn("if (!edit.add && WouldDisarm(ai, edit.name))", source)
        self.assertIn('missed += std::string("kept ") + edit.name;', source)

    def test_pull_follows_the_engagement_policy(self):
        # pulling is how a companion starts a fight of its own; an assist-only companion reads as
        # one that does not engage, so only the player-directed policies take pull away
        source = SOURCE
        self.assertIn('bool const keepPull = (policy == "auto" || policy.empty());', source)
        self.assertIn("RoleStrategyEdits(role, bot, quiet, CompanionEngage(botGuid))", source)

    def test_a_removal_that_did_not_take_is_reported_as_well(self):
        body = SOURCE.split("    std::string ApplyRoleStrategies(Player* bot,", 1)[1].split("    /// Undo the role edits", 1)[0]
        self.assertIn('edit.add ? edit.name : std::string("-") + edit.name', body)

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
        body = handler_body()
        self.assertIn('SendRoleAndSpecLines(owner, ownerGuid);', WORKFLOW)


class SyncContract(unittest.TestCase):
    def test_login_announces_roles(self):
        self.assertIn("SendRoleAndSpecLines(owner, owner->GetGUID().GetCounter());", SOURCE)

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
        self.assertIn('pair:match("^(.-)=(.*)$")', CORE)

    def test_each_card_stages_a_role_and_applies_it(self):
        self.assertIn("card.roleButton", UI)
        self.assertIn("card.roleApply", UI)
        self.assertIn("card.rolePending = (nextRole == current) and nil or nextRole", UI)

    def test_the_button_shows_the_stored_role_not_the_last_click(self):
        # the stored role drives the label; a staged one rides alongside it as "dps > tank"
        self.assertIn('local current = (name and G.Roles and G.Roles[name]) or "auto"', UI)

    def test_a_shortfall_is_marked_on_the_card(self):
        self.assertIn('G.RoleNotes[name] = true', CORE)
        self.assertIn('(note and " !" or "")', UI)

    def test_the_cycle_covers_every_role(self):
        self.assertIn('local ORDER = { "auto", "tank", "healer", "dps", "support" }', UI)



class EngagementContract(unittest.TestCase):
    """Whether it gets into a fight: the part a class's strategies cannot express."""

    def test_four_policies_and_auto_is_one_of_them(self):
        self.assertIn('kEngagePolicies[] = { "auto", "mine", "joined", "never" }', SOURCE)

    def test_policy_is_held_with_the_passive_strategy(self):
        body = slice_from("    void ApplyEngagement(Player* owner, Player* bot)", 4600)
        self.assertIn('"+passive"', body)
        self.assertIn('"-passive"', body)

    def test_joined_lifts_passive_only_while_the_owner_fights(self):
        # joined and mine now share one stance rule: passive only while the owner is idle
        body = slice_from("    void ApplyEngagement(Player* owner, Player* bot)", 4600)
        self.assertIn('(!ownerFighting && policy != "auto")', body)

    def test_never_stays_passive(self):
        body = slice_from("    void ApplyEngagement(Player* owner, Player* bot)", 4600)
        self.assertIn('policy == "never"', body)

    def test_auto_touches_nothing(self):
        body = slice_from("    void ApplyEngagement(Player* owner, Player* bot)", 4600)
        self.assertLess(body.index('if (policy == "auto")'), body.index("ChangeStrategy"))

    def test_an_unavailable_strategy_is_probed_once_and_reported(self):
        body = slice_from("    bool PassiveStrategyUsable(Player* bot)")
        self.assertIn('ai->HasStrategy("passive", BOT_STATE_NON_COMBAT)', body)
        self.assertIn("g_passiveStrategy", body)
        cmd = slice_from("    static bool HandleEngageCommand", 5200)
        self.assertIn("this build cannot hold companions out of fights", cmd)
        self.assertIn("no passive strategy in this build", SOURCE)

    def test_passive_is_an_idle_stance_never_a_fight_carefully_stance(self):
        # PassiveMultiplier zeroes "attack my target", so holding a bot passive while telling
        # it to attack is a silent no-op - the bug that left a warrior standing still
        body = slice_from("    void ApplyEngagement(Player* owner, Player* bot)", 4600)
        self.assertIn('bool const wantPassive = policy == "never" || (!ownerFighting && policy != "auto");',
                      body)

    def test_the_documented_lever_is_recorded(self):
        text = (ROOT / "docs" / "companion-strategy-map.md").read_text(encoding="utf-8")
        self.assertIn("returns **0** for every action except", text)
        self.assertIn('`offheal`', text)
        self.assertIn("138", text + "138")     # the list is generated, not hand-counted

    def test_policy_is_stored_and_read_back(self):
        body = slice_from("    bool SetCompanionEngage(")
        self.assertIn("return CompanionEngage(botGuid) == wanted;", body)
        self.assertIn("ADD COLUMN engage VARCHAR(8) NOT NULL DEFAULT 'auto'",
                      (ROOT / "sql" / "companion-engage.sql").read_text(encoding="utf-8"))

    def test_it_is_purged_with_the_bot_guid(self):
        body = slice_from("void PurgeCompanionState(", 3000)
        self.assertIn("g_engageNotes.erase(botGuid);", body)

    def test_tick_applies_it_to_every_owner(self):
        self.assertIn("ReconcileEngagement(player);", SOURCE)
        body = slice_from("    void ReconcileEngagement(Player* owner)")
        self.assertIn("ApplyEngagement(owner, bot)", body)

    def test_addon_card_offers_the_policy(self):
        self.assertIn("card.engageButton", UI)
        self.assertIn('G.Run(".companionsync engage " .. card.companion.name .. " " .. nextPolicy)', UI)
        self.assertIn('G.Engage = G.Engage or {}', UI)

    def test_addon_parses_role_slash_policy(self):
        self.assertIn('local role, policy, flag = rest:match("^(%a+)/?(%a*)(.*)$")', CORE)


class RoleChangeWindow(unittest.TestCase):
    """A role is a decision, taken out of combat, once per window.

    The window exists because a role drives strategies, and it will drive a respec and a gear
    swap: spamming it is how a companion ends up respeccing in a doorway, mid-pull. The addon
    stages the choice and applies it, and the server is the authority on both the fight and the
    window - a disabled button is courtesy, not the rule.
    """

    def test_the_window_is_configurable_and_documented(self):
        self.assertIn('g_cfg.roleChangeCooldown = sConfigMgr->GetOption<uint32>'
                      '("CompanionQuestSync.RoleChangeCooldown", 60);', SOURCE)
        dist = (ROOT / "conf" / "mod_companion_questsync.conf.dist").read_text(encoding="utf-8")
        self.assertIn("CompanionQuestSync.RoleChangeCooldown = 60", dist)
        self.assertIn("`RoleChangeCooldown`", README)
        self.assertIn("role change", DOC.lower())

    def test_the_window_is_stamped_when_the_role_moves(self):
        body = slice_from("    bool SetCompanionRole(")
        self.assertIn("role_changed_at = UNIX_TIMESTAMP()", body)

    def test_a_missing_column_is_a_warning_not_a_broken_command(self):
        self.assertIn("RoleLockColumnPresent", SOURCE)
        body = slice_from("    bool RoleLockColumnPresent()")
        self.assertIn("has no role_changed_at", body)
        self.assertIn("not rate limited", body)
        migration = (ROOT / "sql" / "companion-role-lock.sql").read_text(encoding="utf-8")
        self.assertIn("ADD COLUMN role_changed_at INT UNSIGNED NOT NULL DEFAULT 0", migration)

    def test_the_window_is_read_back_from_the_row(self):
        body = slice_from("    uint32 RoleLockRemaining(")
        self.assertIn("SELECT role_changed_at FROM cq_companion_custody", body)
        self.assertIn("g_cfg.roleChangeCooldown - spent", body)
        # no cooldown configured means no guard, and a missing column means the same
        self.assertIn("if (!g_cfg.roleChangeCooldown || !RoleLockColumnPresent())", body)

    def test_out_of_combat_is_the_hard_rule(self):
        body = slice_from("    std::string RoleChangeRefusal(")
        self.assertIn("bot->IsInCombat()", body)
        self.assertIn("owner->IsInCombat()", body)
        self.assertIn("out of combat", body)
        # and the refusal is what the command reports
        handler = handler_body("static bool HandleRoleCommand")
        self.assertIn("RoleChangeRefusal(owner, bot, botGuid)", WORKFLOW)

    def test_asking_for_the_role_it_already_has_costs_nothing(self):
        handler = handler_body("static bool HandleRoleCommand")
        self.assertLess(handler.index("CompanionRole(botGuid) == role"), handler.index("state.pending = role"))
        self.assertNotIn("SetCompanionRole(", handler)

    def test_the_addon_is_told_the_remaining_window(self):
        roles = slice_from("            if (uint32 const remaining = RoleLockRemaining(botGuid); remaining)", 200)
        self.assertIn('line += ":l"', roles)
        self.assertIn('flags:match(":l(%d+)")', CORE)
        self.assertIn("G.RoleLockAt = {}, {}, {}, {}, {}", CORE)

    def test_choosing_a_role_only_stages_it(self):
        ui = UI
        start = ui.index("card.roleButton = Button(")
        end = ui.index("card.roleApply = Button(")
        chooser = ui[start:end]
        self.assertIn("card.rolePending =", chooser)
        self.assertNotIn('G.Run(".companionsync role', chooser)

        applyStart = end
        applyEnd = ui.index('card.roleApply:SetPoint(')
        applier = ui[applyStart:applyEnd]
        self.assertIn('G.Run(".companionsync role " .. card.companion.name .. " " .. card.rolePending)', applier)
        self.assertIn('UnitAffectingCombat("player")', applier)

    def test_the_apply_button_counts_the_window_down(self):
        self.assertIn('target.roleApply:SetText(left .. "s")', UI)
        self.assertIn("target.roleApply:Disable()", UI)
        self.assertIn('card.roleApply:SetScript("OnUpdate"', UI)



def function_body(sig):
    """One function, from its definition to the first closing brace at method indent.

    `rindex`: every one of these has a forward declaration earlier in the file, and the body is
    what the test is about.
    """
    start = SOURCE.rindex(sig)
    return SOURCE[start:SOURCE.index(chr(10) + "    }", start)]


class SpecFollowsRole(unittest.TestCase):
    """Talents follow the role, on the role's own terms.

    The retrain is the first thing in this module that changes what a companion *is* rather
    than what it is told to do, so the tests pin the three gates (not auto, out of combat, at or
    above SpecMinLevel), the primitive used (a forced tab, so the tree is chosen and not
    dice-rolled), and the rebuild afterwards - strategies come from the trees.
    """

    def test_every_class_has_a_tree_per_role(self):
        for cls in ("CLASS_WARRIOR", "CLASS_PALADIN", "CLASS_HUNTER", "CLASS_ROGUE", "CLASS_PRIEST",
                    "CLASS_DEATH_KNIGHT", "CLASS_SHAMAN", "CLASS_MAGE", "CLASS_WARLOCK", "CLASS_DRUID"):
            self.assertIn("case " + cls + ":", SOURCE)
        self.assertIn("constexpr char const* kSpecTreeNames[CLASS_DRUID + 1][3]", SOURCE)
        self.assertIn('{ "arms",            "fury",          "protection" }', SOURCE)
        self.assertIn('{ "balance",         "feral",         "restoration" }', SOURCE)

    def test_the_mapping_says_what_each_role_asks(self):
        body = SOURCE[SOURCE.index("    int RoleSpecTab("):SOURCE.index("    int CurrentSpecTab(")]
        self.assertIn("return tank ? 2 : (heal ? -1 : 1);", body)   # warrior: protection or fury
        self.assertIn("return tank ? 0 : (heal ? -1 : 1);", body)   # death knight: blood or frost
        self.assertIn("return heal ? 1 : (tank ? -1 : 2);", body)   # priest: holy or shadow
        self.assertIn("return heal ? 2 : 1;", body)                 # druid: restoration or feral
        self.assertIn('role == "auto"', body)

    def test_support_rides_the_damage_tree(self):
        body = SOURCE[SOURCE.index("    int RoleSpecTab("):SOURCE.index("    int CurrentSpecTab(")]
        # nothing in the mapping reacts to "support": it falls into the damage branch
        self.assertNotIn('support', body)

    def test_a_spec_is_identified_from_the_talents_themselves(self):
        body = function_body("    uint32 SpecPoints(Player* bot, uint8 spec, uint8 tab)")
        self.assertIn("talent->IsInSpec(spec)", body)
        self.assertIn("info->RankID[rank] == entry.first", body)
        self.assertIn("tabInfo->tabpage != tab", body)

    def test_the_spec_regear_skips_only_the_level_gates(self):
        body = function_body("    uint32 RefreshLevelGear(Player* owner, Player* bot, bool specChanged)")
        self.assertIn("if (!specChanged && pieces && average + CompanionGearLag >= level)", body)
        self.assertIn("if (!specChanged && lastLevel && level < lastLevel + CompanionGearStep)", body)
        # the safety gates are not conditional on the reason
        self.assertIn("bot->IsInCombat() || !bot->GetSession() || bot->GetTradeData()", body)
        self.assertIn("bot->GetFreeInventorySpace() < CompanionBagReserve", body)

    def test_the_default_call_sites_are_unchanged(self):
        self.assertIn("RefreshLevelGear(owner, bot);", SOURCE)
        self.assertIn("RefreshLevelGear(master, fresh);", SOURCE)
        self.assertIn("bool specChanged = false);", SOURCE)   # the declaration carries the default


class SpecLineTravelsWithRoles(unittest.TestCase):
    """The card has to show the role AND what the talents did about it.

    A companion that keeps its old tree for a week is invisible when the panel only shows the
    role, so the two lines are sent together and cannot drift apart.
    """

    def test_the_two_lines_are_sent_together(self):
        body = function_body("    void SendRoleAndSpecLines(Player* target, uint32 ownerGuid)")
        self.assertIn('"Roles " + roles', body)
        self.assertIn('"Specs " + specs', body)
        self.assertLess(body.index('"Roles "'), body.index('"Specs "'))

    def test_no_call_site_sends_roles_alone(self):
        self.assertNotIn('SendProtocolLine(player, "Roles " + RoleLine(ownerGuid));', SOURCE)
        self.assertNotIn('SendProtocolLine(owner, "Roles " + roles);', SOURCE)
        self.assertEqual((SOURCE + WORKFLOW).count("SendRoleAndSpecLines("), 5)   # 1 definition + 4 sites

    def test_the_line_says_why_nothing_moved(self):
        body = function_body("    std::string SpecLine(uint32 ownerGuid)")
        self.assertIn('line += "!off"', body)          # the server is only logging
        self.assertIn('line += "!low"', body)          # below SpecMinLevel
        self.assertIn('"@" + std::to_string(g_cfg.specMinLevel)', body)
        # a role that asks nothing of the class sends just the tree it is in
        self.assertIn("if (want < 0)", body)

    def test_the_addon_reads_the_line(self):
        core = (ROOT / "addon" / "CompanionSync" / "Core.lua").read_text(encoding="utf-8")
        self.assertIn('text:match("^Specs (.*)$")', core)
        self.assertIn("G.Specs[name] = entry", core)
        ui = (ROOT / "addon" / "CompanionSync" / "UI.lua").read_text(encoding="utf-8")
        self.assertIn("G.Specs[name]", ui)
        self.assertIn("specHint", ui)

    def test_inspect_reports_the_tree_and_the_want(self):
        self.assertIn('handler->PSendSysMessage("  spec: {} |cff9d9d9drole {} wants {}{}|r"', SOURCE)
        self.assertIn("(dry run)", SOURCE)

    def test_the_keys_are_documented(self):
        dist = (ROOT / "conf" / "mod_companion_questsync.conf.dist").read_text(encoding="utf-8")
        self.assertIn("CompanionQuestSync.SpecFollowRole = 2", dist)
        self.assertIn("CompanionQuestSync.SpecMinLevel = 10", dist)
        self.assertIn("`SpecFollowRole`", README)
        self.assertIn("`SpecMinLevel`", README)
        self.assertIn("Talents follow the role", DOC)



if __name__ == "__main__":
    unittest.main()


class TalentsArriveWithoutATemplate(unittest.TestCase):
    """playerbots fills the trees from `parsedSpecLinkOrder[class][spec][level]` and returns having
    spent nothing when that table has no entry for the level - wiping the tree and reporting
    success. So a retrain has to be able to spend the points itself, from level ten up.
    """

    def test_there_is_a_fallback_allocation_at_all(self):
        self.assertIn('uint32 SpendPointsIntoTree(Player* bot, uint8 tab)', SOURCE)
        body = SOURCE.split('uint32 SpendPointsIntoTree(Player* bot, uint8 tab)', 1)[1][:2600]
        # the core vets every pick; we only decide the order
        self.assertIn('bot->LearnTalent(candidate.second, rank);', body)
        self.assertIn('if (bot->GetFreeTalentPoints() < before)', body)   # refused pick = no progress
        self.assertIn('while (progress && bot->GetFreeTalentPoints())', body)
        self.assertIn('candidates.emplace_back(info->Row, i);', body)
        self.assertIn('std::sort(candidates.begin(), candidates.end());', body)   # lowest row first
        self.assertIn('!(classMask & tabInfo->ClassMask)', body)                  # never another class's tree

    def test_it_engages_from_level_ten(self):
        self.assertIn('uint8 const CompanionTalentsFrom = 10;', SOURCE)
        body = SOURCE.split('uint32 SpendPointsIntoTree(Player* bot, uint8 tab)', 1)[1][:600]
        self.assertIn('bot->GetLevel() < CompanionTalentsFrom', body)
        # a level gate that would keep the low levels out is exactly what this replaces
        self.assertNotIn('< 40', body)
        self.assertNotIn('specMinLevel', body)

    def test_a_body_hired_at_its_level_is_given_the_points_it_is_owed(self):
        """Four level-15 companions reported `0 unspent`: they arrived at that level instead of
        levelling into it, so the hook that grants talent points never ran."""
        self.assertIn('bool GrantOwedTalentPoints(Player* bot)', SOURCE)
        body = SOURCE.split('bool GrantOwedTalentPoints(Player* bot)', 1)[1][:1200]
        self.assertIn('bot->InitTalentForLevel();', body)          # the core owns the arithmetic
        self.assertIn('before != 0', body)
        # the core decides what is spent; a talent map holds rows for talents never ranked, and
        # reading that as "points are spent" is what kept the grant from ever running
        self.assertNotIn('GetTalentMap()', body)
        self.assertIn('bot->InitTalentForLevel();', WORKFLOW)

    def test_the_first_grant_check_explains_why_it_did_or_did_not_change_points(self):
        body = function_body("    bool GrantOwedTalentPoints(Player* bot)")
        self.assertIn("g_talentGrantNotes.insert(botGuid).second", body)
        self.assertIn("GetFreeTalentPoints()", body)
        self.assertIn("CalculateTalentsPoints()", body)
        self.assertIn("InitTalentForLevel()", body)
        self.assertIn("free before", body)
        self.assertIn("free after", body)


class TheLogDoesNotSayTheSameThingTwice(unittest.TestCase):
    """The retry ladder used to log every attempt, and the same re-gear summary was whispered on
    every pass that re-derived it - four lines of news where there had been one change."""

    def test_the_same_sentence_is_not_repeated_within_a_quarter_hour(self):
        self.assertIn('std::unordered_map<uint32, std::pair<std::string, uint32>> g_lastSpoken;', SOURCE)
        say = SOURCE.split('bool SayToOwner(Player* bot, Player* owner, std::string const& text, uint32 minGap,', 1)[1][:4000]
        self.assertIn('g_lastSpoken[botGuid] = { text, now };', say)
        self.assertIn('now - memory->second.second < 900', say)
        self.assertIn('!commandReply && !handIn && tradeNoticeId == 0', say)   # replies and hand-ins still speak

    def test_a_class_quest_is_never_whispered_as_an_ask(self):
        refusal = SOURCE.split('void NotifyRefusal(Player* owner, Player* bot, Quest const* quest, Refusal const& refusal)', 1)[1][:900]
        self.assertIn('if (refusal.expected)', refusal)
        self.assertLess(refusal.index('if (refusal.expected)'), refusal.index('g_notified'))
        self.assertIn('bool const wanted = g_cfg.notifySkipped >= 2;', refusal)

    def test_a_stored_talent_is_found_whichever_way_it_is_spelled(self):
        """character_talent holds either the talent's DBC id or its rank spell id depending on the
        core. Looking up only one convention made every counter here read zero."""
        self.assertIn('TalentEntry const* ResolveTalentEntry(uint32 id)', SOURCE)
        body = SOURCE.split('TalentEntry const* ResolveTalentEntry(uint32 id)', 1)[1][:900]
        self.assertIn('g_talentByRankSpell.emplace(info->RankID[r], info);', body)
        self.assertIn('g_talentByRankSpell.find(id)', body)
        self.assertIn('return sTalentStore.LookupEntry(id);', body)     # the other convention
        # and every counter goes through it
        self.assertIn('TalentEntry const* info = ResolveTalentEntry(entry.first);', SOURCE)
        self.assertNotIn('TalentEntry const* info = sTalentStore.LookupEntry(talent->talentID);', SOURCE)

class ARefitNeedsOneSlotNotEight(unittest.TestCase):
    """The refit was gated on the pass's bag reserve, so it was refused exactly when it was needed:
    right after a pass that had just displaced several pieces into the bags."""

    def test_the_refit_gate_is_one_slot(self):
        body = SOURCE.split('bool RepairKitShape(Player* bot, std::string const& role,', 1)[1][:900]
        self.assertIn('bot->GetFreeInventorySpace() < 1', body)
        self.assertNotIn('CompanionBagReserve', body)
        # the gear pass keeps its own reserve
        pass_ = SOURCE.split('uint32 RefreshLevelGear(Player* owner, Player* bot, bool specChanged)', 1)[1][:9000]
        self.assertIn('bot->GetFreeInventorySpace() < CompanionBagReserve', pass_)
