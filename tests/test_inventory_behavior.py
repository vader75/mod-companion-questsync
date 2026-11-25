"""Compile the actual inventory policies with minimal engine boundary doubles.

Run in a C++ developer shell, or set CXX to the C++20 compiler executable.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_inventory_audit import POLICY, ROOT, WORLD


def fragment(source, first, following):
    start = source.index(first)
    return source[start:source.index(following, start)]


class InventoryBehavior(unittest.TestCase):
    def test_cpp_policies_with_permutations_expiry_and_transaction_outcomes(self):
        compiler = os.environ.get('CXX') or shutil.which('cl') or shutil.which('c++')
        if not compiler:
            # This is one of only two tests that compile and run the module's own code; a
            # suite that silently skips it says nothing about behaviour. Set CQ_REQUIRE_CXX=1
            # in CI so a missing toolchain fails the run instead of hiding behind a skip.
            if os.environ.get('CQ_REQUIRE_CXX'):
                self.fail('No C++ compiler: set CXX or CQ_REQUIRE_CXX=0 to allow skipping.')
            self.skipTest('Run in a C++20 developer shell or set CXX to exercise inventory policies.')
        fixture = Path(__file__).with_name('inventory_behavior.cpp.in').read_text(encoding='utf-8')
        snippets = {
            'QUEST': fragment(POLICY, '    bool QuestNeedsItem', '    bool CanReleaseItem'),
            'REAGENTS': fragment(WORLD, '    static std::map<uint32, uint32> CraftReagentRequirements',
                                 '    static std::vector<std::string> ProduceForCompanion'),
            'MAIL': fragment(POLICY, '    static bool IsDeliveryMail', '    uint8 OwnerUpgradeSlot'),
            'QUANTITY': fragment(POLICY, '    struct DeliveryRule', '    DeliveryRule DeliveryRuleFor'),
            'PLAN': fragment(POLICY, '    static std::vector<std::pair<ObjectGuid, uint32>> PlanDelivery',
                             '    static bool DeliveryStillValid'),
            'RECEIPT_TYPE': fragment(WORLD, '    struct DeliveryReceipt', '    void SendProtocolLine'),
            'LOOKUP': fragment(WORLD, '    Player* FindCompanionPlayer(uint32 botGuid)\n', '    /// `Roles '),
            'OWNER_LOOKUP': fragment(WORLD, '    static Player* FindDeliveryOwner', '    static bool CanTradeSurplusNow'),
            'RECEIPT': fragment(WORLD, '    static void ApplyDeliveryReceipts', '    static void CancelPendingDelivery'),
            'CANCELLATION': fragment(WORLD, '    static void CancelPendingDelivery', '    static void CancelBoundDelivery'),
        }
        for key, code in snippets.items():
            fixture = fixture.replace(f'@@{key}@@', code)
        with tempfile.TemporaryDirectory(dir=ROOT) as tmp:
            folder = Path(tmp)
            source, exe = folder / 'audit.cpp', folder / 'audit.exe'
            source.write_text(fixture, encoding='utf-8')
            msvc = Path(compiler).stem.lower() in {'cl', 'clang-cl'}
            flags = ['/nologo', '/std:c++20', '/EHsc', '/W4', f'/Fe{exe}'] if msvc else [
                '-std=c++20', '-Wall', '-Wextra', '-pthread', '-o', str(exe)]
            compile_result = subprocess.run([compiler, *flags, str(source)], cwd=folder,
                                            capture_output=True, text=True, timeout=60)
            self.assertEqual(compile_result.returncode, 0, compile_result.stdout + compile_result.stderr)
            run_result = subprocess.run([str(exe)], cwd=folder, capture_output=True, text=True, timeout=15)
            self.assertEqual(run_result.returncode, 0, run_result.stdout + run_result.stderr)


if __name__ == '__main__':
    unittest.main()
