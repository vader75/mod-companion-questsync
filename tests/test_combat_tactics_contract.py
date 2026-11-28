"""Combat preferences are server-confirmed and the gate does not persist AI strategies."""
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class CombatTactics(unittest.TestCase):
    def test_addon_offers_three_server_confirmed_choices(self):
        ui = (ROOT / 'addon/CompanionSync/UI.lua').read_text(encoding='utf-8')
        core = (ROOT / 'addon/CompanionSync/Core.lua').read_text(encoding='utf-8')
        for label in ('Tank First', 'Gap Closers', 'Guard Flanks'):
            self.assertIn(label, ui)
        self.assertIn('.companionsync tactics ', ui)
        self.assertNotRegex(ui.split('local function CycleCombatTactic', 1)[-1].split('\nend', 1)[0],
                            r'G\.CombatTactics\[[^\]]+\]\s*=')
        self.assertIn('^Tactics (.*)$', core)
        self.assertIn('G.CombatTactics =', core)

    def test_actual_action_gate_not_strategy_edits(self):
        path = ROOT / 'src/CombatTactics.inc'
        self.assertTrue(path.exists(), 'missing action-time combat tactics gate')
        gate = path.read_text(encoding='utf-8')
        self.assertIn('AddActionExecutionListener', gate)
        self.assertIn('IsEngagedBy(tank)', gate)
        self.assertIn('IsOutdoors()', gate)
        self.assertIn('ReachMeleeAction', gate)
        self.assertNotIn('ChangeStrategy(', gate)
        self.assertNotIn('MoveTo(', gate)

    def test_owner_scoped_storage_and_explicit_defaults(self):
        source = (ROOT / 'src/mod_companion_questsync.cpp').read_text(encoding='utf-8')
        self.assertIn('GetOption<bool>("CompanionQuestSync.TankFirst", false)', source)
        migration = ROOT / 'sql/companion-combat-tactics.sql'
        self.assertTrue(migration.exists(), 'missing preference migration')
        self.assertIn('DEFAULT 0', migration.read_text(encoding='utf-8'))
        gate = (ROOT / 'src/CombatTactics.inc').read_text(encoding='utf-8')
        self.assertIn("AND module = 'cq' AND state = 'owned'", gate)
        self.assertIn('owner_guid = {}', gate)

    def test_realm_charge_fallback_requires_a_live_eligible_override(self):
        source = (ROOT / 'src/mod_companion_questsync.cpp').read_text(encoding='utf-8')
        gate = (ROOT / 'src/CombatTactics.inc').read_text(encoding='utf-8')
        self.assertIn('g_cfg.suppressGapClosers && !HasActiveGapCloserOverride(bot)', source)
        override = gate.split('bool HasActiveGapCloserOverride(Player* bot)', 1)[1].split('Player* TacticsTank', 1)[0]
        self.assertIn('state->rules.load()', override)
        self.assertIn('TacticsEligible(bot, ObjectAccessor::GetPlayer', override)
        self.assertIn('!TacticsEligible(bot, owner)', gate)
        self.assertIn('!bot->GetMap()->IsBattlegroundOrArena()', gate)
        cleanup = gate.split('state->rules.exchange(nullptr)', 1)[1]
        self.assertIn('ReconcileRoleSpells(bot, CompanionRole(previous->botGuid))', cleanup)
