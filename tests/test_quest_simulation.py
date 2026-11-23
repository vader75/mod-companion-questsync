"""Source contracts at the module/core boundary (no world server needed)."""
import unittest
from pathlib import Path

SOURCE = (Path(__file__).resolve().parents[1] / 'src/mod_companion_questsync.cpp').read_text(encoding='utf-8')


def section(first, last):
    return SOURCE.split(first, 1)[1].split(last, 1)[0]


class QuestSimulation(unittest.TestCase):
    def test_follow_is_armed_by_actual_travel_not_walking_between_zones(self):
        script = section('class CompanionQuestSyncPlayerScript', 'void Addmod_companion_questsyncScripts(')
        self.assertTrue('PLAYERHOOK_ON_PLAYER_RESURRECT' in script, 'resurrect hook not registered')
        self.assertIn('PLAYERHOOK_ON_MAP_CHANGED', script)
        self.assertNotIn('void OnPlayerUpdateZone(', script)
        self.assertIn('void ObserveTaxiLanding(', SOURCE)
        follow = section('void FollowOwnerThroughTeleport(', 'void AddSkipped(')
        for guard in ('owner->GetMap()->Instanceable()', 'owner->IsBeingTeleported()', 'bot->IsBeingTeleported()',
                      'owner->GetTransport()', 'bot->GetTransport()', 'owner->GetVehicle()', 'bot->GetVehicle()',
                      'owner->UpdateGroundPositionZ(x, y, z)'):
            self.assertTrue(guard in follow, f'missing travel guard: {guard}')

    def test_ender_hints_rerank_spawn_candidates_and_pins_follow_the_bot_log(self):
        enders = section('struct EnderSpot', 'std::string MapCoords(')
        self.assertTrue('gameobject_questender' in enders, 'GO enders are missing')
        self.assertIn('std::vector<EnderSpot>', enders)
        self.assertNotIn('g_enderCache', enders)
        pins = section('std::string BuildPinLine(', 'void SendPinSync(')
        self.assertNotIn('spot.map == bot->GetMapId()', pins)
        self.assertNotIn('owner->IsQuestRewarded(questId)', pins)
        self.assertIn('spot.map', pins)

    def test_owner_progress_cannot_force_complete_ineligible_companion(self):
        mirror = section('void SyncQuestToBot(', '/// Quest pickup may retry')
        self.assertTrue('bool CompleteEligibleCompanionQuest(' in SOURCE, 'no native completion policy')
        complete = section('bool CompleteEligibleCompanionQuest(', 'void SyncQuestToBot(')
        for gate in ('g_cfg.syncCompletion', 'QUEST_STATUS_INCOMPLETE', 'bot->CanCompleteQuest(questId)',
                     'DescribeMissingItemsUncached(bot, quest).empty()'):
            self.assertTrue(gate in complete.split('bot->CompleteQuest(questId)', 1)[0], f'missing completion gate: {gate}')
        self.assertIn('CompleteEligibleCompanionQuest(bot, quest, report)', mirror)
        self.assertNotIn('bot->CompleteQuest(', mirror)
        extra = section('// Follow the owner out of a quest', 'stillExtra.insert(questId)')
        self.assertIn('g_cfg.dropExtraBotQuests || g_cfg.syncCompletion', extra)
        self.assertLess(extra.index('CompleteEligibleCompanionQuest('), extra.index('if (!g_cfg.dropExtraBotQuests)'))
        self.assertEqual(SOURCE.count('bot->CompleteQuest('), 1)

    def test_repeatable_cycle_is_distinct_from_history_and_timed_picks_need_time(self):
        mirror = section('void SyncQuestToBot(', '/// Quest pickup may retry')
        self.assertNotIn('if (bot->IsQuestRewarded(questId))', mirror)
        self.assertIn('g_ownerCycleSeen', mirror)
        self.assertIn('bool const singleCycle = timed || repeatable', mirror)
        self.assertIn('Timer < IN_MILLISECONDS', mirror)
        self.assertIn('g_ownerCycleSeen.erase', SOURCE.split('void OnPlayerQuestAccept(', 1)[1])
        self.assertIn('HandleQuestLogRemoveQuest(request)', SOURCE)

    def test_unlocked_quest_chest_keeps_scripts_traps_and_instance_loot(self):
        self.assertTrue('bool OpenGroundQuestChest(' in SOURCE, 'unlocked chests need a native path without a lock spell')
        opening = section('bool OpenGroundQuestChest(', 'bool GatherFromGround(')
        operations = ('GetLockId()', 'GO_FLAG_LOCKED', 'OnGossipHello(bot, go)',
                      'GossipHello(bot, false)', 'TriggeringLinkedGameObject', 'SendLoot(go->GetGUID()')
        for item in operations:
            self.assertTrue(item in opening, f'missing unlocked chest behavior: {item}')
        self.assertEqual(sorted(operations, key=opening.index), list(operations))

    def test_ground_collection_uses_the_spawn_loot_lifecycle(self):
        gathering = section('bool GatherFromGround(', 'uint32 GrantMissingGroundItems(')
        for state in ('isSpawned()', 'getLootState() != GO_READY', 'OpenGroundQuestChest(bot, go)', 'IsAtInteractDistance(bot)'):
            self.assertTrue(state in gathering, f'missing native object guard/path: {state}')
        self.assertNotIn('AutoStoreLoot(', gathering)
        self.assertNotIn('SetLootState(', gathering)
        self.assertIn('bool catchUpGroundItems = false;', SOURCE)

    def test_prerequisite_credit_never_stamps_a_live_or_resettable_copy(self):
        prereq = section('uint32 CreditPrerequisites(', 'bool IsGreyFor(')
        before_stamp = prereq.split('bot->SetRewardedQuest(prevId)', 1)[0]
        for gate in ('bot->FindQuestSlot(prevId) < MAX_QUEST_LOG_SIZE',
                     'bot->GetQuestStatus(prevId) != QUEST_STATUS_NONE',
                     'prev->IsRepeatable()', 'prev->IsSeasonal()'):
            self.assertTrue(gate in before_stamp, f'missing prerequisite gate: {gate}')

    def test_hand_in_requires_live_related_interactable_ender(self):
        self.assertIn('WorldObject* FindInteractableQuestEnder(', SOURCE)
        ender = section('WorldObject* FindInteractableQuestEnder(', 'void DeliverHandIns(')
        self.assertIn('hasInvolvedQuest(questId)', ender)
        self.assertIn('CanInteractWithQuestGiver(candidate)', ender)
        self.assertIn('CreatureListSearcher', ender)
        self.assertIn('GameObjectListSearcher', ender)
        turnin = section('void DeliverHandIns(', 'void TidyInventory(')
        gate = turnin.index('if (!giver)')
        reward = turnin.index('HandleQuestgiverChooseRewardOpcode(')
        self.assertIn('continue;', turnin[gate:reward])
        self.assertNotIn('bot->RewardQuest(', turnin)
        self.assertIn('FindQuestSlot(questId) < MAX_QUEST_LOG_SIZE', turnin[reward:])
