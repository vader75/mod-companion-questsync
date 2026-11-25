from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / 'src/mod_companion_questsync.cpp').read_text(encoding='utf-8')
WORKFLOW = (ROOT / 'src/RoleSwitch.inc').read_text(encoding='utf-8')


class RoleSwitchWorkflow(unittest.TestCase):
    def test_druid_role_is_reachable_and_has_a_wire_tree_name(self):
        self.assertIn('kSpecTreeNames[CLASS_DRUID + 1][3]', SOURCE)
        mapping = SOURCE.split('    int RoleSpecTab(', 1)[1].split('    uint32 SpecPoints', 1)[0]
        self.assertNotIn('cls > 10', mapping)
        self.assertIn('case CLASS_DRUID:        return heal ? 2 : 1;', mapping)

    def test_command_and_watcher_share_pending_processor(self):
        command = SOURCE.split('static bool HandleRoleCommand', 1)[1].split('static bool HandleQuietCommand', 1)[0]
        watcher = SOURCE.split('void ReconcileRoles(Player* owner)', 1)[1].split('void ReconcileEngagement', 1)[0]
        self.assertIn('ProcessPendingRoleSwitch(', command)
        self.assertIn('ProcessPendingRoleSwitch(', watcher)
        self.assertNotIn('SetCompanionRole(', command)

    def test_saved_item_guids_are_excluded_from_inventory_release(self):
        release = (ROOT / 'src/CuratedDelivery.inc').read_text(encoding='utf-8')
        self.assertIn('!IsSavedRoleItem(bot->GetGUID().GetCounter(), item->GetGUID())', release)
        self.assertIn('state.saved[previous] = { hands, talents };', WORKFLOW)

    def test_defer_restore_round_trip_and_publish_once(self):
        compiler = os.environ.get('CXX') or shutil.which('cl') or shutil.which('c++')
        if not compiler:
            if os.environ.get('CQ_REQUIRE_CXX'):
                self.fail('C++20 compiler required')
            self.skipTest('C++20 compiler not available')
        fixture = Path(__file__).with_name('role_switch.cpp.in').read_text(encoding='utf-8')
        processor = WORKFLOW[WORKFLOW.index('    bool ProcessPendingRoleSwitch('):]
        reconcile = WORKFLOW.split('    bool ReconcileCompanionSpec(', 1)[1].split('    using StrategySnapshot', 1)[0]
        edits = SOURCE.split('    struct RoleStrategyEdit', 1)[1].split('    // --------------------------------------------------------------- quiet', 1)[0]
        with tempfile.TemporaryDirectory(dir=ROOT) as folder:
            source, exe = Path(folder) / 'test.cpp', Path(folder) / 'test.exe'
            source.write_text(fixture.replace('// PRODUCTION_PROCESS', processor)
                              .replace('// PRODUCTION_SPEC', 'bool ReconcileCompanionSpec(' + reconcile)
                              .replace('// PRODUCTION_EDITS', 'struct RoleStrategyEdit' + edits), encoding='utf-8')
            flags = (['/nologo', '/std:c++20', '/EHsc', f'/Fe{exe}']
                     if Path(compiler).stem.lower() in {'cl', 'clang-cl'} else ['-std=c++20', '-o', str(exe)])
            built = subprocess.run([compiler, *flags, str(source)], cwd=folder, capture_output=True, text=True,
                                   timeout=60)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            for scenario in ('round-trip', 'strategies', 'gear-only', 'wrong-tree', 'controlled'):
                with self.subTest(scenario=scenario):
                    run = subprocess.run([str(exe), scenario], cwd=folder, capture_output=True, text=True, timeout=15)
                    self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == '__main__':
    unittest.main()
