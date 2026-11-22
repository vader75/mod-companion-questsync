/*
 * mod-companion-questsync
 *
 * Keeps a player's companion (the bound bot recorded by mod-wowlegends in
 * `wowlegends_companion`) on the same quest log - and optionally the same
 * level - as its owner, so the two can always quest together.
 *
 * Why this exists
 * ---------------
 * mod-playerbots already syncs COMPLETION one way (AiPlayerbot.SyncQuestWithPlayer:
 * a bot's copy of a quest is force-completed once the master's is complete), and
 * it auto-accepts a quest the master explicitly SHARES from the quest log. What
 * is missing is anything that:
 *   - notices a quest the owner has and the companion does not, and adds it
 *     without the owner clicking "Share quest" once per quest;
 *   - keeps the companion high enough level to take those quests at all
 *     (Player::SatisfyQuestLevel gates on quest_template.MinLevel, and a
 *     companion's level is whatever AddClass pool character it came from);
 *   - reconciles a prerequisite chain the companion never walked.
 *
 * This module does those three things from outside the core and outside
 * mod-playerbots: it only calls public Player/Quest APIs, so it survives a
 * repack update and can be switched off with one config line.
 *
 * Deliberately NOT done
 * ---------------------
 * - Objective counters are not copied. The owner never sees the companion's
 *   quest log, and kill/quest-event credit inside a group already reaches the
 *   companion, so only the STATUS (complete / not complete) has to match for
 *   the pair to hand in together.
 * - Timed quests are not copied (their client timer cannot be transferred).
 * - Companions are found through the binding table, never through AI internals,
 *   so this file has no dependency on mod-playerbots headers or link order.
 */

#include "Bag.h"
#include "Chat.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include <fstream>

// defined in DBCStores.cpp, id-indexed by the AreaTable's own first column
extern DBCStorage<AreaTableEntry> sAreaTableStore;

#include "GossipDef.h"   // PlayerTalkClass::SendQuestGiverStatus
#include "Group.h"
#include "Item.h"
#include "Log.h"
#include "Mail.h"
#include "LootMgr.h"
#include "Map.h"
#include "MapMgr.h"
#include "Cell.h"
#include "CellImpl.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include <chrono>
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "ItemUsageValue.h"  // playerbots' verdict on what an item is worth
#include "PlayerbotAI.h"     // sell vendor trash through the bot's own AI
#include "Playerbots.h"    // GET_PLAYERBOT_AI
#ifdef MOD_PLAYERBOTS
#include "StatsWeightCalculator.h"  // reward choice scored the way the bot's own AI would
#include "PlayerbotFactory.h"      // teach a hand-raised level what a trainer would
#include "RandomPlayerbotMgr.h"    // the per-bot value store keeps the profession choice
#endif
#include "QuestDef.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"

#include <algorithm>
#include <deque>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
    struct SyncSettings
    {
        bool enable             = true;
        bool autoSync           = true;
        bool syncCompletion     = true;
        bool dropExtraBotQuests = true;   // the companion follows the owner out of a quest too
        uint32 dropExtraGrace     = 120;  // seconds before an extra quest counts as abandoned
        bool requireGrouped     = true;
        bool allowCreate        = true;   // may this module mint a companion from the pool?
        bool skipInCombat       = true;
        bool skipTimedQuests    = true;
        bool forceAccept        = false;
        uint32 debug            = 0;     // 0 silent, 1 per-change, 2 + heartbeat
        bool pinSync            = true;
        uint32 pinSyncSeconds   = 600;   // re-send the pin line at least this often
        uint8 poolAccountType = 2;         // playerbots account type that marks an AddClass body
        bool extraArmourSlots = true;      // fill head/shoulder/waist/wrist/hand/back from item_template
        bool respectLegacyBindings = true;  // honour mod-wowlegends' binding table when picking a body
        bool dropFailedBotQuests = false; // a FAILED copy the owner never failed  // keep the addon map reconciled with the companion
        bool announce           = true;
        bool announceLogin      = true;
        bool catchUpGroundItems = true;    // finish a ground-object quest the world will not yield
        bool followThroughTeleport = true; // a hearthstone, portal or summon takes the companion too
        uint8 handInMode         = 1;      // 1 = server-side anywhere, 2 = server-side near the giver,
                                           // 0 = through the bot's own AI
        float handInNear         = 40.0f;  // HandInMode 2: the auto hand-in range, yards
        uint32 handInDelaySeconds = 20;    // a finished quest is visible this long before it rewards
        uint32 catchUpIdleSeconds = 120;   // how long the count must sit still first
        uint32 intervalSeconds  = 20;
        uint32 heartbeatSeconds = 300;  // Debug >= 2 heartbeat window (0 = off)
        uint32 levelMode        = 1;   // 0 off, 1 raise to owner, 2 exact
        uint32 notifySkipped    = 1;   // 0 off, 1 class/race only, 2 all refusals
        bool tidyInventory     = true;
        bool sellJunk          = true;      // ask the AI to sell vendor trash first
        bool tidyVendorJunk    = true;      // at zero slots, bin what playerbots would vendor
        bool mapPins           = true;      // push the companion's quest areas onto the owner's map
        bool tidySurplus       = true;      // last resort: bin spare consumables/materials
        bool gatherCatchUp     = true;      // the land asks a minimum of its gatherers
        bool vendorRestock     = true;      // near a trade/repair NPC: reagents topped up, gear mended
        uint32 extraBagItem    = 20400;     // Pumpkin Bag, 16 slots, no level requirement
        uint32 extraBags       = 4;         // bag slots to fill with it (0 = off)
        uint32 itemKeepQuality = 2;         // never bin anything at or above this (2 = green)
        uint32 keepFreeSlots   = 2;
        uint32 bagMargin       = 8;  // the tidy ladder starts this many slots before the wall
        bool mailHome          = true;    // post keep-worthy overflow to the owner
        std::string bindingTable = "cq_bindings";   // this module's own table
        std::string poolTable    = "cq_pool";       // this module's own ledger
    };

    SyncSettings g_cfg;

    // owner guid -> bot guid, refreshed on demand (a binding changes only when
    // the player runs .companion create/forget, which is rare).
    // owner guid -> (bot guid, expiry). Expiring matters: a companion forgotten
    // and recreated mid-session is a different bot_guid, and an unexpired "no
    // companion" answer would keep the module blind for the whole session - the
    // one drift it could not heal on its own.
    /// A row the module refused to believe, and why. Kept so the sweep can tell the
    /// player: a binding that silently disappears looks like a companion that will not
    /// follow, which is the complaint this module exists to answer.
    struct DroppedBinding
    {
        uint32 botGuid = 0;
        std::string name;
        std::string reason;
    };

    struct BindingSet
    {
        std::vector<uint32> bots;   // every companion bound to one owner
        std::vector<DroppedBinding> dropped;
        uint32 expires = 0;
    };

    std::unordered_map<uint32, BindingSet> g_bindingCache;
    std::unordered_map<uint32, bool> g_multiCompanion;   // owner guid -> more than one companion
    std::unordered_map<uint32, bool> g_multiCompanionBySpeaker;   // bot guid -> its owner has several

    /// Who is speaking: one global bool answered for the whole world and changed per
    /// pass, so an accept-hook whisper between owners read the previous owner's answer.
    /// The prefix decision is per owner, mirrored onto the speaking bot.
    bool MultiCompanionSpeech(Player* bot)
    {
        if (!bot)
            return false;

        if (bot->GetSession() && !bot->GetSession()->IsBot())
            return false;   // the owner asking: never a companion voice

        auto itr = g_multiCompanionBySpeaker.find(bot->GetGUID().GetCounter());
        return itr != g_multiCompanionBySpeaker.end() && itr->second;
    }

    /// Every settling window in the module, in one place.
    ///
    /// These numbers are throttles, not features, and they are where past bugs lived: a
    /// removal inside a teardown, a whisper that repeated every pass, a gear slot refilled
    /// every sweep. When something misbehaves, this table is the first place to look.
    namespace Timing
    {
        // removal and summons
        constexpr uint32 BotRemoveSettle   = 20;  // never remove within ~20 s of a summon
        constexpr uint32 SummonJudge       = 20;  // before declaring a summon failed

        // movement
        constexpr uint32 WalkStep          = 20;  // one giver or gather hop per 20 s
        constexpr uint32 FollowDelay       = 3;   // the destination is still loading
        constexpr float  FollowDistance    = 120.0f;  // yards; closer is not "lost"

        // whispers
        constexpr uint32 WhisperGap        = 45;  // per-owner default gap
        constexpr uint32 PositionNotice    = 180; // "I am in <zone>" repeats after this
        constexpr uint32 POIRetry          = 300; // a pinned POI is retried after this
        constexpr uint32 NudgeRepeat       = 300; // standing asks repeat every five minutes

        // upkeep
        constexpr uint32 SlotOnlyGrace     = 180; // bag space alone does not re-send the roster
        constexpr uint32 GearCheck         = 180; // how often upgrades are looked for
        constexpr uint32 GearRetry         = 300; // an unfilled slot is not retried sooner
        constexpr uint32 SellJunk          = 120; // vendor trash is sold at most this often
        constexpr uint32 JunkNotice        = 120; // "your bags were full" repeats after this
        constexpr uint32 RewardProd        = 60;  // how often a hand-in attempt is repeated
        constexpr uint32 AcceptWindow      = 60;  // a quest accepted this recently is still settling
        constexpr uint32 MailHome          = 120; // how often a "posted home" letter may go out
    }

    uint32 constexpr kBindingCacheSeconds = 60;

    /// Four companions is the whole point of this branch - a party of five with the
    /// owner - so that is the ceiling. A fifth row is almost certainly a seeding
    /// mistake, and one more bot than the group can hold would be swept on every pass
    /// for nothing.
    size_t constexpr kMaxCompanions = 4;

    // Sweep memo: hash of both quest logs plus both levels, with a "last pass
    // was clean" flag. A clean pass over an unchanged pair cannot produce a
    // different result, so it is skipped instead of re-walked every 20 s. Any
    // refusal (or add/complete/drop/level change) marks the pass dirty, so
    // work that failed is always retried rather than cached away.
    struct PassMemo
    {
        uint64 digest = 0;
        bool clean = false;
    };

    std::unordered_map<uint32, PassMemo> g_passMemo;     // owner guid -> memo
    std::unordered_map<uint32, uint32> g_junkNoticeAt;   // bot guid -> unix time

    // Heartbeat window state. Namespace-level rather than members of the world
    // script: the accept hook adds quests too, and a window total that ignored
    // them read "0 added" while the log showed four adds in the same period.
    uint32 g_hbTimer = 0;
    uint32 g_hbPasses = 0;
    uint32 g_hbAdded = 0;
    uint32 g_hbCompleted = 0;
    uint32 g_hbDropped = 0;
    uint32 g_hbReopened = 0;
    uint32 g_hbLocked = 0;
    std::map<uint32, std::string> g_hbLines;        // owner guid -> summary line
    std::map<uint32, uint32> g_hbHookAdds;          // owner guid -> adds via hook

    uint32 g_hbOnlineMs = 0;                        // seconds with a player in world
    uint32 g_hbOnlineProbe = 0;

    /// What a player's session actually did. Logged as one line on logout so a
    /// session can be reviewed without reading every heartbeat window.
    struct SessionTally
    {
        uint32 added = 0;       // via the sweep
        uint32 onAccept = 0;    // via the accept hook
        uint32 completed = 0;
        uint32 reopened = 0;
        uint32 refused = 0;
        uint32 gathered = 0;    // items taken off the ground, by the step or the fallback
        uint32 handedIn = 0;    // server-side rewards (HandInMode = 1)
        uint32 mailed = 0;      // items posted home (MailHome)
        uint32 letters = 0;     // letters sent home
        uint32 destroyed = 0;   // items the tidy ladder destroyed (grey trash only)
        uint8 levelFrom = 0;
        uint8 levelTo = 0;
    };

    std::unordered_map<uint32, SessionTally> g_session;   // owner guid -> tally
    std::map<uint32, std::string> g_lastSessionSummary;   // owner guid -> last logout's digest
    std::map<uint32, std::set<uint32>> g_sessionRefused;  // owner guid -> quests refused once
    std::map<uint32, uint32> g_walkAt;                    // bot guid -> unix time
    std::map<uint32, std::string> g_lastPinSync;          // owner guid -> last pin line sent
    std::map<uint32, uint32> g_heartbeatAt;               // owner guid -> last Hello
    /// Quest items that lie on the ground, by item id -> the game objects that hold them.
    ///
    /// Filled once at startup from `gameobject_questitem`. A "pick it off the ground" quest
    /// ("eight handfuls of oats") is not a kill-and-loot quest at all: the item is in a chest
    /// standing in a field, and no amount of fighting will produce it.
    std::map<uint32, std::vector<uint32>> g_itemGroundObjects;

    void LoadQuestGroundObjects()
    {
        g_itemGroundObjects.clear();

        QueryResult result = WorldDatabase.Query("SELECT GameObjectEntry, ItemId FROM gameobject_questitem");

        if (!result)
            return;

        do
        {
            Field* fields = result->Fetch();
            uint32 const goEntry = fields[0].Get<uint32>();
            uint32 const itemId = fields[1].Get<uint32>();

            if (goEntry && itemId)
                g_itemGroundObjects[itemId].push_back(goEntry);

        } while (result->NextRow());

        LOG_INFO("server.loading", "[CompanionQuestSync] {} ground object(s) indexed for gathering quests",
                 uint32(g_itemGroundObjects.size()));
    }

    std::map<uint32, std::string> g_lastCompSync;         // owner guid -> last companions line
    std::map<uint32, uint32> g_lastCompSyncAt;            // owner guid -> when that line was sent
    /// bot guid -> { has an unfinished ground objective, when that was last checked }
    std::map<uint32, std::pair<bool, uint32>> g_gatherPending;

    /// bot guid -> when the last "posted home" letter went out
    std::map<uint32, uint32> g_mailAt;

    /// (bot guid, quest id) -> when the copy first became ready to hand in.
    ///
    /// The middle ground between instant and manual: the finished quest is visible as a gold
    /// pin for HandInDelaySeconds, then rewards itself.
    std::map<uint64, uint32> g_readyAt;
    std::map<uint64, uint32> g_leadNudgeAt;  // (bot, quest) -> last lead-me whisper

    /// Owners whose teleport has just been seen: the companion is brought along a moment
    /// later, once the client has finished loading the destination.
    std::map<uint32, uint32> g_pendingFollow;   // owner guid -> when the teleport was seen


    /// How many grid searches this pass spent looking for ground objects (Debug >= 3).
    uint32 g_gridSearches = 0;

    /// Does this companion still owe items that lie on the ground? Cheap, and cached by the
    /// caller: it is the gate that keeps the two-second gather tick from walking every quest
    /// log in the world when nine companions out of ten have nothing to gather.
    bool HasPendingGroundObjective(Player* bot)
    {
        if (!bot || g_itemGroundObjects.empty())
            return false;

        for (auto const& entry : bot->getQuestStatusMap())
        {
            if (entry.second.Status != QUEST_STATUS_INCOMPLETE)
                continue;

            Quest const* quest = sObjectMgr->GetQuestTemplate(entry.first);
            if (!quest)
                continue;

            for (uint8 i = 0; i < quest->GetReqItemsCount(); ++i)
            {
                uint32 const itemId = quest->RequiredItemId[i];

                if (!itemId || g_itemGroundObjects.find(itemId) == g_itemGroundObjects.end())
                    continue;

                if (entry.second.ItemCount[i] < quest->RequiredItemCount[i])
                    return true;
            }
        }

        return false;
    }

    /// bot guid -> quest id -> { item count last seen, when that count last moved }
    std::map<uint32, std::map<uint32, std::pair<uint32, uint32>>> g_groundStall;
    /// (bot guid, quest id) whose "I finished gathering this" whisper has already gone out
    std::set<std::pair<uint32, uint32>> g_gatherAnnounced;

    std::map<uint32, std::string> g_lastPoolStock;        // owner guid -> last pool stock line
    std::map<uint32, uint32> g_pinSyncAt;                 // owner guid -> unix time
    std::map<uint32, std::set<uint64>> g_markedNpcs;      // owner guid -> npcs this module marked
    std::map<std::pair<uint32, uint32>, std::string> g_missingCache;  // (bot, quest) -> owed text
    std::map<std::pair<uint32, uint32>, std::string> g_poiCache;      // (zone, quest) -> "map X, Y"
    std::map<std::pair<uint32, uint32>, uint32> g_extraSince;         // (bot, quest) -> first seen extra
    std::set<std::pair<uint32, uint32>> g_failedSeen;                 // (bot, quest) already reported failed
    std::set<uint32> g_deadNoticeAt;                      // bots whose death was announced, one per death
    std::map<uint32, uint32> g_lastGatherZone;            // bot guid -> last zone the gather catch-up ran for
    std::map<uint32, uint32> g_lastVendorRestock;         // bot guid -> unix time of the last vendor stop
    std::unordered_map<uint32, uint32> g_commandBurst;    // owner guid -> unix time of the last command
    std::map<uint32, std::set<uint32>> g_repairing;       // bot guid -> quests being re-collected
    std::unordered_map<uint32, uint32> g_positionNoticeAt;  // bot guid -> unix time
    std::unordered_map<uint32, uint32> g_lastWhisperAt;     // owner guid -> unix time
    std::unordered_map<uint32, uint32> g_lastWhisperByBot;

    /// When a kit item was last created for a body *and slot*, so one unfillable slot does not
    /// throttle the others. Purged with the companion: guids are pooled.
    std::map<uint64, uint32> g_gearCreatedAt;  // bot guid -> unix time, for fairness
    std::map<uint64, uint32> g_stuckNoticeAt;  // owner+bot -> when we last said "summon it"

    /// A whisper waiting for the rate-limit window. `questId` is non-zero for
    /// refusals, so the queue can re-check the world before it speaks: a refusal
    /// held back for 45 s is often obsolete by the time it is sent (the level
    /// caught up, the quest arrived), and telling the player about a solved
    /// problem reads worse than saying nothing.
    struct QueuedWhisper
    {
        std::string text;        // as written, WITHOUT the speaker prefix
        uint32 botGuid = 0;      // who said it: the flush speaks as this companion
        uint32 questId = 0;      // 0 = no revalidation possible, only age applies
        uint32 queuedAt = 0;
        bool vital = false;      // pin-clear notices: never dropped as stale, always first
        bool handIn = false;     // an invitation to hand in: true while the quest is
                                 // still COMPLETE - the opposite predicate to the
                                 // pre-accept lines, which are true while the quest
                                 // is MISSING. Without this, an invitation held
                                 // through the rate limit could outlive its hand-in
                                 // and arrive claiming what already happened.
    };

    struct PendingNotice
    {
        uint32 questId = 0;
        std::string text;
    };

    std::map<uint32, std::vector<PendingNotice>> g_pendingNotices;  // bot guid -> lines
    std::unordered_map<uint32, uint32> g_sellAt;            // bot guid -> unix time
    std::unordered_map<uint32, uint32> g_equipAt;           // bot guid -> unix time
    std::map<uint32, std::deque<QueuedWhisper>> g_whisperQueue;  // owner guid -> waiting texts
    std::map<std::pair<uint32, uint32>, uint32> g_nudgeAt;     // (bot, quest) -> unix time
    std::map<std::pair<uint32, uint32>, std::string> g_openLatch;  // last owed-strings logged
    std::map<std::pair<uint32, uint32>, uint32> g_turnInHintAt;     // (bot, quest) -> unix time
    std::map<std::pair<uint32, uint32>, uint32> g_rewardProdAt;     // (bot, quest) -> unix time
    std::map<std::pair<uint32, uint32>, uint32> g_addedAt;          // (bot, quest) -> when we added it
    std::map<std::pair<uint32, uint32>, uint32> g_addedCount;       // (bot, quest) -> log size at add
    std::map<std::pair<uint32, uint32>, uint32> g_vanishLogged;      // (bot, quest) -> unix time
    std::unordered_map<uint32, std::string> g_pinnedQuests;        // owner guid -> pinned set

    bool AnyPlayerInWorld()
    {
        // Cached for five seconds. The heartbeat probes this once a second, and with a
        // hundred bot sessions in world that is a pointless walk every tick.
        static uint32 cachedAt = 0;
        static bool cached = false;

        uint32 const now = static_cast<uint32>(time(nullptr));
        if (cachedAt && now - cachedAt < 5)
            return cached;

        cachedAt = now;
        cached = false;

        for (auto const& pair : sWorldSessionMgr->GetAllSessions())
        {
            WorldSession* session = pair.second;
            if (!session || session->IsBot())
                continue;

            Player* player = session->GetPlayer();
            if (player && player->IsInWorld())
            {
                cached = true;
                break;
            }
        }

        return cached;
    }

    uint64 QuestLogDigest(Player* player)
    {
        // FNV-1a over level, quest ids and statuses: cheap, order-stable, and
        // sensitive to exactly the things a pass acts on.
        uint64 hash = 1469598103934665603ull;
        auto mix = [&hash](uint64 value)
        {
            hash ^= value;
            hash *= 1099511628211ull;
        };

        mix(player->GetLevel());

        for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 const questId = player->GetQuestSlotQuestId(slot);
            mix(questId);
            if (questId)
                mix(uint64(player->GetQuestStatus(questId)));
        }

        return hash;
    }
    // bot guid -> quest ids already reported to the owner, so an auto pass does
    // not repeat the same line every interval.
    std::unordered_map<uint32, std::set<uint32>> g_notified;

    bool g_tableReady = false;
    bool g_tableWarned = false;

    struct SyncReport
    {
        uint32 added = 0;
        uint32 completed = 0;
        uint32 dropped = 0;
        uint32 classRaceLocked = 0;
        uint32 reopened = 0;      // COMPLETE copies that could never be handed in
        uint32 prereqs = 0;       // ancestors credited so a chained quest is takeable
        uint32 ownerQuestCount = 0;
        uint32 botQuestCount = 0;
        int32 botFreeSlots = -1;  // -1 when the companion is offline
        uint32 failed = 0;        // copies the companion failed on its own
        std::vector<std::string> lines;   // one heartbeat line per companion
        uint8 levelFrom = 0;
        uint8 levelTo = 0;
        uint8 ownerLevel = 0;
        uint8 botLevel = 0;
        std::string ownerName;
        std::string botName;
        std::vector<std::string> skipped;   // human readable, one per refused quest
        std::vector<uint32> refusedIds;     // the same refusals, by quest id
        bool hasCompanion = false;
        bool companionOnline = false;
        bool grouped = false;
    };

    void LoadSettings()
    {
        g_cfg.enable             = sConfigMgr->GetOption<bool>("CompanionQuestSync.Enable", true);
        g_cfg.autoSync           = sConfigMgr->GetOption<bool>("CompanionQuestSync.AutoSync", true);
        g_cfg.syncCompletion     = sConfigMgr->GetOption<bool>("CompanionQuestSync.SyncCompletion", true);
        g_cfg.dropExtraBotQuests = sConfigMgr->GetOption<bool>("CompanionQuestSync.DropExtraBotQuests", true);
        g_cfg.dropExtraGrace     = sConfigMgr->GetOption<uint32>("CompanionQuestSync.DropExtraGraceSeconds", 120);
        g_cfg.requireGrouped     = sConfigMgr->GetOption<bool>("CompanionQuestSync.RequireGrouped", false);
        g_cfg.allowCreate        = sConfigMgr->GetOption<bool>("CompanionQuestSync.AllowCreate", true);
        g_cfg.skipInCombat       = sConfigMgr->GetOption<bool>("CompanionQuestSync.SkipInCombat", true);
        g_cfg.skipTimedQuests    = sConfigMgr->GetOption<bool>("CompanionQuestSync.SkipTimedQuests", true);
        g_cfg.forceAccept        = sConfigMgr->GetOption<bool>("CompanionQuestSync.ForceAccept", false);
        g_cfg.debug              = sConfigMgr->GetOption<uint32>("CompanionQuestSync.Debug", 0);
        g_cfg.pinSync            = sConfigMgr->GetOption<bool>("CompanionQuestSync.PinSync", true);
        g_cfg.pinSyncSeconds     = sConfigMgr->GetOption<uint32>("CompanionQuestSync.PinSyncSeconds", 600);
        g_cfg.dropFailedBotQuests = sConfigMgr->GetOption<bool>("CompanionQuestSync.DropFailedBotQuests", false);
        g_cfg.announce           = sConfigMgr->GetOption<bool>("CompanionQuestSync.AnnounceAtStartup", true);
        g_cfg.announceLogin      = sConfigMgr->GetOption<bool>("CompanionQuestSync.AnnounceOnLogin", true);
        g_cfg.catchUpGroundItems = sConfigMgr->GetOption<bool>("CompanionQuestSync.CatchUpGroundItems", true);
        g_cfg.followThroughTeleport = sConfigMgr->GetOption<bool>("CompanionQuestSync.FollowThroughTeleport", true);
        g_cfg.handInMode         = sConfigMgr->GetOption<uint8>("CompanionQuestSync.HandInMode", 1);
        g_cfg.handInNear         = sConfigMgr->GetOption<float>("CompanionQuestSync.HandInNearDistance", 40.0f);
        g_cfg.handInDelaySeconds = sConfigMgr->GetOption<uint32>("CompanionQuestSync.HandInDelaySeconds", 20);
        g_cfg.catchUpIdleSeconds = sConfigMgr->GetOption<uint32>("CompanionQuestSync.CatchUpIdleSeconds", 120);
        g_cfg.intervalSeconds    = sConfigMgr->GetOption<uint32>("CompanionQuestSync.IntervalSeconds", 30);
        g_cfg.heartbeatSeconds   = sConfigMgr->GetOption<uint32>("CompanionQuestSync.HeartbeatSeconds", 300);
        g_cfg.levelMode          = sConfigMgr->GetOption<uint32>("CompanionQuestSync.LevelMode", 1);
        g_cfg.notifySkipped      = sConfigMgr->GetOption<uint32>("CompanionQuestSync.NotifySkippedQuests", 2);
        g_cfg.tidyInventory      = sConfigMgr->GetOption<bool>("CompanionQuestSync.TidyInventory", true);
        g_cfg.sellJunk           = sConfigMgr->GetOption<bool>("CompanionQuestSync.SellJunk", true);
        g_cfg.tidyVendorJunk     = sConfigMgr->GetOption<bool>("CompanionQuestSync.TidyVendorJunk", true);
        g_cfg.mapPins            = sConfigMgr->GetOption<bool>("CompanionQuestSync.MapPins", true);
        g_cfg.tidySurplus        = sConfigMgr->GetOption<bool>("CompanionQuestSync.TidySurplus", true);
        g_cfg.gatherCatchUp      = sConfigMgr->GetOption<bool>("CompanionQuestSync.GatherCatchUp", true);
        g_cfg.vendorRestock      = sConfigMgr->GetOption<bool>("CompanionQuestSync.VendorRestock", true);
        g_cfg.extraBagItem       = sConfigMgr->GetOption<uint32>("CompanionQuestSync.ExtraBagItem", 20400);
        g_cfg.extraBags          = sConfigMgr->GetOption<uint32>("CompanionQuestSync.ExtraBags", 4);
        g_cfg.itemKeepQuality    = sConfigMgr->GetOption<uint32>("CompanionQuestSync.ItemKeepQuality", 2);
        g_cfg.keepFreeSlots      = sConfigMgr->GetOption<uint32>("CompanionQuestSync.KeepFreeSlots", 2);
        g_cfg.bagMargin          = sConfigMgr->GetOption<uint32>("CompanionQuestSync.BagMargin", 8);
        g_cfg.mailHome           = sConfigMgr->GetOption<bool>("CompanionQuestSync.MailHome", true);
        g_cfg.bindingTable       = sConfigMgr->GetOption<std::string>("CompanionQuestSync.BindingTable", "cq_bindings");
        g_cfg.poolTable          = sConfigMgr->GetOption<std::string>("CompanionQuestSync.PoolTable", "cq_pool");
        g_cfg.respectLegacyBindings =
            sConfigMgr->GetOption<bool>("CompanionQuestSync.RespectLegacyBindings", true);
        g_cfg.extraArmourSlots =
            sConfigMgr->GetOption<bool>("CompanionQuestSync.ExtraArmourSlots", true);
        g_cfg.poolAccountType =
            sConfigMgr->GetOption<uint8>("CompanionQuestSync.PoolAccountType", 2);

        if (g_cfg.intervalSeconds < 5)
            g_cfg.intervalSeconds = 5;

        if (g_cfg.levelMode > 2)
            g_cfg.levelMode = 2;

        // The table names are interpolated into SQL, so accept nothing but a
        // plain identifier. Anything else disables the module rather than
        // risking an injection through the config file.
        auto plainIdentifier = [](std::string const& name)
        {
            if (name.empty())
                return false;

            for (char const c : name)
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                      || (c >= '0' && c <= '9') || c == '_'))
                    return false;

            return true;
        };

        bool ok = plainIdentifier(g_cfg.bindingTable) && plainIdentifier(g_cfg.poolTable);

        if (!ok)
        {
            LOG_ERROR("server.loading", "[CompanionQuestSync] BindingTable '{}' is not a plain identifier - module disabled.", g_cfg.bindingTable);
            g_cfg.enable = false;
        }
    }

    bool CheckBindingTable()
    {
        QueryResult result = CharacterDatabase.Query("SHOW TABLES LIKE '{}'", g_cfg.bindingTable);
        return static_cast<bool>(result);
    }

    /// What columns this binding table actually has.
    ///
    /// `wowlegends_companion` and the multi table differ, and a hand-made table may differ
    /// from both. Probing once is cheaper than a failed query on every cache fill, and it
    /// lets the module work with the bare minimum: owner, bot, and optionally a name and a
    /// creation time.
    bool g_bindingHasName = false;
    bool g_bindingHasCreatedAt = false;
    bool g_bindingReleasedKnown = false;
    bool g_crossOwnerWarned = false;

    void ProbeBindingTable()
    {
        g_bindingHasName = static_cast<bool>(
            CharacterDatabase.Query("SHOW COLUMNS FROM {} LIKE 'companion_name'", g_cfg.bindingTable));
        g_bindingHasCreatedAt = static_cast<bool>(
            CharacterDatabase.Query("SHOW COLUMNS FROM {} LIKE 'created_at'", g_cfg.bindingTable));
        g_bindingReleasedKnown = static_cast<bool>(
            CharacterDatabase.Query("SHOW TABLES LIKE 'wowlegends_companion_released'"));

        // The "since we last parted" digest outlives restarts: it is written at the
        // owner's logout and read at the companion's next arrival, so a world restart
        // between the two must not lose it.
        CharacterDatabase.Execute(
            "CREATE TABLE IF NOT EXISTS cq_digest ("
            "owner_guid INT UNSIGNED NOT NULL PRIMARY KEY, summary TEXT NOT NULL, stamp INT UNSIGNED NOT NULL)");
    }

    /// Everything this module remembers about one companion, forgotten with it.
    ///
    /// Companion guids come from a pool and are reused. A memo, a latch or an
    /// "already told them" set left behind by a released body would quietly apply to the
    /// next companion that inherits the guid - which reads as the module ignoring work
    /// rather than as a stale map.
    template <typename Map>
    void EraseByBot(Map& map, uint32 botGuid)
    {
        for (auto it = map.begin(); it != map.end();)
        {
            if (it->first.first == botGuid)
                it = map.erase(it);
            else
                ++it;
        }
    }

    /// The one thing worth saying to this owner on this pass.
    ///
    /// Several conditions can want a whisper in the same pass - a hand-in ready, items still
    /// owed, a nudge, junk in the bags - and the old shape queued them all, so the player got
    /// a burst and then a stale line forty seconds later. Collect instead, keep the most
    /// actionable, and say exactly one thing.
    struct PassNote
    {
        uint8 priority = 0;      // 3 hand-in ready, 2 work owed, 1 chatter
        std::string text;
        uint32 questId = 0;      // a hand-in note dies with its hand-in (SayToOwner validates)
        bool handIn = false;
    };

    std::map<uint32, PassNote> g_passNotes;   // owner guid -> best note this pass

    void Note(Player* owner, uint8 priority, std::string const& text,
              uint32 questId = 0, bool handIn = false)
    {
        if (!owner || text.empty())
            return;

        PassNote& note = g_passNotes[owner->GetGUID().GetCounter()];

        if (priority >= note.priority)
        {
            // Say what lost, at Debug >= 3: "one message per pass" is only trustworthy if the
            // discarded candidates are visible, otherwise a quiet pass looks like a bug.
            if (g_cfg.debug >= 3 && !note.text.empty() && note.priority != priority)
            {
                LOG_INFO("server.loading",
                         "[CompanionQuestSync]   note for {}: priority {} replaces {} ('{}' dropped)",
                         owner->GetName(), uint32(priority), uint32(note.priority), note.text);
            }

            note = PassNote{ priority, text, questId, handIn };
        }
        else if (g_cfg.debug >= 3)
        {
            LOG_INFO("server.loading",
                     "[CompanionQuestSync]   note for {}: priority {} dropped, {} outranks it ('{}')",
                     owner->GetName(), uint32(priority), uint32(note.priority), text);
        }
    }


    void PurgeCompanionState(uint32 botGuid)
    {
        g_passMemo.erase(botGuid);
        g_lastGatherZone.erase(botGuid);
        g_groundStall.erase(botGuid);
        g_gatherPending.erase(botGuid);

        // guids are pooled: a stale ready-timer would hand the next body's quest in early
        for (auto itr = g_readyAt.begin(); itr != g_readyAt.end();)
            itr = (itr->first >> 32 == botGuid) ? g_readyAt.erase(itr) : std::next(itr);

        g_mailAt.erase(botGuid);

        // guids are pooled, so a stale "already announced" pair would silence the next
        // companion that inherits this body
        for (auto itr = g_gatherAnnounced.begin(); itr != g_gatherAnnounced.end();)
            itr = (itr->first == botGuid) ? g_gatherAnnounced.erase(itr) : std::next(itr);

        g_repairing.erase(botGuid);
        g_notified.erase(botGuid);
        g_pendingNotices.erase(botGuid);
        g_sellAt.erase(botGuid);
        g_equipAt.erase(botGuid);
        g_lastWhisperByBot.erase(botGuid);
        for (uint8 slot = 0; slot < EQUIPMENT_SLOT_END; ++slot)
            g_gearCreatedAt.erase((uint64(botGuid) << 8) | slot);   // guids are pooled

        g_walkAt.erase(botGuid);            // an inherited throttle delays the first step
        g_positionNoticeAt.erase(botGuid);
        g_gearCreatedAt.erase(uint64(botGuid) << 8);   // stray slot-0 key from older builds

        // (bot, quest) pairs: the next body may need the "I made a mess" whisper again
        for (auto itr = g_failedSeen.begin(); itr != g_failedSeen.end();)
            itr = (itr->first == botGuid) ? g_failedSeen.erase(itr) : std::next(itr);

        EraseByBot(g_nudgeAt, botGuid);
        EraseByBot(g_openLatch, botGuid);
        EraseByBot(g_turnInHintAt, botGuid);
        EraseByBot(g_rewardProdAt, botGuid);
        EraseByBot(g_addedAt, botGuid);
        EraseByBot(g_addedCount, botGuid);
        EraseByBot(g_vanishLogged, botGuid);
        EraseByBot(g_extraSince, botGuid);
    }

    void DropBindingRow(uint32 ownerGuid, DroppedBinding const& drop)
    {
        CharacterDatabase.Execute("DELETE FROM {} WHERE owner_guid = {} AND bot_guid = {}",
                                  g_cfg.bindingTable, ownerGuid, drop.botGuid);

        PurgeCompanionState(drop.botGuid);

        LOG_INFO("server.loading", "[CompanionQuestSync] owner {}: dropped binding to {} ({}) - {}",
                 ownerGuid, drop.name, drop.botGuid, drop.reason);
    }

    /// Read and validate one owner's companions.
    ///
    /// Nothing else in the server writes this table, so a row that has stopped making
    /// sense is only ever noticed here. Validation therefore runs whenever the cache is
    /// filled (once a minute per owner) rather than on a timer of its own: that is the
    /// moment the module is looking at the table anyway.
    ///
    /// The four ways a binding goes bad, all of them real:
    ///   - the character behind it is gone (deleted, or the pool body was wiped)
    ///   - it was released after it was bound, and the body belongs to the pool again
    ///   - the same bot is bound to another owner, so two sweeps fight over one character
    ///   - the live body carries a different name than the row (forget-then-create, where
    ///     the body keeps its old name)
    bool FindCompanions(uint32 ownerGuid, std::vector<uint32>& bots)
    {
        uint32 const now = static_cast<uint32>(time(nullptr));

        auto itr = g_bindingCache.find(ownerGuid);
        if (itr != g_bindingCache.end() && itr->second.expires > now)
        {
            bots = itr->second.bots;
            return !bots.empty();
        }

        BindingSet set;
        set.expires = now + kBindingCacheSeconds;

        struct Row
        {
            uint32 botGuid = 0;
            std::string name;
            uint32 created = 0;
        };

        std::vector<Row> rows;

        std::string const nameColumn = g_bindingHasName ? "companion_name" : "''";
        std::string const createdColumn = g_bindingHasCreatedAt ? "UNIX_TIMESTAMP(created_at)" : "0";

        std::string const query = "SELECT bot_guid, " + nameColumn + ", " + createdColumn +
                                  " FROM " + g_cfg.bindingTable + " WHERE owner_guid = " + std::to_string(ownerGuid);

        if (QueryResult result = CharacterDatabase.Query(query.c_str()))
        {
            do
            {
                Field* fields = result->Fetch();
                Row row;
                row.botGuid = fields[0].Get<uint32>();
                row.name = fields[1].Get<std::string>();
                row.created = fields[2].Get<uint32>();

                if (row.botGuid)
                    rows.push_back(row);
            } while (result->NextRow());
        }

        // Duplicates, then the cap. Both are seeding mistakes: a duplicate would have the
        // sweep act twice on one body, and a fifth companion is more bots than a group.
        std::vector<Row> unique;
        for (Row const& row : rows)
        {
            bool dupe = false;
            for (Row const& seen : unique)
                if (seen.botGuid == row.botGuid)
                    dupe = true;

            std::string const shown = row.name.empty() ? std::to_string(row.botGuid) : row.name;

            if (dupe)
            {
                set.dropped.push_back({ row.botGuid, shown, "it is bound twice" });
                continue;
            }

            if (unique.size() >= kMaxCompanions)
            {
                set.dropped.push_back({ row.botGuid, shown, "it is past the four-companion cap" });
                continue;
            }

            unique.push_back(row);
        }
        rows.swap(unique);

        std::string list;
        for (Row const& row : rows)
        {
            if (!list.empty())
                list += ",";

            list += std::to_string(row.botGuid);
        }

        std::map<uint32, std::string> liveNames;
        std::map<uint32, uint32> releasedAt;
        std::map<uint32, std::pair<uint32, uint32>> otherOwner;   // bot -> (owner, created)

        if (!list.empty())
        {
            if (QueryResult chars = CharacterDatabase.Query("SELECT guid, name FROM characters WHERE guid IN ({})", list))
            {
                do
                {
                    Field* fields = chars->Fetch();
                    liveNames[fields[0].Get<uint32>()] = fields[1].Get<std::string>();
                } while (chars->NextRow());
            }

            if (g_bindingReleasedKnown)
            {
                if (QueryResult rel = CharacterDatabase.Query(
                        "SELECT bot_guid, UNIX_TIMESTAMP(released_at) FROM wowlegends_companion_released WHERE bot_guid IN ({})", list))
                {
                    do
                    {
                        Field* fields = rel->Fetch();
                        releasedAt[fields[0].Get<uint32>()] = fields[1].Get<uint32>();
                    } while (rel->NextRow());
                }
            }

            if (QueryResult other = CharacterDatabase.Query(
                    "SELECT bot_guid, owner_guid, " + createdColumn + " FROM " + g_cfg.bindingTable +
                    " WHERE bot_guid IN (" + list + ") AND owner_guid <> " + std::to_string(ownerGuid)))
            {
                do
                {
                    Field* fields = other->Fetch();
                    otherOwner[fields[0].Get<uint32>()] = { fields[1].Get<uint32>(), fields[2].Get<uint32>() };
                } while (other->NextRow());
            }
        }

        for (Row const& row : rows)
        {
            auto const live = liveNames.find(row.botGuid);
            std::string const shown = live != liveNames.end() ? live->second
                                     : (row.name.empty() ? std::to_string(row.botGuid) : row.name);

            if (live == liveNames.end())
            {
                set.dropped.push_back({ row.botGuid, shown, "its character no longer exists" });
                continue;
            }

            // Released after this binding was made: the body went back to the pool, and the
            // pool re-issues bodies. Following it from here means following whoever holds
            // it next.
            if (row.created)
            {
                auto const released = releasedAt.find(row.botGuid);
                if (released != releasedAt.end() && released->second >= row.created)
                {
                    set.dropped.push_back({ row.botGuid, shown, "it was released after you bound it" });
                    continue;
                }
            }

            // The same bot under two owners: two sweeps levelling and re-questing one
            // character against each other, and two companions whispering with one name.
            auto const other = otherOwner.find(row.botGuid);
            if (other != otherOwner.end())
            {
                bool const theirs = (g_bindingHasCreatedAt && row.created)
                    ? (other->second.second < row.created ||
                       (other->second.second == row.created && other->second.first < ownerGuid))
                    : other->second.first < ownerGuid;

                if (theirs)
                {
                    set.dropped.push_back({ row.botGuid, shown, "it is bound to another player" });
                    continue;
                }

                LOG_ERROR("server.loading",
                          "[CompanionQuestSync] bot {} is bound to both {} and {}; {} keeps it - remove the other row",
                          row.botGuid, ownerGuid, other->second.first,
                          (g_bindingHasCreatedAt && row.created) ? "the older binding" : "the lower owner guid");
            }
            else if (!g_bindingHasCreatedAt && !g_crossOwnerWarned)
            {
                g_crossOwnerWarned = true;
                LOG_ERROR("server.loading",
                          "[CompanionQuestSync] binding table '{}' has no created_at column; two players binding "
                          "the same bot cannot be settled by age. Add the column, or keep every bot single-owner.",
                          g_cfg.bindingTable);
            }

            set.bots.push_back(row.botGuid);
        }

        for (DroppedBinding const& drop : set.dropped)
            DropBindingRow(ownerGuid, drop);

        bots = set.bots;
        g_bindingCache[ownerGuid] = std::move(set);
        return !bots.empty();
    }

    /// The first bound companion, for the paths that only ever needed one.
    bool FindCompanionGuid(uint32 ownerGuid, uint32& botGuid)
    {
        std::vector<uint32> bots;
        if (!FindCompanions(ownerGuid, bots))
            return false;

        botGuid = bots.front();
        return true;
    }

    void InvalidateCompanionGuid(uint32 ownerGuid)
    {
        g_bindingCache.erase(ownerGuid);
    }

    Player* FindCompanionPlayer(uint32 botGuid)
    {
        return ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(botGuid));
    }

    /// Bring companions along when the owner teleports.
    ///
    /// A hearthstone, a portal or a summon moves the player and leaves the companion where it
    /// stood - the one thing in the game that should never be left behind in another zone.
    /// Hooked on the teleport rather than inferred from distance: walking away must never
    /// teleport anything, and a hearth must always.
    void FollowOwnerThroughTeleport()
    {
        if (g_pendingFollow.empty())
            return;

        constexpr uint32 kFollowDelaySeconds = Timing::FollowDelay;
        constexpr float kFollowDistanceYards = Timing::FollowDistance;

        uint32 const now = uint32(time(nullptr));

        for (auto itr = g_pendingFollow.begin(); itr != g_pendingFollow.end();)
        {
            uint32 const ownerGuid = itr->first;

            // The one-time window used to vanish while a companion was mid-fight: it was
            // skipped for combat, the entry was erased anyway, and the wipe at the hearth
            // left it behind permanently. Sixty seconds of retry covers a wipe or an
            // escort fight; after that the follow has genuinely missed its moment.
            if (now - itr->second > 60)
            {
                itr = g_pendingFollow.erase(itr);
                continue;
            }

            if (now - itr->second < kFollowDelaySeconds)
            {
                ++itr;
                continue;
            }

            Player* owner = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(ownerGuid));
            std::vector<uint32> bots;

            if (!owner || !owner->IsInWorld() || !FindCompanions(ownerGuid, bots))
            {
                itr = g_pendingFollow.erase(itr);
                continue;
            }

            // The owner releasing to a ghost used to drag the living companion to the
            // graveyard, where it stood abandoned for the whole corpse run. Hold the
            // pending follow instead - the resurrect hook re-arms it - and give up only
            // if the owner stays dead for three minutes.
            if (!owner->IsAlive())
            {
                if (now - itr->second > 180)
                    itr = g_pendingFollow.erase(itr);
                else
                    ++itr;
                continue;
            }

            bool anySkipped = false;

            for (uint32 const botGuid : bots)
            {
                Player* bot = FindCompanionPlayer(botGuid);
                if (!bot || !bot->IsInWorld() || !bot->IsAlive() || bot->IsInCombat())
                {
                    anySkipped = true;   // held for the retry window, not abandoned
                    continue;
                }

                bool const elsewhere = bot->GetMapId() != owner->GetMapId();
                float const away = elsewhere ? 999999.0f : bot->GetDistance(owner);

                if (away <= kFollowDistanceYards)
                    continue;

                float x = owner->GetPositionX() + 2.0f;
                float y = owner->GetPositionY();
                float z = owner->GetPositionZ();

                bot->UpdateGroundPositionZ(x, y, z);
                bot->StopMoving();

                if (elsewhere)
                    bot->TeleportTo(owner->GetMapId(), x, y, z, owner->GetOrientation());
                else
                    bot->NearTeleportTo(x, y, z, owner->GetOrientation());

                LOG_INFO("server.loading",
                         "[CompanionQuestSync] {} followed {} through a teleport - map {}, {} yd behind",
                         bot->GetName(), owner->GetName(), owner->GetMapId(),
                         elsewhere ? 0 : uint32(away));
            }

            // The owner's own death path holds its entry explicitly; a combat-skipped
            // companion holds it for the retry window instead of losing it forever.
            if (!anySkipped)
                itr = g_pendingFollow.erase(itr);
            else
                ++itr;
        }
    }

    void AddSkipped(SyncReport& report, Quest const* quest, std::string const& reason)
    {
        if (report.skipped.size() >= 10)
            return;

        report.skipped.push_back(quest->GetTitle() + " (" + std::to_string(quest->GetQuestId()) + "): " + reason);
        report.refusedIds.push_back(quest->GetQuestId());
    }

    /// Why the companion cannot take this quest. `expected` marks the refusals
    /// that are structural rather than a missing prerequisite: a companion of a
    /// different class or race can never take another class's / race's quests,
    /// so those are not failures of the sync.
    struct Refusal
    {
        std::string reason;
        bool expected = false;
    };

    Refusal ExplainRefusal(Player* bot, Quest const* quest)
    {
        uint32 const botLevel = bot->GetLevel();
        uint32 const maxLevel = quest->GetMaxLevel();

        // A level gap is fixable (LevelMode), so it is reported first: once the
        // companion catches up, the next pass surfaces the real limit.
        if (maxLevel > 0 && botLevel > maxLevel)
            return { "I am level " + std::to_string(botLevel) + ", above its cap of " + std::to_string(maxLevel) + " - I need to be at most that", false };

        if (botLevel < quest->GetMinLevel())
            return { "I need level " + std::to_string(quest->GetMinLevel()) + " first (I am " + std::to_string(botLevel) + ")", false };

        if (!bot->SatisfyQuestClass(quest, false))
            return { "it is a class quest my class cannot take", true };

        if (!bot->SatisfyQuestRace(quest, false))
            return { "it is not open to my race", true };

        if (!bot->SatisfyQuestPreviousQuest(quest, false))
        {
            // Name the *first* predecessor the companion is actually missing.
            // GetPrevQuestId() is only one entry of prevQuests (a chain can list
            // several), so naming it unconditionally sent players after the wrong
            // quest whenever a quest had more than one ancestor.
            Quest const* missing = nullptr;

            for (int32 const prevId : quest->prevQuests)
            {
                if (prevId <= 0)
                    continue;

                Quest const* prev = sObjectMgr->GetQuestTemplate(uint32(prevId));
                if (prev && !bot->IsQuestRewarded(uint32(prevId)))
                {
                    missing = prev;
                    break;
                }
            }

            if (!missing && quest->GetPrevQuestId() > 0)
                missing = sObjectMgr->GetQuestTemplate(uint32(quest->GetPrevQuestId()));

            return { missing ? "I have not finished '" + missing->GetTitle() + "' yet"
                             : std::string("a prerequisite quest is missing"),
                     false };
        }

        if (!bot->SatisfyQuestReputation(quest, false))
            return { "I need more reputation with them first", false };

        if (!bot->SatisfyQuestSkill(quest, false))
            return { "I need the right profession for it", false };

        if (!bot->SatisfyQuestConditions(quest, false))
            return { "its conditions are not met yet", false };

        return { "I am not eligible for it yet", false };
    }

    /// Everything the companion says to its owner goes through here.
    ///
    /// One considerate rule set: at most one whisper per 45 s per owner, never a
    /// wall of them when several quests fail at once, and always something the
    /// player can act on. Unthrottled per-quest whispering is what makes a rough
    /// levelling session feel like an error console.
    bool SayToOwner(Player* bot, Player* owner, std::string const& text, uint32 minGap = 45,
                    uint32 validWhileMissing = 0, bool handIn = false)
    {
        if (!owner || !owner->GetSession())
            return false;

        // With several companions the player cannot tell who is speaking, so the name
        // goes in front. With one it would be noise, and the module is careful not to be
        // noisy.
        std::string const spoken = (MultiCompanionSpeech(bot) && bot)
            ? bot->GetName() + ": " + text
            : text;

        uint32 const ownerGuid = owner->GetGUID().GetCounter();
        uint32 const botGuid = bot ? bot->GetGUID().GetCounter() : 0;
        uint32 const now = static_cast<uint32>(time(nullptr));
        uint32& last = g_lastWhisperAt[ownerGuid];

        // A command the player just issued is a conversation, not a broadcast: its
        // answers speak at once, whatever the ambient gap is. Without this a button
        // press felt dead - the verdicts joined the back of a queue that drains one
        // line per window, and the player had already clicked again by the time the
        // first one arrived.
        auto burst = g_commandBurst.find(ownerGuid);
        bool const inBurst = burst != g_commandBurst.end() && now - burst->second < 20;

        if (!inBurst && last && now - last < minGap)
        {
            // Hold it, do not lose it. The one-shot "help me finish this quest"
            // request used to be dropped here when the window was busy, so a player
            // who was mid-fight simply never heard it again.
            //
            // What is queued is the text alone plus WHO said it: the prefix is applied
            // when it is spoken, by the companion that said it. Baking the name in here
            // meant a line queued by one companion could be spoken by another and read
            // "Soak: Healy: ...".
            auto& queue = g_whisperQueue[ownerGuid];
            bool dup = false;
            for (auto const& entry : queue)
                if (entry.botGuid == botGuid && entry.text == text)
                    dup = true;

            // "Finished 'X' - that pin can go." is the message that removes a map pin:
            // dropping it as stale was how a handed-in quest left a blue pin behind
            // until the next full pin sync. It is spoken first and never aged out.
            // The profession catch-up lines ride the same lane: they are the news the
            // player is watching for, and a queue full of fresh-login invitations
            // would otherwise silence them for the better part of twenty minutes.
            bool const vital = text.rfind("Finished '", 0) == 0 ||
                               text.rfind("I caught my ", 0) == 0 ||
                               text.rfind("I took up ", 0) == 0;

            if (!dup && (vital || queue.size() < 4))
            {
                if (vital)
                    queue.insert(queue.begin(), { text, botGuid, validWhileMissing, now, true, handIn });
                else
                    queue.push_back({ text, botGuid, validWhileMissing, now, false, handIn });
            }

            if (g_cfg.debug >= 2)
                LOG_INFO("server.loading", "[CompanionQuestSync] whisper queued (rate limit): {}", spoken);
            return false;
        }

        last = now;
        if (botGuid)
            g_lastWhisperByBot[botGuid] = now;

        bot->Whisper(spoken, LANG_UNIVERSAL, owner);

        // Log what was actually said. Without this the log only ever recorded what was
        // held back, so a report about wording could not be checked against it.
        if (g_cfg.debug >= 1)
            LOG_INFO("server.loading", "[CompanionQuestSync] {} whispered: {}", bot->GetName(), spoken);

        return true;
    }

    void FlushNotes(Player* owner, Player* speaker)
    {
        auto itr = g_passNotes.find(owner->GetGUID().GetCounter());
        if (itr == g_passNotes.end())
            return;

        std::string const text = itr->second.text;
        uint8 const priority = itr->second.priority;
        uint32 const noteQuest = itr->second.questId;
        bool const noteHandIn = itr->second.handIn;
        g_passNotes.erase(itr);

        if (g_cfg.debug >= 3)
            LOG_INFO("server.loading", "[CompanionQuestSync]   note for {}: sending priority {}", owner->GetName(), uint32(priority));

        if (!text.empty() && speaker)
            SayToOwner(speaker, owner, text, 45, noteQuest, noteHandIn);
    }

    /// Tell the owner, once per quest, that the companion could not take it.
    /// The message comes from the companion itself, which is how every other
    /// bot message in this realm arrives; repeats are suppressed until the quest
    /// leaves and re-enters the owner's log.
    void NotifyRefusal(Player* owner, Player* bot, Quest const* quest, Refusal const& refusal)
    {
        bool const wanted = (refusal.expected ? g_cfg.notifySkipped >= 1 : g_cfg.notifySkipped >= 2);
        if (!wanted)
            return;

        uint32 const botGuid = bot->GetGUID().GetCounter();
        std::set<uint32>& seen = g_notified[botGuid];
        if (!seen.insert(quest->GetQuestId()).second)
            return;

        // Queue rather than whisper: one level-up can refuse half a dozen quests
        // in a single pass, and six whispers in a row is noise. FlushNotices turns
        // the batch into one sentence that ends with a next step.
        g_pendingNotices[botGuid].push_back({ quest->GetQuestId(),
                                              "'" + quest->GetTitle() + "': " + refusal.reason });

        if (g_cfg.debug >= 1)
            LOG_INFO("server.loading", "[CompanionQuestSync] {} notified owner about '{}' ({})", bot->GetName(), quest->GetTitle(), quest->GetQuestId());
    }

    /// Where a quest is handed in, so the companion can say how far it is rather
    /// than just "I cannot do this".
    struct EnderSpot
    {
        bool found = false;
        uint32 entry = 0;
        std::string name;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        uint32 map = 0;
        uint32 zone = 0;
    };

    uint32 ZoneAt(uint32 mapId, uint32 phaseMask, float x, float y, float z)
    {
        Map* map = sMapMgr->FindMap(mapId, 0);
        if (!map)
            return 0;

        return map->GetZoneId(phaseMask, x, y, z);
    }

    std::unordered_map<uint64, EnderSpot> g_enderCache;   // (bot, quest) -> its nearest ender spot

    EnderSpot const& FindQuestEnder(Player* bot, uint32 questId)
    {
        uint32 const botGuid = bot ? bot->GetGUID().GetCounter() : 0;
        auto itr = g_enderCache.find((uint64(botGuid) << 32) | questId);
        if (itr != g_enderCache.end())
            return itr->second;

        EnderSpot spot;

        float const bx = bot ? bot->GetPositionX() : 0.0f;
        float const by = bot ? bot->GetPositionY() : 0.0f;
        uint32 const bmap = bot ? bot->GetMapId() : 0;

        // Event NPCs have one spawn per city - the Pilgrim's Bounty table turned in at
        // whichever row LIMIT 1 happened to return, a table the companions never sat at.
        // Spawns on the companion's own map lead, then the nearest to where it stands.
        QueryResult result = WorldDatabase.Query(
            "SELECT ct.name, c.position_x, c.position_y, c.position_z, c.map, ce.id, c.zoneId "
            "FROM creature_questender ce "
            "JOIN creature_template ct ON ct.entry = ce.id "
            "JOIN creature c ON c.id1 = ce.id "
            "WHERE ce.quest = {} "
            "ORDER BY (c.map <> {}) ASC, "
            "POWER(c.position_x - {}, 2) + POWER(c.position_y - {}, 2) ASC LIMIT 1",
            questId, bmap, bx, by);

        if (result)
        {
            Field* fields = result->Fetch();
            spot.found = true;
            spot.name = fields[0].Get<std::string>();
            spot.x = fields[1].Get<float>();
            spot.y = fields[2].Get<float>();
            spot.z = fields[3].Get<float>();
            spot.map = fields[4].Get<uint32>();
            spot.entry = fields[5].Get<uint32>();
            spot.zone = fields[6].Get<uint32>();

            // creature.zoneId is 0 for plenty of NPCs (Morgan Pestle among them), which
            // is how a Stormwind giver ended up pinned on the Elwynn map.
            if (!spot.zone)
                spot.zone = ZoneAt(spot.map, 1, spot.x, spot.y, spot.z);
        }

        return g_enderCache[(uint64(botGuid) << 32) | questId] = spot;
    }

    /// "map 52, 34" for a world position, in the client's own 0-100 scale, so the
    /// player can find the spot even when the map will not show a pin for a quest
    /// that is not in their own log.
    std::string MapCoords(float x, float y, uint32 zoneId)
    {
        float mapX = x;
        float mapY = y;

        // Map2ZoneCoordinates is the world -> map-percentage direction; the
        // similarly named Zone2MapCoordinates converts the other way, which is why
        // every coordinate this module produced came back out of range and was
        // dropped by the guard below - no "map X, Y", so nothing for the addon to pin.
        Map2ZoneCoordinates(mapX, mapY, zoneId);

        if (mapX < 0.0f || mapX > 100.0f || mapY < 0.0f || mapY > 100.0f)
            return std::string();

        return "map " + std::to_string(uint32(mapX)) + ", " + std::to_string(uint32(mapY));
    }
    /// "map X, Y" for a quest's objective area, taken from its POI points. The
    /// addon pins whatever the companion says out loud, so a hint without coordinates
    /// is a hint that never becomes a pin - which is exactly what happened when only
    /// the "far away" branch carried them.
    std::string PoiCoords(Player* bot, Quest const* quest)
    {
        QuestPOIVector const* poi = sObjectMgr->GetQuestPOIVector(quest->GetQuestId());
        if (!poi)
            return std::string();

        for (QuestPOI const& entry : *poi)
        {
            if (entry.points.empty())
                continue;

            if (entry.MapId != bot->GetMapId())
                continue;

            // POI geometry is static and the conversion depends only on the zone, so the
            // pair (zone, quest) is enough to reuse the answer - ZoneAt walks terrain.
            std::pair<uint32, uint32> const key{ bot->GetZoneId(), quest->GetQuestId() };
            auto const cached = g_poiCache.find(key);
            if (cached != g_poiCache.end())
                return cached->second;

            float x = float(entry.points[0].x);
            float y = float(entry.points[0].y);
            // QuestPOIPoint is 2D; the zone lookup only needs the horizontal position,
            // and passing y as a height (as this once did) put the probe underground.
            float const z = 0.0f;

            // Convert against the POI's own zone, and only when that is the zone the
            // companion is standing in - otherwise the numbers belong to a different
            // map page and read as a real location.
            uint32 const poiZone = ZoneAt(entry.MapId, bot->GetPhaseMask(), x, y, z);
            if (poiZone && poiZone != bot->GetZoneId())
                continue;

            Map2ZoneCoordinates(x, y, poiZone ? poiZone : bot->GetZoneId());

            if (x >= 0.0f && x <= 100.0f && y >= 0.0f && y <= 100.0f)
            {
                std::string const spot = "map " + std::to_string(uint32(x)) + ", " + std::to_string(uint32(y));
                g_poiCache[key] = spot;
                return spot;
            }

            if (g_cfg.debug >= 2)
                LOG_INFO("server.loading", "[CompanionQuestSync] POI {} out of range after conversion: {:.1f}, {:.1f}",
                         quest->GetQuestId(), x, y);
        }

        // Cache the negative too, or a quest with no usable area is re-scanned every pass.
                // keyed by game content (zone x quest), not by body: no purge, and bounded
                // by the quests that exist rather than by the pool
        g_poiCache[{ bot->GetZoneId(), quest->GetQuestId() }] = std::string();
        return std::string();
    }

    std::string DescribeMissingItemsUncached(Player* bot, Quest const* quest);  // defined below

    /// Could this player ever have taken this quest, by class and race alone?
    ///
    /// Deliberately not Player::CanTakeQuest: that also checks level, reputation and the
    /// prerequisite chain, and an abandoned quest often fails those precisely because it
    /// was abandoned. Class and race are the only gates that say "this quest was never
    /// the owner's to hold".
    bool OwnerFitsQuestMask(Player* owner, Quest const* quest)
    {
        uint32 const classMask = quest->GetRequiredClasses();
        if (classMask && !(classMask & (1u << (owner->getClass() - 1))))
            return false;

        uint32 const raceMask = quest->GetAllowableRaces();
        if (raceMask && !(raceMask & (1u << (owner->getRace() - 1))))
            return false;

        return true;
    }

    /// One bag scan per (bot, quest) per pass instead of three. The delivery routine,
    /// the owner-visible markers and the turn-in reporter all ask the same question
    /// about the same quest, and each answer walks the companion's bags.
    std::string DescribeMissingItems(Player* bot, Quest const* quest)
    {
        std::pair<uint32, uint32> const key{ bot->GetGUID().GetCounter(), quest->GetQuestId() };

        auto itr = g_missingCache.find(key);
        if (itr != g_missingCache.end())
            return itr->second;

        std::string text = DescribeMissingItemsUncached(bot, quest);
        g_missingCache[key] = text;
        return text;
    }

    std::string ZoneName(uint32 zoneId)
    {
        // Direct ID lookup. GetAreaEntryByAreaID on this build round-trips through the
        // area's exploration bit and looks THAT up as an entry id - an arbitrary row -
        // which is how a pin standing in Elwynn introduced itself as Tkashi Ruins. The
        // store is id-indexed, so the honest path is one LookupEntry.
        if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(zoneId))
            if (zone->area_name[0])
                return zone->area_name[0];

        return "an unknown zone";
    }

    /// Map numbers and a zone name for a quest's objective area, anchored to the POI's
    /// OWN area id - not to wherever the bot happens to be standing. A freshly summoned
    /// bot reports its previous zone until it has moved, and naming the zone from the bot
    /// meant a quest's pin changed identity (and visibility on other maps) with every
    /// walk. The quest's area is the authority: the same quest, the same pin, the same
    /// zone name, wherever the body is.
    /// Our own WorldMapArea table, parsed straight from the .dbc file.
    ///
    /// Two reasons this exists instead of anything the core offers: the terrain probe
    /// (ZoneAt) has been quietly wrong on this repack - Elwynn POI points reporting as
    /// Tkashi Ruins, a Dun Morogh dam where neither is - and the core's
    /// WorldMapAreaEntry struct does not match its own load format here (the format
    /// string loads column 2 as a string where the struct expects an int), so every
    /// float bound read through it was shifted, and its store is indexed by map_id
    /// besides. The raw file has no such drift.
    struct WorldMapAreaRow
    {
        uint32 mapId;
        int32 areaId;
        float y1, y2, x1, x2;
    };

    std::vector<WorldMapAreaRow> g_worldMapAreas;

    void LoadWorldMapAreas()
    {
        g_worldMapAreas.clear();

        std::string const path = sConfigMgr->GetOption<std::string>("DataDir", "data") + "/dbc/WorldMapArea.dbc";
        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            LOG_ERROR("server.loading", "[CompanionQuestSync] cannot open {} - pin zones fall back to the terrain probe", path);
            return;
        }

        uint32 header[5] = {};
        in.read(reinterpret_cast<char*>(header), sizeof(header));

        uint32 const rows = header[1];
        uint32 const fieldCount = header[2];
        uint32 const rowSize = header[3];

        if (fieldCount < 8 || rowSize < fieldCount * 4)
        {
            LOG_ERROR("server.loading", "[CompanionQuestSync] unexpected WorldMapArea.dbc shape ({} rows, {} fields)", rows, fieldCount);
            return;
        }

        for (uint32 r = 0; r < rows; ++r)
        {
            char row[512] = {};
            in.read(row, rowSize);

            int32 const* f = reinterpret_cast<int32 const*>(row);
            g_worldMapAreas.push_back({ uint32(f[1]), f[2],
                                        reinterpret_cast<float const&>(f[4]), reinterpret_cast<float const&>(f[5]),
                                        reinterpret_cast<float const&>(f[6]), reinterpret_cast<float const&>(f[7]) });
        }

        LOG_INFO("server.loading", "[CompanionQuestSync] loaded {} WorldMapArea bound(s) for pin zones", uint32(g_worldMapAreas.size()));
    }

    /// The zone that owns a world position, from WorldMapArea bounds - data, not terrain.
    /// The smallest-area match wins, so a sub-zone beats its parent when both contain
    /// the point. Bounds are stored high-low on some rows; normalise both axes.
    uint32 WorldMapZoneAt(uint32 mapId, float x, float y)
    {
        uint32 best = 0;
        float bestSize = 0.0f;

        for (WorldMapAreaRow const& row : g_worldMapAreas)
        {
            if (row.mapId != mapId)
                continue;

            float const xlo = std::min(row.x1, row.x2);
            float const xhi = std::max(row.x1, row.x2);
            float const ylo = std::min(row.y1, row.y2);
            float const yhi = std::max(row.y1, row.y2);

            if (x < xlo || x > xhi || y < ylo || y > yhi)
                continue;

            float const size = (xhi - xlo) * (yhi - ylo);
            if (!best || size < bestSize)
            {
                best = uint32(row.areaId);
                bestSize = size;
            }
        }

        return best;
    }

    /// World -> client map percentages, from the same bounds row. This mirrors the
    /// core's Map2ZoneCoordinates math but reads OUR table - the core's typed struct
    /// does not match its load format on this repack, and its store is indexed by
    /// map_id, so an area id cannot be looked up through it at all.
    bool WmaConvert(float& x, float& y, int32 areaId)
    {
        for (WorldMapAreaRow const& row : g_worldMapAreas)
        {
            if (row.areaId != areaId)
                continue;

            float px = (x - row.x1) / ((row.x2 - row.x1) / 100.0f);
            float py = (y - row.y1) / ((row.y2 - row.y1) / 100.0f);
            std::swap(px, py);                       // client axes are swapped

            x = px;
            y = py;
            return true;
        }

        return false;
    }

    /// The zone that owns a world position, probed at the TERRAIN HEIGHT of the point.
    ///
    /// Three authorities were tried and each lied differently: the terrain probe at z=0
    /// (underground - garbage areas), the WorldMapArea bounds (border overlaps are
    /// genuinely inside two rectangles, and a size tie-break cannot know which side of
    /// an invisible line a farm stands on), and GetAreaEntryByAreaID's name round-trip.
    /// The one probe that never lied is the one the core itself runs for players:
    /// height first (vmaps are sound - bodies walk on this terrain), then zone. The
    /// bounds table stays as the fallback for the rare tile the probe cannot read.
    uint32 ZoneAtHeight(uint32 mapId, uint32 phaseMask, float x, float y)
    {
        Map* map = sMapMgr->FindMap(mapId, 0);
        if (!map)
            return WorldMapZoneAt(mapId, x, y);

        float const ground = map->GetHeight(x, y, MAX_HEIGHT);
        uint32 const zone = map->GetZoneId(phaseMask, x, y, ground + 2.0f);

        return zone ? zone : WorldMapZoneAt(mapId, x, y);
    }


    /// content-keyed, bounded by the quests that exist (same policy as g_poiCache)
    // One quest can owe work in several areas: quest_poi holds a row per objective,
    // each with its own points, so the map should show one pin per area, not just the
    // first. Capped at four so a sprawling quest cannot carpet the map; the whisper
    // (`.companionsync quests`) still names every area.
    std::map<uint32, std::vector<std::pair<std::string, std::string>>> g_poiPinCache;

    /// Where the work actually is: the creatures that drop the item the quest requires.
    ///
    /// A wanted poster's POI points at the poster, and an outline's first corner is
    /// wherever the cartographer started the path - neither is the mob. The loot table
    /// is: for every required item, the creatures whose loot holds it, spawned where
    /// they spawn. Nearest to the companion first, capped like every other pin; a quest
    /// with no resolvable source (or a failed query, retried on a slow clock) falls
    /// through to the POI vector.
    std::map<uint32, std::vector<std::tuple<uint32, float, float>>> g_lootSpawns;  // quest -> (map, x, y)
    std::map<uint32, uint32> g_lootSpawnsQueried;                                 // quest -> last attempt

    bool LootSpots(Player* bot, Quest const* quest, std::vector<std::pair<std::string, std::string>>& out)
    {
        uint32 const questId = quest->GetQuestId();
        uint32 const nowTs = static_cast<uint32>(time(nullptr));

        auto spawns = g_lootSpawns.find(questId);
        if (spawns == g_lootSpawns.end() || spawns->second.empty())
        {
            uint32& lastTry = g_lootSpawnsQueried[questId];
            if (lastTry && nowTs - lastTry < 600)
                return false;
            lastTry = nowTs;

            std::vector<std::tuple<uint32, float, float>> found;
            for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
            {
                uint32 const item = quest->RequiredItemId[i];
                if (!item || !quest->RequiredItemCount[i])
                    continue;

                // lootid (template) is what the loot templates key on, not the entry
                QueryResult rows = WorldDatabase.Query(
                    "SELECT c.position_x, c.position_y, c.map FROM creature c "
                    "JOIN creature_template ct ON ct.entry = c.id1 "
                    "JOIN creature_loot_template clt ON clt.entry = ct.lootid AND clt.item = {} "
                    "GROUP BY c.guid", item);

                if (rows)
                    do
                    {
                        Field* f = rows->Fetch();
                        found.push_back({ f[2].Get<uint32>(), f[0].Get<float>(), f[1].Get<float>() });
                    } while (rows->NextRow());
            }

            g_lootSpawns[questId] = found;
            spawns = g_lootSpawns.find(questId);
        }

        if (spawns->second.empty())
            return false;

        // On the companion's map, nearest first, then the usual conversion each pin.
        std::vector<std::tuple<uint32, float, float>> here;
        for (auto const& s : spawns->second)
            if (std::get<0>(s) == bot->GetMapId())
                here.push_back(s);

        std::sort(here.begin(), here.end(), [bot](auto const& a, auto const& b)
        {
            float const ax = std::get<1>(a) - bot->GetPositionX();
            float const ay = std::get<2>(a) - bot->GetPositionY();
            float const bx = std::get<1>(b) - bot->GetPositionX();
            float const by = std::get<2>(b) - bot->GetPositionY();
            return (ax * ax + ay * ay) < (bx * bx + by * by);
        });

        for (auto const& s : here)
        {
            if (out.size() >= 4)
                break;

            float x = std::get<1>(s);
            float y = std::get<2>(s);
            uint32 const zone = ZoneAtHeight(bot->GetMapId(), bot->GetPhaseMask(), x, y);
            if (!zone)
                continue;

            if (!WmaConvert(x, y, int32(zone)) && zone != 12)
                continue;

            if (x < 0.0f || x > 100.0f || y < 0.0f || y > 100.0f)
                continue;

            std::string const numbers = "map " + std::to_string(uint32(x)) + ", " + std::to_string(uint32(y));
            if (numbers.size() <= 5)
                continue;

            if (std::find_if(out.begin(), out.end(),
                             [&numbers](std::pair<std::string, std::string> const& s) { return s.first == numbers; }) != out.end())
                continue;

            out.push_back({ numbers, ZoneName(zone) });
        }

        return !out.empty();
    }

    bool PoiPins(Player* bot, Quest const* quest, std::vector<std::pair<std::string, std::string>>& out)
    {
        auto const cached = g_poiPinCache.find(quest->GetQuestId());
        if (cached != g_poiPinCache.end())
        {
            out = cached->second;
            return !out.empty();
        }

        QuestPOIVector const* poi = sObjectMgr->GetQuestPOIVector(quest->GetQuestId());
        if (!poi)
            return false;

        for (QuestPOI const& entry : *poi)
        {
            if (entry.points.empty())
                continue;

            if (entry.MapId != bot->GetMapId())
                continue;

            float x = float(entry.points[0].x);
            float y = float(entry.points[0].y);

            uint32 const zone = ZoneAtHeight(entry.MapId, bot->GetPhaseMask(), x, y);
            if (!zone)
                continue;

            if (!WmaConvert(x, y, int32(zone)) && zone != 12)
                continue;

            if (x < 0.0f || x > 100.0f || y < 0.0f || y > 100.0f)
                continue;

            // Keep the "map X, Y" form: BuildPinLine strips the prefix itself, and this
            // function stripping it too once ate the first coordinate - "80,45" became
            // "5" and the pin could not be parsed by anyone.
            std::string const numbers = "map " + std::to_string(uint32(x)) + ", " + std::to_string(uint32(y));
            if (numbers.size() <= 5)
                continue;

            // Distinct areas only: several POI rows of one quest often share a point,
            // and the client already merges same-spot pins per companion.
            if (std::find_if(out.begin(), out.end(),
                             [&numbers](std::pair<std::string, std::string> const& s) { return s.first == numbers; }) != out.end())
                continue;

            out.push_back({ numbers, ZoneName(zone) });
            if (out.size() >= 4)
                break;
        }

        g_poiPinCache[quest->GetQuestId()] = out;
        return !out.empty();
    }


    void SendPinSync(Player* owner, bool force);  // defined below
    void SendHeartbeat(Player* owner);            // defined below
#ifdef MOD_PLAYERBOTS
    std::pair<uint32, uint32> TeachTrainerContent(Player* bot, Player* owner);  // defined below
    std::string TeachProfessions(Player* bot, Player* owner);                   // defined below
#endif
    void SendCompSync(Player* owner, bool force);  // defined below
    Player* ResolveCompanion(Player* owner, SyncReport& report);
    /// Put the companion at its owner's side, offset away from `toward`.
    ///
    /// Stepping the bot to the *quest giver* meant a visible blink to somebody
    /// else's coordinates, which reads as a bug because nothing the player did
    /// caused it. A step to a couple of yards beside the owner reads as "the
    /// companion caught up" - and still lands in interaction range whenever the
    /// owner is standing at the giver, which is the normal turn-in.
    void StepBesideOwner(Player* owner, Player* bot, float towardX, float towardY)
    {
        float dx = owner->GetPositionX() - towardX;
        float dy = owner->GetPositionY() - towardY;
        float len = std::sqrt(dx * dx + dy * dy);

        if (len < 0.5f)                                  // owner is standing on the giver
        {
            float const o = owner->GetOrientation() + 3.14159265f;
            dx = std::cos(o);
            dy = std::sin(o);
            len = 1.0f;
        }

        float const x = owner->GetPositionX() + 2.0f * dx / len;
        float const y = owner->GetPositionY() + 2.0f * dy / len;
        float z = owner->GetPositionZ();

        bot->UpdateGroundPositionZ(x, y, z);
        bot->StopMoving();
        bot->NearTeleportTo(x, y, z, bot->GetOrientation());
    }

    /// Step the companion to a ground object it needs, so its own loot logic can take it.
    ///
    /// playerbots recognises a quest game object as loot and opens it happily - it simply
    /// never walks towards one. A companion that follows its owner therefore stood in the
    /// oat field with "Milly's Harvest 0/8" for a whole session while every kill-collect
    /// quest progressed, and the module reported the quest as merely unfinished. This is the
    /// same trick as the quest-giver step, aimed at a chest: put the bot beside the object,
    /// let its own AI open and loot it.
    bool GatherFromGround(Player* owner, Player* bot, std::string& what)
    {
        if (!owner || !bot || !bot->IsAlive() || bot->IsInCombat() || g_itemGroundObjects.empty())
            return false;

        for (auto const& entry : bot->getQuestStatusMap())
        {
            if (entry.second.Status != QUEST_STATUS_INCOMPLETE)
                continue;

            Quest const* quest = sObjectMgr->GetQuestTemplate(entry.first);
            if (!quest)
                continue;

            for (uint8 i = 0; i < quest->GetReqItemsCount(); ++i)
            {
                uint32 const itemId = quest->RequiredItemId[i];
                uint32 const need = quest->RequiredItemCount[i];

                if (!itemId || entry.second.ItemCount[i] >= need)
                    continue;

                auto const ground = g_itemGroundObjects.find(itemId);
                if (ground == g_itemGroundObjects.end())
                    continue;

                uint32 const have = entry.second.ItemCount[i];
                uint32 const missing = need > have ? need - have : 0;
                uint32 collected = 0;
                std::string gatheredFrom;

                // Clear what is still owed in one pass. One object per pass meant eight
                // chests took four minutes of walking to a field the companion is already
                // standing in - the player reads that as "it is not collecting anything".
                for (uint32 const goEntry : ground->second)
                {
                    while (collected < missing)
                    {
                        ++g_gridSearches;
                        GameObject* go = bot->FindNearestGameObject(goEntry, 60.0f, true);

                        if (!go || go->GetGoState() != GO_STATE_READY)
                            break;

                        // Stand beside it first, on the side facing away from the owner, so the
                        // move reads as "the companion went to gather" and not a stray blink.
                        float dx = go->GetPositionX() - owner->GetPositionX();
                        float dy = go->GetPositionY() - owner->GetPositionY();
                        float len = std::sqrt(dx * dx + dy * dy);

                        if (len < 0.5f)
                        {
                            dx = 1.0f;
                            dy = 0.0f;
                            len = 1.0f;
                        }

                        float const x = go->GetPositionX() - 2.0f * dx / len;
                        float const y = go->GetPositionY() - 2.0f * dy / len;
                        float z = go->GetPositionZ();

                        bot->UpdateGroundPositionZ(x, y, z);
                        bot->StopMoving();
                        bot->NearTeleportTo(x, y, z, bot->GetOrientation());

                        // Take what it holds, the way clicking it would. playerbots lists a
                        // quest object as loot and never opens one, so stepping alone left the
                        // count at 0/8 forever while the log cheerfully said "stepped".
                        uint32 const lootId = go->GetGOInfo() ? go->GetGOInfo()->GetLootId() : 0;
                        uint32 taken = 0;

                        if (lootId)
                        {
                            uint32 const before = bot->GetItemCount(itemId, true);
                            bot->AutoStoreLoot(NULL_BAG, NULL_SLOT, lootId, LootTemplates_Gameobject);
                            taken = bot->GetItemCount(itemId, true) - before;
                        }

                        if (!taken)
                        {
                            // no bag room, or the object had nothing: leave it standing rather
                            // than deactivating it, and stop rather than hop in place.
                            if (bot->GetFreeInventorySpace() == 0)
                                SayToOwner(bot, owner, "I reached '" + go->GetName() +
                                                       "' but my bags are full - I will tidy and come back for it.", 90);
                            else if (g_cfg.debug >= 2)
                                LOG_INFO("server.loading", "[CompanionQuestSync] gathering: {} reached '{}' but took nothing",
                                         bot->GetName(), go->GetName());
                            break;
                        }

                        go->SetLootState(GO_JUST_DEACTIVATED);   // emptied, so it cannot be farmed twice
                        collected += taken;
                        gatheredFrom = go->GetName();
                    }

                    if (collected >= missing)
                        break;
                }

                if (!collected)
                    continue;

                what = std::string(gatheredFrom) + " for " + quest->GetTitle();

                g_session[owner->GetGUID().GetCounter()].gathered += collected;

                // Say so, once. A completion with no word from the companion reads as nothing
                // having happened, and the useful half of the message is what comes next.
                if (bot->GetItemCount(itemId, true) >= need
                    && g_gatherAnnounced.insert({ bot->GetGUID().GetCounter(), entry.first }).second)
                {
                    SayToOwner(bot, owner, "I picked up the last of '" + quest->GetTitle() +
                               "' off the ground - my copy is done, so I can hand it in when we are next at the giver.");
                }

                if (g_cfg.debug >= 2)
                    LOG_INFO("server.loading", "[CompanionQuestSync] gathering: {} took {} x{} from '{}' ({} of {} now)",
                             bot->GetName(), collected, itemId, gatheredFrom,
                             uint32(bot->GetItemCount(itemId, true)), need);

                return true;
            }
        }

        return false;
    }

    /// Hand the companion the ground items it still owes for one quest.
    ///
    /// Only items that actually lie on the ground are granted, so a kill-collect quest can
    /// never be finished from here. This is the fallback for "the object was gone, another
    /// player took it, or the step never reached it" - not a way to skip the game.
    uint32 GrantMissingGroundItems(Player* bot, Quest const* quest)
    {
        uint32 granted = 0;

        for (uint8 i = 0; i < quest->GetReqItemsCount(); ++i)
        {
            uint32 const itemId = quest->RequiredItemId[i];
            uint32 const need = quest->RequiredItemCount[i];

            if (!itemId || !need || g_itemGroundObjects.find(itemId) == g_itemGroundObjects.end())
                continue;

            uint32 const have = bot->GetItemCount(itemId, true);
            if (have >= need)
                continue;

            uint32 const missing = need - have;

            if (!bot->AddItem(itemId, missing))
            {
                LOG_INFO("server.loading", "[CompanionQuestSync] catch-up for {}: no bag room for {} item(s) of {}",
                         bot->GetName(), missing, itemId);
                break;
            }

            granted += missing;
        }

        return granted;
    }

    /// Finish a ground-object quest the world will not yield, once it has clearly stalled.
    ///
    /// The gather step keeps trying every pass; this is what happens when it cannot win -
    /// a chest another player looted, a spawn the bot cannot reach, a node that never
    /// respawns. The condition is deliberately narrow: every objective must be a ground
    /// item, the count must not have moved for CatchUpIdleSeconds, and **the owner must
    /// already have been rewarded that quest** - the companion is catching up, not doing
    /// the player's work ahead of them.
    void AutoCatchUpGround(Player* owner, Player* bot)
    {
        if (!owner || !bot || !bot->IsAlive() || g_cfg.catchUpIdleSeconds == 0)
            return;

        uint32 const botGuid = bot->GetGUID().GetCounter();
        uint32 const now = uint32(time(nullptr));

        for (auto const& entry : bot->getQuestStatusMap())
        {
            Quest const* quest = sObjectMgr->GetQuestTemplate(entry.first);
            if (!quest || entry.second.Status != QUEST_STATUS_INCOMPLETE)
                continue;

            if (!owner->IsQuestRewarded(entry.first))
                continue;

            bool groundOnly = true;
            bool owed = false;
            uint32 count = 0;

            for (uint8 i = 0; i < quest->GetReqItemsCount(); ++i)
            {
                uint32 const itemId = quest->RequiredItemId[i];
                if (!itemId)
                    continue;

                if (g_itemGroundObjects.find(itemId) == g_itemGroundObjects.end())
                {
                    groundOnly = false;
                    break;
                }

                count += entry.second.ItemCount[i];

                if (entry.second.ItemCount[i] < quest->RequiredItemCount[i])
                    owed = true;
            }

            if (!groundOnly || !owed)
                continue;

            auto& stall = g_groundStall[botGuid][entry.first];

            if (stall.second == 0 || stall.first != count)
            {
                stall.first = count;
                stall.second = now;
                continue;
            }

            uint32 const stalledFor = now - stall.second;
            if (stalledFor < g_cfg.catchUpIdleSeconds)
                continue;

            uint32 const granted = GrantMissingGroundItems(bot, quest);
            if (!granted)
                continue;

            stall.second = now;

            g_session[owner->GetGUID().GetCounter()].gathered += granted;

            LOG_INFO("server.loading", "[CompanionQuestSync] catch-up: {} took {} ground item(s) for '{}' after {}s with no progress",
                     bot->GetName(), granted, quest->GetTitle(), stalledFor);

            SayToOwner(bot, owner, "I went back for the rest of '" + quest->GetTitle() +
                       "' myself - the field had been picked over. Ready when you are.");
        }
    }

    /// Put the companion next to a point in the world (a quest giver), not next to the player.
    ///
    /// The step-beside-the-owner shape is right when the player is standing at the NPC: the
    /// companion catches up and happens to be in range. It is wrong when they are not, which
    /// is exactly when a hand-in is needed - the bot landed two yards from its owner and
    /// thirty from the giver, playerbots refused the reward, and the player was told the
    /// companion "cannot talk to the quest giver". True, and entirely the module's fault.
    void StepBesidePoint(Player* bot, float x, float y, float z, float radius = 2.5f)
    {
        float ax = bot->GetPositionX() - x;
        float ay = bot->GetPositionY() - y;
        float len = std::sqrt(ax * ax + ay * ay);

        if (len < 0.1f)
        {
            ax = 1.0f;
            ay = 0.0f;
            len = 1.0f;
        }

        float const tx = x + radius * ax / len;
        float const ty = y + radius * ay / len;
        float tz = z;

        bot->UpdateGroundPositionZ(tx, ty, tz);
        bot->StopMoving();
        bot->NearTeleportTo(tx, ty, tz, bot->GetOrientation());
    }

    void WalkToGiver(Player* owner, Player* bot, Creature* npc);  // defined below

    /// Mark the turn-in NPCs where the owner is already done and the companion is not.
    ///
    /// The quest-area POI packet has no colour field - the client picks the shading,
    /// and it only draws areas for quests in its own log. The markers it does take
    /// from the server are the quest-giver icons, so those get a turn-in status: a
    /// quest the player has finished but the companion still owes shows up with the
    /// turn-in icon on that NPC, which is as close to a differently coloured pin as
    /// this client allows. Only for NPCs the player can already see (80 yd).
    /// Collect the NPCs this companion still needs, without touching their icons yet.
    ///
    /// Marking and clearing in one call is wrong with several companions: the second
    /// companion's pass would clear the first one's markers, so a giver only one of
    /// them needs flickered with every sweep. Collect into the owner's union here and
    /// apply it once with ApplyMarks().
    void MarkOwnErrands(Player* owner, Player* bot, std::set<uint64>& wanted)
    {
        if (!g_cfg.mapPins || !owner->PlayerTalkClass)
            return;

        std::string marked;

        for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 const questId = bot->GetQuestSlotQuestId(slot);
            if (!questId)
                continue;

            QuestStatus const botStatus = bot->GetQuestStatus(questId);
            if (botStatus != QUEST_STATUS_INCOMPLETE && botStatus != QUEST_STATUS_COMPLETE)
                continue;

            // The owner must already be clear of it, otherwise the marker is just
            // their own turn-in and says nothing.
            if (!owner->IsQuestRewarded(questId) && owner->GetQuestStatus(questId) != QUEST_STATUS_COMPLETE)
                continue;

            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest)
                continue;

            // Still gathering? Then it is not a turn-in yet, and marking it would
            // read as "go hand this in" when nothing can be handed in.
            if (!DescribeMissingItems(bot, quest).empty())
                continue;

            EnderSpot const& spot = FindQuestEnder(bot, questId);
            if (!spot.found || !spot.entry)
                continue;

            Creature* npc = owner->FindNearestCreature(spot.entry, 80.0f);
            if (!npc)
                continue;

            WalkToGiver(owner, bot, npc);

            wanted.insert(npc->GetGUID().GetRawValue());
            marked += std::to_string(questId) + ",";
        }
    }

    /// What this NPC's icon should be for the *owner's own* quest log.
    ///
    /// The marker packet is the same one the core uses for the player's own status, so
    /// writing a marker also overwrites their icon. Both directions have to respect that:
    /// marking a companion errand on an NPC the player is already using would hide their
    /// own turn-in, and clearing a marker with "nothing here" wipes a question mark the
    /// player had earned - which is exactly why a finished quest showed no `?` over the
    /// giver until the player spoke to it again and the core re-sent the status.
    uint8 OwnerStatusFor(Player* owner, ObjectGuid guid)
    {
        Creature* npc = ObjectAccessor::GetCreatureOrPetOrVehicle(*owner, guid);
        return npc ? uint8(owner->GetQuestDialogStatus(npc)) : uint8(DIALOG_STATUS_NONE);
    }

    /// Apply the owner's union of markers, then restore the icons no companion needs.
    void ApplyMarks(Player* owner, std::set<uint64> const& wanted)
    {
        if (!g_cfg.mapPins || !owner->PlayerTalkClass)
            return;

        uint32 const ownerGuid = owner->GetGUID().GetCounter();

        for (uint64 const guid : wanted)
        {
            uint8 const own = OwnerStatusFor(owner, ObjectGuid(guid));

            // nothing of the player's own there: borrow the icon for the companion's errand
            if (own == uint8(DIALOG_STATUS_NONE))
                owner->PlayerTalkClass->SendQuestGiverStatus(uint8(DIALOG_STATUS_REWARD), ObjectGuid(guid));
        }

        std::set<uint64>& previously = g_markedNpcs[ownerGuid];
        for (uint64 const stale : previously)
        {
            if (wanted.count(stale))
                continue;

            // Put back what the player's own log says, never a flat "nothing here".
            ObjectGuid const guid(stale);
            if (!ObjectAccessor::GetCreatureOrPetOrVehicle(*owner, guid))
                continue;                       // out of sight: the client asks again on approach

            owner->PlayerTalkClass->SendQuestGiverStatus(OwnerStatusFor(owner, guid), guid);
        }

        previously = wanted;

        if (g_cfg.debug >= 2 && !wanted.empty())
            LOG_INFO("server.loading", "[CompanionQuestSync] {} turn-in marker(s) on {} map",
                     uint32(wanted.size()), owner->GetName());
    }

    /// Put the companion's open quest areas on the owner's map.
    ///
    /// The client only asks for quest POIs for quests in its *own* log, so a quest
    /// the companion is behind on never draws. This pushes the same response the
    /// query handler would build (QueryHandler.cpp:427) for the companion's open
    /// quests, one packet, only when the set changes. Experimental by nature: if the
    /// client ignores POIs for quests it does not hold, the packet is simply dropped
    /// and the whisper carries map coordinates instead.
    /// Quest area circles for the owner's map, across every companion at once.
    ///
    /// This used to be called once per companion with one bot's quest log, while the
    /// "already sent" key was stored per owner. With two companions that key alternated
    /// between their two sets, so the packet was re-sent on nearly every pass and the
    /// client - which replaces its quest areas on receipt - only ever showed whichever
    /// companion had passed last. The union is stable, sent once, and shows the whole
    /// group's work.
    void SendQuestPins(Player* owner, std::vector<uint32> const& bots)
    {
        // Retired: the client renders POI circles only for quests in the player's own
        // quest log, so the pushed packet's data for companion-held quests was never
        // drawn - a session with it in place showed no markers at all. The glyph
        // payload is the only thing that reaches the map for a companion's errands.
        (void)owner; (void)bots;
    }

    /// Position is the other half of a desync: the two logs can match while the
    /// companion is a zone away, and nothing used to say so. Reports three cases -
    /// separated, too far from the giver, or actually standing on it - in the same
    /// voice as everything else. Throttled per companion, because this state
    /// persists until somebody moves.
    void ReportPosition(Player* owner, Player* bot)
    {
        if (g_cfg.notifySkipped < 2)
            return;

        uint32 const botGuid = bot->GetGUID().GetCounter();
        uint32 const now = static_cast<uint32>(time(nullptr));
        uint32& last = g_positionNoticeAt[botGuid];
        if (last && now - last < Timing::PositionNotice)
            return;

        if (bot->GetZoneId() && owner->GetZoneId() && bot->GetZoneId() != owner->GetZoneId())
        {
            last = now;
            SayToOwner(bot, owner, "I think I have lost you - I am in " + ZoneName(bot->GetZoneId()) +
                                  " and you are in " + ZoneName(owner->GetZoneId()) +
                                  ". Summon me, or I will follow when you are back, and we can carry on together.");
            return;
        }

        for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 const questId = bot->GetQuestSlotQuestId(slot);
            if (!questId || bot->GetQuestStatus(questId) != QUEST_STATUS_COMPLETE)
                continue;

            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest)
                continue;

            // COMPLETE is not the same as hand-in-able: a delivery quest whose items
            // are still owed cannot be rewarded, so pointing at its giver is wrong -
            // and it reads as "go here now" while the companion is still gathering.
            // Only report a turn-in for a copy it could actually reward.
            if (!DescribeMissingItems(bot, quest).empty())
                continue;

            EnderSpot const& spot = FindQuestEnder(bot, questId);
            if (!spot.found)
                continue;

            if (spot.map != bot->GetMapId())
            {
                last = now;
                SayToOwner(bot, owner, "'" + quest->GetTitle() + "' gets handed in at " + spot.name +
                                      ", and that is on another map from here - we would have to travel there together.");
                return;
            }

            // A standing fact, not news: one hint per quest per ten minutes. Repeating
            // it every sweep is what made it feel like nagging.
            std::pair<uint32, uint32> const hintKey{ bot->GetGUID().GetCounter(), questId };
            uint32& hintAt = g_turnInHintAt[hintKey];
            uint32 const nowTs = static_cast<uint32>(time(nullptr));
            if (hintAt && nowTs - hintAt < 600)
                continue;

            float const dist = bot->GetDistance(spot.x, spot.y, spot.z);

            // Interaction range is 5 yards, not 25: claiming "I am at the giver" from
            // across the square is what made the module say it was handing a quest in
            // while nothing on the bot side had even opened the gossip window.
            if (dist <= 6.0f)
            {
                last = now;
                hintAt = nowTs;

                // Actually ask the bot's AI to hand it in ("r" is playerbots' reward
                // trigger). Previously the module announced a hand-in that nobody was
                // performing - the quest sat COMPLETE with all eight candles in its bags.
                // The narration no longer pulls the bot's own reward trigger: with
                // HandInMode = 1 the reward happens in DeliverHandIns, and every "r" the
                // module fires is a chance for playerbots' own giver search to fail and
                // tell the player "Cannot talk to quest giver".
                if (g_cfg.handInMode == 0)
                {
                    uint32& prodAt = g_rewardProdAt[{ bot->GetGUID().GetCounter(), questId }];
                    if (!prodAt || nowTs - prodAt >= 60)
                    {
                        prodAt = nowTs;
#ifdef MOD_PLAYERBOTS
                        if (PlayerbotAI* ai = GET_PLAYERBOT_AI(bot))
                            ai->HandleCommand(CHAT_MSG_WHISPER, "r", owner);
#endif
                    }
                }

                std::string const here = MapCoords(spot.x, spot.y, bot->GetZoneId());
                SayToOwner(bot, owner, "I am at " + spot.name + " with '" + quest->GetTitle() +
                                      "' ready - handing it in now" +
                                      (here.empty() ? "." : " (" + here + ")."));
                return;
            }

            if (dist > 60.0f)
            {
                last = now;
                hintAt = nowTs;
                std::string const coords = MapCoords(spot.x, spot.y, bot->GetZoneId());

                // If the owner has already been rewarded for this quest, their part is
                // done - asking them along reads as "you still owe this", which they do
                // not. It is the companion's own errand; say it that way.
                if (owner->IsQuestRewarded(questId))
                    Note(owner, 2, "I still owe " + spot.name + " my own copy of '" +
                                   quest->GetTitle() +
                                   "' - I will hand it in next time we are near" +
                                   (coords.empty() ? "." : " (" + coords + ")."),
                                   questId, true);
                else
                    Note(owner, 3, "When we get a chance, could we stop by " + spot.name +
                                   "? I can hand in '" + quest->GetTitle() + "' there - about " +
                                   std::to_string(uint32(dist)) + " yards from me" +
                                   (coords.empty() ? "." : ", " + coords + "."),
                                   questId, true);
                return;
            }
        }
    }

    /// "Large Candle 0/8" - what the companion still owes for a quest, spelled
    /// out. A bare "missing items" whisper told the player nothing they could act
    /// on: not which item, not how many are already in the bags.
    std::string DescribeMissingItemsUncached(Player* bot, Quest const* quest)
    {
        std::string out;

        for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
        {
            uint32 const required = quest->RequiredItemCount[i];
            if (!required)
                continue;

            uint32 const itemId = quest->RequiredItemId[i];
            uint32 const have = bot->GetItemCount(itemId);
            if (have >= required)
                continue;

            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
            std::string const name = proto ? proto->Name1 : ("item " + std::to_string(itemId));

            if (!out.empty())
                out += ", ";

            out += name + " " + std::to_string(have) + "/" + std::to_string(required);
        }

        return out;
    }

    /// A copy can be COMPLETE and still un-hand-in-able: a quest with a
    /// RequiredItemId only rewards when those items are in the companion's own
    /// bags (CanRewardQuest re-checks the counts), and mod-playerbots completes
    /// the copy at the giver regardless of what it carries. Left alone the copy
    /// is dead: it can never be handed in, nothing gives its AI a reason to
    /// collect, and the follow-up quest is then refused for a missing
    /// prerequisite. Seen live with 'Kobold Candles' (60): COMPLETE, 0 of 8 item
    /// 772, and 'Shipment to Stormwind' (61) blocked behind it.
    ///
    /// Reopening is the only honest fix: the companion then has an incomplete
    /// quest whose items it wants (IsItemUsefulForQuest), so it collects them and
    /// completes on its own. Copies that already hold what they owe are untouched.
    void RepairStrandedDelivery(Player* owner, Player* bot, SyncReport& report)
    {
        for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 const questId = bot->GetQuestSlotQuestId(slot);
            if (!questId || bot->GetQuestStatus(questId) != QUEST_STATUS_COMPLETE)
                continue;

            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest || !quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_DELIVER))
                continue;

            bool missing = false;
            for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT && !missing; ++i)
            {
                uint32 const required = quest->RequiredItemCount[i];
                if (required && bot->GetItemCount(quest->RequiredItemId[i]) < required)
                    missing = true;
            }

            if (!missing)
                continue;

            bot->SetQuestStatus(questId, QUEST_STATUS_INCOMPLETE);
            bot->SetQuestSlotState(slot, QUEST_STATE_NONE);
            ++report.reopened;

            LOG_INFO("server.loading", "[CompanionQuestSync] {} reopened '{}' ({}) - it owes the delivery items",
                     bot->GetName(), quest->GetTitle(), questId);

            g_repairing[bot->GetGUID().GetCounter()].insert(questId);

            if (g_cfg.notifySkipped >= 2)
                Note(owner, 2, "Could we help each other finish '" + quest->GetTitle() +
                               "' before moving on? I still owe " + DescribeMissingItems(bot, quest) +
                               " - I will gather them as we go, then I can hand it in.");
        }

        // Second half of the same story: once it has collected what it owed, say
        // so - otherwise the player only sees the original complaint and no finish.
        std::set<uint32>& tracking = g_repairing[bot->GetGUID().GetCounter()];
        for (auto itr = tracking.begin(); itr != tracking.end();)
        {
            Quest const* quest = sObjectMgr->GetQuestTemplate(*itr);
            if (!quest || bot->GetQuestStatus(*itr) == QUEST_STATUS_NONE)
            {
                itr = tracking.erase(itr);
                continue;
            }

            if (bot->GetQuestStatus(*itr) == QUEST_STATUS_COMPLETE && DescribeMissingItems(bot, quest).empty())
            {
                if (g_cfg.notifySkipped >= 2)
                    SayToOwner(bot, owner, "I have everything for '" + quest->GetTitle() +
                                          "' now - handing it in. Thanks for the help!");

                LOG_INFO("server.loading", "[CompanionQuestSync] {} collected what it owed for '{}' ({})",
                         bot->GetName(), quest->GetTitle(), *itr);

                itr = tracking.erase(itr);
                continue;
            }

            ++itr;
        }
    }
    /// One whisper for everything refused since the last flush - at most three
    /// named, then a count - ending with what the player can actually do about it.
    /// Send whatever was held back by the rate limit, one per pass.
    void FlushWhisperQueue(Player* owner, Player* /*fallback*/)
    {
        uint32 const ownerGuid = owner->GetGUID().GetCounter();
        auto itr = g_whisperQueue.find(ownerGuid);
        if (itr == g_whisperQueue.end() || itr->second.empty())
            return;

        uint32 const now = static_cast<uint32>(time(nullptr));
        uint32& last = g_lastWhisperAt[ownerGuid];
        auto burst = g_commandBurst.find(ownerGuid);
        bool const inBurst = burst != g_commandBurst.end() && now - burst->second < 20;
        if (last && now - last < (inBurst ? 2 : Timing::WhisperGap))
            return;

        // Drop what no longer applies before speaking. The queue exists to survive
        // the rate limit, but the world moves on inside that window: a "I need
        // level 5 first" held for 45 s usually lands *after* the companion has the
        // quest, which reads like a broken module rather than a busy one.
        //
        // Every check is against the companion that said it, never the one whose pass
        // happens to be flushing: the old code asked the wrong bot about the wrong quest
        // log and dropped lines that were still true.
        auto& queue = itr->second;
        for (auto entry = queue.begin(); entry != queue.end(); )
        {
            Player* speaker = entry->botGuid ? FindCompanionPlayer(entry->botGuid) : nullptr;

            bool const vital = entry->vital || entry->text.rfind("Finished '", 0) == 0;

            bool stale = !speaker || !speaker->IsInWorld();
            if (!stale && !vital)
            {
                // A held-back line about work that finished inside the window reads
                // like a broken module. But the pin-clear notice IS the work finishing -
                // it must survive its own completion.
                if (entry->questId && entry->handIn)
                {
                    // An invitation to hand in is only true while the quest is still
                    // complete: the moment it is rewarded (or gone), a held-back
                    // "close to the giver" line would arrive describing a past event.
                    // Truth wins over age here - no expiry window can save a lie.
                    stale = speaker->GetQuestStatus(entry->questId) != QUEST_STATUS_COMPLETE;
                }
                else if (entry->questId)
                    stale = speaker->GetQuestStatus(entry->questId) != QUEST_STATUS_NONE ||
                            speaker->IsQuestRewarded(entry->questId);
                else if (entry->queuedAt && now > entry->queuedAt)
                    stale = now - entry->queuedAt > 120;
            }

            if (!stale)
            {
                ++entry;
                continue;
            }

            if (g_cfg.debug >= 2)
                LOG_INFO("server.loading", "[CompanionQuestSync] dropped a stale whisper: {}", entry->text);

            entry = queue.erase(entry);
        }

        if (queue.empty())
            return;

        // Whose turn? The owner gets one whisper per window, so a companion that has
        // never been heard - or not for a while - goes first. Without this the same
        // companion wins every window simply because its pass runs first, and with four
        // companions the last one is never heard at all.
        auto chosen = queue.begin();

        bool const hasVital = [&queue]()
        {
            for (auto const& entry : queue)
                if (entry.vital)
                    return true;
            return false;
        }();

        if (hasVital)
        {
            // The pin-clear jumps the fairness queue: it is the message that keeps the
            // map honest, and a full burst of chatter must not delay it a window.
            for (auto entry = queue.begin(); entry != queue.end(); ++entry)
            {
                if (entry->vital)
                {
                    chosen = entry;
                    break;
                }
            }
        }
        else
        {
            for (auto entry = queue.begin(); entry != queue.end(); ++entry)
            {
                uint32 const entryHeard = g_lastWhisperByBot[entry->botGuid];
                uint32 const chosenHeard = g_lastWhisperByBot[chosen->botGuid];

                if (entryHeard < chosenHeard ||
                    (entryHeard == chosenHeard && entry->queuedAt < chosen->queuedAt))
                    chosen = entry;
            }
        }

        Player* speaker = FindCompanionPlayer(chosen->botGuid);
        if (!speaker)
        {
            queue.erase(chosen);
            return;
        }

        std::string const spoken = MultiCompanionSpeech(speaker) ? speaker->GetName() + ": " + chosen->text
                                                    : chosen->text;

        last = now;
        g_lastWhisperByBot[chosen->botGuid] = now;
        speaker->Whisper(spoken, LANG_UNIVERSAL, owner);
        queue.erase(chosen);

        // In a burst the player is mid-conversation: keep speaking while the short
        // gap allows, so a press's handful of verdicts all arrive together instead
        // of trickling across the next five windows.
        if (inBurst && !queue.empty())
            FlushWhisperQueue(owner, nullptr);

        if (g_cfg.debug >= 2)
            LOG_INFO("server.loading", "[CompanionQuestSync] {} whispered (queued): {}", speaker->GetName(), spoken);
    }

    void FlushNotices(Player* owner, Player* bot)
    {
        uint32 const botGuid = bot->GetGUID().GetCounter();
        auto itr = g_pendingNotices.find(botGuid);
        if (itr == g_pendingNotices.end() || itr->second.empty())
            return;

        std::vector<PendingNotice> const queued = itr->second;
        g_pendingNotices.erase(itr);

        // Re-check each refusal at flush time: a batch built during a level-up is
        // often stale by the time the rate limit allows it out.
        std::vector<std::string> lines;
        for (auto const& notice : queued)
        {
            if (notice.questId && (bot->GetQuestStatus(notice.questId) != QUEST_STATUS_NONE ||
                                   bot->IsQuestRewarded(notice.questId)))
            {
                if (g_cfg.debug >= 2)
                    LOG_INFO("server.loading", "[CompanionQuestSync] dropped a stale notice: {} ({})",
                             notice.text, notice.questId);
                continue;
            }

            lines.push_back(notice.text);
        }

        if (lines.empty())
            return;

        std::string text = "A few quests I cannot take yet: ";
        for (size_t i = 0; i < lines.size() && i < 3; ++i)
        {
            if (i)
                text += "; ";
            text += lines[i];
        }

        if (lines.size() > 3)
            text += "; and " + std::to_string(lines.size() - 3) + " more";

        text += ". Could we sort those out when you get a moment?";
        SayToOwner(bot, owner, text);
    }

    /// Keep asking for help while the companion still owes quest items.
    ///
    /// One request was not enough: it fired once, lost a race with the whisper
    /// rate limit, and the player never heard anything again while the companion sat
    /// on an unfinished quest. Now the request repeats every five minutes until the
    /// items are gathered, and the queue above guarantees it is not dropped.
    void NudgeForHelp(Player* owner, Player* bot)
    {
        if (g_cfg.notifySkipped < 2)
            return;

        uint32 const botGuid = bot->GetGUID().GetCounter();
        uint32 const now = static_cast<uint32>(time(nullptr));

        for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 const questId = bot->GetQuestSlotQuestId(slot);
            if (!questId || bot->GetQuestStatus(questId) != QUEST_STATUS_INCOMPLETE)
                continue;

            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest)
                continue;

            std::string const owed = DescribeMissingItems(bot, quest);
            std::string const where = PoiCoords(bot, quest);

            // Nothing owed, but the quest is still unfinished: point at the objective
            // area instead of staying silent. Kill-and-collect quests have no item debt
            // to report, and those are exactly the ones that leave a companion wandering.
            if (owed.empty())
            {
                if (where.empty())
                    continue;

                std::pair<uint32, uint32> const poiKey{ botGuid, questId };
                uint32& poiAt = g_nudgeAt[poiKey];
                if (poiAt && now - poiAt < Timing::POIRetry)
                    continue;

                poiAt = now;
                SayToOwner(bot, owner, "I still have '" + quest->GetTitle() +
                                      "' to finish for us - the circle is on your map. Want to head that way?",
                                      45, questId);
                return;
            }

            std::pair<uint32, uint32> const key{ botGuid, questId };
            uint32& last = g_nudgeAt[key];
            if (last && now - last < Timing::NudgeRepeat)
                continue;

            last = now;
            Note(owner, 2, "Could we help each other finish '" + quest->GetTitle() +
                           "' before we move on? I still owe " + owed +
                           " - I will gather them as we go" +
                           (where.empty() ? "." : " (" + where + ")."));
            g_nudgeAt[key] = now;
            return;                                  // one request per pass
        }
    }

    /// Raise / set the companion's level according to LevelMode.
    /// quest_template.MinLevel is the hard gate for every quest on the owner's
    /// log, so this runs before any quest is considered.
    void AlignLevel(Player* owner, Player* bot, SyncReport& report)
    {
        if (!g_cfg.levelMode)
            return;

        uint8 const ownerLevel = owner->GetLevel();
        uint8 const botLevel = bot->GetLevel();

        // Raise only, in both modes. GiveLevel downwards strips spell ranks and can
        // leave talents pointing at things the character no longer has, so a companion
        // that out-levelled its owner keeps the level it earned and says so once at
        // debug level instead - "exact" means "brought up to", not "demoted to".
        if (botLevel > ownerLevel)
        {
            if (g_cfg.debug >= 1 && g_cfg.levelMode == 2)
                LOG_INFO("server.loading", "[CompanionQuestSync] {} is level {} and stays there (owner is {}) - not demoting",
                         bot->GetName(), uint32(botLevel), uint32(ownerLevel));
            return;
        }

        if (botLevel == ownerLevel)
            return;

        report.levelFrom = botLevel;
        report.levelTo = ownerLevel;
        bot->GiveLevel(ownerLevel);

#ifdef MOD_PLAYERBOTS
        // A hand-granted level teaches nothing on its own: GiveLevel raises the level
        // and grants talent points nobody spends, because playerbots' auto-learn and
        // auto-pick-talents are gated to random bots and this realm runs none. The
        // teaching itself lives in OnPlayerLevelChanged, which GiveLevel fires -
        // this raise and an organic level-up arrive at the same place, and the
        // Train button still backstops companions raised before the hook existed.
#endif

        if (g_cfg.debug >= 1)
            LOG_INFO("server.loading", "[CompanionQuestSync] {} -> level {} (owner {} level {})",
                     bot->GetName(), uint32(bot->GetLevel()), owner->GetName(), uint32(ownerLevel));
    }

#ifdef MOD_PLAYERBOTS
    /// Teach a bot what a trainer would at its current level, and spend its unspent
    /// talent points along the spec it already has (increment: never reset, never
    /// re-roll). Shared by the level-up path - a raise must bring its own spells - and
    /// the Train button, which parses each companion for anything still missing.
    /// Returns {new spells, talent points spent} for an honest report.
    std::pair<uint32, uint32> TeachTrainerContent(Player* bot, Player* owner)
    {
        uint32 const spellsBefore = bot->GetSpellMap().size();
        uint32 const talentsBefore = bot->GetFreeTalentPoints();

        PlayerbotFactory factory(bot, bot->GetLevel());
        factory.InitSkills();
        factory.InitClassSpells();
        factory.InitAvailableSpells();
        factory.InitTalentsTree(/*increment=*/true);

        uint32 const learned = bot->GetSpellMap().size() > spellsBefore
            ? uint32(bot->GetSpellMap().size() - spellsBefore) : 0;
        uint32 const spent = talentsBefore > bot->GetFreeTalentPoints()
            ? talentsBefore - bot->GetFreeTalentPoints() : 0;

        if (learned || spent)
        {
            SayToOwner(bot, owner,
                       (learned && spent
                           ? "I caught up: learned " + std::to_string(learned) + " spell(s) and spent "
                             + std::to_string(spent) + " talent point(s)."
                           : learned
                               ? "I caught up: learned " + std::to_string(learned) + " spell(s)."
                               : "I caught up: spent " + std::to_string(spent) + " talent point(s)."),
                       30);
        }
        else
        {
            SayToOwner(bot, owner, "I already know everything a trainer could teach me.", 30);
        }

        return { learned, spent };
    }

    /// The class-appropriate profession pair, chosen once per body and kept: one of the
    /// two always gathers, because the point of a companion profession is what it picks
    /// up while walking with you - ore for the plate classes, hides for the leather
    /// ones, herbs for everyone else. The gather strategy does the picking up itself:
    /// on a timer it adds only nodes the bot's own skill can work.
    static std::pair<uint16, uint16> ClassProfessionPair(uint8 cls)
    {
        switch (cls)
        {
            case CLASS_WARRIOR:
            case CLASS_PALADIN:
            case CLASS_DEATH_KNIGHT:
                return { SKILL_MINING, SKILL_BLACKSMITHING };
            case CLASS_HUNTER:
            case CLASS_ROGUE:
            case CLASS_SHAMAN:
                return { SKILL_SKINNING, SKILL_LEATHERWORKING };
            default:
                return { SKILL_HERBALISM, SKILL_ALCHEMY };
        }
    }

    struct ProfessionEntry { uint32 learnSpell; char const* name; uint32 tool; };
    static ProfessionEntry ProfessionOf(uint16 skill)
    {
        switch (skill)
        {
            case SKILL_MINING:         return { 2580, "Mining",         2901 };  // + a Mining Pick
            case SKILL_SKINNING:       return { 8613, "Skinning",       7005 };  // + a Skinning Knife
            case SKILL_BLACKSMITHING:  return { 2018, "Blacksmithing",  0 };
            case SKILL_LEATHERWORKING: return { 2108, "Leatherworking", 0 };
            case SKILL_ALCHEMY:        return { 2259, "Alchemy",        0 };
            default:                   return { 2366, "Herbalism",      0 };
        }
    }

    /// The skill ceiling a trainer would sell at this level - the classic tiers.
    static uint32 ProfessionCap(uint8 level)
    {
        if (level >= 35) return 300;
        if (level >= 20) return 225;
        if (level >= 10) return 150;
        return 75;
    }

    /// The craft recipes a trainer would sell at this level's ceiling: every spell on
    /// the profession's line whose first effect is CREATE_ITEM and whose required
    /// rank sits at or under the cap is learned. Produce resolves its recipes from
    /// the spellbook by what they create - this feeds that spellbook, so the pair's
    /// production half can actually craft what the gathering half pays for. Rides
    /// every profession teach and every level raise, so the recipe set grows with
    /// the ceiling instead of trailing behind it.
    static void TeachCraftRecipes(Player* bot, uint16 skill, uint32 cap)
    {
        uint32 candidates = 0, learned = 0;
        auto const& abilities = GetSkillLineAbilitiesBySkillLine(skill);
        for (SkillLineAbilityEntry const* entry : abilities)
        {
            if (entry->MinSkillLineRank > cap)
                continue;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(entry->Spell);
            if (!info || info->Effects[0].Effect != SPELL_EFFECT_CREATE_ITEM)
                continue;
            ++candidates;
            if (!bot->HasSpell(entry->Spell))
            {
                bot->learnSpell(entry->Spell);
                ++learned;
            }
        }
        if (g_cfg.debug >= 1)
            LOG_INFO("server.loading", "[CompanionQuestSync] recipes: line={} abilities={} candidates={} learned={}",
                     uint32(skill), abilities.size(), candidates, learned);
    }

    /// Teach both professions (or re-cap what a level raise un-capped), hand the tools
    /// a gathering profession needs, and make sure the gather strategy is on so the
    /// companion actively works its own nodes while it follows you. Returns the names
    /// it just took up, for the report.
    std::string TeachProfessions(Player* bot, Player* owner)
    {
        auto const pair = ClassProfessionPair(bot->getClass());

        // The choice is kept for the body: a re-roll would throw away gathered skill.
        uint32 const stored = sRandomPlayerbotMgr.GetValue(bot, "cqProfPair");
        uint32 const want = uint32(pair.first) * 1000 + uint32(pair.second);
        if (!stored)
            sRandomPlayerbotMgr.SetValue(bot, "cqProfPair", want);

        uint32 const cap = ProfessionCap(bot->GetLevel());
        std::string tookUp;

        uint16 const skills[2] = { pair.first, pair.second };
        for (uint16 const skill : skills)
        {
            ProfessionEntry const entry = ProfessionOf(skill);
            if (!bot->HasSpell(entry.learnSpell))
            {
                bot->learnSpell(entry.learnSpell);
                if (!tookUp.empty())
                    tookUp += " and ";
                tookUp += entry.name;
            }
            if (entry.tool && !bot->GetItemCount(entry.tool))
            {
                ItemPosCountVec dest;
                if (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, entry.tool, 1) == EQUIP_ERR_OK)
                    if (Item* tool = bot->StoreNewItem(dest, entry.tool, true))
                        bot->SendNewItem(tool, 1, true, false, true);
            }
            // raise the ceiling only; gathered progress is never reset
            uint16 const cur = bot->GetSkillValue(skill);
            bot->SetSkill(skill, 0, std::min<uint16>(cur, uint16(cap)), uint16(cap));

            // and the recipes the ceiling unlocks - the craft is taught with the craft
            TeachCraftRecipes(bot, skill, cap);
        }

        if (PlayerbotAI* ai = GET_PLAYERBOT_AI(bot))
            ai->ChangeStrategy("+gather", BOT_STATE_NON_COMBAT);

        // A body taught before the pairs existed can carry a stale gathered skill outside
        // its own pair - a shaman-smith picking herbs. With a negligible value it is
        // unlearned rather than left as a third profession the body should not have.
        for (uint16 const other : { uint16(SKILL_HERBALISM), uint16(SKILL_MINING), uint16(SKILL_SKINNING) })
        {
            if (other == pair.first || other == pair.second)
                continue;
            if (bot->GetSkillValue(other) > 5)
                continue;

            bot->removeSpell(ProfessionOf(other).learnSpell, 0, false);
            bot->SetSkill(other, 0, 0, 0);
        }

        if (!tookUp.empty())
            SayToOwner(bot, owner, "I took up " + tookUp +
                       " - I will gather what I pass from now on.", 30);

        return tookUp;
    }

    /// The gathered mats a pair supplies - node drops and the hides that come straight
    /// off corpses. A missing produce reagent inside this set means "go gather";
    /// anything else is the shop's part, and the module stands in for the shopping trip.
    static std::vector<uint32> GatheredReagentsOf(uint16 skill)
    {
        switch (skill)
        {
            case SKILL_HERBALISM:   return { 765, 2447, 2449, 2450, 2452, 2453, 785 };
            case SKILL_MINING:      return { 2770, 2771, 2835, 2836 };
            case SKILL_SKINNING:    return { 2318, 2319, 2934, 783 };
            default:                return {};
        }
    }

    static bool IsGatherableReagent(uint16 skill, uint32 item)
    {
        for (uint32 const gathered : GatheredReagentsOf(skill))
            if (gathered == item)
                return true;
        return false;
    }

    /// What each pair produces at this band, in order: the smelt first for miners -
    /// bars from their own ore - then the pieces the craft feeds.
    static std::vector<uint32> ProductsOf(uint16 skill)
    {
        switch (skill)
        {
            case SKILL_BLACKSMITHING:  return { 2840, 2862 };  // Copper Bar, Rough Sharpening Stone
            case SKILL_LEATHERWORKING: return { 2304 };         // Light Armor Kit
            case SKILL_ALCHEMY:        return { 118 };          // Minor Healing Potion
            default:                   return {};
        }
    }

    /// Item name for the report, or "?" when the template is gone.
    static std::string ItemNameFor(uint32 item)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
        return proto ? std::string(proto->Name1) : std::string("?");
    }

    /// A vendor the companion could be standing at: any live creature with the vendor
    /// or repair flag within range, found the way the core searches its own cells.
    static Creature* VendorNearby(Player* bot, float range)
    {
        std::list<Unit*> units;
        Acore::AnyUnitInObjectRangeCheck check(bot, range);
        Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(bot, units, check);
        Cell::VisitObjects(bot, searcher, range);

        for (Unit* u : units)
            if (Creature* c = u->ToCreature())
                if (c->IsAlive() && (c->HasNpcFlag(UNIT_NPC_FLAG_VENDOR) ||
                                     c->HasNpcFlag(UNIT_NPC_FLAG_REPAIR)))
                    return c;
        return nullptr;
    }

    /// Near a trade or repair NPC the module stands in for the shopping trip a bot
    /// cannot walk: the reagents its known recipes ask that no node yields are
    /// topped up to a small stock (the same stand-in Produce makes when it crafts),
    /// and a repairer's hammer mends what the day's work dented, paying the same
    /// price a player would. One line says what happened; the restock runs at most
    /// once every five minutes per companion, so standing at the vendor is quiet.
    void RestockNearVendor(Player* owner, Player* bot)
    {
        if (!g_cfg.vendorRestock || !bot->IsInWorld() || !bot->IsAlive())
            return;

        Creature* vendor = VendorNearby(bot, 40.0f);
        if (!vendor)
            return;

        uint32 const botGuid = bot->GetGUID().GetCounter();
        uint32 const now = uint32(time(nullptr));
        uint32& last = g_lastVendorRestock[botGuid];
        if (last && now - last < 300)
            return;

        auto const pair = ClassProfessionPair(bot->getClass());
        std::set<uint32> reagents;
        // the BASE products' reagents only - the shop's part of what the companion
        // actually crafts. The whole recipe book was the mistake: a spellbook of 253
        // alchemy crafts turned the restock into a free Dragon Scale dispenser.
        for (uint32 const product : ProductsOf(pair.second))
        {
            for (SkillLineAbilityEntry const* entry : GetSkillLineAbilitiesBySkillLine(pair.second))
            {
                SpellInfo const* info = sSpellMgr->GetSpellInfo(entry->Spell);
                if (!info || !bot->HasSpell(entry->Spell))
                    continue;
                bool makes = false;
                for (uint8 e = 0; e < 3; ++e)
                    if (info->Effects[e].Effect == SPELL_EFFECT_CREATE_ITEM &&
                        uint32(info->Effects[e].ItemType) == product)
                        makes = true;
                if (!makes)
                    continue;
                for (uint8 r = 0; r < MAX_SPELL_REAGENTS; ++r)
                {
                    uint32 const item = uint32(info->Reagent[r]);
                    if (item && !IsGatherableReagent(pair.first, item))
                        reagents.insert(item);
                }
            }
        }

        std::string topped;
        for (uint32 const item : reagents)
        {
            static const uint32 kShopCarried[] = { 3371, 8925, 2320, 2604, 3857, 2605 };
            bool carried = false;
            for (uint32 const shopItem : kShopCarried)
                if (shopItem == item) { carried = true; break; }
            if (!carried)
                continue;
            uint32 const want = 5;
            uint32 const have = bot->GetItemCount(item);
            if (have >= want)
                continue;
            uint32 const give = std::min<uint32>(want - have, 10);
            ItemPosCountVec dest;
            if (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, item, give) != EQUIP_ERR_OK)
                continue;
            if (Item* bought = bot->StoreNewItem(dest, item, true))
            {
                bot->SendNewItem(bought, give, true, false, true);
                if (!topped.empty())
                    topped += ", ";
                topped += std::to_string(give) + "x " + ItemNameFor(item);
            }
        }

        bool repaired = vendor->HasNpcFlag(UNIT_NPC_FLAG_REPAIR) &&
                        bot->DurabilityRepairAll(true, 1.0f, false) > 0;

        g_lastVendorRestock[botGuid] = now;

        if (topped.empty() && !repaired)
            return;

        std::string const said = topped.empty()
            ? std::string("I had my gear repaired while we were here.")
            : "I restocked " + topped + (repaired ? " and had my gear repaired." : ".");
        SayToOwner(bot, owner, said, 30);
    }

    /// The highest gathering requirement the zone's own nodes carry, resolved once per
    /// zone from the gameobject spawns inside its WorldMapArea bounds. The lock table
    /// distinguishes the line itself: Index 2 = the herbalism locks, 3 = the mining
    /// locks (verified against this realm's node templates), and Skill is the required
    /// value - Bruiseweed's nodes ask 100, Briarthorn 70, a Copper Vein 1. When no node
    /// resolves, the caller falls back to the zone level.
    static uint32 ZoneNodeRequirement(std::pair<uint16, uint16> const& pair, uint32 zoneId)
    {
        // keyed by zone AND skill line: the requirement is per line (herbs ask one
        // thing, veins another) - a zone-only key let the first profession's answer
        // poison every other one, and the skinning path's silent zero struck the
        // whole zone blind.
        uint64 const cacheKey = (uint64(zoneId) << 16) | uint64(pair.first);
        static std::map<uint64, uint32> cache;
        auto const cached = cache.find(cacheKey);
        if (cached != cache.end())
            return cached->second;

        uint32 wantedIndex = 0;
        if (pair.first == SKILL_HERBALISM)
            wantedIndex = 2;
        else if (pair.first == SKILL_MINING)
            wantedIndex = 3;
        else
        {
            cache[cacheKey] = 0;
            return 0;                                     // skinning walks the mob levels
        }

        // the zone's bounds, from the same WorldMapArea parse the pin zones use
        WorldMapAreaRow const* bounds = nullptr;
        for (WorldMapAreaRow const& row : g_worldMapAreas)
            if (row.areaId == int32(zoneId))
            {
                if (!bounds || (row.x2 - row.x1) * (row.y2 - row.y1) <
                               (bounds->x2 - bounds->x1) * (bounds->y2 - bounds->y1))
                    bounds = &row;
            }

        uint32 best = 0;
        if (bounds)
        {
            float const x1 = std::min(bounds->x1, bounds->x2);
            float const x2 = std::max(bounds->x1, bounds->x2);
            float const y1 = std::min(bounds->y1, bounds->y2);
            float const y2 = std::max(bounds->y1, bounds->y2);

            QueryResult nodes = WorldDatabase.Query(
                "SELECT g.position_x, g.position_y, gt.data0 FROM gameobject g "
                "JOIN gameobject_template gt ON gt.entry = g.id "
                "WHERE gt.type = 3 AND gt.data0 <> 0 AND g.map = {}", bounds->mapId);
            if (nodes)
            {
                do
                {
                    Field* fields = nodes->Fetch();
                    float const x = fields[0].Get<float>();
                    float const y = fields[1].Get<float>();
                    if (x < x1 || x > x2 || y < y1 || y > y2)
                        continue;

                    LockEntry const* lock = sLockStore.LookupEntry(fields[2].Get<uint32>());
                    if (!lock || lock->Type[0] != 2 || lock->Index[0] != wantedIndex)
                        continue;

                    best = std::max<uint32>(best, lock->Skill[0] ? lock->Skill[0] : 1);
                } while (nodes->NextRow());
            }
        }

        cache[cacheKey] = best;
        LOG_INFO("server.loading", "[CompanionQuestSync] zone {} node requirement ({}): {}", zoneId, uint32(pair.first), best);
        return best;
    }

    /// The land asks a minimum of its gatherers. A companion walking into a zone it
    /// cannot work yet catches its gathering skill up to what the zone asks - once per
    /// zone, with one honest line - and never below what it earned organically. The
    /// approximation is the zone's own level in the classic currency of five points
    /// per level (Westfall asks ~50, Elwynn ~25).
    static void CatchUpGatherSkill(Player* owner, Player* bot)
    {
        // deliberately not gated on combat: this is a stat write, not a spell - a
        // companion that never leaves a fight (a Westfall grind is exactly that) would
        // otherwise wait for a quiet pass that never comes
        if (g_cfg.debug >= 2)
        if (!g_cfg.gatherCatchUp || !owner || !bot->IsInWorld() || !bot->IsAlive())
            return;

        if (owner->GetMapId() != bot->GetMapId() || !owner->GetZoneId())
            return;

        // The classic DBCs leave area_level zero for almost every zone (Westfall, the
        // zone this feature exists for, reports 0), so the level comes from a table of
        // the zones the realm actually serves, taken from those zones' own design; a
        // non-zero area_level (a newer zone) still wins.
        uint32 const zoneId = owner->GetZoneId();
        uint32 zoneLevel = 0;
        auto const pair = ClassProfessionPair(bot->getClass());

        // what the land actually asks: the highest node requirement its own nodes carry
        uint32 const nodeReq = ZoneNodeRequirement(pair, zoneId);
        AreaTableEntry const* area = sAreaTableStore.LookupEntry(zoneId);
        if (area && area->area_level > 0)
            zoneLevel = uint32(area->area_level);
        else
        {
            static constexpr std::pair<uint32, uint32> kZoneLevels[] = {
                {1, 8}, {3, 40}, {4, 50}, {8, 35}, {10, 20}, {11, 20}, {12, 5}, {14, 5}, {15, 35},
                {16, 45}, {17, 10}, {33, 30}, {36, 30}, {38, 10}, {40, 10}, {41, 45}, {44, 15},
                {45, 30}, {46, 50}, {47, 40}, {51, 45}, {65, 74}, {66, 76}, {67, 77}, {85, 5},
                {130, 10}, {141, 5}, {148, 15}, {210, 79}, {215, 5}, {267, 20}, {331, 25},
                {357, 40}, {361, 50}, {394, 74}, {400, 25}, {405, 30}, {406, 20}, {440, 40},
                {490, 50}, {495, 72}, {618, 55}, {1377, 55}, {2817, 77}, {3430, 5}, {3433, 15},
                {3483, 60}, {3518, 65}, {3519, 62}, {3520, 67}, {3521, 62}, {3522, 65},
                {3523, 67}, {3524, 10}, {3525, 15}, {3537, 72}, {3711, 77}, {4080, 70},
            };
            for (auto const& [known, level] : kZoneLevels)
                if (known == zoneId) { zoneLevel = level; break; }
        }

        if (!zoneLevel && !nodeReq)
            return;                                       // an unknown zone asks nothing new

        uint32 const botGuid = bot->GetGUID().GetCounter();
        uint32& lastZone = g_lastGatherZone[botGuid];
        if (lastZone == zoneId)
            return;
        lastZone = zoneId;

        // clamped to the level's own tier: a sloppy WorldMapArea box (this realm's
        // Westfall row spans half the eastern kingdoms) can otherwise drag a neighbour's
        // node in, and no level-12 character should hold a 210 gathering skill
        uint32 const tierCap = ProfessionCap(bot->GetLevel());
        uint32 const zoneMin = std::min<uint32>(nodeReq ? nodeReq : zoneLevel * 5, tierCap);
        uint16 const skill = pair.first;
        uint16 const cur = bot->GetSkillValue(skill);
        if (cur >= zoneMin)
            return;

        uint16 const ceiling = uint16(std::max<uint32>(zoneMin, ProfessionCap(bot->GetLevel())));
        bot->SetSkill(skill, 0, uint16(zoneMin), ceiling);

        // The pair rises together: a gathering skill the zone dragged up is useless to
        // a production profession that cannot work the materials, so the production
        // half is caught up to the same level - what it gathers, it can use.
        uint16 const production = pair.second;
        uint16 const prodCur = bot->GetSkillValue(production);
        if (prodCur < zoneMin)
            bot->SetSkill(production, 0, uint16(zoneMin), ceiling);

        SayToOwner(bot, owner,
                   "I caught my " + std::string(ProfessionOf(skill).name) + " up to " +
                   std::to_string(zoneMin) + " - this is what this land asks.", 30);
    }

    /// A gathering profession is only as good as its tool: skinning needs the knife,
    /// mining the pick. The grant at teach time can lose to a full bag, so the upkeep
    /// keeps trying every pass until the tool is in the pack - then it stays silent.
    static void EnsureProfessionTools(Player* bot)
    {
        // same as the catch-up: the grant is a bag write, and a companion in a running
        // fight would never see a quiet pass
        if (!bot->IsInWorld() || !bot->IsAlive())
            return;

        auto const pair = ClassProfessionPair(bot->getClass());
        uint16 const skills[2] = { pair.first, pair.second };
        for (uint16 const skill : skills)
        {
            ProfessionEntry const entry = ProfessionOf(skill);
            if (!entry.tool || bot->GetItemCount(entry.tool))
                continue;

            ItemPosCountVec dest;
            if (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, entry.tool, 1) != EQUIP_ERR_OK)
                continue;                                   // bags full - try the next pass

            if (Item* tool = bot->StoreNewItem(dest, entry.tool, true))
                bot->SendNewItem(tool, 1, true, false, true);
        }
    }
#endif

    /// How many slots of a quest log are in use, for the heartbeat line.
    uint32 CountQuestLog(Player* player)
    {
        uint32 count = 0;
        for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            if (player->GetQuestSlotQuestId(slot))
                ++count;

        return count;
    }

    /// Give the companion somewhere to put things.
    ///
    /// Chasing bag space with destruction is the wrong end of the problem: a level-6
    /// hunter filled 20 slots with two quest stacks, cloth, water and a dozen gear
    /// pieces, and then could not loot the last Gold Dust it needed. With this on, an
    /// empty bag slot is filled once with a plain 16-slot bag (Pumpkin Bag, no level
    /// requirement), so the companion carries 5 bags instead of 1. The item and the
    /// number of slots are both configurable, and a slot that is already full - or
    /// held by a bag the player gave the companion - is never touched.
    void EnsureBags(Player* owner, Player* bot)
    {
        if (!g_cfg.extraBags || !g_cfg.extraBagItem)
            return;

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(g_cfg.extraBagItem);
        if (!proto || proto->Class != ITEM_CLASS_CONTAINER || !proto->ContainerSlots)
            return;

        uint32 given = 0;

        for (uint8 i = 0; i < g_cfg.extraBags && i < (INVENTORY_SLOT_BAG_END - INVENTORY_SLOT_BAG_START); ++i)
        {
            uint8 const slot = INVENTORY_SLOT_BAG_START + i;
            if (bot->GetBagByPos(slot))
                continue;                                   // already has one

            ItemPosCountVec dest;
            if (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, g_cfg.extraBagItem, 1) != EQUIP_ERR_OK)
                break;

            Item* bag = bot->StoreNewItem(dest, g_cfg.extraBagItem, true);
            if (!bag)
                continue;

            bot->EquipItem(uint16((INVENTORY_SLOT_BAG_0 << 8) | slot), bag, true);
            ++given;
        }

        if (given)
        {
            LOG_INFO("server.loading", "[CompanionQuestSync] {} given {} bag(s) of {} slots ({} free slot(s) now)",
                     bot->GetName(), given, proto->ContainerSlots, bot->GetFreeInventorySpace());

            if (g_cfg.debug >= 1)
                LOG_INFO("server.loading", "[CompanionQuestSync] {} was carrying too much - bags topped up",
                         bot->GetName());
        }
    }

    /// Put clothes on a companion that has none.
    ///
    /// Release wipes a body back to a blank level 1, which includes its gear, so a freshly
    /// created companion arrived naked and died to the first wolf. Rather than keep a table
    /// of starting kit per race and class that would drift from the client's, this asks the
    /// core what it issues a new character of that race and class and stores it the same
    /// way character creation does - which also equips the weapon and the armour.
    /// A companion that already carries a weapon or a chest piece is left alone, so gear the
    /// player has given it is never replaced.
    /// A weapon that fits the class.
    static uint32 WeaponForClass(uint8 classId)
    {
        switch (classId)
        {
            case CLASS_WARRIOR:       return 25;      // Worn Shortsword
            case CLASS_PALADIN:       return 36;      // Worn Mace
            case CLASS_DEATH_KNIGHT:  return 25;
            case CLASS_SHAMAN:        return 36;
            case CLASS_DRUID:         return 36;
            case CLASS_PRIEST:        return 36;
            default:                  return 2092;    // Worn Dagger
        }
    }

    /// Wear a kit item that ended up in the bags instead of on the body.
    ///
    /// "Equip" through CanEquipNewItem can fall back to storing the item in a bag, which is how
    /// a companion came out wearing a weapon and boots while its chest and legs were missing:
    /// two pieces were issued, counted as dressed, put in the bag and then sold as vendor trash.
    /// This walks the bags for the exact item and moves it into the slot with the slot named
    /// explicitly, which is the direction that actually equips.
    bool WearFromBags(Player* bot, uint8 slot, uint32 itemId)
    {
        for (uint8 bagSlot = INVENTORY_SLOT_ITEM_START; bagSlot < INVENTORY_SLOT_ITEM_END; ++bagSlot)
        {
            Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bagSlot);
            if (!item || item->GetEntry() != itemId)
                continue;

            uint16 dest = 0;
            if (bot->CanEquipItem(slot, dest, item, true) != EQUIP_ERR_OK)
                continue;

            bot->SwapItem(uint16((INVENTORY_SLOT_BAG_0 << 8) | bagSlot), dest);

            return bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot) != nullptr;
        }

        return false;
    }

    /// Wear anything in the bags that fits an empty slot.
    ///
    /// A companion that was handed gear and left it in the bag is the same bug from the other
    /// side: the item is "there" but not on the body, so it looks naked while the piece sits in
    /// a backpack. Only empty slots are filled and only what the body may use, so nothing the
    /// player chose to wear is ever displaced.
    uint32 WearLooseGear(Player* bot)
    {
        uint32 worn = 0;

        for (uint8 bagSlot = INVENTORY_SLOT_ITEM_START; bagSlot < INVENTORY_SLOT_ITEM_END; ++bagSlot)
        {
            Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bagSlot);
            if (!item)
                continue;

            ItemTemplate const* proto = item->GetTemplate();
            if (!proto || (proto->Class != ITEM_CLASS_ARMOR && proto->Class != ITEM_CLASS_WEAPON))
                continue;

            if (!bot->CanUseItem(proto))
                continue;

            uint8 const target = bot->FindEquipSlot(proto, NULL_SLOT, false);
            if (target == NULL_SLOT || bot->GetItemByPos(INVENTORY_SLOT_BAG_0, target))
                continue;

            uint16 dest = 0;
            if (bot->CanEquipItem(target, dest, item, false) != EQUIP_ERR_OK)
                continue;

            bot->SwapItem(uint16((INVENTORY_SLOT_BAG_0 << 8) | bagSlot), dest);

            if (bot->GetItemByPos(INVENTORY_SLOT_BAG_0, target))
            {
                ++worn;
                LOG_INFO("server.loading", "[CompanionQuestSync] {} picked up and wore {}",
                         bot->GetName(), proto->Name1);
            }
        }

        return worn;
    }

    /// Kit items this module used to issue that do not work here, so a body wearing one is
    /// re-dressed instead of being left alone.
    ///
    /// There is no way to ask the server whether an item renders: the vest equips, reads back and
    /// counts as worn while the player sees a bare torso. The only practical answer is a list of
    /// the ids that turned out to be wrong, retired by name, and a body found wearing one gets
    /// the current kit instead.
    bool IsRetiredKitItem(uint32 entry)
    {
        switch (entry)
        {
            case 193:      // Tattered Cloth Vest - no visible model in this client
            case 194:      // Tattered Cloth Pants - Horde only (FlagsExtra 1)
            case 2121:     // Thin Cloth Armor - never rendered
            case 3270:     // Flax Vest - never rendered
                return true;
            default:
                return false;
        }
    }

    /// The armour subclass a class can actually wear.
    uint32 ArmourSubclassFor(uint8 clazz)
    {
        switch (clazz)
        {
            case CLASS_WARRIOR:
            case CLASS_PALADIN:
            case CLASS_DEATH_KNIGHT:  return ITEM_SUBCLASS_ARMOR_PLATE;
            case CLASS_HUNTER:
            case CLASS_SHAMAN:        return ITEM_SUBCLASS_ARMOR_MAIL;
            case CLASS_ROGUE:
            case CLASS_DRUID:         return ITEM_SUBCLASS_ARMOR_LEATHER;
            default:                  return ITEM_SUBCLASS_ARMOR_CLOTH;
        }
    }

    /// The plain pieces a level 1 character wears, taken from the game's own starter vocabulary -
    /// Tattered Cloth, Dirty Leather, Rusted Chain - and checked one by one against item_template:
    /// right slot, no faction or class restriction, a display id the client ships.
    ///
    /// Selecting these from the database by formula looked like the general solution and was not:
    /// ordered by item level it produced "Tuxedo Jacket" and "Black Night Elf Helm", ordered by
    /// level it produced NPC-only templates. A short verified list beats a clever query, and two
    /// candidates per row cover the odd item a core refuses.
    struct ArmourSet
    {
        uint32 belt[2];
        uint32 bracers[2];
        uint32 gloves[2];
        uint32 cloak[2];
    };

    ArmourSet const& StarterArmourSet(uint8 clazz)
    {
        // static: built once, and returned by reference
        static ArmourSet const cloth   = { { 3595, 3363 }, { 3596, 3365 }, { 711, 1377 }, { 11847, 1376 } };
        static ArmourSet const leather = { { 1835, 1418 }, { 1836, 2124 }, { 714, 1422 },  { 1376, 0 } };
        static ArmourSet const mail    = { { 2387, 0 },    { 2390, 0 },    { 2391, 0 },     { 1376, 0 } };

        // a plate-wearer starts in mail: nothing plate exists at level 1 in this data
        switch (ArmourSubclassFor(clazz))
        {
            case ITEM_SUBCLASS_ARMOR_LEATHER: return leather;
            case ITEM_SUBCLASS_ARMOR_MAIL:
            case ITEM_SUBCLASS_ARMOR_PLATE:   return mail;
            default:                          return cloth;
        }
    }

    /// Put clothes on a companion, one slot at a time.
    ///
    /// Release wipes a body, so a created companion arrives with nothing on. The core's own
    /// list is the obvious source and it is empty here (`playercreateinfo_item` holds a single
    /// unrelated row), so the kit is issued directly.
    ///
    /// Each slot is filled and then *read back*: the previous version asked the core to equip
    /// and trusted the return value, and a companion turned up wearing a weapon and boots with
    /// its chest and legs missing, because an item that cannot be equipped is stored in the bag
    /// instead and the counter still said "dressed". Now an item that does not end up on the
    /// body is an error in the log, naming the slot and the equip error, and the other slots are
    /// never affected by the one that failed - there is no all-or-nothing guard any more.
    void EnsureStartingGear(Player* /*owner*/, Player* bot)
    {
        struct SlotKit
        {
            uint8 slot;
            uint32 item;
            char const* label;
        };

        // Several candidates per slot. This repack flags some of these items faction-only -
        // Tattered Cloth Pants (194) carries FlagsExtra 1, Horde only - and the core answers
        // EQUIP_ERR_YOU_CAN_NEVER_USE_THAT_ITEM forever, so a single hard-coded id can leave a
        // companion permanently half-dressed. The list is tried in order.
        struct SlotChoice
        {
            uint8 slot;
            uint32 items[3];
            char const* label;
        };

        std::vector<SlotChoice> kit;

        kit.push_back({ EQUIPMENT_SLOT_MAINHAND, { WeaponForClass(uint8(bot->getClass())), 0, 0 }, "weapon" });

        // Robes, not vests. Item 193 (Tattered Cloth Vest) equips and reports as worn but has no
        // visible model in this client - the character sheet showed an empty chest while the
        // server said "wearing chest" - so the fallbacks are the classic starter robes, whose
        // display ids are the ones the client definitely ships.
        kit.push_back({ EQUIPMENT_SLOT_CHEST, { 57, 56, 0 },   "chest" });
        kit.push_back({ EQUIPMENT_SLOT_LEGS,  { 39, 1378, 0 }, "legs"  });
        kit.push_back({ EQUIPMENT_SLOT_FEET,  { 195, 1374, 0 }, "feet" });

        // The rest of the outfit: belt, bracers, gloves and cloak in the class's own armour type.
        // Off-switchable, because it is a curated list rather than the core's own itemisation.
        if (g_cfg.extraArmourSlots)
        {
            ArmourSet const& set = StarterArmourSet(uint8(bot->getClass()));

            kit.push_back({ EQUIPMENT_SLOT_WAIST,  { set.belt[0],    set.belt[1],    0 }, "waist"  });
            kit.push_back({ EQUIPMENT_SLOT_WRISTS, { set.bracers[0], set.bracers[1], 0 }, "wrists" });
            kit.push_back({ EQUIPMENT_SLOT_HANDS,  { set.gloves[0],  set.gloves[1],  0 }, "hands"  });
            kit.push_back({ EQUIPMENT_SLOT_BACK,   { set.cloak[0],   set.cloak[1],   0 }, "back"   });
        }

        uint32 filled = 0;

        for (SlotChoice const& one : kit)
        {
            if (Item* worn = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, one.slot))
            {
                if (!IsRetiredKitItem(worn->GetEntry()))
                    continue;                               // keep what it is wearing

                // an item this module issued and has since withdrawn: take it off, the player
                // cannot see it anyway
                LOG_INFO("server.loading", "[CompanionQuestSync] {} removing retired kit item {} from {}",
                         bot->GetName(), worn->GetEntry(), one.label);

                bot->DestroyItem(INVENTORY_SLOT_BAG_0, one.slot, true);
            }

            uint32 const botGuid = bot->GetGUID().GetCounter();
            uint32 const now = static_cast<uint32>(time(nullptr));
            uint64 const key = (uint64(botGuid) << 8) | one.slot;

            for (uint32 const itemId : one.items)
            {
                if (!itemId)
                    continue;

                // Wear a copy that is already in the bags before making another one: without
                // this, a slot that cannot be filled collects a new item every 30 seconds.
                if (WearFromBags(bot, one.slot, itemId) &&
                    bot->GetItemByPos(INVENTORY_SLOT_BAG_0, one.slot))
                    break;

                auto const created = g_gearCreatedAt.find(key);
                if (created != g_gearCreatedAt.end() && now - created->second < Timing::GearRetry)
                    break;                                  // tried recently, leave this slot alone

                uint16 dest = 0;
                InventoryResult const check = bot->CanEquipNewItem(NULL_SLOT, dest, itemId, true);
                if (check != EQUIP_ERR_OK)
                {
                    if (g_cfg.debug >= 1)
                        LOG_INFO("server.loading",
                                 "[CompanionQuestSync] {} {} candidate {} refused (error {}), trying the next",
                                 bot->GetName(), one.label, itemId, uint32(check));

                    continue;                              // faction or skill locked: next candidate
                }

                bot->AutoUnequipOffhandIfNeed();

                if (!bot->EquipNewItem(dest, itemId, true))
                    continue;

                g_gearCreatedAt[key] = now;

                if (!bot->GetItemByPos(INVENTORY_SLOT_BAG_0, one.slot))
                    WearFromBags(bot, one.slot, itemId);

                break;
            }

            if (Item* worn = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, one.slot))
            {
                ++filled;
                LOG_INFO("server.loading", "[CompanionQuestSync] {} wearing {}: {} ({})",
                         bot->GetName(), one.label, worn->GetTemplate()->Name1, worn->GetEntry());
            }
            else
            {
                LOG_ERROR("server.loading",
                          "[CompanionQuestSync] {} {} is still empty - no candidate could be worn",
                          bot->GetName(), one.label);
            }
        }

        filled += WearLooseGear(bot);

        if (filled)
            LOG_INFO("server.loading", "[CompanionQuestSync] {} dressed ({} slot(s) filled this pass)",
                     bot->GetName(), filled);
    }

    /// Take a companion out of the owner's group.
    ///
    /// A dismissed companion must not stay in the party frame: playerbots usually ungroups on
    /// logout, but not always - the member lingers as an offline row, the group frame keeps a
    /// slot, and the player has to remove it by hand. Doing it explicitly costs nothing and is
    /// also the correct order: leave the group, then log out.
    void LeaveGroup(Player* owner, uint32 botGuid, std::string const& name)
    {
        if (!owner)
            return;

        Group* group = owner->GetGroup();
        if (!group)
            return;

        ObjectGuid const guid = ObjectGuid::Create<HighGuid::Player>(botGuid);
        if (!group->IsMember(guid))
            return;

        group->RemoveMember(guid);

        LOG_INFO("server.loading", "[CompanionQuestSync] {} removed from {}'s group",
                 name, owner->GetName());
    }

    /// Make the client forget a cached unit name.
    ///
    /// A 3.3.5 client caches names per guid for the whole session, so a body renamed to the
    /// player's chosen name still showed as Zz<guid> in the group frame and on the nameplate
    /// while the panel - which reads our own payload - showed the new name. The core has the
    /// opcode but no packet class for it, so it is built raw with a packed guid.
    void InvalidatePlayerName(Player* owner, uint32 botGuid)
    {
        if (!owner || !owner->GetSession())
            return;

        WorldPacket data(SMSG_INVALIDATE_PLAYER, 8);
        data << ObjectGuid::Create<HighGuid::Player>(botGuid);
        owner->GetSession()->SendPacket(&data);
    }

    /// The last tie to mod-wowlegends: its binding table, read only, so two modules never level
    /// and quest the same bot. With RespectLegacyBindings = 0 the clause disappears and this
    /// module knows nothing about that table - the honest position for one that claims to stand
    /// alone, and the setting to use when that module is not installed at all.
    std::string LegacyBindingClause(char const* alias = "c.guid")
    {
        if (!g_cfg.respectLegacyBindings)
            return "";

        return std::string("AND ") + alias + " NOT IN (SELECT bot_guid FROM wowlegends_companion) ";
    }

    /// What a body is actually wearing, slot by slot - the check that was missing.
    std::string DescribeGear(Player* bot)
    {
        static char const* const names[] =
        {
            "head", "neck", "shoulder", "shirt", "chest", "waist", "legs", "feet", "wrists",
            "hands", "finger1", "finger2", "trinket1", "trinket2", "back", "main hand",
            "off hand", "ranged", "tabard"
        };

        std::string out;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!item)
                continue;

            if (!out.empty())
                out += ", ";

            out += std::string(names[slot]) + ": " + item->GetTemplate()->Name1;
        }

        return out.empty() ? "nothing equipped" : out;
    }

    /// Sell before destroying.
    ///
    /// Destroying greys was the only lever the module had, and greys are worth
    /// almost nothing - the bag pressure comes from white gear the companion will
    /// never use. playerbots already knows which items those are (its item usage
    /// value answers VENDOR/AH for them) and owns a "sell vendor trash" action, so
    /// the module asks its AI to run that first: real coin, nothing lost. Without a
    /// vendor in interact range the action is a no-op, which makes it safe to try
    /// whenever the bags get tight.
    /// Get the companion the last few yards to a quest giver.
    ///
    /// playerbots only escorts a bot as far as its master: stand 15 yards from the
    /// NPC and the companion waits behind you, so the hand-in fails with "cannot
    /// talk to the quest giver" and the player has to lead it by hand.
    ///
    /// Walking it does not work. The module used to issue a MovePoint toward the NPC
    /// every 8 s, but playerbots' follow generator runs every tick and drags the bot
    /// back to its master a fraction of a second later - the log showed the gap
    /// *growing* (43 yd, then 50 yd) while it kept "walking". So step it instead,
    /// beside the giver rather than on top of it, and let its own reward action fire
    /// from interaction range.
    void WalkToGiver(Player* owner, Player* bot, Creature* npc)
    {
        if (!npc || bot->IsInCombat() || npc->IsInCombat())
            return;

        float const toNpc = bot->GetDistance(npc);
        if (toNpc <= 6.0f || toNpc > 80.0f)
            return;

        if (owner->GetDistance(bot) > 80.0f)
            return;

        // Only worth stepping when the player is at the giver themselves: the point
        // is to let the companion interact from the owner's side, not to drag it
        // across the zone to an NPC the player is not standing at.
        if (owner->GetDistance(npc) > 10.0f)
            return;

        uint32 const botGuid = bot->GetGUID().GetCounter();
        uint32 const now = static_cast<uint32>(time(nullptr));
        uint32& last = g_walkAt[botGuid];
        if (last && now - last < Timing::WalkStep)
            return;                                  // one hop per twenty seconds

        last = now;

        StepBesideOwner(owner, bot, npc->GetPositionX(), npc->GetPositionY());
        bot->SetFacingToObject(npc);

        if (g_cfg.debug >= 1)
            LOG_INFO("server.loading", "[CompanionQuestSync] stepped {} to {}'s side ({:.0f} yd from {})",
                     bot->GetName(), owner->GetName(), toNpc, npc->GetName());
    }

    /// Hand in the companion's own quests even when the player is nowhere near the giver.
    ///
    /// This is the "I cannot speak to the quest giver" case. The turn-in reporter and
    /// MarkOwnErrands both start from an NPC the *owner* can see, so a companion with a
    /// finished quest and a player across the square got a complaint and nothing else.
    /// Here the giver is found from the quest itself, the companion is stepped beside it
    /// and its own reward action is asked to run - no leading, no standing nearby.
    /// The companion picks its own reward when a quest offers a choice.
    ///
    /// playerbots asks the master to choose; a companion's master is often not there, and a
    /// quest that waits on a choice is a quest that never hands in. The pick is the highest
    /// item level of the options - a defensible "best", and the companion says what it kept.
    uint32 ChooseRewardIndex(Player* bot, Quest const* quest)
    {
#ifdef MOD_PLAYERBOTS
        // Score the choice the way the companion's own AI would (a per-class stat
        // weight, not a raw item level): the module winning the hand-in race must not
        // hand the companion a worse pick than its own turn-in would have taken.
        if (GET_PLAYERBOT_AI(bot))
        {
            StatsWeightCalculator calc(bot);
            uint32 best = 0;
            float bestScore = 0.0f;
            bool any = false;

            for (uint8 i = 0; i < quest->GetRewChoiceItemsCount(); ++i)
            {
                ItemTemplate const* item = sObjectMgr->GetItemTemplate(quest->RewardChoiceItemId[i]);
                if (!item)
                    continue;

                float const score = calc.CalculateItem(quest->RewardChoiceItemId[i]);
                if (!any || score > bestScore)
                {
                    any = true;
                    bestScore = score;
                    best = i;
                }
            }

            if (any)
                return best;
        }
#endif

        // No AI to ask (or playerbots absent): the plain item level remains.
        uint32 best = 0;
        uint32 bestLevel = 0;

        for (uint8 i = 0; i < quest->GetRewChoiceItemsCount(); ++i)
        {
            ItemTemplate const* item = sObjectMgr->GetItemTemplate(quest->RewardChoiceItemId[i]);
            if (!item)
                continue;

            if (item->ItemLevel > bestLevel)
            {
                bestLevel = item->ItemLevel;
                best = i;
            }
        }

        return best;
    }


    void DeliverHandIns(Player* owner, Player* bot)
    {
        if (!bot->IsAlive() || bot->IsInCombat() || (owner && owner->IsInCombat()))
            return;

        uint32 const nowTs = static_cast<uint32>(time(nullptr));
        uint32 const botGuid = bot->GetGUID().GetCounter();

        // Newly-ready invitations, staged and flushed at the end of the pass: the
        // closest quest on the companion's own map speaks first, not whichever quest
        // happened to sit in the first log slot.
        struct Invite { uint32 dist; bool sameZone; EnderSpot spot; Quest const* quest; };
        std::vector<Invite> invites;

        for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 const questId = bot->GetQuestSlotQuestId(slot);
            if (!questId || bot->GetQuestStatus(questId) != QUEST_STATUS_COMPLETE)
            {
                g_readyAt.erase((uint64(botGuid) << 32) | questId);
                continue;
            }

            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest || !DescribeMissingItems(bot, quest).empty())
            {
                g_readyAt.erase((uint64(botGuid) << 32) | questId);
                continue;                          // still owes items: cannot reward
            }

            EnderSpot const& spot = FindQuestEnder(bot, questId);
            if (!spot.found || spot.map != bot->GetMapId())
                continue;

            if (spot.zone && spot.zone != bot->GetZoneId())
                continue;                          // giver is in another zone

            float const dist = bot->GetDistance(spot.x, spot.y, spot.z);

            if (g_cfg.handInMode == 2 && dist > g_cfg.handInNear)
            {
                // The companion can finish the hand-in itself, but only where a player
                // could too: in reach of the giver, not across half the map. Beyond the
                // range it asks to be led - the gold pin on the giver's map is already
                // pointing there - and nothing is rewarded and nothing blinks.
                uint64 const leadKey = (uint64(botGuid) << 32) | questId;
                uint32& nudgedAt = g_leadNudgeAt[leadKey];
                if (!nudgedAt || nowTs - nudgedAt >= 180)
                {
                    nudgedAt = nowTs;
                    SayToOwner(bot, owner, "I can hand in '" + quest->GetTitle() + "' at " +
                               spot.name + " myself, but only up close - lead me within about " +
                               std::to_string(uint32(g_cfg.handInNear)) + " yards of it and I will do the rest.", 90,
                               quest->GetQuestId(), true);
                }
                continue;
            }

            if (g_cfg.handInMode >= 1)
            {
                uint64 const readyKey = (uint64(botGuid) << 32) | questId;

                // A finished quest is shown first - the gold pin and the companion standing
                // at the giver - and rewarded after HandInDelaySeconds. 0 rewards the tick
                // the quest completes.
                if (g_cfg.handInDelaySeconds > 0)
                {
                    // The owner standing at the giver beside the companion is the state
                    // the delay exists to show them - right in front of them now. Hand in
                    // on this pass, so the module's own voice (what it kept) is the one
                    // that speaks: the bot AI's turn-in narrator, which fires on the
                    // owner's gossip click, finds the copy already rewarded and stays
                    // quiet instead of echoing "Quest Rewarded" a second time.
                    bool const ownerAtGiver = owner &&
                        owner->GetDistance(spot.x, spot.y, spot.z) <= 10.0f;

                    uint32& readyAt = g_readyAt[readyKey];

                    if (readyAt == 0)
                    {
                        readyAt = nowTs;

                        if (ownerAtGiver)
                            continue;      // next pass: it is already in front of the player

                        if (g_cfg.debug >= 1)
                            LOG_INFO("server.loading",
                                     "[CompanionQuestSync] {} is ready to hand in '{}' - showing for {}s",
                                     bot->GetName(), quest->GetTitle(), g_cfg.handInDelaySeconds);

                        // The pin is the state, the whisper is the invitation: the delay
                        // would otherwise show the player a gold pin they never notice.
                        // Staged - the flush after the loop sorts by proximity and map.
                        invites.push_back({ uint32(dist), spot.zone == bot->GetZoneId(), spot, quest });

                        continue;
                    }

                    if (!ownerAtGiver && nowTs - readyAt < g_cfg.handInDelaySeconds)
                        continue;

                    g_readyAt.erase(readyKey);
                }

                uint32& prodAt = g_rewardProdAt[{ botGuid, questId }];
                if (prodAt && nowTs - prodAt < Timing::RewardProd)
                    continue;

                prodAt = nowTs;

                // The step to the giver is the visual, not the requirement: the companion is
                // seen walking up and handing in. Reasonable distances only - yanking it
                // across the zone reads as a bug, and the reward does not wait on this.
                if (dist > 6.0f && dist <= 120.0f)
                {
                    uint32& stepAt = g_walkAt[botGuid];
                    if (!stepAt || nowTs - stepAt >= Timing::WalkStep)
                    {
                        stepAt = nowTs;
                        StepBesidePoint(bot, spot.x, spot.y, spot.z);

                        if (owner && owner->GetDistance(spot.x, spot.y, spot.z) > 10.0f)
                            SayToOwner(bot, owner, "I have gone ahead to " + spot.name +
                                       " to hand in '" + quest->GetTitle() +
                                       "' myself - catch up when you can.", 90,
                                       quest->GetQuestId(), true);
                    }
                }

                Creature* giver = bot->FindNearestCreature(spot.entry, 30.0f);

                // A mailed reward wants the giver as its sender. Without the creature in
                // reach the companion goes to the giver's own coordinates whatever the
                // distance and looks again - and if the world still has nothing there, it
                // says so rather than asking the bot's AI to reward (which is the path that
                // reports it cannot talk to the giver).
                if (!giver && quest->GetRewMailTemplateId())
                {
                    if (dist > 6.0f)
                    {
                        uint32& stepAt = g_walkAt[botGuid];
                        if (!stepAt || nowTs - stepAt >= Timing::WalkStep)
                        {
                            stepAt = nowTs;
                            StepBesidePoint(bot, spot.x, spot.y, spot.z);
                        }

                        giver = bot->FindNearestCreature(spot.entry, 30.0f);
                    }

                    if (!giver)
                    {
                        SayToOwner(bot, owner, "I could not find " + spot.name + " to hand in '" +
                                   quest->GetTitle() + "' - the records put it about " +
                                   std::to_string(uint32(spot.x)) + ", " +
                                   std::to_string(uint32(spot.y)) + ". Have a look?", 90);
                        continue;
                    }
                }

                uint32 const reward = ChooseRewardIndex(bot, quest);

                if (!bot->CanRewardQuest(quest, reward, false))
                    continue;                      // the core refuses: say nothing twice

                bot->RewardQuest(quest, reward, giver, true);
                g_readyAt.erase((uint64(botGuid) << 32) | questId);
                ++g_session[owner->GetGUID().GetCounter()].handedIn;

                if (g_cfg.debug >= 1)
                    LOG_INFO("server.loading",
                             "[CompanionQuestSync] {} handed in '{}' ({}) server-side (giver {}, {:.0f} yd)",
                             bot->GetName(), quest->GetTitle(), questId,
                             giver ? giver->GetName() : std::string("none"), dist);

                // The player should never wonder who chose the reward: when the quest offers
                // one, the companion says what it kept.
                if (quest->GetRewChoiceItemsCount() > 1)
                {
                    if (ItemTemplate const* kept = sObjectMgr->GetItemTemplate(quest->RewardChoiceItemId[reward]))
                        SayToOwner(bot, owner, "For '" + quest->GetTitle() + "' I kept the " +
                                   std::string(kept->Name1) + " - the choice was mine to make.", 90);
                }
                else if (dist > 120.0f)
                {
                    SayToOwner(bot, owner, "I took care of '" + quest->GetTitle() + "' at " +
                               spot.name + " myself - it was a long way for a conversation.", 90);
                }

                continue;
            }

            if (dist <= 6.0f)
            {
                uint32& prodAt = g_rewardProdAt[{ botGuid, questId }];
                if (prodAt && nowTs - prodAt < Timing::RewardProd)
                    continue;

                prodAt = nowTs;
#ifdef MOD_PLAYERBOTS
                if (PlayerbotAI* ai = GET_PLAYERBOT_AI(bot))
                    ai->HandleCommand(CHAT_MSG_WHISPER, "r", owner);
#endif

                if (g_cfg.debug >= 1)
                    LOG_INFO("server.loading", "[CompanionQuestSync] asked {} to hand in '{}' ({})",
                             bot->GetName(), quest->GetTitle(), questId);
                continue;
            }

            // Too far to step responsibly: the companion is questing elsewhere, and
            // yanking it 300 yards across the zone would look like a bug.
            if (dist > 120.0f)
                continue;

            // Step to the OWNER, not to the giver: a blink to the NPC's coordinates
            // from across the square is what looked strange. This only helps while
            // the player is standing at the giver - the normal turn-in - so any
            // other case waits for a pass where they are.
            if (!owner || owner->GetDistance(spot.x, spot.y, spot.z) > 10.0f)
                continue;

            uint32& stepAt = g_walkAt[botGuid];
            if (stepAt && nowTs - stepAt < Timing::WalkStep)
                continue;

            stepAt = nowTs;

            // Stand at the giver when the player is not there, or the step lands the bot in
            // the player's bubble and out of interaction range of the NPC it needs.
            float const ownerToGiver = owner->GetDistance(spot.x, spot.y, spot.z);

            if (ownerToGiver <= 10.0f)
            {
                StepBesideOwner(owner, bot, spot.x, spot.y);
            }
            else
            {
                StepBesidePoint(bot, spot.x, spot.y, spot.z);

                // Say where it went: a blink to somebody else's part of the map is a bug
                // unless the companion explains it.
                SayToOwner(bot, owner, "I have gone ahead to " + spot.name + " to hand in '" +
                           quest->GetTitle() + "' myself - catch up when you can.", 90);
            }

            // Speak to the giver in THIS pass. Checking the distance on the next
            // one loses a race the bot cannot win: its follow strategy walks it
            // back to its owner within a few seconds, so the next pass measures
            // 40+ yards again and steps it once more, forever. That loop is what
            // "the companion never hands anything in" looked like in the log.
            //
            // The reward command is dispatched even when the re-read distance still
            // looks far: `NearTeleportTo` does not necessarily have committed the
            // new position this tick (measured: 16 yd right after a 2.5 yd step),
            // and the bot's own reward action re-checks the range when it runs. Log
            // both numbers so a genuinely inert teleport stays visible.
            float const afterStep = bot->GetDistance(spot.x, spot.y, spot.z);

            g_rewardProdAt[{ botGuid, questId }] = nowTs;

#ifdef MOD_PLAYERBOTS
            if (PlayerbotAI* ai = GET_PLAYERBOT_AI(bot))
                ai->HandleCommand(CHAT_MSG_WHISPER, "r", owner);
#endif

            if (g_cfg.debug >= 1)
                LOG_INFO("server.loading", "[CompanionQuestSync] asked {} to hand in '{}' ({}) - {:.0f} yd at dispatch",
                         bot->GetName(), quest->GetTitle(), questId, afterStep);

            return;
        }

        // Several quests can turn ready in one pass; the invitation the player hears
        // first should be the one nearest the companion - on the map it stands on
        // first, then plain distance. The rate limit queues the rest in the same
        // order, so every invitation still arrives, closest first.
        std::sort(invites.begin(), invites.end(), [](Invite const& a, Invite const& b)
        {
            if (a.sameZone != b.sameZone)
                return a.sameZone;
            return a.dist < b.dist;
        });

        for (Invite const& invite : invites)
        {
            SayToOwner(bot, owner,
                       "I am ready to hand in '" + invite.quest->GetTitle() + "' at " +
                       invite.spot.name + " - about " +
                       std::to_string(invite.dist) + " yards from me. Want to come?", 45,
                       invite.quest->GetQuestId(), true);
        }
    }


    /// Hand the quest in the way the game itself does: `RewardQuest` needs no giver, no
    /// gossip window and no distance - `questGiver` is only the mail sender for mailed
    /// rewards. The giver-vision step in playerbots was the whole reason a companion could
    /// stand in front of an NPC and still report that it could not talk to it.
    ///
    /// The step to the giver stays, as the visual: the companion is seen walking up and
    /// handing in. But a failed step can no longer block the reward - the world may not
    /// agree where the NPC is, and the quest still completes.
    void TrySellJunk(Player* owner, Player* bot)
    {
        if (!g_cfg.sellJunk)
            return;

        if (bot->GetFreeInventorySpace() >= g_cfg.keepFreeSlots)
            return;

        uint32 const botGuid = bot->GetGUID().GetCounter();
        uint32 const now = static_cast<uint32>(time(nullptr));
        uint32& last = g_sellAt[botGuid];
        if (last && now - last < Timing::SellJunk)
            return;

        last = now;

        PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
        if (!ai)
            return;

        // Before anything is destroyed: wear the upgrades. A full bag of gear it could
        // simply put on is the most common jam, and equipping frees the slot outright.
        ai->HandleCommand(CHAT_MSG_WHISPER, "equip upgrade", owner);

        // The same command the owner could whisper; it queues onto the bot's own
        // thread, so calling it from the world tick is safe.
        ai->HandleCommand(CHAT_MSG_WHISPER, "s vendor", owner);

        if (g_cfg.debug >= 2)
            LOG_INFO("server.loading", "[CompanionQuestSync] {} asked to sell vendor trash ({} free slot(s))",
                     bot->GetName(), bot->GetFreeInventorySpace());
    }

    /// Wear upgrades even when the bags are not full.
    ///
    /// playerbots has the judgement - "equip upgrade" compares what the companion carries
    /// against what it wears - but this module only ever issued it as a bag-pressure rescue,
    /// so a companion with room in its pack kept a better drop in the pack while fighting in
    /// the old one. Upgrade gear as it arrives, on its own throttle.
    void TryEquipUpgrades(Player* owner, Player* bot)
    {
        if (!bot || !bot->IsAlive() || bot->IsInCombat())
            return;

        uint32 const botGuid = bot->GetGUID().GetCounter();
        uint32 const now = static_cast<uint32>(time(nullptr));
        uint32& last = g_equipAt[botGuid];

        if (last && now - last < Timing::GearCheck)
            return;

        last = now;

        if (PlayerbotAI* ai = GET_PLAYERBOT_AI(bot))
            ai->HandleCommand(CHAT_MSG_WHISPER, "equip upgrade", owner);

        if (g_cfg.debug >= 2)
            LOG_INFO("server.loading", "[CompanionQuestSync] {} asked to wear any upgrade it is carrying", bot->GetName());
    }

    std::string DescribeMissingItems(Player* bot, Quest const* quest);  // defined below
    uint32 TidyInventory(Player* owner, Player* bot);   // defined below, used by the accept path

    /// Post the keep-worthy overflow home instead of binning it. Whites that the tidy
    /// ladder would destroy and greens the ladder spares both go to the owner: nothing
    /// with value is lost, the vendor decision stays with the player, and the letter
    /// works anywhere - unlike "s vendor", which needs a vendor in range.
    uint32 MailHome(Player* owner, Player* bot, std::vector<Item*>& items)
    {
        if (items.empty())
            return 0;

        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

        // The body lists what is inside: a letter worth opening is the point.
        std::string body = "Loot I could not carry - yours to keep, sell or wear as you like:\n";
        for (Item* item : items)
        {
            if (ItemTemplate const* proto = item->GetTemplate())
                body += "- " + std::string(proto->Name1) + " x" + std::to_string(item->GetCount()) + "\n";
        }

        MailDraft draft(std::string("From ") + bot->GetName(), body);

        uint32 mailed = 0;
        for (Item* item : items)
        {
            // the send path the mail handler itself uses: out of the bags, into the letter
            item->SetNotRefundable(bot);
            bot->MoveItemFromInventory(item->GetBagSlot(), item->GetSlot(), true);
            item->DeleteFromInventoryDB(trans);
            if (item->GetState() == ITEM_UNCHANGED)
                item->FSetState(ITEM_CHANGED);
            item->SetOwnerGUID(owner->GetGUID());
            item->SaveToDB(trans);

            draft.AddItem(item);
            ++mailed;
        }

        draft.SendMailTo(trans, MailReceiver(owner->GetGUID().GetCounter()),
                         MailSender(MAIL_NORMAL, bot->GetGUID().GetCounter()),
                         MAIL_CHECK_MASK_HAS_BODY);
        CharacterDatabase.CommitTransaction(trans);

        if (g_cfg.debug >= 1)
            LOG_INFO("server.loading",
                     "[CompanionQuestSync] {} posted {} item(s) home to {}",
                     bot->GetName(), mailed, owner->GetName());

        SessionTally& tally = g_session[owner->GetGUID().GetCounter()];
        tally.mailed += mailed;
        ++tally.letters;

        return mailed;
    }
    /// The companion bins grey loot when its bags are full - say so, once per
    /// couple of minutes, so nothing appears to vanish unexplained.
    void NotifyJunk(Player* owner, Player* bot, uint32 count)
    {
        if (!count)
            return;

        uint32 const now = static_cast<uint32>(time(nullptr));
        uint32& last = g_junkNoticeAt[bot->GetGUID().GetCounter()];
        if (last && now - last < Timing::JunkNotice)
            return;

        last = now;
        SayToOwner(bot, owner, "I cleared some junk out of my bags to make room - nothing worth keeping.", 120);
    }

    /// Credit the companion for prerequisites its owner already finished.
    ///
    /// `CanTakeQuest` -> `SatisfyQuestPreviousQuest` requires every listed
    /// predecessor to be *rewarded*. Copying the owner's active log can never
    /// satisfy that for a companion adopted after the chain was walked, which is
    /// the normal case: quest first, companion second. This walks the same
    /// `prevQuests` chain the core walks and marks the ancestors the owner has
    /// already rewarded - nothing more. `SetRewardedQuest` touches only the
    /// rewarded set and its save flag: no XP, no item, no spell, no log slot, so
    /// a companion cannot be handed loot or a level it did not earn.
    ///
    /// Negative entries in `prevQuests` are "one of this exclusive group"; they
    /// are left alone deliberately - credit for one sibling would be a guess.
    uint32 CreditPrerequisites(Player* owner, Player* bot, Quest const* quest, SyncReport* report)
    {
        std::vector<uint32> pending;
        for (int32 const prevId : quest->prevQuests)
            if (prevId > 0)
                pending.push_back(uint32(prevId));

        std::set<uint32> visited;
        uint32 credited = 0;

        while (!pending.empty() && visited.size() < 40)
        {
            uint32 const prevId = pending.back();
            pending.pop_back();

            if (!prevId || !visited.insert(prevId).second)
                continue;

            Quest const* prev = sObjectMgr->GetQuestTemplate(prevId);
            if (!prev)
                continue;

            for (int32 const grandId : prev->prevQuests)
                if (grandId > 0)
                    pending.push_back(uint32(grandId));

            if (!owner->IsQuestRewarded(prevId) || bot->IsQuestRewarded(prevId))
                continue;

            bot->SetRewardedQuest(prevId);
            ++credited;

            if (g_cfg.debug >= 1)
                LOG_INFO("server.loading",
                         "[CompanionQuestSync] {} credited '{}' ({}) - {} finished it; '{}' is takeable now",
                         bot->GetName(), prev->GetTitle(), prevId, owner->GetName(), quest->GetTitle());
        }

        if (credited && report)
            report->prereqs += credited;

        return credited;
    }

    /// Is this quest grey for the companion? Same rule the playerbots quest-log
    /// cleaner uses (DropObsoleteQuests drops anything more than 5-8 levels below
    /// the bot, by level band).
    bool IsGreyFor(Player* bot, Quest const* quest)
    {
        int32 const botLevel = int32(bot->GetLevel());
        int32 questLevel = quest->GetQuestLevel();
        if (questLevel == -1)
            questLevel = botLevel;

        int32 trivial = 5;
        if (botLevel >= 40)
            trivial = 8;
        else if (botLevel >= 30)
            trivial = 7;
        else if (botLevel >= 20)
            trivial = 6;

        return (botLevel - questLevel) > trivial;
    }

    /// Tell the owner, once, that a quest is out of the companion's league.
    ///
    /// This is not a refusal to fix later: the companion's own cleaner would drop
    /// the copy, the module would hand it back, and the player would watch the
    /// quest be re-accepted every pass. Saying it once is better than a loop.
    void NotifyOutgrown(Player* owner, Player* bot, Quest const* quest)
    {
        if (g_cfg.notifySkipped < 2)
            return;

        std::set<uint32>& seen = g_notified[bot->GetGUID().GetCounter()];
        if (!seen.insert(quest->GetQuestId()).second)
            return;

        SayToOwner(bot, owner, "I have levelled past '" + quest->GetTitle() +
                               "' - finish that one without me.", 45, quest->GetQuestId());
    }

    /// Mirror one quest onto one companion. Shared by the periodic sweep and the
    /// accept hook, so the "slow" and "instant" paths can never drift apart.
    /// `report` is optional: the hook path only needs the notifications/log.
    void SyncQuestToBot(Player* owner, Player* bot, Quest const* quest, SyncReport* report)
    {
        uint32 const questId = quest->GetQuestId();

        // Nothing to do for a quest the companion already finished.
        if (bot->IsQuestRewarded(questId))
        {
            g_addedAt.erase({ bot->GetGUID().GetCounter(), questId });
            return;
        }

        if (g_cfg.skipTimedQuests && quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_TIMED))
        {
            if (report)
                AddSkipped(*report, quest, "timed quest not copied");
            return;
        }

        QuestStatus botStatus = bot->GetQuestStatus(questId);
        QuestStatus const ownerStatus = owner->GetQuestStatus(questId);

        if (botStatus == QUEST_STATUS_NONE)
        {
            // The copy the module added before has gone again. Something removed it
            // (the bot's own AI, a partial hand-in, a factory re-init), and without
            // this line the only visible symptom is "+ 'X'" repeating every pass.
            std::pair<uint32, uint32> const key{ bot->GetGUID().GetCounter(), questId };
            auto const added = g_addedAt.find(key);
            if (added != g_addedAt.end())
            {
                uint32 const now = static_cast<uint32>(time(nullptr));
                uint32& logged = g_vanishLogged[key];
                if (!logged || now - logged > 300)
                {
                    logged = now;

                    uint32 const countNow = CountQuestLog(bot);

                    LOG_INFO("server.loading",
                             "[CompanionQuestSync] '{}' ({}) vanished from {}'s log after {}s "
                             "(log {} -> {} quests, rewarded {}, level {}, {} hp{})",
                             quest->GetTitle(), questId, bot->GetName(),
                             added->second && now > added->second ? now - added->second : 0,
                             g_addedCount[key], countNow,
                             bot->IsQuestRewarded(questId) ? "yes" : "no",
                             uint32(bot->GetLevel()), bot->GetHealth(),
                             bot->IsInCombat() ? ", in combat" : "");
                }
            }

            // Re-adding every pass is churn the player can see (a quest re-accepted
            // over and over). Pace it: one attempt a minute per quest until the
            // vanish line above says why the copy keeps going.
            if (added != g_addedAt.end())
            {
                uint32 const now = static_cast<uint32>(time(nullptr));
                if (added->second && now > added->second && now - added->second < Timing::AcceptWindow)
                {
                    if (report)
                        AddSkipped(*report, quest, "recently added - waiting for it to stick");

                    return;
                }
            }

            if (!g_cfg.forceAccept)
            {
                // A grey quest is dropped by the companion's own quest-log cleaner
                // (playerbots' DropObsoleteQuests) within a pass or two, so adding it
                // again just makes the player watch it be re-accepted. Don't fight
                // the cleaner - say it once and move on.
                if (IsGreyFor(bot, quest))
                {
                    if (report)
                        AddSkipped(*report, quest, "grey for the companion - outgrown");

                    NotifyOutgrown(owner, bot, quest);
                    return;
                }

                // Before asking the core: give the companion the chain history the
                // owner already has, or every chained quest is refused forever.
                CreditPrerequisites(owner, bot, quest, report);

                if (!bot->CanTakeQuest(quest, false))
                {
                    // Name the gate that refused, so the player is not left
                    // guessing why one quest never arrives. Class and race
                    // refusals are expected for a differently-built companion;
                    // they are counted apart and reported to the owner instead
                    // of being treated as sync failures.
                    Refusal const refusal = ExplainRefusal(bot, quest);
                    if (report)
                    {
                        if (refusal.expected)
                            ++report->classRaceLocked;

                        AddSkipped(*report, quest, refusal.reason);
                    }

                    NotifyRefusal(owner, bot, quest, refusal);
                    return;
                }

                if (!bot->SatisfyQuestLog(false))
                {
                    // Say how full: "25/25" is actionable, "full" is not.
                    Refusal const refusal{ "my quest log is full (" +
                                               std::to_string(CountQuestLog(bot)) + "/" +
                                               std::to_string(MAX_QUEST_LOG_SIZE) + ")",
                                           false };
                    if (report)
                        AddSkipped(*report, quest, refusal.reason);

                    NotifyRefusal(owner, bot, quest, refusal);
                    return;
                }

                if (!bot->CanAddQuest(quest, false))
                {
                    // Bags full: free real junk and try once more before giving
                    // up, so a full companion stops silently skipping quests.
                    TidyInventory(owner, bot);

                    if (!bot->CanAddQuest(quest, false))
                    {
                        // Only claim "bags" when a bag is actually the problem.
                        // CanAddQuest fails for two reasons at most: the quest
                        // log (checked above) and room for the starter item it
                        // hands over. Collected quest items live in the quest log,
                        // so a full inventory does not stop those being gathered -
                        // blaming bags for them was simply wrong.
                        std::string reason;
                        uint32 const srcItem = quest->GetSrcItemId();
                        if (srcItem)
                        {
                            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(srcItem);
                            reason = std::string("my bags are full - I need one free slot for ") +
                                     (proto ? proto->Name1 : ("item " + std::to_string(srcItem)));
                        }
                        else
                        {
                            reason = "the server would not let me take it, and my bags are not the reason";
                        }

                        reason += " (" + std::to_string(bot->GetFreeInventorySpace()) + " free slot(s))";

                        Refusal const refusal{ reason, false };
                        if (report)
                            AddSkipped(*report, quest, refusal.reason);

                        NotifyRefusal(owner, bot, quest, refusal);
                        return;
                    }
                }
            }

            if (g_cfg.forceAccept)
                bot->AddQuest(quest, owner);
            else
                bot->AddQuestAndCheckCompletion(quest, owner);

            botStatus = bot->GetQuestStatus(questId);
            if (botStatus == QUEST_STATUS_NONE)
            {
                if (report)
                    AddSkipped(*report, quest, "core refused to add it");

                return;
            }

            if (report)
                ++report->added;

            // Remember when this landed: the next time the copy is missing, the
            // vanish line can say how long it survived and how full the log was.
            g_addedAt[{ bot->GetGUID().GetCounter(), questId }] = static_cast<uint32>(time(nullptr));
            g_addedCount[{ bot->GetGUID().GetCounter(), questId }] = CountQuestLog(bot);

            if (g_cfg.debug >= 1)
                LOG_INFO("server.loading", "[CompanionQuestSync] {} + '{}' ({})", bot->GetName(), quest->GetTitle(), questId);
        }

        if (g_cfg.syncCompletion && ownerStatus == QUEST_STATUS_COMPLETE &&
            (botStatus == QUEST_STATUS_INCOMPLETE || botStatus == QUEST_STATUS_FAILED))
        {
            // A delivery quest only rewards with the items in the companion's own
            // bags (CanRewardQuest re-checks them). Marking such a copy COMPLETE
            // is what leaves it dead - and it is also what made RepairStranded
            // Delivery fight this branch: reopen, complete, reopen, forever.
            // Left incomplete, the companion has an ordinary quest to finish and
            // its own AI keeps hunting the items - the behaviour we want.
            std::string const owed = DescribeMissingItems(bot, quest);
            if (!owed.empty())
            {
                // Log on change, not once per pass: "left it open" repeated every
                // 20 s buried everything else in the log.
                std::string& latch = g_openLatch[{ bot->GetGUID().GetCounter(), questId }];
                if (g_cfg.debug >= 2 && latch != owed)
                {
                    latch = owed;
                    LOG_INFO("server.loading", "[CompanionQuestSync] {} left '{}' ({}) open - it still owes {}",
                             bot->GetName(), quest->GetTitle(), questId, owed);
                }

                return;
            }


            // Only the status is touched: hand-in stays with whoever owns it (the
            // bot AI, or the existing mod-playerbots sync), so nothing here can
            // swallow a reward or a turn-in.
            bot->CompleteQuest(questId);

            if (report)
                ++report->completed;

            if (g_cfg.debug >= 1)
                LOG_INFO("server.loading", "[CompanionQuestSync] {} = '{}' ({}) completed", bot->GetName(), quest->GetTitle(), questId);
        }
    }

    /// Free bag space on a companion.
    ///
    /// Nothing in this fork does it for them: mod-playerbots' sell/repair
    /// "errands" are not part of a companion's strategy set, and the core has no
    /// sell API at module level (selling exists only as HandleSellItemOpcode -,
    /// a vendor interaction plus a packet). So the safe satellite behaviour is to
    /// destroy genuine junk: Poor quality only, never a quest item, never a quest
    /// starter, never an item one of the companion's own live quests still wants.
    /// Returns how many stacks were destroyed.
    uint32 TidyInventory(Player* owner, Player* bot)
    {
        if (!g_cfg.tidyInventory)
            return 0;

        // Soft margin: act while there are still a few slots left, not at the wall. A
        // companion that tidies at zero free slots destroys in a panic; one that tidies
        // at the margin mails a letter and carries on.
        if (bot->GetFreeInventorySpace() >= g_cfg.keepFreeSlots + g_cfg.bagMargin)
            return 0;

        // Item ids the companion's own live quests need: never destroy these.
        std::set<uint32> wanted;
        for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 const questId = bot->GetQuestSlotQuestId(slot);
            Quest const* quest = questId ? sObjectMgr->GetQuestTemplate(questId) : nullptr;
            if (!quest)
                continue;

            for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                if (quest->RequiredItemId[i])
                    wanted.insert(quest->RequiredItemId[i]);
        }

        // Cheapest first: when something has to go, go by vendor value, so the
        // least valuable junk disappears instead of whatever sat in the first bag
        // slot.
        std::vector<std::pair<uint32, uint32>> victims;   // (sell price, item id)
        std::vector<Item*> postHome;                      // white-tier, goes to the owner
        std::set<Item*> posted;                           // one pointer, one letter: the three
                                                          // passes below walk the same slots, and
                                                          // a white trade good qualifies in all of
                                                          // them - moving the same Item* twice is
                                                          // how mail corrupts and worlds crash
        auto offerPost = [&posted, &postHome](Item* item)
        {
            if (postHome.size() >= 12)
                return;

            if (posted.insert(item).second)
                postHome.push_back(item);
        };
        auto consider = [&wanted, &victims, &postHome, &offerPost](Item* item)
        {
            if (!item)
                return;

            ItemTemplate const* proto = item->GetTemplate();
            if (!proto || !proto->ItemId)
                return;

            // Never a container: a bag in the backpack looks like a disposable white item
            // and destroying one costs more slots than it frees.
            if (proto->Class == ITEM_CLASS_QUEST || proto->Class == ITEM_CLASS_CONTAINER || proto->StartQuest)
                return;                                  // quest item / quest starter
            if (wanted.count(proto->ItemId))
                return;                                  // a live quest still needs it

            // Everything below the keep-quality line goes home in a letter - whites
            // and greys alike, the player can vendor a grey for more than its slot is
            // worth. Only what a full letter cannot carry (the 12-item cap) falls
            // through, and a grey's slot is worth more than its copper, so the cap's
            // greys join the destroy ladder while the cap's whites wait for the next
            // letter.
            if (g_cfg.mailHome && proto->Quality >= ITEM_QUALITY_POOR &&
                proto->Bonding != BIND_WHEN_PICKED_UP)
            {
                if (postHome.size() < 12)                // the mail slot limit
                    offerPost(item);
                else if (proto->Quality < g_cfg.itemKeepQuality)
                    victims.emplace_back(proto->SellPrice, proto->ItemId);
                return;
            }

            // The keep-quality rule: at or above it, the item is safe. Default green, so
            // greys are disposable. This is the knob that stops "inventory is full" being
            // permanent - a companion that can never shed an item has no way out of a
            // full bag on its own.
            if (proto->Quality >= g_cfg.itemKeepQuality)
                return;

            victims.emplace_back(proto->SellPrice, proto->ItemId);
        };

        for (uint8 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
            consider(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i));

        for (uint8 i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; ++i)
            if (Bag* bag = bot->GetBagByPos(i))
                for (uint8 j = 0; j < bag->GetBagSize(); ++j)
                    consider(bot->GetItemByPos(i, j));

        // Nothing but white gear left? Down at the wall the companion cannot
        // pick up a quest item at all - it cannot even loot the Gold Dust it owes -
        // which is worse than losing a few copper of vendor trash. Ask playerbots
        // what it would vendor and bin exactly that; nothing it rates EQUIP,
        // QUEST, SKILL, DISENCHANT or AH is touched.
        if (g_cfg.tidyVendorJunk && bot->GetFreeInventorySpace() < g_cfg.keepFreeSlots + g_cfg.bagMargin)
        {
            if (PlayerbotAI* ai = GET_PLAYERBOT_AI(bot))
            {
                auto considerVendorJunk = [&wanted, &victims, &postHome, &offerPost, ai](Item* item)
                {
                    if (!item)
                        return;

                    ItemTemplate const* proto = item->GetTemplate();
                    if (!proto || !proto->ItemId)
                        return;

                    if (proto->Class == ITEM_CLASS_QUEST || proto->Class == ITEM_CLASS_CONTAINER || proto->StartQuest)
                        return;
                    if (wanted.count(proto->ItemId))
                        return;
                    if (g_cfg.mailHome && proto->Quality >= ITEM_QUALITY_NORMAL &&
                        proto->Bonding != BIND_WHEN_PICKED_UP)
                    {
                        if (postHome.size() < 12)
                            offerPost(item);
                        return;
                    }
                    if (proto->Quality >= g_cfg.itemKeepQuality)
                        return;                       // green and better are never binned

                    ItemUsage const usage = ai->GetAiObjectContext()
                                               ->GetValue<ItemUsage>("item usage", proto->ItemId)
                                               ->Get();
                    if (usage != ITEM_USAGE_VENDOR)
                        return;

                    victims.emplace_back(proto->SellPrice, proto->ItemId);
                };

                for (uint8 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
                    considerVendorJunk(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i));

                for (uint8 i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; ++i)
                    if (Bag* bag = bot->GetBagByPos(i))
                        for (uint8 j = 0; j < bag->GetBagSize(); ++j)
                            considerVendorJunk(bot->GetItemByPos(i, j));
            }
        }

        // Still wedged? Spare consumables and materials are worth a few copper each and
        // the companion does not craft - down at the wall one of them is worth more as
        // an empty slot than as an item. Cheapest first, surplus stacks only, and never
        // the hearthstone, gear or anything a quest needs.
        if (g_cfg.tidySurplus && bot->GetFreeInventorySpace() < g_cfg.keepFreeSlots + g_cfg.bagMargin)
        {
            auto considerSurplus = [&wanted, &victims, &postHome, &offerPost](Item* item)
            {
                if (!item)
                    return;

                ItemTemplate const* proto = item->GetTemplate();
                if (!proto || !proto->ItemId)
                    return;

                if (proto->Class != ITEM_CLASS_CONSUMABLE &&
                    proto->Class != ITEM_CLASS_TRADE_GOODS &&
                    proto->Class != ITEM_CLASS_RECIPE)
                    return;

                if (proto->StartQuest || wanted.count(proto->ItemId))
                    return;

                if (g_cfg.mailHome && proto->Quality >= ITEM_QUALITY_NORMAL &&
                    proto->Bonding != BIND_WHEN_PICKED_UP)
                {
                    if (postHome.size() < 12)
                        offerPost(item);
                    return;
                }

                if (proto->Quality >= g_cfg.itemKeepQuality)
                    return;

                if (item->GetCount() < 2 && proto->Class != ITEM_CLASS_RECIPE)
                    return;                       // keep the last of anything useful

                victims.emplace_back(proto->SellPrice, proto->ItemId);
            };

            for (uint8 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
                considerSurplus(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i));

            for (uint8 i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; ++i)
                if (Bag* bag = bot->GetBagByPos(i))
                    for (uint8 j = 0; j < bag->GetBagSize(); ++j)
                        considerSurplus(bot->GetItemByPos(i, j));
        }

        std::sort(victims.begin(), victims.end(),
                  [](std::pair<uint32, uint32> const& a, std::pair<uint32, uint32> const& b)
                  { return a.first < b.first; });

        // The letter goes first and often enough to matter: posting home frees the same
        // slots destroying would, and the cooldown keeps the mailbox from filling with
        // one letter per pass.
        if (!postHome.empty() && owner)
        {
            uint32& lastAt = g_mailAt[bot->GetGUID().GetCounter()];
            uint32 const nowTs = static_cast<uint32>(time(nullptr));

            if (!lastAt || nowTs - lastAt >= Timing::MailHome)
            {
                MailHome(owner, bot, postHome);
                lastAt = nowTs;

                SayToOwner(bot, owner,
                           "I posted what I could not carry home to you - the letter is in your mailbox.");
            }
        }

        uint32 destroyed = 0;
        for (auto const& victim : victims)
        {
            if (bot->GetFreeInventorySpace() >= g_cfg.keepFreeSlots)
                break;

            uint32 const entry = victim.second;
            uint32 const count = bot->GetItemCount(entry);
            if (!count)
                continue;

            bot->DestroyItemCount(entry, count, true);
            ++destroyed;
            if (owner)
                g_session[owner->GetGUID().GetCounter()].destroyed += count;

            if (g_cfg.debug >= 1)
                LOG_INFO("server.loading", "[CompanionQuestSync] {} destroyed {}x item {} to free bag space",
                         bot->GetName(), count, entry);
        }

        return destroyed;
    }

    /// Resolve the owner's companion and check it is a live bot we may touch.
    /// Returns nullptr (with what we learned recorded in `report`) if not.
    Player* ResolveCompanion(Player* owner, SyncReport& report, uint32 botGuid)
    {
        if (!botGuid)
            return nullptr;

        report.hasCompanion = true;

        Player* bot = FindCompanionPlayer(botGuid);
        if (!bot || !bot->IsInWorld())
            return nullptr;

        // Safety rail: only ever act on a BOT session. A hand-edited or stale
        // binding row must not be able to level or re-quest a real player's
        // character, and self-binding must do nothing at all.
        if (bot->GetGUID() == owner->GetGUID())
            return nullptr;

        if (!bot->GetSession() || !bot->GetSession()->IsBot())
        {
            if (g_cfg.debug >= 1)
                LOG_ERROR("server.loading", "[CompanionQuestSync] binding {} -> {} does not point at a bot session; refusing to touch it",
                          owner->GetName(), bot->GetName());

            return nullptr;
        }

        report.companionOnline = true;
        report.ownerName = owner->GetName();
        report.botName = bot->GetName();
        report.botFreeSlots = int32(bot->GetFreeInventorySpace());
        report.ownerLevel = owner->GetLevel();
        report.botLevel = bot->GetLevel();

        // Grouping gates QUEST SYNCING, not upkeep. It used to return here, which meant a
        // bound companion that had slipped out of the group got no bags, no gear and no
        // level alignment at all - silently, because nothing was logged. The callers now
        // decide what a missing group suppresses.
        if (g_cfg.requireGrouped)
        {
            Group* group = owner->GetGroup();
            report.grouped = group && group->IsMember(bot->GetGUID());
        }

        return bot;
    }

    /// Copy the owner's quest log onto the companion. Returns what happened so
    /// both the command output and the debug log can use it.
    SyncReport SyncCompanionFor(Player* owner, uint32 botGuid, std::set<uint64>& marks)
    {
        // Measured, not guessed: Debug >= 2 logs how long a whole pass takes, which is
        // the number that matters when asking what this module costs the server.
        g_missingCache.clear();     // bag contents can change between passes

        auto const startedAt = std::chrono::steady_clock::now();

        // Section timings, so "it feels slow" can be answered with numbers instead of a
        // guess: which part of a pass costs the microseconds, and how many grid searches
        // the gather step spent finding objects.
        struct PassProfile
        {
            uint64 gather = 0;
            uint64 handIn = 0;
        } profile;

        g_gridSearches = 0;

        auto lastSection = startedAt;
        auto lap = [&lastSection]() -> uint64
        {
            auto const now = std::chrono::steady_clock::now();
            uint64 const us = uint64(std::chrono::duration_cast<std::chrono::microseconds>(now - lastSection).count());
            lastSection = now;
            return us;
        };

        struct PassTimer
        {
            std::chrono::steady_clock::time_point start;
            Player* owner;
            PassProfile* profile;

            ~PassTimer()
            {
                if (g_cfg.debug < 3)
                    return;

                auto const us = static_cast<uint64>(std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - start).count());

                LOG_INFO("server.loading", "[CompanionQuestSync] pass for {} took {} us",
                         owner ? owner->GetName() : "?", us);

                if (g_cfg.debug < 3)
                    return;

                uint64 const measured = profile->gather + profile->handIn;

                LOG_INFO("server.loading",
                         "[CompanionQuestSync]   profile {}: gather {} us, hand-in {} us, rest {} us, {} game-object search(es)",
                         owner ? owner->GetName() : "?", profile->gather, profile->handIn,
                         us > measured ? us - measured : 0, g_gridSearches);
            }
        } timer{ startedAt, owner, &profile };

        SyncReport report;

        Player* bot = ResolveCompanion(owner, report, botGuid);
        if (!bot)
        {
            // Say which of the three it is. One combined message cost an evening of guessing
            // when the log said "could not be resolved" and the database said "online".
            Player* body = FindCompanionPlayer(botGuid);

            char const* why = !body                                ? "no player object for that guid"
                            : !body->IsInWorld()                   ? "logged in but not in the world"
                            : (!body->GetSession() || !body->GetSession()->IsBot())
                                                                   ? "not a bot session"
                            : "resolved but rejected";

            if (g_cfg.debug >= 1)
                LOG_INFO("server.loading",
                         "[CompanionQuestSync] pass for {} skipped: companion {} - {}",
                         owner->GetName(), botGuid, why);

            // A companion that is logged in but has no map cannot be helped from here, and
            // the player has no way to tell it apart from "the module is broken": say the one
            // thing that fixes it, once every ten minutes.
            if (body && !body->IsInWorld())
            {
                uint64 const key = (uint64(owner->GetGUID().GetCounter()) << 32) | botGuid;
                uint32 const now = static_cast<uint32>(time(nullptr));
                auto const said = g_stuckNoticeAt.find(key);

                if (said == g_stuckNoticeAt.end() || now - said->second > 600)
                {
                    g_stuckNoticeAt[key] = now;

                    ChatHandler(owner->GetSession()).PSendSysMessage(
                        "|cff20ff20Companion quest sync|r: {} is bound but is not in the world - "
                        "summon it and we can pick up where we left off.", body->GetName());
                }
            }

            return report;
        }

        // Death used to be silent: the player found the corpse only by looking. Say it
        // once per death - the set latches at the first pass of the death and clears
        // when the body breathes again - never once per 30 s.
        if (!bot->IsAlive())
        {
            if (g_deadNoticeAt.insert(botGuid).second)
                SayToOwner(bot, owner, "I have fallen where I stood - I will need a revive, "
                                       "or summon me once you are somewhere safe.", 0);
        }
        else
            g_deadNoticeAt.erase(botGuid);

        // Combat defers only the LEVEL change. Quest copying used to be blocked
        // here outright, which is why a companion refused to pick up a quest
        // mid-fight and the owner had to share it by hand - questing is mostly
        // combat. Adding a quest or marking one complete is safe in combat.
        bool const inCombat = g_cfg.skipInCombat && (owner->IsInCombat() || bot->IsInCombat());

        // Snapshot the owner's log once: it is used for the sync itself, for the
        // "drop extras" comparison and to prune stale notifications.
        std::vector<uint32> ownerQuests;
        ownerQuests.reserve(MAX_QUEST_LOG_SIZE);
        for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 const questId = owner->GetQuestSlotQuestId(slot);
            if (questId)
                ownerQuests.push_back(questId);
        }

        report.ownerQuestCount = uint32(ownerQuests.size());
        report.botQuestCount = CountQuestLog(bot);

        uint32 const ownerGuid = owner->GetGUID().GetCounter();
        uint64 const digest = QuestLogDigest(owner) ^ (QuestLogDigest(bot) * 1099511628211ull);

        // Hand-ins run before the digest short-circuit: a companion standing too far
        // from its giver changes nothing in either log, so the digest keeps matching and
        // the step would never be retried - the quest would sit COMPLETE until something
        // else moved. The routine carries its own throttles, so repeating it is cheap.
        // Gathering goes before the hand-in: a ground-object quest can only be walked and
        // looted, and there is nothing to deliver until it is done.
        {
            std::string gathered;

            if (GatherFromGround(owner, bot, gathered) && g_cfg.debug >= 2)
                LOG_INFO("server.loading", "[CompanionQuestSync] gathering for {}", gathered);
        }

        if (g_cfg.catchUpGroundItems)
            AutoCatchUpGround(owner, bot);

        profile.gather = lap();

        DeliverHandIns(owner, bot);
        RepairStrandedDelivery(owner, bot, report);   // retry-worthy: belongs above the memo

        profile.handIn = lap();

        // Extras are cleared before the memo, not after. A pass that ends "clean" with an
        // unchanged digest short-circuits every later pass, so a drop that waits out a
        // grace period never got a second look: the quest stayed on the companion forever
        // while the heartbeat cheerfully reported "0 dropped".

        // A copy the companion failed on its own - an escort that died, a script that
        // timed out - can never be finished, and the sweep would otherwise keep it
        // forever while the player wonders why the log never matches. Say it once, and
        // drop it only if the operator asked for that.
        {
            for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            {
                uint32 const questId = bot->GetQuestSlotQuestId(slot);
                if (!questId || bot->GetQuestStatus(questId) != QUEST_STATUS_FAILED)
                    continue;

                ++report.failed;

                Quest const* failedQuest = sObjectMgr->GetQuestTemplate(questId);
                if (!failedQuest)
                    continue;

                std::pair<uint32, uint32> const key{ bot->GetGUID().GetCounter(), questId };
                if (g_failedSeen.insert(key).second)
                    SayToOwner(bot, owner, "I made a mess of '" + failedQuest->GetTitle() + "' - it went wrong on "
                                          "my own and I cannot pick it up again. Drop it, or take it with me when "
                                          "you have a moment?", 120);

                if (!g_cfg.dropFailedBotQuests)
                    continue;

                bot->SetQuestSlot(slot, 0);
                bot->TakeQuestSourceItem(questId, false);
                bot->RemoveActiveQuest(questId, false);
                ++report.dropped;

                LOG_INFO("server.loading", "[CompanionQuestSync] {} dropped failed quest {} ('{}')",
                         bot->GetName(), questId, failedQuest->GetTitle());
            }
        }

        // Follow the owner out of a quest, not just into one.
        //
        // The old version of this only asked whether the owner held the quest right
        // now, so a copy the owner had already handed in looked "extra" and was thrown
        // away - the opposite of the intent, and enough to strand a companion mid-chain.
        // A quest counts as abandoned only when the owner neither holds it nor has been
        // rewarded for it, and only after a grace period, so a quest the owner accepted
        // a moment ago is never yanked out from under the accept hook.
        if (g_cfg.dropExtraBotQuests)
        {
            uint32 const nowTs = static_cast<uint32>(time(nullptr));
            uint32 const botGuid = bot->GetGUID().GetCounter();

            std::set<uint32> stillExtra;

            for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            {
                uint32 const questId = bot->GetQuestSlotQuestId(slot);
                if (!questId || bot->IsQuestRewarded(questId))
                    continue;

                if (std::find(ownerQuests.begin(), ownerQuests.end(), questId) != ownerQuests.end())
                    continue;

                // Already handed in by the owner. A copy the companion can still
                // progress by itself (it owes items) is catching up - leave it alone,
                // that is the whole point of the module. But a copy with nothing to
                // gather has no credit it can earn on its own: the owner's COMPLETE
                // window it could have synced from is transient, and a quest the
                // player finishes and turns in inside one pass (a feast-table
                // minigame, an instant hand-in) leaves the companion standing at
                // INCOMPLETE forever, narrating it every cycle. The durable truth is
                // the owner's reward; with the owed guard showing nothing skipped,
                // completing the copy now is honest - the work was done together at
                // the same table, and the hand-in flow takes it from here.
                if (owner->IsQuestRewarded(questId))
                {
                    Quest const* rewarded = sObjectMgr->GetQuestTemplate(questId);
                    if (!rewarded || !DescribeMissingItems(bot, rewarded).empty())
                        continue;      // still gathering its own copy: catching up

                    if (bot->GetQuestStatus(questId) == QUEST_STATUS_INCOMPLETE)
                    {
                        bot->CompleteQuest(questId);
                        ++report.completed;

                        if (g_cfg.debug >= 1)
                            LOG_INFO("server.loading",
                                     "[CompanionQuestSync] {} = '{}' ({}) completed alongside the owner's reward",
                                     bot->GetName(), rewarded->GetTitle(), questId);
                    }
                    continue;
                }

                Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
                if (!quest)
                    continue;

                // A quest the owner could never take was not mirrored from them, so it is
                // not theirs to abandon. Test the masks against the owner, not for their
                // presence: AllowableRaces is set on almost every quest (1101 is simply
                // "Alliance"), so treating any mask as a lock exempted the entire game and
                // no extra was ever dropped.
                if (!OwnerFitsQuestMask(owner, quest))
                    continue;

                std::pair<uint32, uint32> const key{ botGuid, questId };
                uint32& since = g_extraSince[key];
                if (!since)
                    since = nowTs;

                if (nowTs - since < g_cfg.dropExtraGrace)
                {
                    stillExtra.insert(questId);
                    continue;                                   // wait out the grace period
                }

                bot->SetQuestSlot(slot, 0);
                bot->TakeQuestSourceItem(questId, false);
                bot->RemoveActiveQuest(questId, false);
                ++report.dropped;

                g_extraSince.erase(key);

                SayToOwner(bot, owner, "You let '" + quest->GetTitle() + "' go, so I laid mine down too - "
                                      "it made no sense to keep gathering for it on my own.", 120);

                if (g_cfg.debug >= 1)
                    LOG_INFO("server.loading", "[CompanionQuestSync] {} - dropped quest {} ('{}'), owner no longer has it",
                             bot->GetName(), questId, quest->GetTitle());
            }

            // Forget the clock for copies that came back (owner re-accepted the quest).
            for (auto itr = g_extraSince.begin(); itr != g_extraSince.end(); )
            {
                if (itr->first.first == botGuid && !stillExtra.count(itr->first.second))
                    itr = g_extraSince.erase(itr);
                else
                    ++itr;
            }
        }


        // Upkeep runs before the memo, not after it. Bags and gear are not quest-log work:
        // once a companion's logs settled the digest short-circuit returned above them, so a
        // body created from the pool stayed naked forever while every pass reported "in sync".
        EnsureBags(owner, bot);
        EnsureStartingGear(owner, bot);

        if (g_cfg.requireGrouped && !report.grouped)
        {
            if (g_cfg.debug >= 2)
                LOG_INFO("server.loading",
                         "[CompanionQuestSync] {} is not in {}'s group - upkeep only this pass",
                         bot->GetName(), owner->GetName());

            return report;
        }

#ifdef MOD_PLAYERBOTS
        // A tool that failed to land once must not wait for a "dirty" pass: this is a
        // keep-trying guarantee, not change-driven work, so it runs before the memo
        // early-return the same way the upkeep it belongs to does. The gather catch-up
        // rides the same slot - what the land asks does not wait for a dirty pass either.
        EnsureProfessionTools(bot);
        CatchUpGatherSkill(owner, bot);
        // the recipes the current ceiling unlocks, self-healing: a body whose teach
        // predated the recipe pass, or whose skill rose without a raise, catches up
        // here rather than waiting for a button - Produce reads its spellbook
        uint16 const recipeSkills[2] = { ClassProfessionPair(bot->getClass()).first,
                                         ClassProfessionPair(bot->getClass()).second };
        uint32 const recipeCap = ProfessionCap(bot->GetLevel());
        TeachCraftRecipes(bot, recipeSkills[0], recipeCap);
        TeachCraftRecipes(bot, recipeSkills[1], recipeCap);
        RestockNearVendor(owner, bot);   // near a trade or repair NPC: restock, mend, quietly
#endif

        // Keyed by companion: one bot's unchanged logs must not suppress another's pass.
        auto const memo = g_passMemo.find(botGuid);
        if (memo != g_passMemo.end() && memo->second.clean && memo->second.digest == digest)
        {
            // Nothing changed and the last pass had nothing left over: the same
            // work would reach the same result, so skip the walk entirely.
            return report;
        }

        if (!inCombat)
            AlignLevel(owner, bot, report);

        // Keep a couple of slots free so quest items can be picked up and
        // quest source items stored, instead of discovering the shortage only
        // when an accept is refused.
        TryEquipUpgrades(owner, bot);                  // better gear goes on as it arrives
        TrySellJunk(owner, bot);                       // earn coin first
        NotifyJunk(owner, bot, TidyInventory(owner, bot));    // mail home, then destroy only what is left

        // A level change is the one thing here that other modules react to
        // (mod-playerbots re-evaluates spells/gear, mod-wowlegends' path rules
        // can veto the level outright). Let them settle for one interval before
        // stuffing quests in, so a levelled companion is never handed a log it
        // was about to be re-geared for.
        if (report.levelFrom != 0)
        {
            if (g_cfg.debug >= 1)
                LOG_INFO("server.loading", "[CompanionQuestSync] {} levelled; quest sync deferred to the next pass", bot->GetName());

            g_passMemo[botGuid] = { digest, false };   // levels moved: always re-run
            return report;
        }

        NudgeForHelp(owner, bot);
        ReportPosition(owner, bot);
        MarkOwnErrands(owner, bot, marks);

        for (uint32 const questId : ownerQuests)
        {
            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest)
                continue;

            SyncQuestToBot(owner, bot, quest, &report);
        }

        // Forget notifications whose quest left the owner's log, so re-taking it
        // later reports again.
        std::set<uint32>& notified = g_notified[bot->GetGUID().GetCounter()];
        for (auto itr = notified.begin(); itr != notified.end();)
        {
            if (std::find(ownerQuests.begin(), ownerQuests.end(), *itr) != ownerQuests.end())
                ++itr;
            else
                itr = notified.erase(itr);
        }

        report.botQuestCount = CountQuestLog(bot);

        FlushNotices(owner, bot);
        FlushWhisperQueue(owner, bot);

        // The quest's disappearance is announced by the payload itself: the pin sync
        // below resends in the same pass, and the map clears the moment it lands - a
        // whisper repeating the removal afterwards only arrived after the fact ("that
        // pin can go" under a map that had already let it go). The map is the message.

        // Authoritative pin list, so a stale marker cannot survive a desync.

        bool const clean = report.added == 0 && report.completed == 0 && report.dropped == 0 &&
                           report.levelFrom == 0 && report.skipped.empty();
        g_passMemo[botGuid] = { QuestLogDigest(owner) ^ (QuestLogDigest(bot) * 1099511628211ull), clean };
        return report;
    }

    /// One line that describes the companion's pins exactly, so the addon can
    /// rebuild its map instead of guessing from the hints it happened to hear.
    ///
    ///     Pin sync 3: 43,65,h=Kobold Candles|64,59,g=Gold Dust Exchange|...
    ///
    /// h = it can hand that quest in now, g = it still has work there. A pin that
    /// is not in this line is not real, which is what lets the map clean up after a
    /// desync: a whisper lost to a rate limit, a logout, or an abandoned quest.
    /// The whole map, for every companion bound to this owner.
    ///
    /// One payload per owner, not one per companion: the addon treats it as the whole
    /// truth and deletes whatever it does not mention, so two companions sending their
    /// own payloads would erase each other on every pass.
    ///
    /// Entries sharing a coordinate are merged into one marker with both quest names,
    /// because a hand-in both companions need is one place to walk to, not two pins on
    /// the same tile.
    /// A companion name as the pin grammar can survive it.
    ///
    /// The addon splits entries on `,` and `|` and the name on `@`, so a name carrying
    /// any of those would take the whole payload apart. Truncated too: four companions
    /// on one marker must still leave a label a player can read.
    std::string SafePinName(std::string const& name)
    {
        std::string out;

        for (char const c : name)
        {
            // The separators would split the payload grammar; control characters would
            // split a line. Everything else passes - a name is a name.
            if (c == ',' || c == '|' || c == '@' || c == '=')
                continue;

            if (static_cast<unsigned char>(c) < 32)
                continue;

            out += c;
        }

        if (out.size() > 12)
            out = out.substr(0, 12);

        return out;
    }

    /// How far each companion still is from completing a quest, as have/need per
    /// objective ("3/8" - the quest log's own language). Only objectives with work left
    /// speak: a finished objective says nothing, and a finished quest says nothing at all.
    std::string OwedText(Player* bot, Quest const* quest)
    {
        std::string out;
        for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
        {
            int32 const entry = quest->RequiredNpcOrGo[i];
            uint32 const need = quest->RequiredNpcOrGoCount[i];
            if (!entry || !need)
                continue;

            uint32 const have = bot->GetReqKillOrCastCurrentCount(quest->GetQuestId(), entry);
            if (have >= need)
                continue;
            if (!out.empty())
                out += ";";
            out += std::to_string(std::min<uint32>(have, need)) + "/" + std::to_string(need);
        }
        for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
        {
            uint32 const item = quest->RequiredItemId[i];
            uint32 const need = quest->RequiredItemCount[i];
            if (!item || !need)
                continue;

            uint32 const have = bot->GetItemCount(item);
            if (have >= need)
                continue;
            if (!out.empty())
                out += ";";
            out += std::to_string(std::min<uint32>(have, need)) + "/" + std::to_string(need);
        }
        return out;
    }

    std::string BuildPinLine(Player* owner)
    {
        struct Entry
        {
            std::string numbers;
            char style;
            std::string title;
            std::string zone;   // the giver's zone, named by the SERVER: the client's own
                                // location is not the pin's location, and guessing it was
                                // how pins ended up stamped with the wrong map
            uint32 extra = 0;   // companions folded into this marker beyond the second
        };

        std::vector<Entry> entries;

        std::vector<uint32> bots;
        if (!FindCompanions(owner->GetGUID().GetCounter(), bots))
            return std::string();

        for (uint32 const botGuid : bots)
        {
            Player* bot = FindCompanionPlayer(botGuid);
            if (!bot || !bot->IsInWorld())
                continue;

            for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            {
                uint32 const questId = bot->GetQuestSlotQuestId(slot);
                if (!questId)
                    continue;

                Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
                if (!quest)
                    continue;

                // Two glyph kinds, both with server-computed coordinates and a zone
                // the server names - the client never guesses location:
                //   h = ready to hand in, at the giver (a POI circle cannot say this)
                //   g = work left, at the objective area - necessary because the client
                //       only draws native POI circles for quests in the PLAYER's own log;
                //       the pushed packets for companion-held quests display nothing,
                //       which the first session without g glyphs proved in the field.
                std::string coords;
                std::string zoneName;
                std::vector<std::pair<std::string, std::string>> workLeftSpots;
                char style = 'g';

                if (bot->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE)
                {
                    EnderSpot const& spot = FindQuestEnder(bot, questId);

                    // The giver pin exists wherever the quest is: the coordinates are the
                    // giver's own, the zone is the giver's own, and the client shows the
                    // pin on that zone's map. The old same-zone guard - against the
                    // BOT's zone - refused the pin whenever the party was one zone away,
                    // which is exactly when the player needs pointing.
                    if (spot.found && spot.map == bot->GetMapId())
                    {
                        uint32 const zone = ZoneAtHeight(spot.map, 1, spot.x, spot.y);
                        int32 const useZone = int32(zone ? zone : spot.zone);

                        float gx = spot.x, gy = spot.y;
                        if (WmaConvert(gx, gy, useZone) && gx >= 0.0f && gx <= 100.0f && gy >= 0.0f && gy <= 100.0f)
                        {
                            coords = "map " + std::to_string(uint32(gx)) + ", " + std::to_string(uint32(gy));
                            zoneName = ZoneName(uint32(useZone));
                            style = 'h';
                        }
                    }
                }
                else if (!owner->IsQuestRewarded(questId))
                {
                    // Same rule the POI push used: the area is worth drawing while the
                    // owner has not been credited for the quest themselves. The zone is
                    // the QUEST's area, named from the POI data - never the bot's
                    // transient position. The loot table leads (the work is where the
                    // required item drops); the POI vector is the fallback. A quest owing
                    // work in several areas draws one pin per area, each on its own map.
                    if (!LootSpots(bot, quest, workLeftSpots))
                        PoiPins(bot, quest, workLeftSpots);
                }

                // The pin list for this quest: one entry for a ready hand-in (the
                // giver), or one per objective area for work still owed.
                std::vector<std::tuple<std::string, char, std::string, std::string>> spots;
                if (!coords.empty())
                    spots.push_back({ coords, style, "", zoneName });
                else
                    for (auto const& spot : workLeftSpots)
                        spots.push_back({ spot.first, 'g', "", spot.second });

                for (auto const& [coords, style, _unused, zoneName] : spots)
                {
                    // "map 43, 65" -> "43,65"
                    std::string numbers = coords.substr(4);
                    numbers.erase(std::remove_if(numbers.begin(), numbers.end(),
                                                 [](char c) { return c == ' '; }), numbers.end());

                    if (numbers.empty())
                        continue;

                    std::string title = quest->GetTitle();
                    if (title.size() > 22)
                        title = title.substr(0, 20) + "..";

                    // a title cannot carry the separators, or the addon would split it
                    std::replace(title.begin(), title.end(), '|', '/');
                    std::replace(title.begin(), title.end(), '=', ':');

                    if (MultiCompanionSpeech(bot))
                        title += "@" + SafePinName(bot->GetName());

                    // what this companion still owes, as the quest log counts it -
                    // "3/8" after the name, nothing when the quest is ready to hand in
                    if (bot->GetQuestStatus(questId) != QUEST_STATUS_COMPLETE)
                    {
                        std::string owed = OwedText(bot, quest);
                        if (!owed.empty())
                            title += ":" + owed;
                    }

                    bool merged = false;
                    for (Entry& existing : entries)
                    {
                        if (existing.numbers != numbers || existing.zone != zoneName)
                            continue;

                        // Every owner reads on the pin: the client gives the names their
                        // own line under the quest, so a full party of four fits where the
                        // old one-line marker forced counting past the second name.
                        existing.title += " + " + title;

                        merged = true;
                        break;
                    }

                    if (!merged)
                        entries.push_back({ numbers, style, title, zoneName, 0 });
                }
            }
        }

        std::string line;

        for (Entry const& entry : entries)
        {
            if (!line.empty())
                line += "|";

            line += entry.numbers + "," + entry.style + "=" + entry.title;

            if (!entry.zone.empty())
                line += ",z=" + entry.zone;

            if (entry.extra)
                line += " +" + std::to_string(entry.extra);
        }

        return line;
    }

    /// Send the pin line when it changes, and again every ten minutes in case one
    /// was missed. A whisper, because that is the only channel the addon can read.
    /// Deliver one protocol line to the client that can read it.
    ///
    /// A system message rather than a whisper. Whispers need a speaker, and the payloads are
    /// needed most when no companion is out - the roster, the pool picker, the map. With no
    /// companion in the world there was nobody to whisper, so the client silently got
    /// nothing and the window showed an empty pool. The addon hides these lines from the
    /// chat frame by their CSYNC prefix, so the player never sees them.
    void SendProtocolLine(Player* owner, std::string const& line)
    {
        if (!owner || !owner->GetSession())
            return;

        ChatHandler handler(owner->GetSession());
        handler.SendSysMessage(("CSYNC|" + line).c_str());
    }

    /// Who speaks the protocol lines (pin and companion payloads).
    ///
    /// Not the sync resolver: that one refuses a companion which is not grouped, which is
    /// right for reconciling quests and wrong for sending a whisper. With RequireGrouped
    /// on and a companion standing outside the party, every payload was computed and then
    /// silently dropped - the map stayed empty and the addon showed nothing, with no error
    /// anywhere. Any bound companion that has a session can speak.
    Player* ResolveProtocolSpeaker(Player* owner)
    {
        std::vector<uint32> bots;
        if (!FindCompanions(owner->GetGUID().GetCounter(), bots))
            return nullptr;

        for (uint32 const botGuid : bots)
        {
            Player* bot = FindCompanionPlayer(botGuid);
            if (bot && bot->IsInWorld() && bot->GetSession())
                return bot;
        }

        return nullptr;
    }

    /// The presence heartbeat. Payloads are coalesced - a quiet stretch sends nothing -
    /// and the client reads silence as "the module stopped answering" after its patience
    /// window, which is exactly what it should read. So the module must speak inside that
    /// window even when nothing changed: the login Hello becomes a per-owner heartbeat,
    /// throttled to once a minute, hidden by the client's CSYNC filter like every
    /// payload. With it, "not answering" finally means what it says.
    void SendHeartbeat(Player* owner)
    {
        if (!owner || !owner->GetSession())
            return;

        uint32 const ownerGuid = owner->GetGUID().GetCounter();
        uint32 const now = static_cast<uint32>(time(nullptr));
        uint32& last = g_heartbeatAt[ownerGuid];

        if (last && now - last < 60)
            return;

        last = now;
        SendProtocolLine(owner, "Hello " + std::to_string(g_cfg.intervalSeconds));

        // The heartbeat is a state publish, not just presence: a client that just
        // /reloaded is an empty renderer, and coalescing would never re-send an
        // unchanged roster to it. Once a minute the module republishes the full
        // truth - companions and pins - so the window is never waiting on a change
        // that never comes.

        if (g_cfg.debug >= 1)
            LOG_INFO("server.loading", "[CompanionQuestSync] heartbeat -> {}: Hello {}",
                     owner->GetName(), g_cfg.intervalSeconds);

        SendCompSync(owner, true);
        SendPinSync(owner, true);
    }

    void SendPinSync(Player* owner, bool force = false)
    {
        if (!g_cfg.pinSync || !owner)
            return;

        uint32 const ownerGuid = owner->GetGUID().GetCounter();
        std::string const payload = BuildPinLine(owner);
        SyncReport pinReport;   // only used to resolve a speaker

        std::string& last = g_lastPinSync[ownerGuid];
        uint32 const now = static_cast<uint32>(time(nullptr));
        uint32 const sentAt = g_pinSyncAt[ownerGuid];

        if (!force && payload == last && (sentAt && now - sentAt < g_cfg.pinSyncSeconds))
            return;

        // Chunk the payload. A companion carrying more than about five pin-worthy
        // quests overflowed the whisper length limit, and the old fixed cap of four
        // dropped the rest silently - which is how a quest could sit in the companion's
        // log with no pin on the map. "Pin sync 1/3: ..." tells the addon to hold the
        // parts until the set is complete; a single part stays "Pin sync 4: ...".
        std::vector<std::string> chunks;
        std::string current;
        size_t start = 0;
        uint32 entries = 0;

        while (start <= payload.size())
        {
            size_t const bar = payload.find('|', start);
            std::string entry = payload.substr(start, bar == std::string::npos ? std::string::npos : bar - start);

            if (!current.empty() && current.size() + entry.size() + 1 > 150)
            {
                chunks.push_back(current);
                current.clear();
            }

            if (!current.empty())
                current += "|";

            current += entry;
            ++entries;

            if (bar == std::string::npos)
                break;

            start = bar + 1;
        }

        if (!current.empty())
            chunks.push_back(current);

        if (chunks.empty())
            chunks.push_back(std::string());

        for (size_t i = 0; i < chunks.size(); ++i)
        {
            std::string line = "Pin sync ";

            if (chunks.size() > 1)
                line += std::to_string(i + 1) + "/" + std::to_string(chunks.size()) + ": ";
            else
                line += std::to_string(entries) + ": ";

            line += chunks[i];

            // Grey and quiet: the addon needs the line, the player does not need to read
            // it. Colour codes are stripped by CompanionPins before it parses.
            SendProtocolLine(owner, line);
        }

        last = payload;
        g_pinSyncAt[ownerGuid] = now;

        if (g_cfg.debug >= 2)
        {
            // The payload itself at debug 2: it is the only record of what the client was
            // told, and the multi-companion test asserts on the @Name suffixes.
            LOG_INFO("server.loading", "[CompanionQuestSync] pin sync -> {}: {} part(s), {} entries: {}",
                     owner->GetName(), uint32(chunks.size()), entries, payload);
        }
        else if (g_cfg.debug >= 1)
        {
            LOG_INFO("server.loading", "[CompanionQuestSync] pin sync -> {}: {} part(s), {} entries",
                     owner->GetName(), uint32(chunks.size()), entries);
        }
    }

    void PrintReport(ChatHandler* handler, Player* owner, SyncReport const& report, bool manual)
    {
        if (!report.hasCompanion)
        {
            if (manual)
                handler->PSendSysMessage("|cffff8000Companion quest sync|r: you have no companion bound. Use |cffffff00.companion create <race> <class> <name>|r first.");
            return;
        }

        if (!report.companionOnline)
        {
            if (manual)
                handler->PSendSysMessage("|cffff8000Companion quest sync|r: your companion is not online. Use |cffffff00.companion summon|r.");
            return;
        }

        if (g_cfg.requireGrouped && !report.grouped)
        {
            if (manual)
                handler->PSendSysMessage("|cffff8000Companion quest sync|r: your companion is not in your group - quest copying "
                                         "is skipped (hand-ins and bag upkeep still run).");
            return;
        }

        if (!manual)
            return;

        handler->PSendSysMessage("|cff20ff20Companion quest sync|r: {} quest(s) added, {} marked complete{}.",
                                 report.added, report.completed,
                                 report.levelFrom ? std::string(", level ") + std::to_string(report.levelFrom) + " -> " + std::to_string(report.levelTo) : "");

        if (report.levelFrom)
        {
            handler->PSendSysMessage("  level changed this pass - quests are copied on the next pass, once the companion has settled.");
        }

        if (report.classRaceLocked)
        {
            handler->PSendSysMessage("  |cffffd200{} class/race-locked quest(s)|r skipped - expected when the companion is a different class or race.",
                                     report.classRaceLocked);
        }

        for (std::string const& line : report.skipped)
            handler->PSendSysMessage("  |cffff6060skipped|r {}", line);
    }

    /// One line for the heartbeat window: who, what level, how many quests, and
    /// whether the two logs currently line up.
    std::string DescribeReport(SyncReport const& report)
    {
        std::string line = report.ownerName + " (lvl " + std::to_string(report.ownerLevel) + ") "
                         + std::to_string(report.ownerQuestCount) + "q / "
                         + report.botName + " (lvl " + std::to_string(report.botLevel) + ") "
                         + std::to_string(report.botQuestCount) + "q";

        line += (report.ownerQuestCount == report.botQuestCount) ? " - in sync" : " - catching up";

        if (report.botFreeSlots >= 0)
            line += ", " + std::to_string(report.botFreeSlots) + " free slot(s)";

        if (report.failed)
            line += ", " + std::to_string(report.failed) + " failed";

        if (report.classRaceLocked)
            line += ", " + std::to_string(report.classRaceLocked) + " class/race locked";

        if (report.prereqs)
            line += ", " + std::to_string(report.prereqs) + " prereq credited";

        if (report.levelFrom)
            line += ", levelled " + std::to_string(report.levelFrom) + " -> " + std::to_string(report.levelTo);

        return line;
    }

    Player* ResolveCompanion(Player* owner, SyncReport& report)
    {
        uint32 botGuid = 0;
        if (!FindCompanionGuid(owner->GetGUID().GetCounter(), botGuid))
            return nullptr;

        return ResolveCompanion(owner, report, botGuid);
    }

    /// Tell the owner about bindings the module stopped believing, once.
    void ReportDroppedBindings(Player* owner)
    {
        auto itr = g_bindingCache.find(owner->GetGUID().GetCounter());
        if (itr == g_bindingCache.end() || itr->second.dropped.empty())
            return;

        std::vector<DroppedBinding> const dropped = itr->second.dropped;
        itr->second.dropped.clear();

        if (!owner->GetSession())
            return;

        ChatHandler handler(owner->GetSession());
        for (DroppedBinding const& one : dropped)
        {
            // Say it in the log as well as the chat frame: a dropped binding is invisible to
            // anyone debugging from the log, and a test that greps for it finds nothing.
            LOG_INFO("server.loading", "[CompanionQuestSync] {} dropped binding {} ({}): {}",
                     owner->GetName(), one.name, one.botGuid, one.reason);

            handler.PSendSysMessage("|cffff8000Companion|r: no longer following |cffffff00{}|r: {}. "
                                    "|cffffff00.companionsync list|r shows who is bound, "
                                    "|cffffff00.companionsync bind <name>|r adds one.",
                                    one.name, one.reason);
        }

        if (g_cfg.pinSync)
            SendPinSync(owner, true);
    }

    /// Drive playerbots through the owner's own session, exactly as mod-wowlegends does.
    ///
    /// The playerbots command is SEC_PLAYER, so the owner is authorised for it, and its
    /// OnBotLogin does the part that actually matters: put the bot in the group, switch
    /// follow on, enable combat. Reimplementing that here would drift from it.
    void DispatchPlayerbots(Player* owner, std::string const& sub)
    {
        if (!owner || !owner->GetSession())
            return;

        ChatHandler(owner->GetSession()).ParseCommands(".playerbots bot " + sub);
    }

    /// A summon is asynchronous - playerbots prepares the body and logs it in on its own
    /// schedule - so the outcome is checked a few seconds later rather than assumed. A
    /// silent no-op is the single most expensive failure this module can have: the player
    /// asks for their companion, nothing happens, and there is nothing to go on.
    struct PendingSummon
    {
        uint32 botGuid = 0;
        uint32 ownerGuid = 0;
        std::string name;
        uint32 deadline = 0;
        bool expectOnline = true;   // false for a dismiss
    };

    std::map<uint32, PendingSummon> g_pendingSummon;   // bot guid -> what we are waiting for

    /// The name a companion answers to, online or not.

    std::string CompanionNameFor(uint32 botGuid)
    {
        if (Player* bot = FindCompanionPlayer(botGuid))
            return bot->GetName();

        if (QueryResult row = CharacterDatabase.Query("SELECT name FROM characters WHERE guid = {}", botGuid))
            return row->Fetch()[0].Get<std::string>();

        return std::to_string(botGuid);
    }

    // -----------------------------------------------------------------------
    // companion creation
    //
    // mod-wowlegends can mint one companion per player, into its own table. Several
    // companions need their own bodies, so this module claims them itself: it reads the
    // pool (AddClass accounts, the released ledger, the AHBot guid) exactly as that module
    // does, and writes only to its own binding table. Nothing in mod-wowlegends changes
    // behaviour, and none of its rows are touched.
    // -----------------------------------------------------------------------

    /// Account ids the playerbots pool considers AddClass bodies (account_type = 2).
    std::string AddClassAccountCsv()
    {
        std::string csv;

        if (QueryResult result = PlayerbotsDatabase.Query(
                "SELECT account_id FROM playerbots_account_type WHERE account_type = "
                + std::to_string(g_cfg.poolAccountType)))
        {
            do
            {
                if (!csv.empty())
                    csv.push_back(',');

                csv += std::to_string(result->Fetch()[0].Get<uint32>());
            } while (result->NextRow());
        }

        return csv;
    }

    /// One free body of this race and class, or 0.
    ///
    /// The guards are the pool's own: an AddClass account, offline, not guilded, not the
    /// auction bot, not already bound here or in mod-wowlegends' table, and not released
    /// in the last quarter of an hour (those are still being cleaned up).
    uint32 PickPoolBody(uint8 race, uint8 clazz)
    {
        std::string const accounts = AddClassAccountCsv();
        if (accounts.empty())
            return 0;

        std::string sql =
            "SELECT c.guid FROM characters c "
            "WHERE c.account IN (" + accounts + ") "
            "AND c.race = " + std::to_string(uint32(race)) + " "
            "AND c.class = " + std::to_string(uint32(clazz)) + " "
            "AND c.online = 0 "
            "AND c.guid NOT IN (SELECT guid FROM guild_member) "
            "AND c.guid NOT IN (SELECT bot_guid FROM " + g_cfg.bindingTable + ") "
            + LegacyBindingClause();

        uint32 const ahbot = sConfigMgr->GetOption<uint32>("AuctionHouseBot.GUID", 0);
        if (ahbot)
            sql += "AND c.guid <> " + std::to_string(ahbot) + " ";

        // No re-claim cooldown. That rule belongs to mod-wowlegends, whose cleanup is
        // asynchronous; this module resets and renames a released body before it records the
        // release, so the body is ready the moment it is written down. Holding it back for a
        // quarter of an hour made a just-released race/class look empty - which is exactly
        // what "human priest not available" was.

        sql += "LIMIT 1";

        if (QueryResult result = CharacterDatabase.Query(sql.c_str()))
            return result->Fetch()[0].Get<uint32>();

        return 0;
    }

    /// Blank a body so a new owner starts from nothing, the way the pool expects.
    void ResetPoolBody(uint32 botGuid)
    {
        CharacterDatabase.DirectExecute(
            "DELETE ii FROM item_instance ii INNER JOIN character_inventory ci ON ci.item = ii.guid WHERE ci.guid = {}",
            botGuid);
        CharacterDatabase.DirectExecute("DELETE FROM item_instance WHERE owner_guid = {}", botGuid);
        CharacterDatabase.DirectExecute("DELETE FROM character_inventory WHERE guid = {}", botGuid);

        for (char const* table : { "character_queststatus", "character_queststatus_daily",
                                   "character_queststatus_weekly", "character_queststatus_monthly",
                                   "character_queststatus_seasonal", "character_queststatus_rewarded" })
            CharacterDatabase.DirectExecute(fmt::format("DELETE FROM {} WHERE guid = {}", table, botGuid));

        CharacterDatabase.DirectExecute(
            "UPDATE characters SET level = 1, xp = 0, money = 0, rest_bonus = 0, at_login = 0, health = 100 "
            "WHERE guid = {}", botGuid);
    }

    /// Make sure the core knows this body, name and all.
    ///
    /// A body that exists only in the database - a pool body cloned from a template, or one
    /// this module just renamed - has no entry in sCharacterCache. playerbots' AddPlayerBot
    /// looks the account id up there and returns silently when it finds nothing, so a summon
    /// of a freshly created companion did nothing at all, with no error anywhere.
    void EnsureCharacterCache(uint32 botGuid, std::string const& name)
    {
        ObjectGuid const guid = ObjectGuid::Create<HighGuid::Player>(botGuid);

        if (sCharacterCache->GetCharacterAccountIdByGuid(guid))
        {
            sCharacterCache->UpdateCharacterData(guid, name);
            return;
        }

        if (QueryResult row = CharacterDatabase.Query(
                "SELECT account, gender, race, class, level FROM characters WHERE guid = {}", botGuid))
        {
            Field* fields = row->Fetch();
            sCharacterCache->AddCharacterCacheEntry(guid, fields[0].Get<uint32>(), name,
                                                    fields[1].Get<uint8>(), fields[2].Get<uint8>(),
                                                    fields[3].Get<uint8>(), fields[4].Get<uint8>());
        }
    }

    /// Rename a body and prove it stuck - a rename the database refused must never be
    /// reported as done, because the next summon would then look for a name nobody has.
    bool RenameBody(uint32 botGuid, std::string const& name)
    {
        std::string escaped = name;
        CharacterDatabase.EscapeString(escaped);

        CharacterDatabase.DirectExecute("UPDATE characters SET name = '{}' WHERE guid = {}", escaped, botGuid);
        EnsureCharacterCache(botGuid, name);

        if (QueryResult row = CharacterDatabase.Query("SELECT name FROM characters WHERE guid = {}", botGuid))
            return row->Fetch()[0].Get<std::string>() == name;

        return false;
    }

    bool NameTaken(std::string const& name)
    {
        std::string escaped = name;
        CharacterDatabase.EscapeString(escaped);

        return static_cast<bool>(CharacterDatabase.Query("SELECT guid FROM characters WHERE name = '{}'", escaped));
    }

    bool ValidCompanionName(std::string const& name)
    {
        if (name.size() < 2 || name.size() > 12)
            return false;

        for (char const c : name)
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
                return false;

        return true;
    }

    /// How many free bodies the pool has per race and class: `race-class=count`, `|`
    /// separated. The picker greys out what the pool cannot serve, which is the difference
    /// between a dropdown that works and one that fails after the player has chosen.
    std::string BuildPoolStockLine()
    {
        std::string const accounts = AddClassAccountCsv();
        if (accounts.empty())
            return std::string();

        std::string sql =
            "SELECT c.race, c.class, COUNT(*) FROM characters c "
            "WHERE c.account IN (" + accounts + ") "
            "AND c.online = 0 "
            "AND c.guid NOT IN (SELECT guid FROM guild_member) "
            "AND c.guid NOT IN (SELECT bot_guid FROM " + g_cfg.bindingTable + ") "
            + LegacyBindingClause();

        uint32 const ahbot = sConfigMgr->GetOption<uint32>("AuctionHouseBot.GUID", 0);
        if (ahbot)
            sql += "AND c.guid <> " + std::to_string(ahbot) + " ";

        sql += "GROUP BY c.race, c.class ORDER BY c.race, c.class";

        std::string line;

        if (QueryResult result = CharacterDatabase.Query(sql.c_str()))
        {
            do
            {
                Field* fields = result->Fetch();
                uint32 const race = fields[0].Get<uint8>();
                uint32 const clazz = fields[1].Get<uint8>();
                uint32 const count = fields[2].Get<uint32>();

                if (!count)
                    continue;

                if (!line.empty())
                    line += "|";

                line += std::to_string(race) + "-" + std::to_string(clazz) + "=" + std::to_string(count);
            } while (result->NextRow());
        }

        return line;
    }

    /// Send a payload as a grey whisper, chunked like the pin and companion lines.
    void SendChunkedPayload(Player* owner, std::string const& label, std::string const& payload,
                            uint32 entries, std::string& last)
    {
        std::vector<std::string> chunks;
        std::string current;
        size_t start = 0;

        while (start <= payload.size())
        {
            size_t const bar = payload.find('|', start);
            std::string entry = payload.substr(start, bar == std::string::npos ? std::string::npos : bar - start);

            if (!current.empty() && current.size() + entry.size() + 1 > 150)
            {
                chunks.push_back(current);
                current.clear();
            }

            if (!current.empty())
                current += "|";

            current += entry;

            if (bar == std::string::npos)
                break;

            start = bar + 1;
        }

        if (!current.empty())
            chunks.push_back(current);

        if (chunks.empty())
            chunks.push_back(std::string());

        Player* speaker = ResolveProtocolSpeaker(owner);

        for (size_t i = 0; i < chunks.size(); ++i)
        {
            std::string line = label + " ";

            if (chunks.size() > 1)
                line += std::to_string(i + 1) + "/" + std::to_string(chunks.size()) + ": ";
            else
                line += std::to_string(entries) + ": ";

            line += chunks[i];

            SendProtocolLine(owner, line);
        }

        last = payload;
    }

    void SendPoolStock(Player* owner, bool force = false)
    {
        if (!g_cfg.pinSync || !owner || !owner->GetSession() || !g_cfg.allowCreate)
            return;

        uint32 const ownerGuid = owner->GetGUID().GetCounter();
        std::string const payload = BuildPoolStockLine();

        std::string& last = g_lastPoolStock[ownerGuid];
        if (!force && payload == last)
            return;

        uint32 entries = 0;
        for (char const c : payload)
            if (c == '|')
                ++entries;
        if (!payload.empty())
            ++entries;

        SendChunkedPayload(owner, "Pool stock", payload, entries, last);

        if (g_cfg.debug >= 2)
            LOG_INFO("server.loading", "[CompanionQuestSync] pool stock -> {}: {} combos: {}",
                     owner->GetName(), entries, payload);
    }

    /// Mint one companion: claim a pool body, blank it, name it, bind it, call it out.
    ///
    /// Returns false with `error` set to something a player can act on. Every failure is
    /// named, because "it did not work" from a button is useless.
    bool CreateCompanion(uint32 ownerGuid, std::string const& ownerName, uint8 race, uint8 clazz,
                         std::string const& name, uint32& botGuid, std::string& error)
    {
        if (!ValidCompanionName(name))
        {
            error = "a name is 2 to 12 letters, nothing else";
            return false;
        }

        if (NameTaken(name))
        {
            error = "the name " + name + " is already taken";
            return false;
        }

        std::vector<uint32> bound;
        if (FindCompanions(ownerGuid, bound) && bound.size() >= kMaxCompanions)
        {
            error = "you already have the maximum of " + std::to_string(kMaxCompanions) + " companions";
            return false;
        }

        botGuid = PickPoolBody(race, clazz);
        if (!botGuid)
        {
            error = "the pool has no free body for that race and class right now";
            return false;
        }

        std::string poolName;
        if (QueryResult row = CharacterDatabase.Query("SELECT name FROM characters WHERE guid = {}", botGuid))
            poolName = row->Fetch()[0].Get<std::string>();

        ResetPoolBody(botGuid);

        if (!RenameBody(botGuid, name))
        {
            error = "the body could not be renamed - try another name";
            return false;
        }

        if (g_bindingHasName)
        {
            std::string escaped = name;
            CharacterDatabase.EscapeString(escaped);

            CharacterDatabase.DirectExecute(
                "INSERT INTO {} (owner_guid, bot_guid, companion_name) VALUES ({}, {}, '{}')",
                g_cfg.bindingTable, ownerGuid, botGuid, escaped);
        }
        else
        {
            CharacterDatabase.DirectExecute(
                "INSERT INTO {} (owner_guid, bot_guid) VALUES ({}, {})", g_cfg.bindingTable, ownerGuid, botGuid);
        }

        InvalidateCompanionGuid(ownerGuid);

        LOG_INFO("server.loading", "[CompanionQuestSync] {} created companion {} (race {}, class {}) from pool body {} (was '{}')",
                 ownerName, name, uint32(race), uint32(clazz), botGuid, poolName);

        return true;
    }

    /// Does the owner carry a quest this companion does not? That is the one thing the
    /// window has to show that the quest-log view does not: work waiting on the *player*.
    bool OwnerHasQuestCompanionLacks(Player* owner, Player* bot)
    {
        for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 const questId = owner->GetQuestSlotQuestId(slot);
            if (!questId)
                continue;

            if (owner->GetQuestStatus(questId) == QUEST_STATUS_NONE)
                continue;

            if (bot->GetQuestStatus(questId) == QUEST_STATUS_NONE && !bot->IsQuestRewarded(questId))
                return true;
        }

        return false;
    }

    /// One line per companion, for the client.
    ///
    ///     Comp sync 2: Healy,5,1,32,4,ogs,110|Soak,9,2,32,2,o,105
    ///                  name,class,race,level,quests,flags,free slots
    ///
    /// Flags: `o` in the world, `g` in your party, `s` same level as you, `b` behind (you
    /// carry a quest it has not got). Compact on purpose: this rides a whisper, and the
    /// addon splits on `,` and `|` exactly like the pin payload, so names go through
    /// SafePinName.
    std::string BuildCompLine(Player* owner)
    {
        std::vector<uint32> bots;
        if (!FindCompanions(owner->GetGUID().GetCounter(), bots))
            return std::string();

        std::string line;

        for (uint32 const botGuid : bots)
        {
            Player* bot = FindCompanionPlayer(botGuid);
            bool const online = bot && bot->IsInWorld();

            std::string name;
            std::string flags;
            uint32 classId = 0;
            uint32 raceId = 0;
            uint32 level = 0;
            uint32 quests = 0;
            uint32 freeSlots = 0;

            if (online)
            {
                name = bot->GetName();
                classId = uint32(bot->getClass());
                raceId = uint32(bot->getRace());
                level = bot->GetLevel();
                quests = CountQuestLog(bot);
                int32 const free = int32(bot->GetFreeInventorySpace());
                freeSlots = free > 0 ? uint32(free) : 0;

                flags += "o";

                Group* group = owner->GetGroup();
                if (group && group->IsMember(bot->GetGUID()))
                    flags += "g";

                if (bot->GetLevel() == owner->GetLevel())
                    flags += "s";

                if (OwnerHasQuestCompanionLacks(owner, bot))
                    flags += "b";

                // Dead or ghost: the card in the window must not read like a healthy one.
                if (!bot->IsAlive())
                    flags += "d";
            }
            else
            {
                // Offline: the name is all there is. The addon dims the row and offers
                // Summon, which is the only action that makes sense for a body that is not
                // in the world.
                name = CompanionNameFor(botGuid);
            }

            if (!line.empty())
                line += "|";

            line += SafePinName(name) + "," + std::to_string(classId) + "," + std::to_string(raceId) + "," +
                    std::to_string(level) + "," + std::to_string(quests) + "," +
                    (flags.empty() ? "-" : flags) + "," + std::to_string(freeSlots);

#ifdef MOD_PLAYERBOTS
            // the gathering half of the pair, so the card shows where the skill stands -
            // a catch-up is visible the moment the number jumps
            if (online)
            {
                auto const pair = ClassProfessionPair(bot->getClass());
                uint16 const skill = pair.first;
                line += ",y=" + std::string(ProfessionOf(skill).name) + " " +
                        std::to_string(bot->GetSkillValue(skill)) + "/" + std::to_string(ProfessionCap(bot->GetLevel()));
            }
#endif
        }

        return line;
    }

    /// Send the companions line when it changes, on login, and on demand.
    ///
    /// Same channel and cadence as the pin payload: a whisper, because that is the only
    /// thing the client can read. Nothing else tells the addon who is bound - commands go
    /// one way, and a client cannot query the server.
    /// The roster with its free-slot column removed, one entry at a time.
    ///
    /// Bag space drifts on its own as a companion vendors and loots, and comparing whole
    /// payloads meant the entire roster was re-sent every other pass for a number nobody
    /// watches. Everything that matters - a name, a level, a quest count, a flag, a
    /// companion appearing or going - still differs here and still goes out immediately.
    std::string CompSyncSignature(std::string const& payload)
    {
        std::string out;
        out.reserve(payload.size());

        size_t start = 0;
        while (start <= payload.size())
        {
            size_t const bar = payload.find('|', start);
            std::string entry = payload.substr(start, bar == std::string::npos ? std::string::npos : bar - start);

            size_t const comma = entry.find_last_of(',');
            if (comma != std::string::npos)
                entry.erase(comma);

            if (!out.empty())
                out += "|";

            out += entry;

            if (bar == std::string::npos)
                break;

            start = bar + 1;
        }

        return out;
    }

    void SendCompSync(Player* owner, bool force = false)
    {
        if (!g_cfg.pinSync || !owner || !owner->GetSession())
            return;

        uint32 const ownerGuid = owner->GetGUID().GetCounter();
        std::string const payload = BuildCompLine(owner);

        std::string& last = g_lastCompSync[ownerGuid];
        uint32& lastAt = g_lastCompSyncAt[ownerGuid];
        uint32 const now = static_cast<uint32>(time(nullptr));

        // A change that is only the free-slot column waits for the slower clock, so the
        // client is still told (the number is in the payload) but not every 30 seconds.
        constexpr uint32 kSlotOnlyGraceSeconds = Timing::SlotOnlyGrace;

        if (!force && !last.empty())
        {
            if (payload == last)
                return;

            if (CompSyncSignature(payload) == CompSyncSignature(last)
                && now - lastAt < kSlotOnlyGraceSeconds)
                return;
        }

        std::vector<std::string> chunks;
        std::string current;
        size_t start = 0;
        uint32 entries = 0;

        while (start <= payload.size())
        {
            size_t const bar = payload.find('|', start);
            std::string entry = payload.substr(start, bar == std::string::npos ? std::string::npos : bar - start);

            if (!current.empty() && current.size() + entry.size() + 1 > 150)
            {
                chunks.push_back(current);
                current.clear();
            }

            if (!current.empty())
                current += "|";

            current += entry;
            ++entries;

            if (bar == std::string::npos)
                break;

            start = bar + 1;
        }

        if (!current.empty())
            chunks.push_back(current);

        if (chunks.empty())
            chunks.push_back(std::string());

        SyncReport compReport;   // only used to resolve a speaker

        for (size_t i = 0; i < chunks.size(); ++i)
        {
            std::string line = "Comp sync ";

            if (chunks.size() > 1)
                line += std::to_string(i + 1) + "/" + std::to_string(chunks.size()) + ": ";
            else
                line += std::to_string(entries) + ": ";

            line += chunks[i];

            SendProtocolLine(owner, line);
        }

        last = payload;
        lastAt = now;

        if (g_cfg.debug >= 2)
            LOG_INFO("server.loading", "[CompanionQuestSync] comp sync -> {}: {} part(s), {} entries: {}",
                     owner->GetName(), uint32(chunks.size()), entries, payload);
    }

    /// Which bound companion the player meant, by live name or by the name on the row.
    uint32 ResolveCompanion(Player* player, std::string const& name, std::vector<uint32> const& bound)
    {
        for (uint32 const candidate : bound)
        {
            Player* bot = FindCompanionPlayer(candidate);
            if (bot && bot->GetName() == name)
                return candidate;
        }

        if (g_bindingHasName)
        {
            std::string escaped = name;
            CharacterDatabase.EscapeString(escaped);

            if (QueryResult row = CharacterDatabase.Query("SELECT bot_guid FROM {} WHERE owner_guid = {} AND companion_name = '{}'",
                                                          g_cfg.bindingTable, player->GetGUID().GetCounter(), escaped))
                return row->Fetch()[0].Get<uint32>();
        }

        return 0;
    }

    /// The same lookup as ResolveCompanion, for a character who is not logged in.
    uint32 ResolveCompanionForOwner(uint32 ownerGuid, std::string const& name, std::vector<uint32> const& bound)
    {
        for (uint32 const candidate : bound)
            if (CompanionNameFor(candidate) == name)
                return candidate;

        if (g_bindingHasName)
        {
            std::string escaped = name;
            CharacterDatabase.EscapeString(escaped);

            if (QueryResult row = CharacterDatabase.Query(
                    "SELECT bot_guid FROM {} WHERE owner_guid = {} AND companion_name = '{}'",
                    g_cfg.bindingTable, ownerGuid, escaped))
                return row->Fetch()[0].Get<uint32>();
        }

        return 0;
    }

    /// When each body was last seen up, so a removal is never issued at a bot playerbots is
    /// still finishing the login of.
    ///
    /// Every removal that took the worldserver down today was of a companion summoned seconds
    /// earlier, and every removal that worked was of one that had been out for minutes.
    std::map<uint32, uint32> g_botUpAt;

    void CheckPendingSummons()
    {
        if (g_pendingSummon.empty())
            return;

        uint32 const now = static_cast<uint32>(time(nullptr));

        for (auto it = g_pendingSummon.begin(); it != g_pendingSummon.end();)
        {
            if (now < it->second.deadline)
            {
                ++it;
                continue;
            }

            PendingSummon const pending = it->second;
            it = g_pendingSummon.erase(it);

            Player* bot = FindCompanionPlayer(pending.botGuid);
            bool const online = bot && bot->IsInWorld();
            Player* owner = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(pending.ownerGuid));

            if (pending.expectOnline)
            {
                if (online)
                {
                    g_botUpAt[pending.botGuid] = static_cast<uint32>(time(nullptr));

                    LOG_INFO("server.loading", "[CompanionQuestSync] summon came up: {} ({})", pending.name, pending.botGuid);

                    // Dress it on the spot. Waiting for the next sweep means the player watches
                    // a naked companion walk around for up to half a minute, and "it spawns
                    // dressed" is the whole point of a companion arriving.
                    if (Player* fresh = FindCompanionPlayer(pending.botGuid); fresh && fresh->IsInWorld())
                    {
                        Player* master = ObjectAccessor::FindConnectedPlayer(
                            ObjectGuid::Create<HighGuid::Player>(pending.ownerGuid));

                        if (master)
                        {
                            // The companion answers "what have you been up to?" before it
                            // is asked: the last session's digest, once, then it is gone.
                            // The row is the truth - it survives a world restart between the
                            // logout that wrote it and this arrival.
                            std::string summary;
                            if (QueryResult digest = CharacterDatabase.Query(
                                    "SELECT summary FROM cq_digest WHERE owner_guid = {}",
                                    pending.ownerGuid))
                            {
                                summary = (*digest)[0].Get<std::string>();
                                CharacterDatabase.Execute("DELETE FROM cq_digest WHERE owner_guid = {}",
                                                          pending.ownerGuid);
                            }
                            else
                            {
                                auto last = g_lastSessionSummary.find(pending.ownerGuid);
                                if (last != g_lastSessionSummary.end())
                                    summary = last->second;
                                if (!summary.empty())
                                    g_lastSessionSummary.erase(last);
                            }

                            if (!summary.empty())
                                SayToOwner(fresh, master,
                                           "Welcome back! Since we last parted: " + summary + ".", 0);
                        }

                        // A pool bot that died in a previous session can arrive with the
                        // death still written on it: the character's stand state survives a
                        // dismiss/re-summon, and a living body standing in UNIT_STAND_STATE_DEAD
                        // renders as the death animation frozen mid-fall - the folded corpse in
                        // the grass the owner then reports as broken. Spawn means alive and
                        // standing, and the module says so outright instead of trusting the
                        // stale state the character brought with it.
                        if (!fresh->IsAlive())
                            fresh->ResurrectPlayer(1.0f, false);
                        fresh->SetStandState(UNIT_STAND_STATE_STAND);

                        EnsureBags(master, fresh);
                        EnsureStartingGear(master, fresh);

                        LOG_INFO("server.loading", "[CompanionQuestSync] {} dressed at spawn: {}",
                                 pending.name, DescribeGear(fresh));
                    }

                    if (owner && owner->GetSession() && g_cfg.requireGrouped)
                    {
                        Group* group = owner->GetGroup();
                        if (!group || !group->IsMember(bot->GetGUID()))
                        {
                            ChatHandler handler(owner->GetSession());
                            handler.PSendSysMessage("|cffff8000Companion|r: |cffffff00{}|r is out, but not in your party, so "
                                                    "quest sync will skip it. Invite it (or drop |cffffff00RequireGrouped|r).",
                                                    bot->GetName());
                        }
                    }
                }
                else
                {
                    LOG_ERROR("server.loading", "[CompanionQuestSync] summon did not come up: {} ({})",
                              pending.name, pending.botGuid);

                    if (owner && owner->GetSession())
                    {
                        ChatHandler handler(owner->GetSession());
                        handler.PSendSysMessage("|cffff8000Companion|r: |cffffff00{}|r did not come out. If playerbots refused it, "
                                                "the body may have been released - |cffffff00.companionsync unbind {}|r then "
                                                "|cffffff00.companionsync bind {}|r once it is with you.",
                                                pending.name, pending.name, pending.name);
                    }
                }
            }
            else if (online)
            {
                LOG_ERROR("server.loading", "[CompanionQuestSync] dismiss did not take: {} ({}) is still in the world",
                          pending.name, pending.botGuid);

                if (owner && owner->GetSession())
                    ChatHandler(owner->GetSession()).PSendSysMessage(
                        "|cffff8000Companion|r: |cffffff00{}|r is still with you; playerbots did not let it go.",
                        pending.name);
            }
            else
            {
                LOG_INFO("server.loading", "[CompanionQuestSync] dismiss took: {} ({}) left", pending.name, pending.botGuid);
            }
        }
    }

    // -----------------------------------------------------------------------
    // releasing a companion back to the pool
    //
    // The last piece that used to belong to mod-wowlegends. Nothing here reads or writes
    // its tables: the ledger this module consults and writes is its own.
    // -----------------------------------------------------------------------

    bool g_poolLedgerReady = false;

    void EnsurePoolLedger()
    {
        if (g_poolLedgerReady)
            return;

        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS " + g_cfg.poolTable + " ("
            " bot_guid INT UNSIGNED NOT NULL,"
            " released_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
            " reason VARCHAR(32) NOT NULL DEFAULT '',"
            " PRIMARY KEY (bot_guid)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");

        g_poolLedgerReady = true;
    }

    void LedgerRelease(uint32 botGuid, char const* reason)
    {
        EnsurePoolLedger();

        CharacterDatabase.DirectExecute(
            "INSERT INTO " + g_cfg.poolTable + " (bot_guid, released_at, reason) VALUES ({}, NOW(), '{}') "
            "ON DUPLICATE KEY UPDATE released_at = NOW(), reason = '{}'",
            botGuid, reason, reason);
    }

    /// A neutral name for a body that belongs to nobody. Unique per body, inside the
    /// 12-character limit, and obviously not a player.
    std::string PoolNameFor(uint32 botGuid)
    {
        // Two letters and the id: pool guids run to eight digits, and a character name is
        // capped at twelve. "Zzpool" + guid would be fourteen and the rename would be
        // refused - which is how a release could fail at the very last step.
        return "Zz" + std::to_string(botGuid);
    }

    struct PendingRelease
    {
        uint32 ownerGuid = 0;
        uint32 botGuid = 0;
        std::string name;
        uint32 deadline = 0;
        bool needsDismiss = false;   // still in the world; send it away from the update tick
    };

    std::vector<PendingRelease> g_pendingRelease;

    /// Tell the owner what happened to a release, once it is really done.
    void ReportRelease(PendingRelease const& pending, bool ok, std::string const& why)
    {
        LOG_INFO("server.loading", "[CompanionQuestSync] release {} ({}) {}: {}",
                 pending.name, pending.botGuid, ok ? "complete" : "FAILED", why);

        Player* owner = ObjectAccessor::FindConnectedPlayer(
            ObjectGuid::Create<HighGuid::Player>(pending.ownerGuid));

        if (!owner || !owner->GetSession())
            return;

        ChatHandler handler(owner->GetSession());

        if (ok)
            handler.PSendSysMessage("|cff20ff20Companion quest sync|r: |cffffff00{}|r has gone back to the pool. "
                                    "The body is free for anyone; you can mint another any time.",
                                    pending.name);
        else
            handler.PSendSysMessage("|cffff8000Companion quest sync|r: |cffffff00{}|r could not be released - {}. "
                                    "Try |cffffff00.companionsync release {}|r again.",
                                    pending.name, why, pending.name);
    }

    /// Finish a release once the body is out of the world, or give up and say so.
    void CheckPendingReleases()
    {
        if (g_pendingRelease.empty())
            return;

        uint32 const now = static_cast<uint32>(time(nullptr));

        for (auto it = g_pendingRelease.begin(); it != g_pendingRelease.end();)
        {
            PendingRelease pending = *it;

            Player* bot = FindCompanionPlayer(pending.botGuid);
            bool const online = bot && bot->IsInWorld();

            // still in the world: the dismiss was issued from the command handler, so give it
            // time to actually go before touching the body
            if (online)
            {
                LOG_INFO("server.loading",
                         "[CompanionQuestSync] release step 4: waiting for {} to leave ({} s left)",
                         pending.name, uint32(pending.deadline > now ? pending.deadline - now : 0));

                ++it;
                continue;
            }

            if (online && now < pending.deadline)
            {
                ++it;
                continue;
            }

            if (online)
            {
                // Still with us long after the dismiss: stop asking, tell the player.
                it = g_pendingRelease.erase(it);
                ReportRelease(pending, false, "it is still in the world");
                continue;
            }

            it = g_pendingRelease.erase(it);

            // The body is offline and about to stop being a companion, so the sweep will never
            // look at it again: if its party slot is not released here it stays behind as an
            // offline member that nothing owns.
            if (Player* owner = ObjectAccessor::FindConnectedPlayer(
                    ObjectGuid::Create<HighGuid::Player>(pending.ownerGuid)))
            {
                if (Group* group = owner->GetGroup())
                {
                    ObjectGuid const botGuidObj = ObjectGuid::Create<HighGuid::Player>(pending.botGuid);

                    if (group->IsMember(botGuidObj))
                    {
                        group->RemoveMember(botGuidObj);

                        LOG_INFO("server.loading",
                                 "[CompanionQuestSync] {} removed from {}'s group before release",
                                 pending.name, owner->GetName());
                    }
                }
            }

            LOG_INFO("server.loading", "[CompanionQuestSync] release step 5: finalising {} ({})",
                     pending.name, pending.botGuid);

            ResetPoolBody(pending.botGuid);

            LOG_INFO("server.loading", "[CompanionQuestSync] release step 6: body reset");

            if (!RenameBody(pending.botGuid, PoolNameFor(pending.botGuid)))
            {
                ReportRelease(pending, false, "the body could not be renamed back");
                continue;
            }

            LOG_INFO("server.loading", "[CompanionQuestSync] release step 7: renamed, unbinding");

            CharacterDatabase.DirectExecute("DELETE FROM {} WHERE bot_guid = {}",
                                            g_cfg.bindingTable, pending.botGuid);
            LedgerRelease(pending.botGuid, "release");

            LOG_INFO("server.loading", "[CompanionQuestSync] release step 8: ledger written");

            PurgeCompanionState(pending.botGuid);
            InvalidateCompanionGuid(pending.ownerGuid);

            if (Player* owner = ObjectAccessor::FindConnectedPlayer(
                    ObjectGuid::Create<HighGuid::Player>(pending.ownerGuid)))
            {
                if (g_cfg.pinSync)
                {
                    SendPinSync(owner, true);
                    SendCompSync(owner, true);
                    SendPoolStock(owner, true);
                }
            }

            ReportRelease(pending, true, "back in the pool");
        }
    }

    /// Start a release: dismiss the body, then finish when it is really gone.
    bool BeginRelease(uint32 ownerGuid, std::string const& ownerName, uint32 botGuid, std::string& error)
    {
        LOG_INFO("server.loading", "[CompanionQuestSync] release step 1: begin (owner {}, bot {})",
                 ownerGuid, botGuid);
        for (PendingRelease const& pending : g_pendingRelease)
            if (pending.botGuid == botGuid)
            {
                error = "that release is already under way";
                return false;
            }

        std::string const name = CompanionNameFor(botGuid);

        LOG_INFO("server.loading", "[CompanionQuestSync] release step 2: name resolved ({})", name);

        // A summon check may still be armed for this body. Releasing dismisses it, and the check
        // would then report "summon did not come up" ten seconds after the release had already
        // succeeded - a contradiction that reads like two features fighting.
        g_pendingSummon.erase(botGuid);

        // Never dispatch playerbots from here. `bot remove` deletes the bot synchronously,
        // and doing that inside a chat command handler took the worldserver down: the player
        // was disconnected and the process died with no dump. The dismiss is queued and sent
        // from the update tick, where the rest of this module's work already happens.
        Player* bot = FindCompanionPlayer(botGuid);
        bool const online = bot && bot->IsInWorld();

        Player* owner = ObjectAccessor::FindConnectedPlayer(
            ObjectGuid::Create<HighGuid::Player>(ownerGuid));

        uint32 const upAt = g_botUpAt.count(botGuid) ? g_botUpAt[botGuid] : 0;
        uint32 const now = static_cast<uint32>(time(nullptr));

        if (online && owner && upAt && now - upAt < Timing::SummonJudge)
        {
            error = "that companion has only just come out - give it a few seconds and release again";
            return false;
        }

        if (online && owner)
        {
            LOG_INFO("server.loading",
                     "[CompanionQuestSync] release step 3: body is out, dismissing it now");

            // Inline, from the command handler: that is exactly how mod-wowlegends' own
            // `.companion forget` dismisses, and it is the one place playerbots' synchronous
            // teardown is provably safe. Issuing it from the update tick instead - which this
            // module tried twice - takes the process down at the log line, with no dump.
            // Nothing below touches the body: the reset waits for the tick, once it is offline.
            LOG_INFO("server.loading", "[CompanionQuestSync] release: dispatching remove {}", name);

            DispatchPlayerbots(owner, "remove " + name);

            LOG_INFO("server.loading", "[CompanionQuestSync] release: remove dispatched");
        }
        else if (online)
        {
            LOG_INFO("server.loading",
                     "[CompanionQuestSync] release step 3: body is out but its owner is offline - "
                     "it will be reset on the next tick it is gone");
        }

        g_pendingRelease.push_back({ ownerGuid, botGuid, name,
                                     static_cast<uint32>(time(nullptr)) + 15, false });

        LOG_INFO("server.loading", "[CompanionQuestSync] {} releasing {} ({})", ownerName, name, botGuid);
        return true;
    }

    /// Sweep every companion bound to this owner.
    ///
    /// One pass per companion, aggregated into a single report so callers that only
    /// want "did anything happen" keep working. `lines` carries one heartbeat line per
    /// companion, which is the only way several can be told apart in the log.
    SyncReport SyncCompanion(Player* owner)
    {
        SyncReport aggregate;

        if (!g_cfg.enable)
            return aggregate;

        std::vector<uint32> bots;
        FindCompanions(owner->GetGUID().GetCounter(), bots);

        // A binding the module refused to believe is news the player needs: a companion
        // that silently stops being tracked reads as "it stopped following me", and the
        // next thing they do is file a bug about the module rather than about the row.
        ReportDroppedBindings(owner);

        // Speech drains at the owner level too: a line queued while a companion was out
        // must not wait for a companion to come back out before it may speak or be
        // dropped as stale - each entry resolves its own speaker, and stale entries drop.
        FlushWhisperQueue(owner, nullptr);

        // Presence belongs to the OWNER, not to whichever companion happens to be out:
        // the heartbeat used to run inside the per-companion pass, which never ran while
        // no companion was summoned - the owner sat online with a login Hello and then
        // silence, and the window read "not responding" while the module was alive and
        // simply idle. One Hello a minute from the moment the owner is in the world,
        // zero companions or four.
        SendHeartbeat(owner);

        if (bots.empty())
            return aggregate;

        // A companion that is not in the world must not hold a party slot. This has to run for
        // *every* bound companion before anything tries to resolve one - the resolver returns
        // early for an offline body, which is exactly the case that needs tidying, so putting
        // it behind the resolver meant it never ran at all.
        for (uint32 const botGuid : bots)
        {
            Player* bot = FindCompanionPlayer(botGuid);
            if (bot && bot->IsInWorld())
                continue;

            Group* group = owner->GetGroup();
            if (!group)
                break;

            ObjectGuid const guid = ObjectGuid::Create<HighGuid::Player>(botGuid);
            if (!group->IsMember(guid))
                continue;

            group->RemoveMember(guid);

            LOG_INFO("server.loading", "[CompanionQuestSync] companion {} was not in the world - "
                                       "removed it from {}'s group", botGuid, owner->GetName());
        }

        aggregate.hasCompanion = true;
        g_multiCompanion[owner->GetGUID().GetCounter()] = bots.size() > 1;

        for (uint32 const botGuid : bots)
            g_multiCompanionBySpeaker[botGuid] = bots.size() > 1;

        // One union of turn-in markers for the owner, applied after every companion has
        // contributed: applying them per companion let the second pass clear the first
        // companion's marker, so a giver only one of them needed flickered each sweep.
        std::set<uint64> marks;

        for (uint32 const botGuid : bots)
        {
            SyncReport const one = SyncCompanionFor(owner, botGuid, marks);

            // Completion marking happens inside the pass, after the hand-in step that ran at
            // the top of it - so a quest that became complete this pass would otherwise wait
            // a full interval to be handed in. DeliverHandIns is throttled, so this is cheap.
            if (Player* bot = FindCompanionPlayer(botGuid); bot && bot->IsInWorld())
                DeliverHandIns(owner, bot);

            aggregate.added += one.added;
            aggregate.completed += one.completed;
            aggregate.dropped += one.dropped;
            aggregate.reopened += one.reopened;
            aggregate.classRaceLocked += one.classRaceLocked;
            aggregate.prereqs += one.prereqs;
            aggregate.failed += one.failed;
            aggregate.companionOnline = aggregate.companionOnline || one.companionOnline;
            aggregate.ownerName = one.ownerName;
            aggregate.ownerLevel = one.ownerLevel;
            aggregate.ownerQuestCount = one.ownerQuestCount;

            aggregate.skipped.insert(aggregate.skipped.end(), one.skipped.begin(), one.skipped.end());
            aggregate.refusedIds.insert(aggregate.refusedIds.end(), one.refusedIds.begin(), one.refusedIds.end());

            if (one.companionOnline)
                aggregate.lines.push_back(DescribeReport(one));
        }

        ApplyMarks(owner, marks);

        // One payload and one set of quest-area circles for the whole owner, after every
        // companion has had its pass. Both must aggregate: the client replaces what it
        // holds on receipt, so per-companion sends would take turns erasing each other.
        SendQuestPins(owner, bots);
        SendPinSync(owner, false);
        SendCompSync(owner, false);

        // One message, after everything is known: the highest-priority thing this pass found.
        for (uint32 const botGuid : bots)
        {
            if (Player* speaker = FindCompanionPlayer(botGuid); speaker && speaker->IsInWorld())
            {
                FlushNotes(owner, speaker);
                break;
            }
        }

        return aggregate;
    }
}

    // -----------------------------------------------------------------------
    // integrity
    //
    // The module keeps its own tables, so it also owns keeping them honest. The core deletes
    // a character or an account without telling anyone who pointed at it, and a binding row
    // that outlives its character is worse than no row at all: the player sees a companion
    // that cannot be summoned, and two owners can end up pointed at one body. Nothing here
    // touches core tables - it only removes rows of ours that refer to characters that are
    // gone, and rows that would make the module act twice on one body.
    // -----------------------------------------------------------------------

    struct IntegrityReport
    {
        uint32 rows = 0;
        uint32 danglingOwner = 0;
        uint32 danglingBot = 0;
        uint32 duplicate = 0;
        uint32 doubleBound = 0;      // bound in this table and in mod-wowlegends' table
        uint32 ledgerOrphans = 0;
        uint32 fixed = 0;
        std::vector<std::string> notes;
    };

    /// Check (and with fix, repair) every reference the module owns.
    IntegrityReport RunIntegrity(bool fix)
    {
        IntegrityReport out;

        std::string const nameColumn = g_bindingHasName ? "companion_name" : "''";
        std::string const createdColumn = g_bindingHasCreatedAt ? "UNIX_TIMESTAMP(created_at)" : "0";

        struct Row
        {
            uint32 owner = 0;
            uint32 bot = 0;
            std::string name;
            uint32 created = 0;
        };

        std::vector<Row> rows;
        std::set<uint32> guids;

        if (QueryResult result = CharacterDatabase.Query(
                "SELECT owner_guid, bot_guid, " + nameColumn + ", " + createdColumn +
                " FROM " + g_cfg.bindingTable))
        {
            do
            {
                Field* f = result->Fetch();
                Row row;
                row.owner = f[0].Get<uint32>();
                row.bot = f[1].Get<uint32>();
                row.name = f[2].Get<std::string>();
                row.created = f[3].Get<uint32>();
                rows.push_back(row);
                guids.insert(row.owner);
                guids.insert(row.bot);
            } while (result->NextRow());
        }

        out.rows = uint32(rows.size());

        std::set<uint32> alive;
        if (!guids.empty())
        {
            std::string list;
            for (uint32 const guid : guids)
            {
                if (!list.empty())
                    list += ",";
                list += std::to_string(guid);
            }

            if (QueryResult chars = CharacterDatabase.Query("SELECT guid FROM characters WHERE guid IN ({})", list))
            {
                do
                {
                    alive.insert(chars->Fetch()[0].Get<uint32>());
                } while (chars->NextRow());
            }
        }

        // the same body under another module: two modules would level and quest one bot
        std::set<uint32> legacyBound;
        if (g_cfg.respectLegacyBindings)
        if (QueryResult legacy = CharacterDatabase.Query("SELECT bot_guid FROM wowlegends_companion"))
        {
            do
            {
                legacyBound.insert(legacy->Fetch()[0].Get<uint32>());
            } while (legacy->NextRow());
        }

        std::set<uint32> seenBots;

        for (Row const& row : rows)
        {
            std::string const shown = row.name.empty() ? std::to_string(row.bot) : row.name;
            bool doomed = false;
            char const* why = nullptr;

            if (!alive.count(row.owner))
            {
                ++out.danglingOwner;
                doomed = true;
                why = "the owner character no longer exists";
            }
            else if (!alive.count(row.bot))
            {
                ++out.danglingBot;
                doomed = true;
                why = "the companion body no longer exists";
            }
            else if (seenBots.count(row.bot))
            {
                ++out.duplicate;
                doomed = true;
                why = "the same body is bound twice";
            }
            else if (legacyBound.count(row.bot))
            {
                ++out.doubleBound;
                out.notes.push_back(shown + " is also bound in wowlegends_companion");
            }

            if (!doomed)
            {
                seenBots.insert(row.bot);
                continue;
            }

            out.notes.push_back(shown + " (" + std::to_string(row.bot) + "): " + why);

            if (fix)
            {
                CharacterDatabase.DirectExecute("DELETE FROM {} WHERE owner_guid = {} AND bot_guid = {}",
                                                g_cfg.bindingTable, row.owner, row.bot);
                g_bindingCache.erase(row.owner);
                ++out.fixed;
            }
        }

        // ledger rows for bodies that are gone: harmless but misleading at 3am
        if (QueryResult ledger = CharacterDatabase.Query(
                "SELECT bot_guid FROM " + g_cfg.poolTable + " WHERE bot_guid NOT IN (SELECT guid FROM characters)"))
        {
            do
            {
                uint32 const botGuid = ledger->Fetch()[0].Get<uint32>();
                ++out.ledgerOrphans;
                out.notes.push_back("ledger entry for missing body " + std::to_string(botGuid));

                if (fix)
                {
                    CharacterDatabase.DirectExecute("DELETE FROM " + g_cfg.poolTable + " WHERE bot_guid = {}", botGuid);
                    ++out.fixed;
                }
            } while (ledger->NextRow());
        }

        // A body the ledger says is back in the pool must carry a pool name. The rename at
        // release time is written while the body is going offline, and the body's own logout
        // save has overwritten it with the player-facing name three times today - Soak, Healy
        // and Brandan all came back named after a completed release. Re-pooling here is
        // idempotent and does not care who wrote last.
        if (fix)
        {
            if (QueryResult stale = CharacterDatabase.Query(
                    "SELECT p.bot_guid FROM " + g_cfg.poolTable + " p "
                    "JOIN characters c ON c.guid = p.bot_guid "
                    "WHERE c.name <> CONCAT('Zz', p.bot_guid) AND c.online = 0 "
                    "AND p.bot_guid NOT IN (SELECT bot_guid FROM " + g_cfg.bindingTable + ") "
                    + LegacyBindingClause("p.bot_guid") + "LIMIT 20"))
            {
                do
                {
                    uint32 const guid = stale->Fetch()[0].Get<uint32>();

                    if (RenameBody(guid, PoolNameFor(guid)))
                    {
                        LOG_INFO("server.loading",
                                 "[CompanionQuestSync] re-pooled body {} - it came back from a "
                                 "release still carrying its player name", guid);
                        ++out.fixed;
                    }
                } while (stale->NextRow());
            }
        }

        // The ledger is a record, not state: nothing reads it after the release it describes.
        // Keeping a sensible window stops it growing without bound on a long-lived realm.
        if (fix)
        {
            CharacterDatabase.DirectExecute(
                "DELETE FROM " + g_cfg.poolTable + " WHERE released_at < NOW() - INTERVAL 180 DAY");

            // A companion that is not in the world must not hold a party slot - bound or not.
            // The first version of this excluded bound bodies, which is exactly the case that
            // happens after a restart: the bot is logged out, its group row survives, and the
            // party shows a companion that is not there. Only bot accounts qualify, and never
            // the leader, because a group without its leader is the group's own business.
            std::string const botAccounts = AddClassAccountCsv();

            if (!botAccounts.empty())
            {
                CharacterDatabase.Execute(
                    "DELETE gm FROM group_member gm "
                    "JOIN `groups` g ON g.guid = gm.guid "
                    "JOIN characters c ON c.guid = gm.memberGuid "
                    "WHERE c.online = 0 "
                    "AND gm.memberGuid <> g.leaderGuid "
                    "AND c.account IN (" + botAccounts + ")");

                // and a party of one is not a party: drop the husk so the client does not
                // open a group frame with a single member in it
                CharacterDatabase.Execute(
                    "DELETE gm FROM group_member gm "
                    "WHERE gm.guid IN (SELECT guid FROM ("
                    "    SELECT g.guid FROM `groups` g "
                    "    LEFT JOIN group_member m ON m.guid = g.guid "
                    "    GROUP BY g.guid HAVING COUNT(m.memberGuid) < 2) AS lonely)");

                CharacterDatabase.Execute(
                    "DELETE g FROM `groups` g "
                    "LEFT JOIN group_member m ON m.guid = g.guid "
                    "WHERE m.memberGuid IS NULL");
            }
        }

        return out;
    }

    /// Periodic repair: dangling references only, never a row that still points at two
    /// real characters. Runs rarely, because it is a full scan of our tables.
    void SweepIntegrity()
    {
        static uint32 nextRun = 0;

        uint32 const now = static_cast<uint32>(time(nullptr));
        if (nextRun && now < nextRun)
            return;

        nextRun = now + 600;

        IntegrityReport const report = RunIntegrity(true);

        if (report.fixed)
            LOG_INFO("server.loading",
                     "[CompanionQuestSync] integrity: repaired {} broken reference(s) of {} binding row(s) "
                     "({} owner gone, {} body gone, {} duplicate)",
                     report.fixed, report.rows, report.danglingOwner, report.danglingBot, report.duplicate);
    }

class CompanionQuestSyncWorldScript : public WorldScript
{
public:
    // Hooks are opt-in in this core (CALL_ENABLED_HOOKS): a WorldScript that
    // does not name them here never receives them, which silently made
    // ".reload config" a no-op for this module - every option change needed a
    // restart. Named now, so a config edit can be applied without downtime.
    CompanionQuestSyncWorldScript() : WorldScript("CompanionQuestSyncWorldScript",
        { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP, WORLDHOOK_ON_UPDATE }) { }

    void OnAfterConfigLoad(bool reload) override
    {
        // Both hooks fire once at startup (config load, then world startup);
        // announce only on the first one and on an explicit .reload config.
        Refresh(reload);
    }

    void OnStartup() override
    {
        Refresh(false);

        if (g_cfg.enable)
            LoadQuestGroundObjects();
    }

    void OnUpdate(uint32 diff) override
    {
        if (!g_cfg.enable || !g_tableReady)
            return;

        // Summon, dismiss and release outcomes, independent of the sync timer: a player who
        // asked for something should not wait for the next sweep to learn it did not work.
        CheckPendingSummons();
        CheckPendingReleases();
        SweepIntegrity();

        // Ground gathering runs on its own clock, not the pass's. A chest is a hop and a
        // loot, and the whole point is that the companion is seen taking it - on a thirty
        // second sweep the player stands in the field watching nothing happen, then sees a
        // burst, which reads as "delayed" however few items are involved.
        _gatherAccumulator += diff;

        if (_gatherAccumulator >= kGatherTickMs)
        {
            _gatherAccumulator = 0;

            FollowOwnerThroughTeleport();

            for (auto const& pair : sWorldSessionMgr->GetAllSessions())
            {
                WorldSession* session = pair.second;
                if (!session || session->IsBot() || !session->GetPlayer())
                    continue;

                Player* player = session->GetPlayer();

                std::vector<uint32> bots;
                if (!FindCompanions(player->GetGUID().GetCounter(), bots))
                    continue;

                uint32 const nowTs = static_cast<uint32>(time(nullptr));

                for (uint32 const botGuid : bots)
                {
                    Player* bot = FindCompanionPlayer(botGuid);
                    if (!bot || !bot->IsInWorld())
                    {
                        g_gatherPending.erase(botGuid);
                        continue;
                    }

                    // Walk the quest log every twenty seconds, not every two: the tick that
                    // follows is the expensive one, and most companions have no ground
                    // objective at all.
                    auto& gate = g_gatherPending[botGuid];

                    if (!gate.first && gate.second && nowTs - gate.second < Timing::WalkStep)
                        continue;

                    gate.second = nowTs;
                    gate.first = HasPendingGroundObjective(bot);

                    if (!gate.first)
                        continue;

                    std::string gathered;

                    if (GatherFromGround(player, bot, gathered))
                    {
                        if (g_cfg.debug >= 2)
                            LOG_INFO("server.loading", "[CompanionQuestSync] gathering for {}", gathered);

                        // Hand it in now instead of at the next pass: the quest just became
                        // complete, and the player is standing here watching.
                        DeliverHandIns(player, bot);

                        gate.first = HasPendingGroundObjective(bot);
                    }
                }
            }
        }

        if (!g_cfg.autoSync)
            return;

        // Heartbeat window (Debug >= 2): one summary line per window so a log
        // check can answer "is it doing anything" without waiting for a change.
        bool const heartbeat = g_cfg.debug >= 2 && g_cfg.heartbeatSeconds > 0;
        if (heartbeat)
        {
            g_hbTimer += diff;

            // Online time next to the pass count: seven passes in a 300 s window
            // looks like a stall until you know the player was there for 140 s.
            g_hbOnlineProbe += diff;
            if (g_hbOnlineProbe >= 1000)
            {
                g_hbOnlineProbe = 0;
                if (AnyPlayerInWorld())
                    g_hbOnlineMs += 1000;
            }
        }

        _accumulator += diff;
        uint32 const intervalMs = g_cfg.intervalSeconds * 1000;
        if (_accumulator < intervalMs)
            return;

        _accumulator = 0;

        bool synced = false;

        for (auto const& pair : sWorldSessionMgr->GetAllSessions())
        {
            WorldSession* session = pair.second;
            if (!session || session->IsBot())
                continue;

            Player* player = session->GetPlayer();
            if (!player || !player->IsInWorld())
                continue;

            SyncReport const report = SyncCompanion(player);
            if (!report.companionOnline)
                continue;

            synced = true;

            SessionTally& tally = g_session[player->GetGUID().GetCounter()];
            tally.added += report.added;
            tally.completed += report.completed;
            tally.reopened += report.reopened;
            // Count each quest once per session. The sweep re-checks a blocked quest
            // every 20 s, so adding skipped.size() per pass reported 23 "refusals" for
            // a single blocked quest - which made a quiet session look broken.
            std::set<uint32>& seenRefused = g_sessionRefused[player->GetGUID().GetCounter()];
            for (uint32 const refusedId : report.refusedIds)
                if (seenRefused.insert(refusedId).second)
                    ++tally.refused;
            if (report.levelFrom && !tally.levelFrom)
                tally.levelFrom = report.levelFrom;
            if (report.levelTo)
                tally.levelTo = report.levelTo;

            if (g_hbTimer >= g_cfg.heartbeatSeconds * 1000)
            {
                g_hbAdded += report.added;
                g_hbCompleted += report.completed;
                g_hbDropped += report.dropped;
                g_hbReopened += report.reopened;
                g_hbLocked += report.classRaceLocked;
                std::vector<std::string> lines = report.lines;

                if (lines.empty())
                    lines.push_back(DescribeReport(report));

                auto const hook = g_hbHookAdds.find(player->GetGUID().GetCounter());
                if (hook != g_hbHookAdds.end() && hook->second)
                    lines.back() += " (+" + std::to_string(hook->second) + " on accept)";

                // Several companions under one owner: keep them as separate lines so the
                // log never blends two bots' quest counts into one meaningless row.
                std::string joined;
                for (std::string const& entry : lines)
                    joined += (joined.empty() ? "" : " | ") + entry;

                g_hbLines[player->GetGUID().GetCounter()] = joined;
            }
        }

        if (synced)
            ++g_hbPasses;

        if (heartbeat && g_hbTimer >= g_cfg.heartbeatSeconds * 1000)
        {
            if (!g_hbPasses && !g_hbAdded && !g_hbCompleted && !g_hbDropped)
            {
                // Two different silences, and saying the wrong one is what made an
                // idle-but-healthy module look dead: a player IS in the world here,
                // there is simply no companion to reconcile (none bound, or dismissed).
                if (AnyPlayerInWorld())
                {
                    LOG_INFO("server.loading",
                             "[CompanionQuestSync] heartbeat: {}s window - a player is in the world and no companion is online; nothing to sync.",
                             g_cfg.heartbeatSeconds);
                }
                else
                {
                    // Nothing to reconcile because nobody was in the world: say so
                    // instead of a row of zeroes that reads like the module stalled.
                    LOG_INFO("server.loading", "[CompanionQuestSync] heartbeat: no players online in the last {}s - module idle.",
                             g_cfg.heartbeatSeconds);
                }
            }
            else
            {
                LOG_INFO("server.loading", "[CompanionQuestSync] heartbeat: {} pass(es) over {}s (player in world {}s) - {} added, {} completed, {} dropped, {} class/race locked.",
                         g_hbPasses, g_cfg.heartbeatSeconds, g_hbOnlineMs / 1000, g_hbAdded, g_hbCompleted, g_hbDropped, g_hbReopened, g_hbLocked);
            }

            for (auto const& line : g_hbLines)
                LOG_INFO("server.loading", "[CompanionQuestSync]   {}", line.second);

            g_hbTimer = 0;
            g_hbPasses = 0;
            g_hbAdded = g_hbCompleted = g_hbDropped = g_hbLocked = g_hbReopened = 0;
            g_hbHookAdds.clear();
            g_hbOnlineMs = 0;
            g_hbLines.clear();
        }
    }

private:
    void Refresh(bool reload)
    {
        LoadSettings();

        if (!g_cfg.enable)
            return;

        g_tableReady = CheckBindingTable();
        LoadWorldMapAreas();

        if (g_tableReady)
        {
            ProbeBindingTable();
            EnsurePoolLedger();
        }

        if (!g_tableReady)
        {
            if (!g_tableWarned || reload)
                LOG_INFO("server.loading", "[CompanionQuestSync] table '{}' not found - module idle (no companion system installed?).", g_cfg.bindingTable);

            g_tableWarned = true;
            return;
        }

        g_bindingCache.clear();

        if (!_announced || reload)
        {
            // Startup banner: same idea as mod-playerbots' "This server runs
            // with ..." line, printed to the console/log at boot so the module's
            // presence is visible without reading the config list.
            if (g_cfg.announce)
            {
                LOG_INFO("server.loading", " ");
                LOG_INFO("server.loading", "  +-----------------------------------------------------------+");
                LOG_INFO("server.loading", "  |  This server runs with mod-companion-questsync            |");
                LOG_INFO("server.loading", "  |  Companion quest + level sync     (.companionsync)        |");
                LOG_INFO("server.loading", "  +-----------------------------------------------------------+");
            }

            LOG_INFO("server.loading", "[CompanionQuestSync] enabled (auto {}, every {}s, level mode {}, completion sync {}, notify {}).",
                     g_cfg.autoSync ? "on" : "off", g_cfg.intervalSeconds, g_cfg.levelMode,
                     g_cfg.syncCompletion ? "on" : "off", g_cfg.notifySkipped);
            _announced = true;
        }
    }

    uint32 _accumulator = 0;
    uint32 _gatherAccumulator = 0;

    /// How often the gather step runs. Short enough that the companion is seen taking the
    /// object, long enough that a grid search for one spawn costs nothing.
    static constexpr uint32 kGatherTickMs = 2000;
    bool _announced = false;

    // Heartbeat window state (Debug >= 2).

    std::unordered_map<uint32, std::string> g_hbLines;
};


class CompanionQuestSyncCommandScript : public CommandScript
{
public:
    CompanionQuestSyncCommandScript() : CommandScript("CompanionQuestSyncCommandScript") { }

    // -----------------------------------------------------------------------
    // .companionadmin - the same behaviour, for a named player, from the console
    // -----------------------------------------------------------------------

    static std::vector<std::string> SplitArgs(char const* args, size_t want)
    {
        std::string text = args ? args : "";
        std::vector<std::string> tokens;
        size_t pos = 0;

        while (pos < text.size() && tokens.size() < want)
        {
            size_t const start = text.find_first_not_of(" \t", pos);
            if (start == std::string::npos)
                break;

            size_t const end = text.find_first_of(" \t", start);
            tokens.push_back(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
            pos = end == std::string::npos ? text.size() : end;
        }

        return tokens;
    }

    /// The player a console command acts on, online or not.
    ///
    /// Support work happens when people are not logged in, and so does testing: a release
    /// that only works while the owner is in the world is a release nobody can fix at 2am.
    /// When the player is offline the body is not dismissed first (there is nothing to
    /// dismiss) and the reset runs on the next tick.
    static Player* ConsoleTarget(ChatHandler* handler, std::string const& name, uint32& ownerGuid)
    {
        Player* player = ObjectAccessor::FindPlayerByName(name);
        if (player && player->IsInWorld())
        {
            ownerGuid = player->GetGUID().GetCounter();
            return player;
        }

        std::string escaped = name;
        CharacterDatabase.EscapeString(escaped);

        if (QueryResult row = CharacterDatabase.Query("SELECT guid FROM characters WHERE name = '{}'", escaped))
        {
            ownerGuid = row->Fetch()[0].Get<uint32>();
            handler->PSendSysMessage("|cffaaaaaaCompanion quest sync|r: {} is offline - acting on their saved companions.",
                                     name);
            return nullptr;
        }

        handler->PSendSysMessage("|cffff8000Companion quest sync|r: no character called {}.", name);
        return nullptr;
    }

    static Player* ConsoleTarget(ChatHandler* handler, std::string const& name)
    {
        uint32 ignored = 0;
        return ConsoleTarget(handler, name, ignored);
    }

    /// .companionadmin gear <player> - what each bound body is wearing, slot by slot.
    ///
    /// Added because "the companion is naked" could not be checked without a client:
    /// the equipment lives in the live entity, the database lags by a save interval, and
    /// asking the player to read a character sheet is not a test.
    static bool HandleAdminGear(ChatHandler* handler, char const* args)
    {
        std::vector<std::string> const a = SplitArgs(args, 1);
        if (a.empty())
        {
            handler->PSendSysMessage("|cffff8000usage|r: companionadmin gear <player>");
            return true;
        }

        uint32 ownerGuid = 0;
        ConsoleTarget(handler, a[0], ownerGuid);

        std::vector<uint32> bots;
        if (!ownerGuid || !FindCompanions(ownerGuid, bots))
        {
            handler->PSendSysMessage("|cffff8000Companion quest sync|r: {} has no companions.", a[0]);
            return true;
        }

        for (uint32 const botGuid : bots)
        {
            std::string const name = CompanionNameFor(botGuid);

            if (Player* bot = FindCompanionPlayer(botGuid); bot && bot->IsInWorld())
                handler->PSendSysMessage("  |cffffff00{}|r: {}", name, DescribeGear(bot));
            else
                handler->PSendSysMessage("  |cffffff00{}|r: not in the world", name);
        }

        return true;
    }

    /// .companionadmin dress <player> - run the upkeep that clothes a body, now.
    static bool HandleAdminDress(ChatHandler* handler, char const* args)
    {
        std::vector<std::string> const a = SplitArgs(args, 1);
        if (a.empty())
        {
            handler->PSendSysMessage("|cffff8000usage|r: companionadmin dress <player>");
            return true;
        }

        uint32 ownerGuid = 0;
        ConsoleTarget(handler, a[0], ownerGuid);

        std::vector<uint32> bots;
        if (!ownerGuid || !FindCompanions(ownerGuid, bots))
        {
            handler->PSendSysMessage("|cffff8000Companion quest sync|r: {} has no companions.", a[0]);
            return true;
        }

        for (uint32 const botGuid : bots)
        {
            std::string const name = CompanionNameFor(botGuid);
            Player* bot = FindCompanionPlayer(botGuid);

            if (!bot || !bot->IsInWorld())
            {
                handler->PSendSysMessage("  |cffffff00{}|r: not in the world, nothing done", name);
                continue;
            }

            EnsureStartingGear(nullptr, bot);
            handler->PSendSysMessage("  |cffffff00{}|r: {}", name, DescribeGear(bot));
        }

        return true;
    }

    /// .companionadmin integrity [fix]
    ///
    /// Reports every reference this module owns that no longer resolves: bindings whose
    /// owner or body was deleted, one body bound twice, a body also bound by
    /// mod-wowlegends, and ledger rows for bodies that are gone. With "fix" the broken rows
    /// are removed. Works from the console, because the mess is found when nobody is online.
    static bool HandleAdminIntegrity(ChatHandler* handler, char const* args)
    {
        std::string const mode = args ? TrimArg(args) : std::string();
        bool const fix = (mode == "fix");

        IntegrityReport const report = RunIntegrity(fix);

        handler->PSendSysMessage("|cff20ff20Companion quest sync|r: integrity over {} binding row(s){}",
                                 report.rows, fix ? ", repairing" : ", read only");

        handler->PSendSysMessage("  |cffaaaaaaowners gone: {}   bodies gone: {}   bound twice: {}   "
                                 "also in wowlegends_companion: {}   ledger orphans: {}|r",
                                 report.danglingOwner, report.danglingBot, report.duplicate,
                                 report.doubleBound, report.ledgerOrphans);

        for (std::string const& note : report.notes)
            handler->PSendSysMessage("  |cffff8000-|r {}", note);

        if (fix)
            handler->PSendSysMessage("  |cff20ff20{} row(s) removed|r", report.fixed);
        else if (report.danglingOwner || report.danglingBot || report.duplicate || report.ledgerOrphans)
            handler->PSendSysMessage("  |cffffd200run 'companionadmin integrity fix' to remove them|r");

        return true;
    }

    /// .companionadmin create <player> <race> <class> <name>
    static bool HandleAdminCreate(ChatHandler* handler, char const* args)
    {
        std::vector<std::string> const a = SplitArgs(args, 4);
        if (a.size() < 4)
        {
            handler->PSendSysMessage("|cffff8000usage|r: companionadmin create <player> <race> <class> <name>");
            return true;
        }

        // Works for an offline character: support and testing both happen when nobody is
        // logged in, and a console tool that only works in game is worth much less.
        uint32 ownerGuid = 0;
        Player* player = ConsoleTarget(handler, a[0], ownerGuid);
        if (!ownerGuid)
            return true;

        uint8 const race = ParseRaceToken(a[1]);
        uint8 const clazz = ParseClassToken(a[2]);
        if (!race || !clazz)
        {
            handler->PSendSysMessage("|cffff8000Companion quest sync|r: unknown race or class.");
            return true;
        }

        uint32 botGuid = 0;
        std::string error;
        if (!CreateCompanion(ownerGuid, a[0], race, clazz, a[3], botGuid, error))
        {
            handler->PSendSysMessage("|cffff8000Companion quest sync|r: {}.", error);
            return true;
        }

        if (player)
        {
            SendCompSync(player, true);
            SendPoolStock(player, true);
        }

        handler->PSendSysMessage("|cff20ff20Companion quest sync|r: created {} (body {}) for {}.",
                                 a[3], botGuid, a[0]);
        return true;
    }

    /// .companionadmin release <player> <companion>
    static bool HandleAdminRelease(ChatHandler* handler, char const* args)
    {
        std::vector<std::string> const a = SplitArgs(args, 2);
        if (a.size() < 2)
        {
            handler->PSendSysMessage("|cffff8000usage|r: companionadmin release <player> <companion>");
            return true;
        }

        uint32 ownerGuid = 0;
        Player* player = ConsoleTarget(handler, a[0], ownerGuid);
        if (!ownerGuid)
            return true;

        std::vector<uint32> bound;
        if (!FindCompanions(ownerGuid, bound))
        {
            handler->PSendSysMessage("|cffff8000Companion quest sync|r: {} has no companions.", a[0]);
            return true;
        }

        uint32 const botGuid = ResolveCompanionForOwner(ownerGuid, a[1], bound);
        if (!botGuid)
        {
            handler->PSendSysMessage("|cffff8000Companion quest sync|r: {} is not one of {}'s companions.",
                                     a[1], a[0]);
            return true;
        }

        std::string error;
        if (!BeginRelease(ownerGuid, a[0], botGuid, error))
        {
            handler->PSendSysMessage("|cffff8000Companion quest sync|r: {}.", error);
            return true;
        }

        handler->PSendSysMessage("|cff20ff20Companion quest sync|r: releasing {} (body {}) for {}.",
                                 a[1], botGuid, a[0]);
        return true;
    }

    /// .companionadmin pool <player> - push the stock line to that player's client.
    static bool HandleAdminPool(ChatHandler* handler, char const* args)
    {
        std::vector<std::string> const a = SplitArgs(args, 1);
        if (a.empty())
        {
            handler->PSendSysMessage("|cffff8000usage|r: companionadmin pool <player>");
            return true;
        }

        Player* player = ConsoleTarget(handler, a[0]);
        if (!player)
            return true;

        SendPoolStock(player, true);
        handler->PSendSysMessage("|cff20ff20Companion quest sync|r: pool stock sent to {}.", a[0]);
        return true;
    }

    /// Finish every ground-object quest a player's companions still owe. Shared by the
    /// player command (the addon's button) and the admin twin, so both run the same code.
    static void CatchUpForOwner(ChatHandler* handler, uint32 ownerGuid)
    {
        std::vector<uint32> bots;

        if (!g_cfg.enable || !ownerGuid || !FindCompanions(ownerGuid, bots))
        {
            handler->PSendSysMessage("|cffff8000Companion quest sync|r: no companion bound.");
            return;
        }

        uint32 total = 0;

        for (uint32 const botGuid : bots)
        {
            Player* bot = FindCompanionPlayer(botGuid);

            if (!bot || !bot->IsInWorld())
            {
                handler->PSendSysMessage("|cffff8000{}|r is not in the world - summon it first.", botGuid);
                continue;
            }

            uint32 forBot = 0;

            for (auto const& entry : bot->getQuestStatusMap())
            {
                Quest const* quest = sObjectMgr->GetQuestTemplate(entry.first);
                if (!quest || entry.second.Status != QUEST_STATUS_INCOMPLETE)
                    continue;

                uint32 const granted = GrantMissingGroundItems(bot, quest);
                if (!granted)
                    continue;

                forBot += granted;
                g_session[ownerGuid].gathered += granted;

                handler->PSendSysMessage("|cff20ff20{}|r: gathered the rest of '{}' ({} item(s)).",
                                         bot->GetName(), quest->GetTitle(), granted);
            }

            if (!forBot)
                handler->PSendSysMessage("|cffaaaaaa{}|r owes nothing that lies on the ground.", bot->GetName());

            total += forBot;
        }

        if (total)
            handler->PSendSysMessage("|cff20ff20Companion quest sync|r: {} item(s) gathered - the hand-in runs on the next pass.", total);
    }

    static bool HandleCatchUpCommand(ChatHandler* handler, char const* /*args*/)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        CatchUpForOwner(handler, player->GetGUID().GetCounter());
        return true;
    }

    /// Console form, so the same path is testable with nobody logged in (the companion
    /// still has to be in the world: objective counts live on the entity).
    static bool HandleAdminCatchUp(ChatHandler* handler, char const* args)
    {
        std::vector<std::string> const a = SplitArgs(args, 1);

        if (a.empty())
        {
            handler->PSendSysMessage("|cffff8000usage|r: companionadmin catchup <player>");
            return true;
        }

        uint32 ownerGuid = 0;
        ConsoleTarget(handler, a[0], ownerGuid);

        if (!ownerGuid)
        {
            handler->PSendSysMessage("|cffff8000{}|r is not a character here.", a[0]);
            return true;
        }

        CatchUpForOwner(handler, ownerGuid);
        return true;
    }

    static ChatCommandTable GetAdminCommands()
    {
        static ChatCommandTable adminTable =
        {
            { "create",  HandleAdminCreate,  SEC_ADMINISTRATOR, Console::Yes },
            { "release", HandleAdminRelease, SEC_ADMINISTRATOR, Console::Yes },
            { "pool",    HandleAdminPool,    SEC_ADMINISTRATOR, Console::Yes },
            { "integrity", HandleAdminIntegrity, SEC_ADMINISTRATOR, Console::Yes },
            { "gear",    HandleAdminGear,    SEC_ADMINISTRATOR, Console::Yes },
            { "dress",   HandleAdminDress,   SEC_ADMINISTRATOR, Console::Yes },
            { "catchup", HandleAdminCatchUp, SEC_ADMINISTRATOR, Console::Yes },
        };

        static ChatCommandTable baseTable =
        {
            { "companionadmin", adminTable },
        };

        return baseTable;
    }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable companionSyncTable =
        {
            { "pins",    HandlePinsCommand,   SEC_PLAYER, Console::No },
            { "refresh", HandleRefreshCommand, SEC_PLAYER, Console::No },
            { "quests", HandleQuestsCommand, SEC_PLAYER, Console::No },
            { "catchup", HandleCatchUpCommand, SEC_PLAYER, Console::No },
            { "status", HandleStatusCommand, SEC_PLAYER, Console::No },
            { "level",  HandleLevelCommand,  SEC_PLAYER, Console::No },
            { "create", HandleCreateCommand, SEC_PLAYER, Console::No },
            { "pool",   HandlePoolCommand,   SEC_PLAYER, Console::No },
            { "release", HandleReleaseCommand, SEC_PLAYER, Console::No },
            { "summon", HandleSummonCommand, SEC_PLAYER, Console::No },
            { "learn", HandleLearnCommand, SEC_PLAYER, Console::No },
            { "professions", HandleProfessionsCommand, SEC_PLAYER, Console::No },
            { "produce", HandleProduceCommand, SEC_PLAYER, Console::No },
            { "trade", HandleTradeCommand, SEC_PLAYER, Console::No },
            { "revive", HandleReviveCommand, SEC_PLAYER, Console::No },
            { "dismiss", HandleDismissCommand, SEC_PLAYER, Console::No },
            { "bind",   HandleBindCommand,   SEC_PLAYER, Console::No },
            { "unbind", HandleUnbindCommand, SEC_PLAYER, Console::No },
            { "list",   HandleListCommand,   SEC_PLAYER, Console::No },
            { "",       HandleSyncCommand,   SEC_PLAYER, Console::No },
        };

        static ChatCommandTable baseTable =
        {
            { "companionsync", companionSyncTable },
        };

        // admin verbs live in their own command so a player's table stays SEC_PLAYER
        for (ChatCommand const& admin : GetAdminCommands())
            baseTable.push_back(admin);

        return baseTable;
    }

    /// Trim what a player typed after the verb.
    static std::string TrimArg(char const* args)
    {
        std::string name = args ? args : "";
        name.erase(0, name.find_first_not_of(" \t"));
        if (!name.empty())
            name.erase(name.find_last_not_of(" \t") + 1);

        return name;
    }

    /// .companionsync bind <name> - make an online bot one of your companions.
    ///
    /// Only a bot that is in the world can be bound. Guids are pooled: binding one that is
    /// not running is a promise the module cannot keep, because the body can be released
    /// and re-issued to someone else, and the row follows the guid rather than the
    /// companion.
    static bool HandleBindCommand(ChatHandler* handler, char const* args)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        if (!g_cfg.enable || !g_tableReady)
        {
            handler->PSendSysMessage("|cffff8000Companion quest sync|r: disabled, or the binding table "
                                     "'{}' is missing.", g_cfg.bindingTable);
            return true;
        }

        std::string const name = TrimArg(args);
        if (name.empty())
        {
            handler->PSendSysMessage("|cffff8000Companion quest sync|r: which bot? "
                                     "|cffffff00.companionsync bind <name>|r");
            return true;
        }

        Player* bot = ObjectAccessor::FindPlayerByName(name);
        if (!bot || !bot->IsInWorld())
        {
            handler->PSendSysMessage("|cffff8000Companion quest sync|r: |cffffff00{}|r is not in the world. "
                                     "Bring it out first (|cffffff00.playerbots bot add {}|r), then bind it - "
                                     "a body that is not running cannot be promised.", name, name);
            return true;
        }

        if (bot == player || !bot->GetSession() || !bot->GetSession()->IsBot())
        {
            handler->PSendSysMessage("|cffff8000Companion quest sync|r: {} is not a bot character, so it cannot "
                                     "be a companion.", bot->GetName());
            return true;
        }

        uint32 const ownerGuid = player->GetGUID().GetCounter();
        uint32 const botGuid = bot->GetGUID().GetCounter();

        std::vector<uint32> bound;
        FindCompanions(ownerGuid, bound);

        if (std::find(bound.begin(), bound.end(), botGuid) != bound.end())
        {
            handler->PSendSysMessage("|cffff8000Companion quest sync|r: {} is already your companion.", bot->GetName());
            return true;
        }

        if (bound.size() >= kMaxCompanions)
        {
            handler->PSendSysMessage("|cffff8000Companion quest sync|r: you already have {} companions, which is the "
                                     "limit - a group of {} with you. |cffffff00.companionsync unbind <name>|r "
                                     "first.", uint32(kMaxCompanions), uint32(kMaxCompanions + 1));
            return true;
        }

        if (QueryResult other = CharacterDatabase.Query("SELECT owner_guid FROM {} WHERE bot_guid = {} AND owner_guid <> {}",
                                                        g_cfg.bindingTable, botGuid, ownerGuid))
        {
            handler->PSendSysMessage("|cffff8000Companion quest sync|r: {} is bound to someone else; ask them to "
                                     "release it first.", bot->GetName());
            return true;
        }

        if (g_bindingHasName)
        {
            std::string escaped = bot->GetName();
            CharacterDatabase.EscapeString(escaped);

            CharacterDatabase.Execute("INSERT INTO {} (owner_guid, bot_guid, companion_name) VALUES ({}, {}, '{}')",
                                      g_cfg.bindingTable, ownerGuid, botGuid, escaped);
        }
        else
        {
            CharacterDatabase.Execute("INSERT INTO {} (owner_guid, bot_guid) VALUES ({}, {})",
                                      g_cfg.bindingTable, ownerGuid, botGuid);
        }

        InvalidateCompanionGuid(ownerGuid);
        FindCompanions(ownerGuid, bound);

        LOG_INFO("server.loading", "[CompanionQuestSync] {} bound {} ({}) - {} of {} companions",
                 player->GetName(), bot->GetName(), botGuid, uint32(bound.size()), uint32(kMaxCompanions));

        // Catch the new companion up now rather than on the next tick: it may be standing
        // in a different zone with a different quest log, and the first thing the player
        // does is look at it.
        SyncCompanion(player);

        SendCompSync(player, true);

        handler->PSendSysMessage("|cff20ff20Companion quest sync|r: |cffffff00{}|r is now your companion "
                                 "({}/{}). Its quests follow yours from now on.",
                                 bot->GetName(), uint32(bound.size()), uint32(kMaxCompanions));
        return true;
    }

    /// .companionsync unbind <name> - stop tracking a companion. The bot keeps existing and
    /// keeps being yours; it just stops being reconciled.
    static bool HandleUnbindCommand(ChatHandler* handler, char const* args)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        std::string const name = TrimArg(args);
        if (name.empty())
        {
            handler->PSendSysMessage("|cffff8000Companion quest sync|r: which companion? "
                                     "|cffffff00.companionsync unbind <name>|r");
            return true;
        }

        uint32 const ownerGuid = player->GetGUID().GetCounter();

        std::vector<uint32> bound;
        FindCompanions(ownerGuid, bound);

        uint32 botGuid = 0;
        for (uint32 const candidate : bound)
        {
            Player* bot = FindCompanionPlayer(candidate);
            if (bot && bot->GetName() == name)
            {
                botGuid = candidate;
                break;
            }
        }

        if (!botGuid && g_bindingHasName)
        {
            std::string escaped = name;
            CharacterDatabase.EscapeString(escaped);

            if (QueryResult row = CharacterDatabase.Query("SELECT bot_guid FROM {} WHERE owner_guid = {} AND companion_name = '{}'",
                                                          g_cfg.bindingTable, ownerGuid, escaped))
                botGuid = row->Fetch()[0].Get<uint32>();
        }

        if (!botGuid)
        {
            handler->PSendSysMessage("|cffff8000Companion quest sync|r: |cffffff00{}|r is not one of your companions. "
                                     "|cffffff00.companionsync list|r shows who is.", name);
            return true;
        }

        CharacterDatabase.Execute("DELETE FROM {} WHERE owner_guid = {} AND bot_guid = {}",
                                  g_cfg.bindingTable, ownerGuid, botGuid);
        PurgeCompanionState(botGuid);
        InvalidateCompanionGuid(ownerGuid);

        LOG_INFO("server.loading", "[CompanionQuestSync] {} unbound {} ({}) - it stays a bot, it just stops following",
                 player->GetName(), name, botGuid);

        if (g_cfg.pinSync)
        {
            SendPinSync(player, true);
            SendCompSync(player, true);
        }

        handler->PSendSysMessage("|cff20ff20Companion quest sync|r: stopped tracking |cffffff00{}|r. The bot is "
                                 "still yours; it simply does not follow your quest log any more.", name);
        return true;
    }

    /// Race and class by name or id, so a player can type and the addon can send numbers.
    static uint8 ParseRaceToken(std::string token)
    {
        for (char& c : token)
            c = char(std::tolower(c));

        static std::map<std::string, uint8> const byName = {
            { "human", 1 }, { "orc", 2 }, { "dwarf", 3 }, { "nightelf", 4 }, { "undead", 5 },
            { "scourge", 5 }, { "tauren", 6 }, { "gnome", 7 }, { "troll", 8 },
            { "bloodelf", 10 }, { "draenei", 11 },
        };

        auto const itr = byName.find(token);
        if (itr != byName.end())
            return itr->second;

        uint32 const id = uint32(atoi(token.c_str()));
        return id && id <= 11 ? uint8(id) : 0;
    }

    static uint8 ParseClassToken(std::string token)
    {
        for (char& c : token)
            c = char(std::tolower(c));

        static std::map<std::string, uint8> const byName = {
            { "warrior", 1 }, { "paladin", 2 }, { "hunter", 3 }, { "rogue", 4 }, { "priest", 5 },
            { "deathknight", 6 }, { "dk", 6 }, { "shaman", 7 }, { "mage", 8 }, { "warlock", 9 },
            { "druid", 11 },
        };

        auto const itr = byName.find(token);
        if (itr != byName.end())
            return itr->second;

        uint32 const id = uint32(atoi(token.c_str()));
        return id && id <= 11 ? uint8(id) : 0;
    }

    /// .companionsync create <race> <class> <name> - mint a companion from the pool.
    static bool HandleCreateCommand(ChatHandler* handler, char const* args)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        if (!g_cfg.enable || !g_cfg.allowCreate)
        {
            handler->PSendSysMessage("|cffff8000Companion quest sync|r: creating companions is disabled here.");
            return true;
        }

        std::string text = args ? args : "";
        std::vector<std::string> tokens;
        size_t pos = 0;

        while (pos < text.size() && tokens.size() < 3)
        {
            size_t const start = text.find_first_not_of(" \t", pos);
            if (start == std::string::npos)
                break;

            size_t const end = text.find_first_of(" \t", start);
            tokens.push_back(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
            pos = end == std::string::npos ? text.size() : end;
        }

        if (tokens.size() < 3)
        {
            handler->PSendSysMessage("|cffff8000Companion quest sync|r: |cffffff00.companionsync create <race> <class> <name>|r "
                                     "- for example: create human priest Healy");
            return true;
        }

        uint8 const race = ParseRaceToken(tokens[0]);
        uint8 const clazz = ParseClassToken(tokens[1]);

        if (!race || !clazz)
        {
            handler->PSendSysMessage("|cffff8000Companion quest sync|r: I do not know that race or class. "
                                     "Races: human, orc, dwarf, nightelf, undead, tauren, gnome, troll, bloodelf, draenei. "
                                     "Classes: warrior, paladin, hunter, rogue, priest, deathknight, shaman, mage, warlock, druid.");
            return true;
        }

        uint32 botGuid = 0;
        std::string error;

        if (!CreateCompanion(player->GetGUID().GetCounter(), player->GetName(), race, clazz,
                             tokens[2], botGuid, error))
        {
            handler->PSendSysMessage("|cffff8000Companion quest sync|r: {}.", error);
            return true;
        }

        std::vector<uint32> bound;
        FindCompanions(player->GetGUID().GetCounter(), bound);

        SendCompSync(player, true);
        SendPoolStock(player, true);
        InvalidatePlayerName(player, botGuid);

        DispatchPlayerbots(player, "add " + tokens[2]);
        g_pendingSummon[botGuid] = { botGuid, player->GetGUID().GetCounter(), tokens[2],
                                     static_cast<uint32>(time(nullptr)) + 10, true };

        handler->PSendSysMessage("|cff20ff20Companion quest sync|r: |cffffff00{}|r is yours - {}/{} companions. "
                                 "Calling it to your side; its quests follow yours from now on.",
                                 tokens[2], uint32(bound.size()), uint32(kMaxCompanions));
        return true;
    }

    /// .companionsync pool - what the pool can still hand out, per race and class.
    static bool HandlePoolCommand(ChatHandler* handler, char const* /*args*/)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        SendPoolStock(player, true);

        std::string const line = BuildPoolStockLine();
        if (line.empty())
            handler->PSendSysMessage("|cffff8000Companion quest sync|r: the pool is empty right now.");

        return true;
    }

    /// .companionsync release <name> - hand a companion back to the pool for good.
    static bool HandleReleaseCommand(ChatHandler* handler, char const* args)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        std::string const name = TrimArg(args);
        if (name.empty())
        {
            handler->PSendSysMessage("|cffff8000Companion quest sync|r: which companion? "
                                     "|cffffff00.companionsync release <name>|r");
            return true;
        }

        std::vector<uint32> bound;
        if (!g_cfg.enable || !FindCompanions(player->GetGUID().GetCounter(), bound))
        {
            handler->PSendSysMessage("|cffff8000Companion quest sync|r: no companions bound.");
            return true;
        }

        uint32 const botGuid = ResolveCompanion(player, name, bound);
        if (!botGuid)
        {
            handler->PSendSysMessage("|cffff8000Companion quest sync|r: |cffffff00{}|r is not one of your companions.",
                                     name);
            return true;
        }

        std::string error;
        if (!BeginRelease(player->GetGUID().GetCounter(), player->GetName(), botGuid, error))
        {
            handler->PSendSysMessage("|cffff8000Companion quest sync|r: {}.", error);
            return true;
        }

        handler->PSendSysMessage("|cffffff00Companion quest sync|r: releasing |cffffff00{}|r - it leaves the world, "
                                 "is wiped back to level 1 and returns to the pool. This cannot be undone.",
                                 CompanionNameFor(botGuid));
        return true;
    }

    /// .companionsync summon [name] - bring a companion out, or every companion that is not
    /// out. No argument means "the whole group", which is what a player wants after a wipe.
    static bool HandleSummonCommand(ChatHandler* handler, char const* args)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        std::vector<uint32> bound;
        if (!g_cfg.enable || !FindCompanions(player->GetGUID().GetCounter(), bound))
        {
            handler->PSendSysMessage("|cffff8000Companion quest sync|r: no companions bound. "
                                     "|cffffff00.companionsync bind <name>|r adds one.");
            return true;
        }

        std::string const want = TrimArg(args);
        std::vector<uint32> targets;

        // ReportPosition advises "Summon me" to a companion left in another zone, and the
        // old command answered "already with you" - the one refusal that contradicted the
        // module's own advice and left the companion stranded with no remedy. An in-world
        // companion beyond 120 yd is RECALLED; only a body at your side earns that phrase.
        auto recall = [player](Player* bot) -> std::string
        {
            if (!bot || !bot->IsInWorld() || !bot->IsAlive())
                return "dead or absent";

            if (bot->IsInCombat())
                return "fighting";

            if (MapEntry const* entry = sMapStore.LookupEntry(player->GetMapId());
                entry && (entry->IsDungeon() || entry->IsRaid()))
                return "you are inside an instanced dungeon";

            if (bot->GetMapId() == player->GetMapId() && bot->GetDistance(player) <= 120.0f)
                return "";   // genuinely beside the player

            float x = player->GetPositionX() + 2.0f;
            float y = player->GetPositionY();
            float z = player->GetPositionZ();
            bot->UpdateGroundPositionZ(x, y, z);
            bot->StopMoving();

            if (bot->GetMapId() != player->GetMapId())
                bot->TeleportTo(player->GetMapId(), x, y, z, player->GetOrientation());
            else
                bot->NearTeleportTo(x, y, z, player->GetOrientation());

            LOG_INFO("server.loading", "[CompanionQuestSync] {} recalled to {} - was {} yd away",
                     bot->GetName(), player->GetName(),
                     bot->GetMapId() == player->GetMapId() ? uint32(bot->GetDistance(player)) : 0);

            return "";
        };

        if (want.empty())
        {
            for (uint32 const botGuid : bound)
            {
                Player* bot = FindCompanionPlayer(botGuid);
                if (!bot || !bot->IsInWorld())
                    targets.push_back(botGuid);
            }

            if (targets.empty())
            {
                uint32 recalled = 0;
                std::string blocked;

                for (uint32 const botGuid : bound)
                {
                    if (Player* bot = FindCompanionPlayer(botGuid))
                    {
                        std::string const why = recall(bot);
                        if (why.empty())
                            ++recalled;
                        else if (blocked.empty())
                            blocked = why;
                    }
                }

                if (recalled)
                {
                    handler->PSendSysMessage("|cff20ff20Companion quest sync|r: {} companion(s) recalled to your side.",
                                             recalled);
                    return true;
                }

                handler->PSendSysMessage("|cff20ff20Companion quest sync|r: all {} are already with you{}.",
                                         uint32(bound.size()),
                                         blocked.empty() ? std::string(".") : " (one could not be moved: " + blocked + ")");
                return true;
            }
        }
        else
        {
            uint32 const botGuid = ResolveCompanion(player, want, bound);
            if (!botGuid)
            {
                handler->PSendSysMessage("|cffff8000Companion quest sync|r: |cffffff00{}|r is not one of your companions. "
                                         "|cffffff00.companionsync list|r shows who is.", want);
                return true;
            }

            Player* bot = FindCompanionPlayer(botGuid);
            if (bot && bot->IsInWorld())
            {
                std::string const why = recall(bot);
                if (why.empty())
                {
                    handler->PSendSysMessage("|cff20ff20Companion quest sync|r: |cffffff00{}|r recalled to your side.",
                                             bot->GetName());
                    SayToOwner(bot, player, "I am here - recalled to your side.");
                }
                else if (why == "fighting")
                {
                    handler->PSendSysMessage("|cffff8000Companion quest sync|r: {} is fighting - I will not pull it "
                                             "out of combat. Try again once it is safe.", bot->GetName());
                }
                else if (why == "dead or absent")
                {
                    handler->PSendSysMessage("|cffff8000Companion quest sync|r: {} is dead - revive it, or release and "
                                             "re-summon once you are safe.", bot->GetName());
                }
                else
                {
                    handler->PSendSysMessage("|cffff8000Companion quest sync|r: {} cannot be recalled: {}.",
                                             bot->GetName(), why);
                }
                return true;
            }

            targets.push_back(botGuid);
        }

        uint32 const ownerGuid = player->GetGUID().GetCounter();
        uint32 const now = static_cast<uint32>(time(nullptr));

        for (uint32 const botGuid : targets)
        {
            std::string const name = CompanionNameFor(botGuid);

            // a body the core has never seen cannot be added; make it known first
            EnsureCharacterCache(botGuid, name);
            InvalidatePlayerName(player, botGuid);

            DispatchPlayerbots(player, "add " + name);
            g_pendingSummon[botGuid] = { botGuid, ownerGuid, name, now + 10, true };

            handler->PSendSysMessage("|cff20ff20Companion quest sync|r: calling |cffffff00{}|r to your side.", name);
        }

        return true;
    }

    /// .companionsync dismiss [name] - send a companion away, or all of them. A dismissed
    /// companion stays bound: its quest log keeps following the player's.
    /// .companionsync revive <name> - the window's button for a fallen companion.
    ///
    /// Beside the owner the body simply rises (at the half-strength a real resurrection
    /// would carry). Beyond reach the companion releases its spirit at the graveyard
    /// instead: its ghost makes its own way back, and the resurrect hook re-arms the
    /// follow, so it rejoins on its own. Either way one click, and the companion says
    /// what it is doing.
    /// Craft what the companion's own gathering already paid for. The recipe is found
    /// among the bot's OWN known spells by what it creates, its reagents are read from
    /// the spell itself, and only the shop-bought pieces are granted - gathered reagents
    /// are waited for, never conjured. Products land in bags, get used in play, and any
    /// overflow rides MailHome. Returns the lines for the chat report.
    static std::vector<std::string> ProduceForCompanion(Player* bot, Player* owner)
    {
        auto const pair = ClassProfessionPair(bot->getClass());
        std::vector<std::string> report;
        std::string made;
        std::string waiting;

        for (uint32 const product : ProductsOf(pair.second))
        {
            // the recipe: a spell the companion knows that creates this item
            uint32 spellId = 0;
            for (auto const& entry : bot->GetSpellMap())
            {
                SpellInfo const* info = sSpellMgr->GetSpellInfo(entry.first);
                if (!info)
                    continue;
                for (uint8 e = 0; e < 3; ++e)
                    if (info->Effects[e].Effect == SPELL_EFFECT_CREATE_ITEM &&
                        uint32(info->Effects[e].ItemType) == product)
                        spellId = entry.first;
            }

            if (!spellId)
            {
                waiting += (waiting.empty() ? "" : ", ") + ItemNameFor(product) + " (recipe not learned)";
                continue;
            }

            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);

            // reagents: grant the shop-bought pieces a small stack; gathered pieces wait
            for (uint8 r = 0; r < MAX_SPELL_REAGENTS; ++r)
            {
                uint32 const item = uint32(info->Reagent[r]);
                uint32 const need = info->ReagentCount[r];
                if (!item || !need || bot->GetItemCount(item) >= need)
                    continue;

                if (IsGatherableReagent(pair.first, item))
                {
                    waiting += (waiting.empty() ? "" : ", ") +
                               ItemNameFor(item) + " " + std::to_string(bot->GetItemCount(item)) + "/" + std::to_string(need);
                }
                else
                {
                    uint32 const give = std::min<uint32>(5, need * 2);
                    ItemPosCountVec dest;
                    if (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, item, give) == EQUIP_ERR_OK)
                        if (Item* bought = bot->StoreNewItem(dest, item, true))
                            bot->SendNewItem(bought, give, true, false, true);
                }
            }

            // up to two batches while the reagents hold out, then verify honestly
            uint32 const before = bot->GetItemCount(product);
            for (uint8 batch = 0; batch < 2; ++batch)
            {
                bool ready = true;
                for (uint8 r = 0; r < MAX_SPELL_REAGENTS && ready; ++r)
                {
                    uint32 const item = uint32(info->Reagent[r]);
                    uint32 const need = info->ReagentCount[r];
                    ready = !item || !need || bot->GetItemCount(item) >= need;
                }
                if (!ready)
                    break;

                bot->CastSpell(bot, spellId, true);
            }

            uint32 const produced = bot->GetItemCount(product) - before;
            if (produced)
            {
                if (!made.empty())
                    made += ", ";
                made += std::to_string(produced) + "x " + ItemNameFor(product);
            }
        }

        if (!made.empty())
        {
            report.push_back("made " + made);
            SayToOwner(bot, owner, "I made " + made + " - I will use what fits, the rest goes to your mailbox.", 30);
        }
        if (!waiting.empty())
        {
            report.push_back("waiting on " + waiting);
            SayToOwner(bot, owner, "I am waiting on " + waiting + " - I will keep gathering.", 30);
        }
        if (report.empty())
        {
            report.push_back("nothing to produce");
            SayToOwner(bot, owner, "I have nothing I can craft just now - I keep an eye on what I gather.", 30);
        }

        return report;
    }

    /// .companionsync trade <name> - the card's Trade button.
    ///
    /// The companion opens a trade with the owner and fills it with its surplus: every
    /// gathered mat and produced item above the five it keeps for itself, biggest stacks
    /// first, six slots (the trade window's own limit). The owner accepts; the bot's
    /// accept-trade trigger answers on its side. Nothing else in its bags - gear, quest
    /// items, food - is ever offered.
    static bool HandleTradeCommand(ChatHandler* handler, char const* args)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        if (!g_cfg.enable)
        {
            handler->PSendSysMessage("|cffff8000Companion quest sync|r: the module is disabled.");
            return true;
        }

        std::string const want = TrimArg(args);
        if (want.empty())
        {
            handler->PSendSysMessage("|cffff8000usage|r: .companionsync trade <name>");
            return true;
        }

        std::vector<uint32> bound;
        if (!FindCompanions(player->GetGUID().GetCounter(), bound))
        {
            handler->PSendSysMessage("|cffff8000Companion quest sync|r: no companions bound.");
            return true;
        }

        uint32 const botGuid = ResolveCompanion(player, want, bound);
        if (!botGuid)
        {
            handler->PSendSysMessage("|cffff8000Companion quest sync|r: |cffffff00{}|r is not one of your companions.", want);
            return true;
        }

        Player* bot = FindCompanionPlayer(botGuid);
        if (!bot || !bot->IsInWorld() || !bot->IsAlive())
        {
            handler->PSendSysMessage("|cffff8000Companion quest sync|r: {} is not standing in the world.", CompanionNameFor(botGuid));
            return true;
        }

        if (bot->IsInCombat())
        {
            SayToOwner(bot, player, "I am in the middle of something - ask me when it is calm.", 30);
            return true;
        }

        if (bot->GetDistance(player) > 11.0f)
        {
            SayToOwner(bot, player, "Come a little closer and I will hand it over.", 30);
            return true;
        }

#ifdef MOD_PLAYERBOTS
        if (bot->GetTrader())
        {
            SayToOwner(bot, player, "I am already in a trade - give me a moment.", 30);
            return true;
        }

        auto const pair = ClassProfessionPair(bot->getClass());

        // five of every produced item and gathered mat stays in its own bags - the
        // reserve is consumed across every stack of the same item, not per stack
        std::map<uint32, uint32> keepLeft;
        for (uint32 const product : ProductsOf(pair.second))
            keepLeft[product] = 5;
        for (uint32 const id : GatheredReagentsOf(pair.first))
            keepLeft[id] = 5;

        std::vector<std::pair<Item*, uint32>> offers;
        auto consider = [&](Item* item)
        {
            if (!item || !item->CanBeTraded())
                return;

            auto const kept = keepLeft.find(item->GetEntry());
            if (kept == keepLeft.end())
                return;                                       // never its gear, quest items or food

            uint32 const count = item->GetCount();
            uint32 const spare = count > kept->second ? count - kept->second : 0;
            kept->second -= std::min(count, kept->second);
            if (!spare)
                return;

            // SetItem trades the WHOLE stack: the reserve sits inside this stack, so the
            // spare is split into its own stack first and that stack goes in the trade
            Item* tradeItem = item;
            if (spare < count)
            {
                uint16 const dstPos = FindFreeBagPos(bot);
                if (!dstPos)
                    return;            // no room to keep the reserve whole - the stack waits

                bot->SplitItem(item->GetPos(), dstPos, spare);
                tradeItem = bot->GetItemByPos(dstPos);
                if (!tradeItem)
                    return;
            }
            offers.push_back({ tradeItem, spare });
        };

        for (uint8 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
            consider(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i));

        for (uint8 i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; ++i)
            if (Bag* bag = bot->GetBagByPos(i))
                for (uint8 j = 0; j < bag->GetBagSize(); ++j)
                    consider(bot->GetItemByPos(i, j));

        std::sort(offers.begin(), offers.end(),
                  [](auto const& a, auto const& b) { return a.second > b.second; });

        if (offers.empty())
        {
            SayToOwner(bot, player, "I have nothing spare to hand you - I keep what I gather for the road.", 30);
            return true;
        }

        // the trade window holds six stacks; the rest waits for another press
        if (offers.size() > 6)
            offers.resize(6);

        WorldPacket pInit(CMSG_INITIATE_TRADE);
        pInit << player->GetGUID();
        bot->GetSession()->HandleInitiateTradeOpcode(pInit);

        TradeData* td = bot->GetTradeData();
        if (!td)
        {
            SayToOwner(bot, player, "The trade did not open - try again in a moment.", 30);
            return true;
        }

        uint32 offered = 0;
        for (uint8 slot = 0; slot < offers.size(); ++slot)
        {
            td->SetItem(TradeSlots(slot), offers[slot].first);
            ++offered;
        }

        if (!offered)
        {
            SayToOwner(bot, player, "Nothing would go in the trade - try again in a moment.", 30);
            return true;
        }

        SayToOwner(bot, player, "I put " + std::to_string(offered) +
                   " stack(s) in the trade - the rest stays for the road. Accept when you are ready.", 30);
#endif

        return true;
    }

    /// A free bag position for a split stack, packed the way SplitItem wants it.
    static uint16 FindFreeBagPos(Player* bot)
    {
        for (uint16 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
            if (!bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
                return uint16((INVENTORY_SLOT_BAG_0 << 8) | i);
        for (uint16 i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; ++i)
            if (Bag* bag = bot->GetBagByPos(i))
                for (uint16 j = 0; j < bag->GetBagSize(); ++j)
                    if (!bot->GetItemByPos(i, j))
                        return uint16((i << 8) | j);
        return 0;
    }

    /// .companionsync produce [name] - the card's Produce button.
    ///
    /// One click makes the companion work its own gathering: smelt its ore, cut its
    /// stones, brew what its herbs are for. Shop-bought reagents are granted in small
    /// stacks (the module standing in for the shopping trip); gathered reagents are
    /// waited for. Products are used in play and the overflow rides MailHome.
    static bool HandleProduceCommand(ChatHandler* handler, char const* args)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        if (!g_cfg.enable)
        {
            handler->PSendSysMessage("|cffff8000Companion quest sync|r: the module is disabled.");
            return true;
        }

        std::string const want = TrimArg(args);
        if (want.empty())
        {
            handler->PSendSysMessage("|cffff8000usage|r: .companionsync produce <name>");
            return true;
        }

        std::vector<uint32> bound;
        if (!FindCompanions(player->GetGUID().GetCounter(), bound))
        {
            handler->PSendSysMessage("|cffff8000Companion quest sync|r: no companions bound.");
            return true;
        }

        uint32 const botGuid = ResolveCompanion(player, want, bound);
        if (!botGuid)
        {
            handler->PSendSysMessage("|cffff8000Companion quest sync|r: |cffffff00{}|r is not one of your companions.", want);
            return true;
        }

        Player* bot = FindCompanionPlayer(botGuid);
        if (!bot || !bot->IsInWorld() || !bot->IsAlive())
        {
            handler->PSendSysMessage("|cffff8000Companion quest sync|r: {} is not standing in the world.", CompanionNameFor(botGuid));
            return true;
        }

        if (bot->IsInCombat())
        {
            SayToOwner(bot, player, "I am in the middle of something - ask me when it is calm.", 30);
            return true;
        }

#ifdef MOD_PLAYERBOTS
        for (std::string const& line : ProduceForCompanion(bot, player))
            handler->PSendSysMessage("|cffc94d{}|r: {}.", bot->GetName(), line);
#endif

        return true;
    }

    /// .companionsync learn [name] - the window's Train button.
    ///
    /// One click parses every companion: teach what a trainer would at the level the
    /// companion already has, and spend its unspent talent points along its spec.
    /// Companions raised before the teaching fix are the reason this exists - a raise
    /// now brings its own spells, but what was missed while it did not stays missing
    /// until something parses for it.
    /// .companionsync professions [name] - the window's Professions button.
    ///
    /// One click parses every companion: teach the class-appropriate pair it does not
    /// have, re-cap what a level raise un-capped, hand the gathering tools, and make
    /// sure the gather strategy is on. The chat summary answers at a glance: the pair,
    /// each gathered skill against its ceiling.
    static bool HandleProfessionsCommand(ChatHandler* handler, char const* args)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        if (!g_cfg.enable)
        {
            handler->PSendSysMessage("|cffff8000Companion quest sync|r: the module is disabled.");
            return true;
        }

        std::string const want = TrimArg(args);
        std::vector<uint32> bound;
        if (!FindCompanions(player->GetGUID().GetCounter(), bound))
        {
            handler->PSendSysMessage("|cffff8000Companion quest sync|r: no companions bound.");
            return true;
        }

        std::vector<uint32> targets;
        if (want.empty())
        {
            for (uint32 const botGuid : bound)
                if (Player* bot = FindCompanionPlayer(botGuid); bot && bot->IsInWorld())
                    targets.push_back(botGuid);
        }
        else
        {
            uint32 const botGuid = ResolveCompanion(player, want, bound);
            if (!botGuid)
            {
                handler->PSendSysMessage("|cffff8000Companion quest sync|r: |cffffff00{}|r is not one of your companions.", want);
                return true;
            }
            if (Player* bot = FindCompanionPlayer(botGuid); bot && bot->IsInWorld())
                targets.push_back(botGuid);
        }

        if (targets.empty())
        {
            handler->PSendSysMessage("|cffffc94dCompanion quest sync|r: nobody in the world to teach.");
            return true;
        }

        for (uint32 const botGuid : targets)
        {
            Player* bot = FindCompanionPlayer(botGuid);
            if (!bot || !bot->IsInWorld() || !bot->IsAlive())
                continue;

#ifdef MOD_PLAYERBOTS
            std::string const tookUp = TeachProfessions(bot, player);

            auto const pair = ClassProfessionPair(bot->getClass());
            std::string const a = ProfessionOf(pair.first).name;
            std::string const b = ProfessionOf(pair.second).name;
            std::string const aNew = tookUp.find(a) == std::string::npos ? "" : " (new)";
            std::string const bNew = tookUp.find(b) == std::string::npos ? "" : " (new)";
            handler->PSendSysMessage("|cff20ff20{}|r: {}{} {}, {}{} {} - gathering on.",
                                     bot->GetName(),
                                     a, aNew, bot->GetSkillValue(pair.first),
                                     b, bNew, bot->GetSkillValue(pair.second));
#endif
        }

        return true;
    }

    static bool HandleLearnCommand(ChatHandler* handler, char const* args)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        if (!g_cfg.enable)
        {
            handler->PSendSysMessage("|cffff8000Companion quest sync|r: the module is disabled.");
            return true;
        }

        std::string const want = TrimArg(args);
        std::vector<uint32> bound;
        if (!FindCompanions(player->GetGUID().GetCounter(), bound))
        {
            handler->PSendSysMessage("|cffff8000Companion quest sync|r: no companions bound.");
            return true;
        }

        std::vector<uint32> targets;
        if (want.empty())
        {
            for (uint32 const botGuid : bound)
                if (Player* bot = FindCompanionPlayer(botGuid); bot && bot->IsInWorld())
                    targets.push_back(botGuid);
        }
        else
        {
            uint32 const botGuid = ResolveCompanion(player, want, bound);
            if (!botGuid)
            {
                handler->PSendSysMessage("|cffff8000Companion quest sync|r: |cffffff00{}|r is not one of your companions.", want);
                return true;
            }
            if (Player* bot = FindCompanionPlayer(botGuid); bot && bot->IsInWorld())
                targets.push_back(botGuid);
        }

        if (targets.empty())
        {
            handler->PSendSysMessage("|cffffc94dCompanion quest sync|r: nobody in the world to teach.");
            return true;
        }

        uint32 totalLearned = 0;
        uint32 totalSpent = 0;
        uint32 taught = 0;

        for (uint32 const botGuid : targets)
        {
            Player* bot = FindCompanionPlayer(botGuid);
            if (!bot || !bot->IsInWorld() || !bot->IsAlive())
                continue;

#ifdef MOD_PLAYERBOTS
            auto const [learned, spent] = TeachTrainerContent(bot, player);
            totalLearned += learned;
            totalSpent += spent;
            if (learned || spent)
                ++taught;
            handler->PSendSysMessage("|cffffc94d{}|r: {}.", bot->GetName(),
                (learned || spent)
                    ? ("learned " + std::to_string(learned) + " spell(s), spent " + std::to_string(spent) + " talent point(s)")
                    : std::string("already fully trained"));
#endif
        }

        if (totalLearned || totalSpent)
            handler->PSendSysMessage("|cff20ff20Companion quest sync|r: {} companion(s) caught up - {} spell(s), {} talent point(s).",
                                     taught, totalLearned, totalSpent);
        else
            handler->PSendSysMessage("|cff20ff20Companion quest sync|r: everybody already knows what a trainer can teach.");

        return true;
    }

    static bool HandleReviveCommand(ChatHandler* handler, char const* args)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        if (!g_cfg.enable)
        {
            handler->PSendSysMessage("|cffff8000Companion quest sync|r: the module is disabled.");
            return true;
        }

        std::string const want = TrimArg(args);
        if (want.empty())
        {
            handler->PSendSysMessage("|cffff8000usage|r: .companionsync revive <name>");
            return true;
        }

        std::vector<uint32> bound;
        if (!FindCompanions(player->GetGUID().GetCounter(), bound))
        {
            handler->PSendSysMessage("|cffff8000Companion quest sync|r: no companions bound.");
            return true;
        }

        uint32 const botGuid = ResolveCompanion(player, want, bound);
        if (!botGuid)
        {
            handler->PSendSysMessage("|cffff8000Companion quest sync|r: |cffffff00{}|r is not one of your companions.", want);
            return true;
        }

        Player* bot = FindCompanionPlayer(botGuid);
        if (!bot || !bot->IsInWorld())
        {
            handler->PSendSysMessage("|cffff8000Companion quest sync|r: {} is not in the world.", CompanionNameFor(botGuid));
            return true;
        }

        if (bot->IsAlive())
        {
            SayToOwner(bot, player, "I am still standing - we go on.", 30);
            return true;
        }

        bool const isGhost = bot->HasAuraType(SPELL_AURA_GHOST);

        if (!isGhost && bot->GetDistance(player) <= 40.0f)
        {
            bot->ResurrectPlayer(0.5f);
            SayToOwner(bot, player, "Back on my feet - we go on.", 30);
            handler->PSendSysMessage("|cff20ff20Companion quest sync|r: {} rises beside you.", bot->GetName());
        }
        else if (!isGhost)
        {
            // Beyond reach: release the spirit exactly the way the client's own Release
            // button does (BuildPlayerRepop -> graveyard), and let the ghost make its
            // own way home - the resurrect hook re-arms the follow, so it rejoins alone.
            bot->RemovePet(nullptr, PET_SAVE_NOT_IN_SLOT, true);
            bot->BuildPlayerRepop();
            bot->RepopAtGraveyard();
            SayToOwner(bot, player, "I could not rise here - I will make my way back from the graveyard.", 30);
            handler->PSendSysMessage("|cffffc94dCompanion quest sync|r: {} was too far - its spirit released at the graveyard.", bot->GetName());
        }
        else
        {
            SayToOwner(bot, player, "I make my way back from the graveyard - I will join you when I rise.", 30);
            handler->PSendSysMessage("|cffffc94dCompanion quest sync|r: {} is already a ghost on its way back.", bot->GetName());
        }

        return true;
    }

    static bool HandleDismissCommand(ChatHandler* handler, char const* args)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        std::vector<uint32> bound;
        if (!g_cfg.enable || !FindCompanions(player->GetGUID().GetCounter(), bound))
        {
            handler->PSendSysMessage("|cffff8000Companion quest sync|r: no companions bound.");
            return true;
        }

        std::string const want = TrimArg(args);
        std::vector<uint32> targets;

        if (want.empty())
        {
            for (uint32 const botGuid : bound)
                if (Player* bot = FindCompanionPlayer(botGuid); bot && bot->IsInWorld())
                    targets.push_back(botGuid);

            if (targets.empty())
            {
                handler->PSendSysMessage("|cff20ff20Companion quest sync|r: none of them are with you.");
                return true;
            }
        }
        else
        {
            uint32 const botGuid = ResolveCompanion(player, want, bound);
            if (!botGuid)
            {
                handler->PSendSysMessage("|cffff8000Companion quest sync|r: |cffffff00{}|r is not one of your companions. "
                                         "|cffffff00.companionsync list|r shows who is.", want);
                return true;
            }

            Player* bot = FindCompanionPlayer(botGuid);
            if (!bot || !bot->IsInWorld())
            {
                handler->PSendSysMessage("|cff20ff20Companion quest sync|r: {} is not with you.", CompanionNameFor(botGuid));
                return true;
            }

            targets.push_back(botGuid);
        }

        uint32 const ownerGuid = player->GetGUID().GetCounter();
        uint32 const now = static_cast<uint32>(time(nullptr));

        for (uint32 const botGuid : targets)
        {
            std::string const name = CompanionNameFor(botGuid);

            // Give playerbots time to finish the login before pulling the body back out. A
            // removal issued seconds after a summon has killed this process three times today.
            uint32 const upAt = g_botUpAt.count(botGuid) ? g_botUpAt[botGuid] : 0;
            if (upAt && now - upAt < Timing::BotRemoveSettle)
            {
                uint32 const wait = 20 - (now - upAt);
                handler->PSendSysMessage("|cffffd200Companion quest sync|r: |cffffff00{}|r is still "
                                         "settling after the summon - give it {}s and ask again.",
                                         name, wait);
                continue;
            }

            // Inline, from the handler - the path mod-wowlegends uses for its own dismiss.
            // Logged either side: when this path took the process down there was no
            // dump and nothing in the log, so the boundary has to be visible.
            LOG_INFO("server.loading", "[CompanionQuestSync] dismiss: dispatching remove {} ({})",
                     name, botGuid);

            DispatchPlayerbots(player, "remove " + name);

            LOG_INFO("server.loading", "[CompanionQuestSync] dismiss: remove {} dispatched", name);
            g_pendingSummon[botGuid] = { botGuid, ownerGuid, name, now + 10, false };

            handler->PSendSysMessage("|cff20ff20Companion quest sync|r: |cffffff00{}|r stands down - "
                                     "|cffffff00.companionsync summon {}|r calls it back.", name, name);
        }

        return true;
    }

    /// .companionsync list - what is bound, and what each companion is doing.
    static bool HandleListCommand(ChatHandler* handler, char const* /*args*/)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        std::vector<uint32> bound;
        if (!g_cfg.enable || !FindCompanions(player->GetGUID().GetCounter(), bound))
        {
            handler->PSendSysMessage("|cffff8000Companion quest sync|r: no companions bound. "
                                     "|cffffff00.companionsync bind <name>|r adds one.");
            return true;
        }

        handler->PSendSysMessage("|cff20ff20Companion quest sync|r: {} of {} bound.", uint32(bound.size()), uint32(kMaxCompanions));

        for (uint32 const botGuid : bound)
        {
            Player* bot = FindCompanionPlayer(botGuid);
            if (!bot || !bot->IsInWorld())
            {
                handler->PSendSysMessage("  |cffaaaaaa?|r  guid {} - not in the world (summon it)", botGuid);
                continue;
            }

            Group* group = player->GetGroup();
            bool const grouped = group && group->IsMember(bot->GetGUID());

            handler->PSendSysMessage("  |cffffff00{}|r  level {}, {} quest(s), {}{}{}",
                                     bot->GetName(), uint32(bot->GetLevel()), uint32(CountQuestLog(bot)),
                                     grouped ? "in your group" : "|cffff8000not in your group|r",
                                     g_cfg.requireGrouped && !grouped ? " - skipped until it is" : "",
                                     bot->GetLevel() != player->GetLevel() ? ", |cffffd200level differs - .companionsync level|r" : "");
        }

        return true;
    }

    static bool HandleSyncCommand(ChatHandler* handler, char const* /*args*/)
    {
        if (!g_cfg.enable)
        {
            handler->PSendSysMessage("|cffff8000Companion quest sync|r is disabled in the server configuration.");
            return true;
        }

        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        SyncReport const report = SyncCompanion(player);
        PrintReport(handler, player, report, true);
        return true;
    }

    /// .companionsync pins - resend the reconciliation on demand, which is the fix
    /// for "the map says one thing and the companion another" without a relog.
    /// .companionsync refresh - push the current state to an addon and say nothing.
    ///
    /// The client cannot ask for state, so it only ever knew what the server happened to
    /// push - which is on change and on a five-minute heartbeat. A panel left open showed a
    /// companion that had been dismissed ten seconds earlier until the player closed and
    /// reopened it. This is the cheap, silent answer the panel polls while it is visible.
    static bool HandleRefreshCommand(ChatHandler* handler, char const* /*args*/)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player || !g_cfg.enable)
            return false;

        // Not forced: the panel asks every 15 seconds, and a forced send pushed two full
        // payloads whether or not anything had changed. Left unforced, the server's own
        // change detection answers "did the roster move" and stays silent when it did not.
        SendPinSync(player, false);
        SendCompSync(player, false);

        return true;
    }

    static bool HandlePinsCommand(ChatHandler* handler, char const* /*args*/)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        uint32 botGuid = 0;
        if (!g_cfg.enable || !FindCompanionGuid(player->GetGUID().GetCounter(), botGuid))
        {
            handler->PSendSysMessage("|cffff8000Companion quest sync|r: no companion bound.");
            return true;
        }

        std::vector<uint32> bots;
        FindCompanions(player->GetGUID().GetCounter(), bots);

        uint32 online = 0;
        for (uint32 const guid : bots)
        {
            Player* bot = FindCompanionPlayer(guid);
            if (bot && bot->IsInWorld())
                ++online;
        }

        if (!online)
        {
            handler->PSendSysMessage("|cffff8000Companion quest sync|r: your companion is not online.");
            return true;
        }

        SendPinSync(player, true);
        handler->PSendSysMessage("|cff20ff20Companion quest sync|r: map pins resent - the addon rebuilds from that line.");
        return true;
    }

    /// .companionsync quests - what the companion is actually carrying.
    ///
    /// The log answers this for whoever reads the log. The player has no window into a
    /// character that is not theirs, so without this the only way to learn that the
    /// companion is two candles short of a hand-in is to wait for a whisper.
    /// .companionsync quests - what every bound companion is carrying.
    ///
    /// The log answers this for whoever reads the log. The player has no window into a
    /// character that is not theirs, so without this the only way to learn that a
    /// companion is two candles short of a hand-in is to wait for a whisper. Walks the
    /// whole binding, so four companions are one command.
    static bool HandleQuestsCommand(ChatHandler* handler, char const* /*args*/)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        std::vector<uint32> bots;
        if (!g_cfg.enable || !FindCompanions(player->GetGUID().GetCounter(), bots))
        {
            handler->PSendSysMessage("|cffff8000Companion quest sync|r: no companion bound.");
            return true;
        }

        for (uint32 const botGuid : bots)
        {
            Player* bot = FindCompanionPlayer(botGuid);
            if (!bot || !bot->IsInWorld())
            {
                handler->PSendSysMessage("|cffff8000{}|r is not online.", CompanionNameFor(botGuid));
                continue;
            }

            handler->PSendSysMessage("|cff20ff20{}|r is carrying {} quest(s), {} free bag slot(s):",
                                     bot->GetName(), CountQuestLog(bot), uint32(bot->GetFreeInventorySpace()));

            uint32 listed = 0;

            for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            {
                uint32 const questId = bot->GetQuestSlotQuestId(slot);
                if (!questId)
                    continue;

                Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
                if (!quest)
                    continue;

                ++listed;

                QuestStatus const status = bot->GetQuestStatus(questId);
                std::string const owed = DescribeMissingItems(bot, quest);

                std::string line = "  " + quest->GetTitle() + " (" + std::to_string(questId) + ") ";

                if (status == QUEST_STATUS_FAILED)
                    line += "|cffff6060failed|r";
                else if (status == QUEST_STATUS_COMPLETE)
                    line += owed.empty() ? "|cff20ff20ready to hand in|r" : "|cffd0a060complete, still owes|r";
                else
                    line += "in progress";

                if (!owed.empty())
                    line += " - owes " + owed;
                else if (status != QUEST_STATUS_COMPLETE)
                    line += " - everything gathered";

                EnderSpot const& spot = FindQuestEnder(bot, questId);
                if (spot.found)
                {
                    if (spot.zone && spot.zone != bot->GetZoneId())
                        line += " - hand in at " + spot.name + " in " + ZoneName(spot.zone);
                    else if (status == QUEST_STATUS_COMPLETE)
                        line += " - hand in at " + spot.name + " (" +
                                std::to_string(uint32(bot->GetDistance(spot.x, spot.y, spot.z))) + " yd)";
                }

                std::string const area = PoiCoords(bot, quest);
                if (!area.empty())
                    line += " [" + area + "]";

                handler->SendSysMessage(line);
            }

            if (!listed)
                handler->PSendSysMessage("  nothing open - it is waiting for you to pick something up.");
        }

        return true;
    }

    static bool HandleStatusCommand(ChatHandler* handler, char const* /*args*/)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        uint32 botGuid = 0;
        if (!g_cfg.enable || !FindCompanionGuid(player->GetGUID().GetCounter(), botGuid))
        {
            handler->PSendSysMessage("|cffff8000Companion quest sync|r: no companion bound.");
            return true;
        }

        Player* bot = FindCompanionPlayer(botGuid);
        if (!bot || !bot->IsInWorld())
        {
            handler->PSendSysMessage("|cffff8000Companion quest sync|r: companion guid {} is offline.", botGuid);
            return true;
        }

        uint32 ownerQuests = 0;
        for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            if (player->GetQuestSlotQuestId(slot))
                ++ownerQuests;

        uint32 botQuests = 0;
        for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            if (bot->GetQuestSlotQuestId(slot))
                ++botQuests;

                std::vector<uint32> bound;
        FindCompanions(player->GetGUID().GetCounter(), bound);
        if (bound.size() > 1)
            handler->PSendSysMessage("|cff20ff20Companion quest sync|r: {} companions bound - this line covers {}; |cffffff00.companionsync quests|r covers them all.",
                                     bound.size(), bot->GetName());

        handler->PSendSysMessage("|cff20ff20Companion quest sync|r: |cffffff00{}|r level {} with {} quest(s); you are level {} with {} quest(s). Auto sync {}, level mode {}. |cffffff00.companionsync quests|r lists them.",
                                 bot->GetName(), uint32(bot->GetLevel()), botQuests,
                                 uint32(player->GetLevel()), ownerQuests,
                                 g_cfg.autoSync ? "on" : "off", g_cfg.levelMode);

        // What the companion actually did this session - the same numbers the logout
        // digest reports, answerable at any moment instead of only at the end.
        auto last = g_session.find(player->GetGUID().GetCounter());
        if (last != g_session.end())
        {
            SessionTally const& tally = last->second;
            if (tally.added + tally.onAccept + tally.completed + tally.handedIn +
                tally.gathered + tally.mailed + tally.destroyed > 0)
            {
                handler->PSendSysMessage("|cff20ff20Session|r: {} quest(s) accepted, {} completed ({} handed in), {} item(s) gathered, {} posted home in {} letter(s), {} grey item(s) binned.",
                                         tally.added + tally.onAccept,
                                         tally.completed + tally.handedIn, tally.handedIn,
                                         tally.gathered, tally.mailed, tally.letters, tally.destroyed);
            }
        }

        // Counts alone cannot answer "why has it not taken my quest?". Walk the
        // difference and name the reason for each, capped so the answer stays
        // readable in the chat frame.
        uint32 pending = 0;
        uint32 shown = 0;

        for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 const questId = player->GetQuestSlotQuestId(slot);
            if (!questId)
                continue;

            if (bot->GetQuestStatus(questId) != QUEST_STATUS_NONE || bot->IsQuestRewarded(questId))
                continue;

            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest)
                continue;

            ++pending;
            if (shown >= 5)
                continue;

            Refusal const refusal = ExplainRefusal(bot, quest);
            handler->PSendSysMessage("  |cffffd200waiting|r {}: {}", quest->GetTitle(), refusal.reason);
            ++shown;
        }

        if (pending > shown)
            handler->PSendSysMessage("  |cffaaaaaa... and {} more|r", pending - shown);

        if (!pending)
            handler->PSendSysMessage("  |cff20ff20nothing waiting|r - every quest on your log is on the companion's.");

        // Level mode 1 is raise-only, so the companion can end up above the
        // player - and then the player's own quests can be refused for being
        // below its level. Name it, because it reads like a sync failure.
        if (g_cfg.levelMode == 1 && bot->GetLevel() > player->GetLevel())
            handler->PSendSysMessage("  |cffffd200level mode 1|r lets the companion sit above you; quests capped below its level ({} - against {} now) are refused. |cffffff00LevelMode 2|r pins both levels instead.",
                                     uint32(bot->GetLevel()), uint32(player->GetLevel()));

        return true;
    }

    static bool HandleLevelCommand(ChatHandler* handler, char const* /*args*/)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        if (!g_cfg.enable || !g_cfg.levelMode)
        {
            handler->PSendSysMessage("|cffff8000Companion quest sync|r: level alignment is disabled (LevelMode 0).");
            return true;
        }

        std::vector<uint32> bots;
        if (!FindCompanions(player->GetGUID().GetCounter(), bots))
        {
            handler->PSendSysMessage("|cffff8000Companion quest sync|r: no companion bound.");
            return true;
        }

        // Every companion at once: with several bound, aligning the first and saying
        // nothing about the rest is how a group ends up half-levelled.
        uint32 aligned = 0;
        uint32 offline = 0;
        uint32 already = 0;

        for (uint32 const botGuid : bots)
        {
            Player* bot = FindCompanionPlayer(botGuid);
            if (!bot || !bot->IsInWorld())
            {
                ++offline;
                continue;
            }

            if (bot->GetLevel() == player->GetLevel())
            {
                ++already;
                continue;
            }

            if (bot->GetLevel() > player->GetLevel())
            {
                // AlignLevel refuses to demote on purpose: GiveLevel downwards strips
                // spell ranks while talents may still point at them. The command must
                // not do quietly what the sync refuses to do at all.
                handler->PSendSysMessage("|cffff8000{}|r is level {} and stays there - the sync never demotes.",
                                         bot->GetName(), uint32(bot->GetLevel()));
                continue;
            }

            uint8 const from = bot->GetLevel();
            bot->GiveLevel(player->GetLevel());
            ++aligned;

            handler->PSendSysMessage("|cff20ff20Companion quest sync|r: {} levelled {} -> {}.",
                                     bot->GetName(), uint32(from), uint32(bot->GetLevel()));
        }

        if (already)
            handler->PSendSysMessage("|cff20ff20Companion quest sync|r: {} already level {}.",
                                     already == 1 ? "one companion was" : std::to_string(already) + " companions were",
                                     uint32(player->GetLevel()));

        if (offline)
            handler->PSendSysMessage("|cffff8000Companion quest sync|r: {} not online, left alone.",
                                     offline == 1 ? "one companion is" : std::to_string(offline) + " companions are");

        if (!aligned && !offline && !already)
            handler->PSendSysMessage("|cffff8000Companion quest sync|r: nothing to align.");

        return true;
    }
};

/// The player-side half of the banner: mod-playerbots announces itself when a
/// real player enters the world, and a module the player can actually use should
/// say so too. Bots are skipped; the message respects the same Enable switch and
/// stays quiet if the companion table is missing (nothing could sync anyway).
    /// The owner is interacting with an NPC - accepting, gossiping, buying bread.
    /// Pull each bound body that is within a short walk but not beside the owner to the
    /// owner's side. The companions' own AI can only process a quest menu within
    /// interaction distance of the target, and it says "Cannot talk to quest giver" on
    /// every attempt from a body-length away - including for an innkeeper the owner
    /// merely clicked, whose menu is empty. Tightening the party removes the distance
    /// from the equation entirely; the step is throttled and never fights combat.
    void PullPartyClose(Player* player)
    {
        std::vector<uint32> bound;
        if (!FindCompanions(player->GetGUID().GetCounter(), bound))
            return;

        uint32 const nowTs = static_cast<uint32>(time(nullptr));
        for (uint32 const botGuid : bound)
        {
            Player* bot = FindCompanionPlayer(botGuid);
            if (!bot || !bot->IsInWorld() || !bot->IsAlive() || bot->IsInCombat())
                continue;

            float const toOwner = bot->GetDistance(player);
            if (toOwner <= 4.0f || toOwner > 40.0f)
                continue;

            uint32& stepAt = g_walkAt[bot->GetGUID().GetCounter()];
            if (stepAt && nowTs - stepAt < Timing::WalkStep)
                continue;

            stepAt = nowTs;
            StepBesideOwner(player, bot, player->GetPositionX(), player->GetPositionY());
        }
    }

class CompanionQuestSyncCreatureScript : public AllCreatureScript
{
public:
    CompanionQuestSyncCreatureScript() : AllCreatureScript("CompanionQuestSyncCreatureScript") {}

    /// The owner clicked an NPC - an innkeeper, a vendor, a banker, anything. The
    /// companions' AI mirrors the owner's target and tries to process it as a quest menu,
    /// and its distance gate says "Cannot talk to quest giver" while a body stands a few
    /// yards off - even for an NPC that has no quests at all. The gossip moment is the
    /// signal: tighten the party around the owner and the distance never matters.
    /// Returning false leaves the gossip itself untouched.
    bool CanCreatureGossipHello(Player* player, Creature* /*creature*/) override
    {
        if (!g_cfg.enable || !g_tableReady || !player
            || !player->GetSession() || player->GetSession()->IsBot())
            return false;

        PullPartyClose(player);
        return false;
    }
};

class CompanionQuestSyncPlayerScript : public PlayerScript
{
public:
    // Hook registration is not optional in this core: a PlayerScript that does
    // not name its hooks is never called (ScriptRegistry only invokes the
    // scripts listed for each hook), which is why the login greeting silently
    // did nothing before.
    CompanionQuestSyncPlayerScript() : PlayerScript("CompanionQuestSyncPlayerScript",
        { PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LOGOUT, PLAYERHOOK_ON_PLAYER_QUEST_ACCEPT,
          PLAYERHOOK_ON_QUEST_ABANDON, PLAYERHOOK_ON_BEFORE_TELEPORT,
          PLAYERHOOK_ON_LEVEL_CHANGED, PLAYERHOOK_ON_BEFORE_SEND_CHAT_MESSAGE }) { }

    /// A hearthstone, a portal or a summon is about to move the player: note it, and let the
    /// fast tick bring the companion along once the destination has finished loading.
    bool OnPlayerBeforeTeleport(Player* player, uint32 mapid, float /*x*/, float /*y*/, float /*z*/,
                                float /*orientation*/, uint32 /*options*/, Unit* /*target*/) override
    {
        if (!g_cfg.enable || !g_cfg.followThroughTeleport || !player || !player->GetSession())
            return true;

        if (player->GetSession()->IsBot())      // a bot's own teleports are not the player's
            return true;

        // never drag a companion into a battleground or an arena
        if (MapEntry const* entry = sMapStore.LookupEntry(mapid); entry && entry->IsBattlegroundOrArena())
            return true;

        // Nor into a dungeon or raid: TeleportTo on an instancable map resolves to the
        // BOT's own instance copy, so the companion "teleports" and never appears at the
        // player's side. Better visibly left behind than invisibly lost.
        if (MapEntry const* entry = sMapStore.LookupEntry(mapid); entry && (entry->IsDungeon() || entry->IsRaid()))
            return true;

        // A ghost's teleport is a corpse run; the living companion stays where it is and
        // the resurrect hook re-arms the follow when the owner is themselves again.
        if (!player->IsAlive())
            return true;

        std::vector<uint32> bots;
        if (!FindCompanions(player->GetGUID().GetCounter(), bots))
            return true;

        g_pendingFollow[player->GetGUID().GetCounter()] = uint32(time(nullptr));

        return true;
    }

    /// A companion that levels organically - grinding with the party, turn-ins its
    /// own AI performed - earns its talent points and trainer spells through the
    /// core's GiveLevel, which grants nothing on its own. The hook is the moment
    /// those points unlock: teach them then, so Train all is a backstop rather than
    /// the only teacher. The module's own raises fire the same hook through
    /// GiveLevel, so both paths arrive here and nobody is taught twice.
    void OnPlayerLevelChanged(Player* player, uint8 /*oldLevel*/) override
    {
        if (!g_cfg.enable || !g_tableReady)
            return;
        if (!player || !player->GetSession() || !player->GetSession()->IsBot())
            return;

        // only a body this module owns: the query is one indexed row, and a level
        // change is rare enough that no cache is worth building for it
        QueryResult row = CharacterDatabase.Query(("SELECT owner_guid FROM " + g_cfg.bindingTable +
            " WHERE bot_guid = " + std::to_string(player->GetGUID().GetCounter())).c_str());
        if (!row)
            return;

        Player* owner = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>((*row)[0].Get<uint32>()));
        if (!owner)
            return;

        TeachTrainerContent(player, owner);   // spells, talents - as they unlock
        TeachProfessions(player, owner);      // ceiling, tools, recipes
    }

    /// Any .companionsync command opens a short conversation window: the answers a
    /// command generates speak at once instead of joining the ambient rate limit's
    /// queue. The hook sees every chat line including dot-commands, so one place
    /// marks the burst for all of the module's verbs.
    void OnPlayerBeforeSendChatMessage(Player* player, uint32& /*type*/, uint32& /*lang*/, std::string& msg) override
    {
        if (!player || !player->GetSession() || player->GetSession()->IsBot())
            return;
        if (msg.rfind(".companionsync", 0) == 0 || msg.rfind(".companionadmin", 0) == 0)
            g_commandBurst[player->GetGUID().GetCounter()] = uint32(time(nullptr));
    }

    void OnPlayerLogin(Player* player) override
    {
        if (!g_cfg.enable || !g_tableReady)
            return;

        if (!player->GetSession() || player->GetSession()->IsBot())
            return;

        ChatHandler handler(player->GetSession());

        // Reconcile the map and the roster on the way in: a pin left behind by a session
        // that ended badly is cleared the moment the player logs back in, and the window
        // has its state before it opens.
        //
        // This used to sit behind `announceLogin`, which meant that turning off two chat
        // lines also turned off the login payload - the client then saw nothing for up to
        // a full pass and reported the module as not answering.
        // The hello is a CSYNC protocol line, sent whatever else is switched off: it is
        // how the client knows the module is loaded, and it carries the pass interval so
        // the client's patience scales with the config instead of a hard-coded guess.
        SendProtocolLine(player, "Hello " + std::to_string(g_cfg.intervalSeconds));

        if (g_cfg.pinSync)
        {
            SendPinSync(player, true);
            SendCompSync(player, true);
            SendPoolStock(player, true);
        }

        // Sweep once immediately: the timer would leave the first thirty seconds of a
        // session showing whatever was true when the player last logged out.
        if (g_cfg.autoSync && g_cfg.enable)
            SyncCompanion(player);

        if (g_cfg.announceLogin)
        {
            handler.SendSysMessage("|cff00ff00This server runs with |cff00ccffmod-companion-questsync|r |cffcccccc- companion quest + level sync|r");
            handler.SendSysMessage("|cff00ff00Companion:|r quests and level follow yours. |cffffff00.companionsync status|r for a report, |cffffff00.companionsync|r to sync now.");
        }
    }

    /// One line per session on the way out. The heartbeat answers "is it alive";
    /// this answers "what did it actually do while I played".
    void OnPlayerLogout(Player* player) override
    {
        if (!player || !g_cfg.enable)
            return;

        // Only owners get a tally (the sweep skips bot sessions), so a companion
        // logging out finds nothing and stays quiet.
        uint32 const ownerGuid = player->GetGUID().GetCounter();

        // Drop the per-player state with the player. Bot-keyed maps are bounded by the
        // bot count, but owner-keyed ones would otherwise sit on strings for every
        // character that had ever bound a companion - small, and still worth not
        // carrying across sessions (a stale pin sync line would also suppress the first
        // reconciliation of the next login).
        // Companions keep their own state, and bot guids are pooled: erase what belongs to
        // this player's companions as well, or the next body to inherit a guid inherits the
        // memo, latches and "already told them" sets of the one that came before it.
        std::vector<uint32> companions;
        FindCompanions(ownerGuid, companions);
        for (uint32 const botGuid : companions)
            PurgeCompanionState(botGuid);

        g_pendingFollow.erase(ownerGuid);
        g_lastPinSync.erase(ownerGuid);
        g_heartbeatAt.erase(ownerGuid);
        g_lastCompSync.erase(ownerGuid);
        g_lastCompSyncAt.erase(ownerGuid);
        g_lastPoolStock.erase(ownerGuid);
        g_pinSyncAt.erase(ownerGuid);
        g_markedNpcs.erase(ownerGuid);
        g_sessionRefused.erase(ownerGuid);
        g_whisperQueue.erase(ownerGuid);
        g_lastWhisperAt.erase(ownerGuid);

        for (uint32 const botGuid : companions)
            g_multiCompanionBySpeaker.erase(botGuid);
        g_pinnedQuests.erase(ownerGuid);

        // owner+bot keyed: a stale 10-minute window would suppress the "summon it"
        // notice for the next body this owner binds
        for (auto itr = g_stuckNoticeAt.begin(); itr != g_stuckNoticeAt.end();)
            itr = (itr->first >> 32 == ownerGuid) ? g_stuckNoticeAt.erase(itr) : std::next(itr);

        auto itr = g_session.find(ownerGuid);
        if (itr == g_session.end())
            return;

        SessionTally const tally = itr->second;
        g_session.erase(itr);

        if (!tally.added && !tally.onAccept && !tally.completed && !tally.refused && !tally.reopened &&
            !tally.gathered && !tally.handedIn && !tally.mailed && !tally.destroyed && !tally.levelFrom)
            return;

        std::string level;
        if (tally.levelFrom && tally.levelTo)
            level = ", level " + std::to_string(tally.levelFrom) + " -> " + std::to_string(tally.levelTo);

        // Carry a one-line summary across sessions: the owner's next companion arrival
        // opens with it, so "what did we do?" never needs the log.
        std::string summary = std::to_string(tally.added + tally.onAccept) + " quest(s) accepted, " +
                              std::to_string(tally.completed + tally.handedIn) + " completed";
        if (tally.handedIn)
            summary += " (" + std::to_string(tally.handedIn) + " handed in)";
        if (tally.gathered)
            summary += ", " + std::to_string(tally.gathered) + " gathered";
        if (tally.letters)
            summary += ", " + std::to_string(tally.mailed) + " item(s) posted home in " +
                       std::to_string(tally.letters) + " letter(s)";
        if (tally.destroyed)
            summary += ", " + std::to_string(tally.destroyed) + " grey item(s) binned";
        if (level.size())
            summary += level;

        g_lastSessionSummary[ownerGuid] = summary;
        CharacterDatabase.Execute(
            "REPLACE INTO cq_digest (owner_guid, summary, stamp) VALUES ({}, '{}', {})",
            ownerGuid, summary, static_cast<uint32>(time(nullptr)));

        LOG_INFO("server.loading",
                 "[CompanionQuestSync] session: {} - {} quest(s) added ({} on accept), {} completed, {} reopened, {} refused, {} item(s) gathered off the ground, {} handed in, {} mailed ({} letter(s)), {} destroyed{}",
                 player->GetName(), tally.added + tally.onAccept, tally.onAccept, tally.completed, tally.reopened,
                 tally.refused, tally.gathered, tally.handedIn, tally.mailed, tally.letters, tally.destroyed, level);
    }
    /// Instant mirror: the moment a real player accepts a quest, hand it to the
    /// companion. The 20 s sweep stays as the safety net (logins, level gates,
    /// anything the hook could not do), but this removes the wait - and it runs
    /// regardless of combat, which the sweep deliberately no longer blocks.
    /// Mirror an abandon as instantly as an accept.
    ///
    /// The sweep also notices a quest the owner no longer holds, but it has to wait out
    /// a grace period and only ticks while the player is in world - drop a quest and log
    /// out and the companion kept gathering for nothing. This runs in the same call the
    /// client made, so the copy goes at the same moment.
    void OnPlayerResurrect(Player* player, float /*restore_percent*/, bool& /*applySickness*/) override
    {
        if (!g_cfg.enable || !g_cfg.followThroughTeleport || !player || !player->GetSession())
            return;

        if (player->GetSession()->IsBot())
            return;

        // After a spirit heal the pair may start the run apart: re-arm the follow so the
        // companion closes the gap (or crosses back from wherever the corpse run left it).
        std::vector<uint32> bots;
        if (!FindCompanions(player->GetGUID().GetCounter(), bots))
            return;

        g_pendingFollow[player->GetGUID().GetCounter()] = uint32(time(nullptr));
    }

    void OnPlayerUpdateZone(Player* player, uint32 /*newZone*/, uint32 /*newArea*/) override
    {
        if (!g_cfg.enable || !g_cfg.followThroughTeleport || !player || !player->GetSession())
            return;

        if (player->GetSession()->IsBot())
            return;

        // A cross-continent flight is the one routine trip the companion never made:
        // movement is not a teleport, so it stayed behind and whispered "I have lost
        // you" every three minutes. Arm the follow when the flight ENDS - the zone
        // change that lands - never mid-flight, which would drop the companion onto a
        // moving griffon. Zone changes while grounded are ordinary walking; the
        // executor's own distance gate (120 yd) keeps them harmless.
        if (player->IsInFlight() || player->IsInCombat() || !player->IsAlive())
            return;

        std::vector<uint32> bots;
        if (!FindCompanions(player->GetGUID().GetCounter(), bots))
            return;

        g_pendingFollow[player->GetGUID().GetCounter()] = uint32(time(nullptr));
    }

    void OnPlayerMapChanged(Player* player) override
    {
        if (!g_cfg.enable || !g_cfg.followThroughTeleport || !player || !player->GetSession())
            return;

        if (player->GetSession()->IsBot())
            return;

        // Map changes: continent switches and ports. The BeforeTeleport hook covers
        // casts and items; this catches the paths those miss.
        if (player->IsInFlight() || player->IsInCombat() || !player->IsAlive())
            return;

        std::vector<uint32> bots;
        if (!FindCompanions(player->GetGUID().GetCounter(), bots))
            return;

        g_pendingFollow[player->GetGUID().GetCounter()] = uint32(time(nullptr));
    }

    void OnPlayerQuestAbandon(Player* player, uint32 questId) override
    {
        if (!g_cfg.enable || !g_cfg.dropExtraBotQuests || !g_tableReady || !questId)
            return;

        if (!player || !player->GetSession() || player->GetSession()->IsBot())
            return;

        // The owner still holds it? Then nothing was abandoned (a partial drop or a
        // branch swap can fire this while the quest stays in the log).
        if (player->GetQuestStatus(questId) != QUEST_STATUS_NONE)
            return;

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);

        // Every bound copy drops, not just the first: with four companions, mirroring the
        // abandon on companion 1 alone left the other three gathering for a quest the
        // player had thrown away until the sweep's grace period expired.
        std::vector<uint32> bound;
        if (!FindCompanions(player->GetGUID().GetCounter(), bound))
            return;

        for (uint32 const botGuid : bound)
        {
            Player* bot = FindCompanionPlayer(botGuid);
            if (!bot || !bot->IsInWorld())
                continue;

            if (bot->IsQuestRewarded(questId))
                continue;

            uint16 slot = UINT16_MAX;
            for (uint16 s = 0; s < MAX_QUEST_LOG_SIZE; ++s)
                if (bot->GetQuestSlotQuestId(s) == questId) { slot = s; break; }

            if (slot == UINT16_MAX)
                continue;

            // The full drop sequence, as the sweep does it: the instant hook used to
            // RemoveActiveQuest alone, leaving the quest's source item orphaned in the
            // bags - later mailed home as "loot" for a quest nobody holds.
            bot->SetQuestSlot(slot, 0);
            bot->TakeQuestSourceItem(questId, false);
            bot->RemoveActiveQuest(questId, false);
            g_extraSince.erase({ botGuid, questId });
            g_readyAt.erase((uint64(botGuid) << 32) | questId);

            if (quest)
            {
                SayToOwner(bot, player, "You let '" + quest->GetTitle() + "' go, so I laid mine down too - "
                                      "it made no sense to keep gathering for it on my own.", 120);

                LOG_INFO("server.loading", "[CompanionQuestSync] {} dropped '{}' ({}) - {} abandoned it",
                         bot->GetName(), quest->GetTitle(), questId, player->GetName());
            }
        }
    }

    void OnPlayerQuestAccept(Player* player, Quest const* quest) override
    {
        if (!g_cfg.enable || !g_tableReady || !quest)
            return;

        // Our own additions for the companion arrive here too; never recurse.
        if (!player->GetSession() || player->GetSession()->IsBot())
            return;

        // Bag contents are the thing this hook is about to change; a value cached by the
        // last sweep would describe the state before the accept.
        g_missingCache.clear();

        // Every bound copy takes the quest at once, not just the first: mirroring onto
        // companion 1 alone left the others waiting a full interval, which reads as
        // "the others are broken" when the player can see them all standing there.
        std::vector<uint32> bound;
        if (!FindCompanions(player->GetGUID().GetCounter(), bound))
            return;

        uint32 const questId = quest->GetQuestId();

        for (uint32 const botGuid : bound)
        {
            Player* bot = FindCompanionPlayer(botGuid);
            if (!bot || !bot->IsInWorld())
                continue;

            QuestStatus const before = bot->GetQuestStatus(questId);

            SyncQuestToBot(player, bot, quest, nullptr);

            bool const landed = before == QUEST_STATUS_NONE && bot->GetQuestStatus(questId) != QUEST_STATUS_NONE;
            if (!landed)
                continue;

            // Count it: this path is how most quests actually arrive, so leaving it
            // out of the window totals made the heartbeat under-report the truth.
            ++g_hbAdded;
            ++g_hbHookAdds[player->GetGUID().GetCounter()];
            ++g_session[player->GetGUID().GetCounter()].onAccept;

            FlushNotices(player, bot);

            if (g_cfg.debug >= 1)
                LOG_INFO("server.loading", "[CompanionQuestSync] accept hook: {} picked up '{}' ({}) immediately",
                         bot->GetName(), quest->GetTitle(), questId);
        }

        // The owner accepting a quest is the moment a giver matters: keep the party
        // tight, so the AI's own take succeeds within interaction distance.
        PullPartyClose(player);
    }
};

void Addmod_companion_questsyncScripts()
{
    new CompanionQuestSyncWorldScript();
    new CompanionQuestSyncCommandScript();
    new CompanionQuestSyncPlayerScript();
    new CompanionQuestSyncCreatureScript();
}
