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
#include "GossipDef.h"   // PlayerTalkClass::SendQuestGiverStatus
#include "Group.h"
#include "Item.h"
#include "Log.h"
#include "LootMgr.h"
#include "Map.h"
#include "MapMgr.h"
#include <chrono>
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "ItemUsageValue.h"  // playerbots' verdict on what an item is worth
#include "PlayerbotAI.h"     // sell vendor trash through the bot's own AI
#include "Playerbots.h"    // GET_PLAYERBOT_AI
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
        uint32 extraBagItem    = 20400;     // Pumpkin Bag, 16 slots, no level requirement
        uint32 extraBags       = 4;         // bag slots to fill with it (0 = off)
        uint32 itemKeepQuality = 2;         // never bin anything at or above this (2 = green)
        uint32 keepFreeSlots   = 2;
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
    bool g_multiCompanion = false;   // owner has more than one companion
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
        uint8 levelFrom = 0;
        uint8 levelTo = 0;
    };

    std::unordered_map<uint32, SessionTally> g_session;   // owner guid -> tally
    std::map<uint32, std::set<uint32>> g_sessionRefused;  // owner guid -> quests refused once
    std::map<uint32, std::set<uint32>> g_seenQuests;      // bot guid -> quests in its log last pass
    std::map<uint32, uint32> g_walkAt;                    // bot guid -> unix time
    std::map<uint32, std::string> g_lastPinSync;          // owner guid -> last pin line sent
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
        g_cfg.extraBagItem       = sConfigMgr->GetOption<uint32>("CompanionQuestSync.ExtraBagItem", 20400);
        g_cfg.extraBags          = sConfigMgr->GetOption<uint32>("CompanionQuestSync.ExtraBags", 4);
        g_cfg.itemKeepQuality    = sConfigMgr->GetOption<uint32>("CompanionQuestSync.ItemKeepQuality", 2);
        g_cfg.keepFreeSlots      = sConfigMgr->GetOption<uint32>("CompanionQuestSync.KeepFreeSlots", 2);
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
    };

    std::map<uint32, PassNote> g_passNotes;   // owner guid -> best note this pass

    void Note(Player* owner, uint8 priority, std::string const& text)
    {
        if (!owner || text.empty())
            return;

        PassNote& note = g_passNotes[owner->GetGUID().GetCounter()];

        if (priority >= note.priority)
            note = PassNote{ priority, text };
    }


    void PurgeCompanionState(uint32 botGuid)
    {
        g_passMemo.erase(botGuid);
        g_groundStall.erase(botGuid);
        g_gatherPending.erase(botGuid);

        // guids are pooled, so a stale "already announced" pair would silence the next
        // companion that inherits this body
        for (auto itr = g_gatherAnnounced.begin(); itr != g_gatherAnnounced.end();)
            itr = (itr->first == botGuid) ? g_gatherAnnounced.erase(itr) : std::next(itr);

        g_seenQuests.erase(botGuid);
        g_repairing.erase(botGuid);
        g_notified.erase(botGuid);
        g_pendingNotices.erase(botGuid);
        g_sellAt.erase(botGuid);
        g_equipAt.erase(botGuid);
        g_lastWhisperByBot.erase(botGuid);
        for (uint8 slot = 0; slot < EQUIPMENT_SLOT_END; ++slot)
            g_gearCreatedAt.erase((uint64(botGuid) << 8) | slot);   // guids are pooled

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
                    uint32 validWhileMissing = 0)
    {
        if (!owner || !owner->GetSession())
            return false;

        // With several companions the player cannot tell who is speaking, so the name
        // goes in front. With one it would be noise, and the module is careful not to be
        // noisy.
        std::string const spoken = (g_multiCompanion && bot)
            ? bot->GetName() + ": " + text
            : text;

        uint32 const ownerGuid = owner->GetGUID().GetCounter();
        uint32 const botGuid = bot ? bot->GetGUID().GetCounter() : 0;
        uint32 const now = static_cast<uint32>(time(nullptr));
        uint32& last = g_lastWhisperAt[ownerGuid];

        if (last && now - last < minGap)
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

            if (queue.size() < 4 && !dup)
                queue.push_back({ text, botGuid, validWhileMissing, now });

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
        g_passNotes.erase(itr);

        if (!text.empty() && speaker)
            SayToOwner(speaker, owner, text);
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

    std::unordered_map<uint32, EnderSpot> g_enderCache;   // quest id -> ender spot

    EnderSpot const& FindQuestEnder(uint32 questId)
    {
        auto itr = g_enderCache.find(questId);
        if (itr != g_enderCache.end())
            return itr->second;

        EnderSpot spot;

        QueryResult result = WorldDatabase.Query(
            "SELECT ct.name, c.position_x, c.position_y, c.position_z, c.map, ce.id, c.zoneId "
            "FROM creature_questender ce "
            "JOIN creature_template ct ON ct.entry = ce.id "
            "JOIN creature c ON c.id1 = ce.id "
            "WHERE ce.quest = {} LIMIT 1", questId);

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

        return g_enderCache[questId] = spot;
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
        if (AreaTableEntry const* zone = GetAreaEntryByAreaID(zoneId))
            if (zone->area_name[0])
                return zone->area_name[0];

        return "an unknown zone";
    }

    void SendPinSync(Player* owner, bool force);  // defined below
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
                            if (g_cfg.debug >= 2)
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

            EnderSpot const& spot = FindQuestEnder(questId);
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
        if (!g_cfg.mapPins || !owner->GetSession())
            return;

        uint32 const ownerGuid = owner->GetGUID().GetCounter();
        std::set<uint32> wanted;

        for (uint32 const botGuid : bots)
        {
            Player* bot = FindCompanionPlayer(botGuid);
            if (!bot || !bot->IsInWorld())
                continue;

            for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            {
                uint32 const questId = bot->GetQuestSlotQuestId(slot);
                if (!questId || bot->GetQuestStatus(questId) != QUEST_STATUS_INCOMPLETE)
                    continue;

                // Only skip what the owner has already been credited for. Their copy being
                // COMPLETE does not draw the area any more (the objectives are done), and
                // that is exactly the case where the companion still has work to do there.
                if (owner->IsQuestRewarded(questId))
                    continue;

                if (!sObjectMgr->GetQuestPOIVector(questId))
                    continue;

                wanted.insert(questId);
            }
        }

        std::string key;
        for (uint32 const id : wanted)
            key += std::to_string(id) + ",";

        if (g_pinnedQuests[ownerGuid] == key)
            return;

        g_pinnedQuests[ownerGuid] = key;

        if (wanted.empty())
            return;

        std::vector<uint32> const list(wanted.begin(), wanted.end());

        WorldPacket data(SMSG_QUEST_POI_QUERY_RESPONSE, 8 + list.size() * 40);
        data << uint32(list.size());

        for (uint32 const questId : list)
        {
            QuestPOIVector const* poi = sObjectMgr->GetQuestPOIVector(questId);
            data << uint32(questId);
            data << uint32(poi ? poi->size() : 0);

            if (!poi)
                continue;

            for (QuestPOI const& entry : *poi)
            {
                data << uint32(entry.Id);
                data << int32(entry.ObjectiveIndex);
                data << uint32(entry.MapId);
                data << uint32(entry.AreaId);
                data << uint32(entry.FloorId);
                data << uint32(entry.Unk3);
                data << uint32(entry.Unk4);
                data << uint32(entry.points.size());

                for (QuestPOIPoint const& point : entry.points)
                {
                    data << int32(point.x);
                    data << int32(point.y);
                }
            }
        }

        owner->GetSession()->SendPacket(&data);

        if (g_cfg.debug >= 2)
            LOG_INFO("server.loading", "[CompanionQuestSync] pushed {} quest area(s) to {} map",
                     uint32(list.size()), owner->GetName());
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
        if (last && now - last < 180)
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

            EnderSpot const& spot = FindQuestEnder(questId);
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
                uint32& prodAt = g_rewardProdAt[{ bot->GetGUID().GetCounter(), questId }];
                if (!prodAt || nowTs - prodAt >= 60)
                {
                    prodAt = nowTs;
#ifdef MOD_PLAYERBOTS
                    if (PlayerbotAI* ai = GET_PLAYERBOT_AI(bot))
                        ai->HandleCommand(CHAT_MSG_WHISPER, "r", owner);
#endif
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
                                   (coords.empty() ? "." : " (" + coords + ")."));
                else
                    Note(owner, 3, "When we get a chance, could we stop by " + spot.name +
                                   "? I can hand in '" + quest->GetTitle() + "' there - about " +
                                   std::to_string(uint32(dist)) + " yards from me" +
                                   (coords.empty() ? "." : ", " + coords + "."));
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
        if (last && now - last < 45)
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

            bool stale = !speaker || !speaker->IsInWorld();
            if (!stale && entry->queuedAt && now > entry->queuedAt)
                stale = now - entry->queuedAt > 120;
            if (!stale && entry->questId)
                stale = speaker->GetQuestStatus(entry->questId) != QUEST_STATUS_NONE ||
                        speaker->IsQuestRewarded(entry->questId);

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
        for (auto entry = queue.begin(); entry != queue.end(); ++entry)
        {
            uint32 const entryHeard = g_lastWhisperByBot[entry->botGuid];
            uint32 const chosenHeard = g_lastWhisperByBot[chosen->botGuid];

            if (entryHeard < chosenHeard ||
                (entryHeard == chosenHeard && entry->queuedAt < chosen->queuedAt))
                chosen = entry;
        }

        Player* speaker = FindCompanionPlayer(chosen->botGuid);
        if (!speaker)
        {
            queue.erase(chosen);
            return;
        }

        std::string const spoken = g_multiCompanion ? speaker->GetName() + ": " + chosen->text
                                                    : chosen->text;

        last = now;
        g_lastWhisperByBot[chosen->botGuid] = now;
        speaker->Whisper(spoken, LANG_UNIVERSAL, owner);
        queue.erase(chosen);

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
                if (poiAt && now - poiAt < 300)
                    continue;

                poiAt = now;
                SayToOwner(bot, owner, "I still have '" + quest->GetTitle() +
                                      "' to finish for us - the area is on the map at " + where +
                                      ". Want to head that way?", 45, questId);
                return;
            }

            std::pair<uint32, uint32> const key{ botGuid, questId };
            uint32& last = g_nudgeAt[key];
            if (last && now - last < 300)
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

        if (g_cfg.debug >= 1)
            LOG_INFO("server.loading", "[CompanionQuestSync] {} -> level {} (owner {} level {})",
                     bot->GetName(), uint32(bot->GetLevel()), owner->GetName(), uint32(ownerLevel));
    }

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
                if (created != g_gearCreatedAt.end() && now - created->second < 300)
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
        if (last && now - last < 20)
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
    void DeliverHandIns(Player* owner, Player* bot)
    {
        if (!bot->IsAlive() || bot->IsInCombat() || (owner && owner->IsInCombat()))
            return;

        uint32 const nowTs = static_cast<uint32>(time(nullptr));
        uint32 const botGuid = bot->GetGUID().GetCounter();

        for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 const questId = bot->GetQuestSlotQuestId(slot);
            if (!questId || bot->GetQuestStatus(questId) != QUEST_STATUS_COMPLETE)
                continue;

            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest || !DescribeMissingItems(bot, quest).empty())
                continue;                          // still owes items: cannot reward

            EnderSpot const& spot = FindQuestEnder(questId);
            if (!spot.found || spot.map != bot->GetMapId())
                continue;

            if (spot.zone && spot.zone != bot->GetZoneId())
                continue;                          // giver is in another zone

            float const dist = bot->GetDistance(spot.x, spot.y, spot.z);

            if (dist <= 6.0f)
            {
                uint32& prodAt = g_rewardProdAt[{ botGuid, questId }];
                if (prodAt && nowTs - prodAt < 60)
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
            if (stepAt && nowTs - stepAt < 20)
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
    }

    void TrySellJunk(Player* owner, Player* bot)
    {
        if (!g_cfg.sellJunk)
            return;

        if (bot->GetFreeInventorySpace() >= g_cfg.keepFreeSlots)
            return;

        uint32 const botGuid = bot->GetGUID().GetCounter();
        uint32 const now = static_cast<uint32>(time(nullptr));
        uint32& last = g_sellAt[botGuid];
        if (last && now - last < 120)
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

        if (last && now - last < 180)
            return;

        last = now;

        if (PlayerbotAI* ai = GET_PLAYERBOT_AI(bot))
            ai->HandleCommand(CHAT_MSG_WHISPER, "equip upgrade", owner);

        if (g_cfg.debug >= 2)
            LOG_INFO("server.loading", "[CompanionQuestSync] {} asked to wear any upgrade it is carrying", bot->GetName());
    }

    std::string DescribeMissingItems(Player* bot, Quest const* quest);  // defined below
    uint32 TidyInventory(Player* bot);   // defined below, used by the accept path
    /// The companion bins grey loot when its bags are full - say so, once per
    /// couple of minutes, so nothing appears to vanish unexplained.
    void NotifyJunk(Player* owner, Player* bot, uint32 count)
    {
        if (!count)
            return;

        uint32 const now = static_cast<uint32>(time(nullptr));
        uint32& last = g_junkNoticeAt[bot->GetGUID().GetCounter()];
        if (last && now - last < 120)
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
                if (added->second && now > added->second && now - added->second < 60)
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
                    TidyInventory(bot);

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
    uint32 TidyInventory(Player* bot)
    {
        if (!g_cfg.tidyInventory)
            return 0;

        if (bot->GetFreeInventorySpace() >= g_cfg.keepFreeSlots)
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
        auto consider = [&wanted, &victims](Item* item)
        {
            if (!item)
                return;

            ItemTemplate const* proto = item->GetTemplate();
            if (!proto || !proto->ItemId)
                return;

            // Never a container: a bag in the backpack looks like a disposable white item
            // and destroying one costs more slots than it frees.
            // The keep-quality rule: at or above it, the item is safe. Default green, so
            // greys and whites are disposable and green-and-better never is. This is the
            // knob that stops "inventory is full" being permanent - a companion that can
            // never shed a white item has no way out of a full bag on its own.
            if (proto->Quality >= g_cfg.itemKeepQuality)
                return;
            if (proto->Class == ITEM_CLASS_QUEST || proto->Class == ITEM_CLASS_CONTAINER || proto->StartQuest)
                return;                                  // quest item / quest starter
            if (wanted.count(proto->ItemId))
                return;                                  // a live quest still needs it

            victims.emplace_back(proto->SellPrice, proto->ItemId);
        };

        for (uint8 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
            consider(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i));

        for (uint8 i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; ++i)
            if (Bag* bag = bot->GetBagByPos(i))
                for (uint8 j = 0; j < bag->GetBagSize(); ++j)
                    consider(bot->GetItemByPos(i, j));

        // Nothing but white gear left? At zero free slots the companion cannot
        // pick up a quest item at all - it cannot even loot the Gold Dust it owes -
        // which is worse than losing a few copper of vendor trash. Ask playerbots
        // what it would vendor and bin exactly that; nothing it rates EQUIP,
        // QUEST, SKILL, DISENCHANT or AH is touched.
        if (g_cfg.tidyVendorJunk && bot->GetFreeInventorySpace() == 0)
        {
            if (PlayerbotAI* ai = GET_PLAYERBOT_AI(bot))
            {
                auto considerVendorJunk = [&wanted, &victims, ai](Item* item)
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
        // the companion does not craft - at zero free slots one of them is worth more as
        // an empty slot than as an item. Cheapest first, surplus stacks only, and never
        // the hearthstone, gear or anything a quest needs.
        if (g_cfg.tidySurplus && bot->GetFreeInventorySpace() == 0)
        {
            auto considerSurplus = [&wanted, &victims](Item* item)
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
                if (g_cfg.debug < 2)
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

                // Already handed in by the owner: the companion is catching up, not
                // carrying a stray copy. Leave it alone - that is the whole point of
                // the module.
                if (owner->IsQuestRewarded(questId))
                    continue;

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
        NotifyJunk(owner, bot, TidyInventory(bot));    // destroy only what is left

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

        // Quest cleared: say so once, so the addon can drop its pin instead of
        // leaving a marker on a quest that is already done.
        {
            std::set<uint32>& previous = g_seenQuests[bot->GetGUID().GetCounter()];
            std::set<uint32> current;

            for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
                if (uint32 const id = bot->GetQuestSlotQuestId(slot))
                    current.insert(id);

            for (uint32 const gone : previous)
            {
                if (current.count(gone) || !bot->IsQuestRewarded(gone))
                    continue;

                if (Quest const* done = sObjectMgr->GetQuestTemplate(gone))
                    SayToOwner(bot, owner, "Finished '" + done->GetTitle() + "' - that pin can go.", 30);
            }

            previous = current;
        }

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

    std::string BuildPinLine(Player* owner)
    {
        struct Entry
        {
            std::string numbers;
            char style;
            std::string title;
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

                std::string coords;
                char style = 'g';

                if (bot->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE)
                {
                    EnderSpot const& spot = FindQuestEnder(questId);

                    // Only pin a giver standing in the same zone. Stormwind and Elwynn
                    // share map 0, so a map check alone let Morgan Pestle be pinned on
                    // the Elwynn map at a plausible-looking but wrong spot.
                    if (spot.found && spot.map == bot->GetMapId() && spot.zone == bot->GetZoneId())
                        coords = MapCoords(spot.x, spot.y, spot.zone);

                    style = 'h';
                }

                if (coords.empty())
                    coords = PoiCoords(bot, quest);

                if (coords.empty())
                    continue;

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

                if (g_multiCompanion)
                    title += "@" + SafePinName(bot->GetName());

                bool merged = false;
                for (Entry& existing : entries)
                {
                    if (existing.numbers != numbers || existing.style != style)
                        continue;

                    // Two names read fine; four do not fit on a map marker. Past the
                    // second, count them instead - the full list is a click away in
                    // `.companionsync quests`.
                    if (existing.extra || existing.title.find(" + ") != std::string::npos)
                        ++existing.extra;
                    else
                        existing.title += " + " + title;

                    merged = true;
                    break;
                }

                if (!merged)
                    entries.push_back({ numbers, style, title, 0 });
            }
        }

        std::string line;

        for (Entry const& entry : entries)
        {
            if (!line.empty())
                line += "|";

            line += entry.numbers + "," + entry.style + "=" + entry.title;

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
            // it. Colour codes are stripped by WLPins before it parses.
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
                handler->PSendSysMessage("|cffff8000Companion quest sync|r: your companion is not in your group - nothing was synced.");
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
        constexpr uint32 kSlotOnlyGraceSeconds = 180;

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

        if (online && owner && upAt && now - upAt < 20)
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
        g_multiCompanion = bots.size() > 1;

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

                    if (!gate.first && gate.second && nowTs - gate.second < 20)
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
                handler->PSendSysMessage("|cff20ff20Companion quest sync|r: all {} are already with you.", uint32(bound.size()));
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
                handler->PSendSysMessage("|cff20ff20Companion quest sync|r: {} is already with you.", bot->GetName());
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
            if (upAt && now - upAt < 20)
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

        SendPinSync(player, true);
        SendCompSync(player, true);

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
                handler->PSendSysMessage("|cffff8000{}|r is not online.", botGuid);
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

                EnderSpot const& spot = FindQuestEnder(questId);
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
class CompanionQuestSyncPlayerScript : public PlayerScript
{
public:
    // Hook registration is not optional in this core: a PlayerScript that does
    // not name its hooks is never called (ScriptRegistry only invokes the
    // scripts listed for each hook), which is why the login greeting silently
    // did nothing before.
    CompanionQuestSyncPlayerScript() : PlayerScript("CompanionQuestSyncPlayerScript",
        { PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LOGOUT, PLAYERHOOK_ON_PLAYER_QUEST_ACCEPT,
          PLAYERHOOK_ON_QUEST_ABANDON }) { }

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

        g_lastPinSync.erase(ownerGuid);
        g_lastCompSync.erase(ownerGuid);
        g_lastCompSyncAt.erase(ownerGuid);
        g_lastPoolStock.erase(ownerGuid);
        g_pinSyncAt.erase(ownerGuid);
        g_markedNpcs.erase(ownerGuid);
        g_sessionRefused.erase(ownerGuid);
        g_whisperQueue.erase(ownerGuid);
        g_lastWhisperAt.erase(ownerGuid);
        g_pinnedQuests.erase(ownerGuid);

        auto itr = g_session.find(ownerGuid);
        if (itr == g_session.end())
            return;

        SessionTally const tally = itr->second;
        g_session.erase(itr);

        if (!tally.added && !tally.onAccept && !tally.completed && !tally.refused && !tally.reopened &&
            !tally.gathered && !tally.levelFrom)
            return;

        std::string level;
        if (tally.levelFrom && tally.levelTo)
            level = ", level " + std::to_string(tally.levelFrom) + " -> " + std::to_string(tally.levelTo);

        LOG_INFO("server.loading",
                 "[CompanionQuestSync] session: {} - {} quest(s) added ({} on accept), {} completed, {} reopened, {} refused, {} item(s) gathered off the ground{}",
                 player->GetName(), tally.added + tally.onAccept, tally.onAccept, tally.completed, tally.reopened,
                 tally.refused, tally.gathered, level);
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

        SyncReport report;
        Player* bot = ResolveCompanion(player, report);
        if (!bot)
            return;

        bool holds = false;
        for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE && !holds; ++slot)
            holds = bot->GetQuestSlotQuestId(slot) == questId;

        if (!holds)
            return;

        if (bot->IsQuestRewarded(questId))
            return;

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);

        bot->RemoveActiveQuest(questId, false);
        g_extraSince.erase({ bot->GetGUID().GetCounter(), questId });

        if (quest)
        {
            SayToOwner(bot, player, "You let '" + quest->GetTitle() + "' go, so I laid mine down too - "
                                  "it made no sense to keep gathering for it on my own.", 120);

            LOG_INFO("server.loading", "[CompanionQuestSync] {} dropped '{}' ({}) - {} abandoned it",
                     bot->GetName(), quest->GetTitle(), questId, player->GetName());
        }
    }

    void OnPlayerQuestAccept(Player* player, Quest const* quest) override
    {
        if (!g_cfg.enable || !g_tableReady || !quest)
            return;

        // Our own additions for the companion arrive here too; never recurse.
        if (!player->GetSession() || player->GetSession()->IsBot())
            return;

        SyncReport report;
        Player* bot = ResolveCompanion(player, report);
        if (!bot)
            return;

        // Bag contents are the thing this hook is about to change; a value cached by the
        // last sweep would describe the state before the accept.
        g_missingCache.clear();

        uint32 const questId = quest->GetQuestId();
        QuestStatus const before = bot->GetQuestStatus(questId);

        SyncQuestToBot(player, bot, quest, nullptr);

        bool const landed = before == QUEST_STATUS_NONE && bot->GetQuestStatus(questId) != QUEST_STATUS_NONE;
        if (!landed)
            return;

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
};

void Addmod_companion_questsyncScripts()
{
    new CompanionQuestSyncWorldScript();
    new CompanionQuestSyncCommandScript();
    new CompanionQuestSyncPlayerScript();
}
