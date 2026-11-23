"""Run the compiled policy fixture in a C++ developer shell (or set CXX)."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / 'src' / 'mod_companion_questsync.cpp').read_text(encoding='utf-8')
CONF = (ROOT / 'conf' / 'mod_companion_questsync.conf.dist').read_text(encoding='utf-8')
CURATED = (ROOT / 'src' / 'CuratedDelivery.inc').read_text(encoding='utf-8')


def section(start, end):
    _, separator, after = SOURCE.partition(start)
    assert separator, f'missing source integration: {start}'
    return after.split(end, 1)[0]


class VendorErrandTests(unittest.TestCase):
    def test_compiled_real_policy(self):
        self.assertTrue((ROOT / 'src' / 'VendorErrandPolicy.h').is_file(), 'vendor errand policy not implemented')
        compiler = os.environ.get('CXX') or shutil.which('cl') or shutil.which('c++')
        if not compiler:
            self.skipTest('Run in a C++20 developer shell or set CXX.')
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            source, exe = folder / 'errand.cpp', folder / 'errand.exe'
            fixture = Path(__file__).with_name('vendor_errand_behavior.cpp.in').read_text(encoding='utf-8')
            release = 'static bool CanReleaseTemplate('
            fixture = fixture.replace('@@RELEASE@@', release + CURATED.split(release, 1)[1].split('bool IsCraftedSupply(', 1)[0])
            junk = 'static bool IsManagedVendorJunk('
            fixture = fixture.replace('@@JUNK@@', junk + section(junk, 'static bool ManagedSaleDue('))
            sale = 'static uint32 SellVendorErrandJunk('
            fixture = fixture.replace('@@SALE@@', sale + section(sale, 'static void UpdateVendorErrands()'))
            source.write_text(fixture, encoding='utf-8')
            msvc = Path(compiler).stem.lower() in {'cl', 'clang-cl'}
            flags = ['/nologo', '/std:c++20', '/EHsc', '/W4', '/WX', f'/I{ROOT / "src"}', f'/Fe{exe}'] if msvc else [
                '-std=c++20', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'src'), '-o', str(exe)]
            result = subprocess.run([compiler, *flags, str(source)], cwd=folder, capture_output=True,
                                    text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], cwd=folder, capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_opt_in_and_active_poll_before_module_early_return(self):
        self.assertIn('CompanionQuestSync.VendorErrand = 0', CONF)
        self.assertIn('GetOption<bool>("CompanionQuestSync.VendorErrand", false)', SOURCE)
        update = section('void OnUpdate(uint32 diff) override', 'if (!g_cfg.autoSync)')
        self.assertLess(update.index('UpdateVendorErrands();'), update.index('if (!g_cfg.enable'))
        self.assertIn('ScanVendorErrandStop(player, bots);', update)

    def test_exact_validated_route_and_spline_only_cancel(self):
        walk = section('static bool StartVendorErrand(', 'static void ScanVendorErrandStop(')
        self.assertIn('path.GetPathType() != PATHFIND_NORMAL', walk)
        self.assertIn('IsBoundedPath(path.GetPath()', walk)
        self.assertIn('VendorRouteIsClear(bot, path.GetPath())', walk)
        self.assertIn('init.MovebyPath(path.GetPath())', walk)
        cancel = section('static void StopVendorErrandMovement(', 'static void CancelVendorErrandsFor(')
        self.assertLess(cancel.index('movespline->GetId() == errand.splineId'), cancel.index('StopMoving()'))
        self.assertNotRegex(walk + cancel, r'MovePoint\(|MovementExpired|->Clear\(|Teleport|Mail|DestroyItem')

    def test_arrival_uses_exact_merchant_and_existing_sale_policy(self):
        arrival = section('static bool ArrivedAtErrandDestination(', 'static uint32 SellVendorErrandJunk(')
        self.assertIn('timePassed() == bot->movespline->Duration()', arrival)
        self.assertIn('IsWithinLOSInMap(vendor)', arrival)
        self.assertIn('HasSupplyAuthority(bot, owner, true)', arrival)
        self.assertIn('GetNPCIfCanInteractWith(errand.vendor', arrival)
        sell = section('static uint32 SellVendorErrandJunk(', 'static void UpdateVendorErrands()')
        self.assertIn('request.VendorGuid = errand.vendor;', sell)
        self.assertIn('IsManagedVendorJunk(owner, bot, item)', sell)
        self.assertIn('HandleSellItemOpcode', sell)
        self.assertNotIn('SellManagedJunk(', sell)
        self.assertIn('SellManagedJunk(owner, bot);', SOURCE)  # existing opportunistic callers
        purge = section('void PurgeCompanionState(', '    bool HasCurrentCustody(')
        self.assertIn('CancelVendorErrandsFor(botGuid);', purge)

    def test_owner_lease_and_active_pair_restore_no_follow_state(self):
        walk = section('static bool StartVendorErrand(', 'static void ScanVendorErrandStop(')
        self.assertIn('MayStart(bot->GetFreeInventorySpace(), !g_vendorErrands.count(ownerGuid))', walk)
        self.assertIn('g_vendorErrands.emplace(ownerGuid', walk)
        pair = section('static bool SafeVendorWalkPair(', 'static bool EligibleErrandMerchant(')
        self.assertIn('!OwnerAtVendorStop(owner) || !CanManageBags(owner, bot)', pair)
        follow = section('static bool NormalVendorFollow(', 'static bool SafeVendorWalkPair(')
        self.assertIn('follow->GetTarget() == owner', follow)
        self.assertNotIn('MoveFollow(', walk + pair + follow)

    def test_late_cancel_marks_bot_before_releasing_lease(self):
        cancel = section('static void RetireVendorErrand(', 'static void CancelVendorErrandsFor(uint32 botGuid)\n')
        self.assertLess(cancel.index('g_vendorSaleSuppressed'), cancel.index('g_vendorErrands.erase'))
        ordinary = section('uint32 SellManagedJunk(', 'namespace VendorWalk')
        self.assertLess(ordinary.index('VendorSaleBlocked('), ordinary.index('Cell::VisitObjects'))
        blocked = section('static bool VendorSaleBlocked(uint32 ownerGuid, uint32 botGuid)\n', 'static VendorWalk::Point VendorPosition(')
        self.assertIn('g_vendorSaleSuppressed.count({ownerGuid, botGuid})', blocked)
        self.assertIn('g_vendorSaleSuppressed[{ownerGuid, errand.botGuid}]', cancel)
        self.assertIn('VendorSaleBlocked(owner->GetGUID().GetCounter(), bot->GetGUID().GetCounter())', ordinary)
        scan = section('static void ScanVendorErrandStop(', '/// Wear upgrades')
        self.assertIn('g_vendorSaleSuppressed.find({ownerGuid, botGuid})', scan)
        self.assertIn('MayRearm(', scan)
        self.assertIn('g_vendorSaleSuppressed.erase({ownerGuid, botGuid});\n        InvalidateCompanionGuid(ownerGuid);\n        g_pendingRelease', SOURCE)

    def test_logout_countdown_and_shared_ground_gather_entry(self):
        ground = section('static bool OnVendorWalkGround(', 'static bool OwnerAtVendorStop(')
        self.assertIn('isLogingOut()', ground)
        gather = section('bool GatherFromGround(', 'uint32 GrantMissingGroundItems(')
        self.assertLess(gather.index('HasVendorErrandLease('), gather.index('StepBesidePoint('))
        self.assertIn('GatherFromGround(owner, bot, gathered)', SOURCE)  # full sync
        self.assertIn('GatherFromGround(player, bot, gathered)', SOURCE)  # two-second lane

    def test_formation_waits_for_point_generator_to_expire(self):
        follow = section('static bool NormalVendorFollow(', 'static bool EligibleErrandMerchant(')
        self.assertIn('HasStrategy("follow", BOT_STATE_NON_COMBAT)', follow)
        self.assertIn('HasStrategy("stay", BOT_STATE_NON_COMBAT)', follow)
        self.assertIn('GetMotionSlotType(MOTION_SLOT_IDLE) == IDLE_MOTION_TYPE', follow)
        self.assertNotIn('== POINT_MOTION_TYPE', follow)

    def test_errand_sale_resolves_players_item_and_exact_merchant_per_stack(self):
        sale = section('static uint32 SellVendorErrandJunk(', 'static void UpdateVendorErrands()')
        self.assertIn('std::vector<ObjectGuid>', sale)
        stack = sale.split('for (ObjectGuid const itemGuid : items)', 1)[1]
        dispatch = stack.split('HandleSellItemOpcode', 1)[0]
        for check in ('FindCompanionPlayer(', 'FindDeliveryOwner(', 'ReadyVendorErrandSale(', 'GetItemByGuid('):
            self.assertIn(check, dispatch)
        self.assertNotIn('VisitCarriedItems', stack)


if __name__ == '__main__':
    unittest.main()
