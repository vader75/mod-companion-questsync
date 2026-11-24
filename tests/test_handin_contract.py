"""Keep companion hand-in feedback separate from native player quest status."""
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

SOURCE = (Path(__file__).resolve().parents[1] / 'src/mod_companion_questsync.cpp').read_text(encoding='utf-8')


class HandInContract(unittest.TestCase):
    def test_cpp_map_snapshot_keeps_each_copy_and_omits_unmappable_or_inactive_quests(self):
        compiler = os.environ.get('CXX') or shutil.which('cl') or shutil.which('c++')
        if not compiler:
            self.skipTest('Run in a C++20 developer shell or set CXX to exercise map snapshots.')
        snippets = []
        for first, following in (
            ('    // Share current bag counts', '    /// A copy can be COMPLETE'),
            ('    bool WmaConvert(', '    /// The zone that owns'),
            ('    struct EnderPin', '    /// "map 52, 34"'),
            ('    std::optional<EnderPin> PlotEnder(EnderSpot const& spot)\n', '    /// content-keyed, bounded by the quests'),
            ('    WorldObject* FindReadyQuestEnder(Player* bot, Quest const* quest)\n', '    void DeliverHandIns('),
            ('    std::string SafePinName(', '    /// How far each companion'),
            ('    // The per-copy suffix', '    /// Send the pin line'),
        ):
            start = SOURCE.index(first)
            snippets.append(SOURCE[start:SOURCE.index(following, start)])
        fixture = Path(__file__).with_name('handin_pins.cpp.in').read_text(encoding='utf-8')
        with tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parent) as tmp:
            source, exe = Path(tmp) / 'pins.cpp', Path(tmp) / 'pins.exe'
            source.write_text(fixture.replace('@@PRODUCTION@@', '\n'.join(snippets)), encoding='utf-8')
            msvc = Path(compiler).stem.lower() in {'cl', 'clang-cl'}
            flags = ['/nologo', '/std:c++20', '/EHsc', '/utf-8', f'/Fe{exe}'] if msvc else ['-std=c++20', '-o', str(exe)]
            for args in ([compiler, *flags, str(source)], [str(exe)]):
                result = subprocess.run(args, cwd=tmp, capture_output=True, text=True, timeout=60)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_periodic_whispers_do_not_use_a_recent_command_as_permission(self):
        self.assertNotIn('g_commandBurst', SOURCE)
        queue = SOURCE.split('void FlushWhisperQueue(', 1)[1].split('auto& queue =', 1)[0]
        self.assertIn('now - last < Timing::WhisperGap', queue)

    def test_distant_hand_in_hint_is_shared_by_all_companions_of_the_owner(self):
        turnin = SOURCE.split('void DeliverHandIns(', 1)[1].split('if (!giver)\n', 1)[0]
        self.assertIn('TakeHandInTravelHint(owner, questId, nowTs)', turnin)
        gate = SOURCE.split('bool TakeHandInTravelHint(', 1)[1].split('void ReportPosition(', 1)[0]
        self.assertIn('owner->GetGUID().GetCounter()', gate)
        self.assertIn('| questId', gate)

    def test_position_travel_hints_share_dedup_but_arrival_does_not(self):
        report = SOURCE.split('void ReportPosition(', 1)[1].split('std::string DescribeMissingItemsUncached(', 1)[0]
        cross_map, rest = report.split('if (spot.map != bot->GetMapId())', 1)[1].split('if (giver)', 1)
        arrival, distant = rest.split('if (dist > 60.0f)', 1)
        self.assertIn('TakeHandInTravelHint(owner, questId, now)', cross_map)
        self.assertIn('45, questId, true);', cross_map)
        self.assertIn('TakeHandInTravelHint(owner, questId, now)', distant)
        self.assertNotIn('TakeHandInTravelHint', arrival)

    def test_no_second_hand_in_for_a_body_rejected_by_the_sync_resolver(self):
        sweep = SOURCE.split('SyncReport SyncCompanion(Player* owner)', 1)[1].split('aggregate.added += one.added;', 1)[0]
        self.assertNotIn('DeliverHandIns(owner, bot)', sweep)
        sync = SOURCE.split('SyncReport SyncCompanionFor(', 1)[1].split('bool CreateCompanion(', 1)[0]
        self.assertLess(sync.index('Player* bot = ResolveCompanion('), sync.index('DeliverHandIns(owner, bot)'))
        self.assertIn('if (report.completed)\n            DeliverHandIns(owner, bot);', sync)

    def test_reward_path_requires_personal_control_and_current_tracked_custody(self):
        turnin = SOURCE.split('void DeliverHandIns(', 1)[1].split('bool CanManageBags(', 1)[0]
        self.assertIn('if (!IsControlledCompanion(bot, owner)', turnin.split('for (uint16 slot', 1)[0])
        check = 'if (!IsControlledCompanion(bot, owner) || !HasCurrentCustody(ownerGuid, botGuid, true))'
        before_native_reward = turnin[:turnin.index('HandleQuestgiverChooseRewardOpcode')].rsplit('prodAt = nowTs;', 1)[1]
        self.assertIn(check + '\n                return;', before_native_reward)

    def test_missing_or_uninteractable_ender_is_only_a_navigation_hint(self):
        turnin = SOURCE.split('void DeliverHandIns(', 1)[1].split('bool CanManageBags(', 1)[0]
        missing = turnin.split('if (!giver)', 1)[1].split('continue;', 1)[0]
        self.assertIn('FindQuestEnder(bot, questId)', turnin)
        self.assertIn('"no-interactable-ender"', missing)
        self.assertNotIn('StepBesidePoint(', turnin)
        self.assertNotIn('AssistNearbyHandIns(', SOURCE)

    def test_module_hand_ins_use_only_the_native_chosen_giver_path(self):
        turnin = SOURCE.split('void DeliverHandIns(', 1)[1].split('bool CanManageBags(', 1)[0]
        self.assertIn('g_cfg.handInMode == 0)\n            return;', turnin)
        self.assertIn('HandleQuestgiverChooseRewardOpcode(request)', turnin)
        self.assertNotIn('HandleCommand(CHAT_MSG_WHISPER, "r", owner)', turnin)

    def test_conditional_object_ender_uses_native_quest_activation(self):
        finder = SOURCE.split('WorldObject* FindInteractableQuestEnder(', 1)[1].split('std::list<Creature*>', 1)[0]
        self.assertRegex(finder, r'!object->HasGameObjectFlag\(GO_FLAG_INTERACT_COND\)\s*\|\|\s*object->ActivateToQuest\(bot\)')
        for gate in ('object->isSpawned()', 'GO_FLAG_NOT_SELECTABLE', 'hasInvolvedQuest(questId)', 'CanInteractWithQuestGiver(candidate)'):
            self.assertIn(gate, finder)

    def test_companion_does_not_send_native_questgiver_status(self):
        self.assertNotIn('->SendQuestGiverStatus(', SOURCE)

    def test_addon_pin_checks_that_the_companion_has_its_turnin_items(self):
        pins = SOURCE.split('std::string BuildPinLine(', 1)[1].split('void SendPinSync(', 1)[0]
        self.assertIn('bool const ready =', pins)
        self.assertIn('DescribeMissingItemsUncached(bot, quest).empty()', pins)
        self.assertIn('ready ? FindReadyQuestEnder(bot, quest) : nullptr', pins)

    def test_reward_refusal_is_logged_at_the_existing_attempt_cadence(self):
        turnin = SOURCE.split('void DeliverHandIns(', 1)[1].split('bot->RewardQuest(', 1)[0]
        self.assertIn('LogHandInWait(owner, bot, quest, spot, dist, "reward-check")', turnin)
        self.assertLess(turnin.index('Timing::RewardProd'), turnin.index('"reward-check"'))

    def test_queued_arrival_whisper_does_not_claim_reward_and_expires_with_quest(self):
        report = SOURCE.split('void ReportPosition(', 1)[1].split('if (dist > 60.0f)', 1)[0]
        self.assertNotIn('handing it in now', report)
        self.assertIn('questId, true, 0, giver->GetGUID());', report)

    def test_ready_pin_never_falls_back_to_static_ender(self):
        pins = SOURCE.split('std::string BuildPinLine(', 1)[1].split('void SendPinSync(', 1)[0]
        self.assertRegex(pins, r"giver\s*\?\s*'h'\s*:\s*ready\s*\?\s*'u'\s*:\s*'p'")
        self.assertIn('ready ? FindReadyQuestEnder(bot, quest) : nullptr', pins)
        self.assertRegex(pins, r'if \(giver\)\s*pin = PlotEnder\(DescribeQuestEnder\(giver\)\);\s*else\s*FindQuestEnder\(bot, questId, &pin\);')
        gate = SOURCE.split('WorldObject* FindReadyQuestEnder(Player* bot, Quest const* quest)\n', 1)[1].split('void DeliverHandIns(', 1)[0]
        for check in ('QUEST_STATUS_COMPLETE', 'FindInteractableQuestEnder(', 'CanRewardQuest('):
            self.assertIn(check, gate)

    def test_active_copy_only_and_work_pins_not_replaced_by_pending_ender(self):
        pins = SOURCE.split('std::string BuildPinLine(', 1)[1].split('void SendPinSync(', 1)[0]
        self.assertRegex(pins, r'if \(status != QUEST_STATUS_INCOMPLETE && status != QUEST_STATUS_COMPLETE\)\s+continue;')
        self.assertRegex(pins, r'if \(!ready\)\s*\{[^}]+LootSpots\(bot, quest, workLeftSpots\)\)\s*PoiPins')
        self.assertRegex(pins, r"for \(auto const& spot : workLeftSpots\)\s*spots.push_back\(\{ spot.first, 'g', spot.second \}")

    def test_new_styles_and_fractional_positions_require_explicit_support(self):
        pins = SOURCE.split('std::string BuildPinLine(', 1)[1].split('void SendPinSync(', 1)[0]
        self.assertIn('if (giver || handInStates)', pins)
        self.assertIn('handInStates ? fmt::format("{:.4f}, {:.4f}"', pins)
        self.assertIn('if (versioned || MultiCompanionSpeech(bot))', pins)
        self.assertIn('existing.style != style', pins)
        command = SOURCE.split('static bool HandlePinsCommand(', 1)[1].split('uint32 botGuid = 0;', 1)[0]
        self.assertIn('bool const pinItems = protocol == "v2 epoch handin items";', command)
        self.assertIn('stream.handInStates = protocol == "v2 epoch handin" || pinItems;', command)
        self.assertIn('stream.pinItems = pinItems;', command)
        self.assertIn('SendProtocolLine(player, "Pin set handin 1");', command)
        self.assertIn('SendProtocolLine(player, "Pin set items 1");', command)

    def test_static_marker_checks_phase_and_converts_in_givers_own_map(self):
        finder = SOURCE.split('EnderSpot FindQuestEnder(', 1)[1].split('std::string MapCoords(', 1)[0]
        self.assertIn('if ((forPin || sameMap) && !(candidate.phase & bot->GetPhaseMask()))', finder)
        plot = SOURCE.split('std::optional<EnderPin> PlotEnder(EnderSpot const& spot)\n', 1)[1].split('/// content-keyed', 1)[0]
        self.assertIn('ZoneAtHeight(spot.map, spot.phase, spot.x, spot.y)', plot)
        self.assertIn('WmaConvert(gx, gy, int32(zone), spot.map)', plot)
        self.assertIn('gx >= 0.0f && gx <= 100.0f && gy >= 0.0f && gy <= 100.0f', plot)
        self.assertRegex(plot, r'if \(zoneName.empty\(\)\)\s*return std::nullopt;')

    def test_arrival_and_delayed_whisper_require_the_same_live_giver(self):
        report = SOURCE.split('void ReportPosition(', 1)[1].split('if (dist > 60.0f)', 1)[0]
        self.assertIn('FindReadyQuestEnder(bot, quest)', report)
        self.assertIn('giver->GetGUID()', report)
        self.assertIn('giver->GetGUID() != entry->readyGiver', SOURCE)
