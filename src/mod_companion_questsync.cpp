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
        bool skipInCombat       = true;
        bool skipTimedQuests    = true;
        bool forceAccept        = false;
        uint32 debug            = 0;     // 0 silent, 1 per-change, 2 + heartbeat
        bool pinSync            = true;
        uint32 pinSyncSeconds   = 600;   // re-send the pin line at least this often
        bool dropFailedBotQuests = false; // a FAILED copy the owner never failed  // keep the addon map reconciled with the companion
        bool announce           = true;
        bool announceLogin      = true;
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
        std::string bindingTable = "wowlegends_companion";
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
        uint8 levelFrom = 0;
        uint8 levelTo = 0;
    };

    std::unordered_map<uint32, SessionTally> g_session;   // owner guid -> tally
    std::map<uint32, std::set<uint32>> g_sessionRefused;  // owner guid -> quests refused once
    std::map<uint32, std::set<uint32>> g_seenQuests;      // bot guid -> quests in its log last pass
    std::map<uint32, uint32> g_walkAt;                    // bot guid -> unix time
    std::map<uint32, std::string> g_lastPinSync;          // owner guid -> last pin line sent
    std::map<uint32, std::string> g_lastCompSync;         // owner guid -> last companions line
    std::map<uint32, uint32> g_pinSyncAt;                 // owner guid -> unix time
    std::map<uint32, std::set<uint64>> g_markedNpcs;      // owner guid -> npcs this module marked
    std::map<std::pair<uint32, uint32>, std::string> g_missingCache;  // (bot, quest) -> owed text
    std::map<std::pair<uint32, uint32>, std::string> g_poiCache;      // (zone, quest) -> "map X, Y"
    std::map<std::pair<uint32, uint32>, uint32> g_extraSince;         // (bot, quest) -> first seen extra
    std::set<std::pair<uint32, uint32>> g_failedSeen;                 // (bot, quest) already reported failed
    std::map<uint32, std::set<uint32>> g_repairing;       // bot guid -> quests being re-collected
    std::unordered_map<uint32, uint32> g_positionNoticeAt;  // bot guid -> unix time
    std::unordered_map<uint32, uint32> g_lastWhisperAt;     // owner guid -> unix time
    std::unordered_map<uint32, uint32> g_lastWhisperByBot;  // bot guid -> unix time, for fairness

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
        g_cfg.requireGrouped     = sConfigMgr->GetOption<bool>("CompanionQuestSync.RequireGrouped", true);
        g_cfg.skipInCombat       = sConfigMgr->GetOption<bool>("CompanionQuestSync.SkipInCombat", true);
        g_cfg.skipTimedQuests    = sConfigMgr->GetOption<bool>("CompanionQuestSync.SkipTimedQuests", true);
        g_cfg.forceAccept        = sConfigMgr->GetOption<bool>("CompanionQuestSync.ForceAccept", false);
        g_cfg.debug              = sConfigMgr->GetOption<uint32>("CompanionQuestSync.Debug", 0);
        g_cfg.pinSync            = sConfigMgr->GetOption<bool>("CompanionQuestSync.PinSync", true);
        g_cfg.pinSyncSeconds     = sConfigMgr->GetOption<uint32>("CompanionQuestSync.PinSyncSeconds", 600);
        g_cfg.dropFailedBotQuests = sConfigMgr->GetOption<bool>("CompanionQuestSync.DropFailedBotQuests", false);
        g_cfg.announce           = sConfigMgr->GetOption<bool>("CompanionQuestSync.AnnounceAtStartup", true);
        g_cfg.announceLogin      = sConfigMgr->GetOption<bool>("CompanionQuestSync.AnnounceOnLogin", true);
        g_cfg.intervalSeconds    = sConfigMgr->GetOption<uint32>("CompanionQuestSync.IntervalSeconds", 20);
        g_cfg.heartbeatSeconds   = sConfigMgr->GetOption<uint32>("CompanionQuestSync.HeartbeatSeconds", 300);
        g_cfg.levelMode          = sConfigMgr->GetOption<uint32>("CompanionQuestSync.LevelMode", 1);
        g_cfg.notifySkipped      = sConfigMgr->GetOption<uint32>("CompanionQuestSync.NotifySkippedQuests", 1);
        g_cfg.tidyInventory      = sConfigMgr->GetOption<bool>("CompanionQuestSync.TidyInventory", true);
        g_cfg.sellJunk           = sConfigMgr->GetOption<bool>("CompanionQuestSync.SellJunk", true);
        g_cfg.tidyVendorJunk     = sConfigMgr->GetOption<bool>("CompanionQuestSync.TidyVendorJunk", true);
        g_cfg.mapPins            = sConfigMgr->GetOption<bool>("CompanionQuestSync.MapPins", true);
        g_cfg.tidySurplus        = sConfigMgr->GetOption<bool>("CompanionQuestSync.TidySurplus", true);
        g_cfg.extraBagItem       = sConfigMgr->GetOption<uint32>("CompanionQuestSync.ExtraBagItem", 20400);
        g_cfg.extraBags          = sConfigMgr->GetOption<uint32>("CompanionQuestSync.ExtraBags", 4);
        g_cfg.itemKeepQuality    = sConfigMgr->GetOption<uint32>("CompanionQuestSync.ItemKeepQuality", 2);
        g_cfg.keepFreeSlots      = sConfigMgr->GetOption<uint32>("CompanionQuestSync.KeepFreeSlots", 2);
        g_cfg.bindingTable       = sConfigMgr->GetOption<std::string>("CompanionQuestSync.BindingTable", "wowlegends_companion");

        if (g_cfg.intervalSeconds < 5)
            g_cfg.intervalSeconds = 5;

        if (g_cfg.levelMode > 2)
            g_cfg.levelMode = 2;

        // The table name is interpolated into SQL, so accept nothing but a
        // plain identifier. Anything else disables the module rather than
        // risking an injection through the config file.
        bool ok = !g_cfg.bindingTable.empty();
        for (char const c : g_cfg.bindingTable)
        {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
            {
                ok = false;
                break;
            }
        }

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

    void PurgeCompanionState(uint32 botGuid)
    {
        g_passMemo.erase(botGuid);
        g_seenQuests.erase(botGuid);
        g_repairing.erase(botGuid);
        g_notified.erase(botGuid);
        g_pendingNotices.erase(botGuid);
        g_sellAt.erase(botGuid);
        g_lastWhisperByBot.erase(botGuid);

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

    /// Apply the owner's union of markers, then clear the ones no companion needs.
    void ApplyMarks(Player* owner, std::set<uint64> const& wanted)
    {
        if (!g_cfg.mapPins || !owner->PlayerTalkClass)
            return;

        uint32 const ownerGuid = owner->GetGUID().GetCounter();

        for (uint64 const guid : wanted)
            owner->PlayerTalkClass->SendQuestGiverStatus(DIALOG_STATUS_REWARD, ObjectGuid(guid));

        std::set<uint64>& previously = g_markedNpcs[ownerGuid];
        for (uint64 const stale : previously)
        {
            if (wanted.count(stale))
                continue;

            owner->PlayerTalkClass->SendQuestGiverStatus(DIALOG_STATUS_NONE, ObjectGuid(stale));
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
                    SayToOwner(bot, owner, "I still owe " + spot.name + " my own copy of '" +
                                          quest->GetTitle() +
                                          "' - I will hand it in next time we are near" +
                                          (coords.empty() ? "." : " (" + coords + ")."));
                else
                    SayToOwner(bot, owner, "When we get a chance, could we stop by " + spot.name +
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
                SayToOwner(bot, owner, "Could we help each other finish '" + quest->GetTitle() +
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
            SayToOwner(bot, owner, "Could we help each other finish '" + quest->GetTitle() +
                                  "' before we move on? I still owe " + owed +
                                  " - I will gather them as we go" +
                                  (where.empty() ? "." : " (" + where + ")."), 45, questId);
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

            StepBesideOwner(owner, bot, spot.x, spot.y);

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

        if (g_cfg.requireGrouped)
        {
            Group* group = owner->GetGroup();
            report.grouped = group && group->IsMember(bot->GetGUID());
            if (!report.grouped)
                return nullptr;
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
        struct PassTimer
        {
            std::chrono::steady_clock::time_point start;
            Player* owner;

            ~PassTimer()
            {
                if (g_cfg.debug < 2)
                    return;

                auto const us = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - start).count();

                LOG_INFO("server.loading", "[CompanionQuestSync] pass for {} took {} us",
                         owner ? owner->GetName() : "?", us);
            }
        } timer{ startedAt, owner };

        SyncReport report;

        Player* bot = ResolveCompanion(owner, report, botGuid);
        if (!bot)
            return report;

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
        DeliverHandIns(owner, bot);
        RepairStrandedDelivery(owner, bot, report);   // retry-worthy: belongs above the memo

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
        EnsureBags(owner, bot);                        // room before anything else
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
            if (Player* speaker = ResolveCompanion(owner, pinReport))
                speaker->Whisper("|cff7a7a7a" + line + "|r", LANG_UNIVERSAL, owner);
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
            handler.PSendSysMessage("|cffff8000Companion|r: no longer following |cffffff00{}|r: {}. "
                                    "|cffffff00.companionsync list|r shows who is bound, "
                                    "|cffffff00.companionsync bind <name>|r adds one.",
                                    one.name, one.reason);

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
    ///     Comp sync 2: Healy,5,32,4,ogs,110|Soak,1,32,2,o,105
    ///                  name,class,level,quests,flags,free slots
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
            uint32 level = 0;
            uint32 quests = 0;
            uint32 freeSlots = 0;

            if (online)
            {
                name = bot->GetName();
                classId = uint32(bot->getClass());
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

            line += SafePinName(name) + "," + std::to_string(classId) + "," + std::to_string(level) + "," +
                    std::to_string(quests) + "," + (flags.empty() ? "-" : flags) + "," + std::to_string(freeSlots);
        }

        return line;
    }

    /// Send the companions line when it changes, on login, and on demand.
    ///
    /// Same channel and cadence as the pin payload: a whisper, because that is the only
    /// thing the client can read. Nothing else tells the addon who is bound - commands go
    /// one way, and a client cannot query the server.
    void SendCompSync(Player* owner, bool force = false)
    {
        if (!g_cfg.pinSync || !owner || !owner->GetSession())
            return;

        uint32 const ownerGuid = owner->GetGUID().GetCounter();
        std::string const payload = BuildCompLine(owner);

        std::string& last = g_lastCompSync[ownerGuid];
        uint32 const now = static_cast<uint32>(time(nullptr));

        if (!force && payload == last && !last.empty())
            return;

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

            if (Player* speaker = ResolveCompanion(owner, compReport))
                speaker->Whisper("|cff7a7a7a" + line + "|r", LANG_UNIVERSAL, owner);
        }

        last = payload;

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
                    LOG_INFO("server.loading", "[CompanionQuestSync] summon came up: {} ({})", pending.name, pending.botGuid);

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

        aggregate.hasCompanion = true;
        g_multiCompanion = bots.size() > 1;

        // One union of turn-in markers for the owner, applied after every companion has
        // contributed: applying them per companion let the second pass clear the first
        // companion's marker, so a giver only one of them needed flickered each sweep.
        std::set<uint64> marks;

        for (uint32 const botGuid : bots)
        {
            SyncReport const one = SyncCompanionFor(owner, botGuid, marks);

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

        return aggregate;
    }
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
    }

    void OnUpdate(uint32 diff) override
    {
        if (!g_cfg.enable || !g_tableReady)
            return;

        // Summon and dismiss outcomes, independent of the sync timer: a player who asked
        // for a companion should not wait for the next sweep to learn it did not work.
        CheckPendingSummons();

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
            ProbeBindingTable();

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
    bool _announced = false;

    // Heartbeat window state (Debug >= 2).

    std::unordered_map<uint32, std::string> g_hbLines;
};


class CompanionQuestSyncCommandScript : public CommandScript
{
public:
    CompanionQuestSyncCommandScript() : CommandScript("CompanionQuestSyncCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable companionSyncTable =
        {
            { "pins",   HandlePinsCommand,   SEC_PLAYER, Console::No },
            { "quests", HandleQuestsCommand, SEC_PLAYER, Console::No },
            { "status", HandleStatusCommand, SEC_PLAYER, Console::No },
            { "level",  HandleLevelCommand,  SEC_PLAYER, Console::No },
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

            DispatchPlayerbots(player, "remove " + name);
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
        if (!g_cfg.enable || !g_cfg.announceLogin || !g_tableReady)
            return;

        if (!player->GetSession() || player->GetSession()->IsBot())
            return;

        ChatHandler handler(player->GetSession());

        // Reconcile the map on the way in: a pin left behind by a session that ended
        // badly is cleared the moment the player logs back in.
        if (g_cfg.pinSync)
        {
            SendPinSync(player, true);
            SendCompSync(player, true);
        }

        // Sweep once immediately: the timer would leave the first thirty seconds of a
        // session showing whatever was true when the player last logged out.
        if (g_cfg.autoSync && g_cfg.enable)
            SyncCompanion(player);

        handler.SendSysMessage("|cff00ff00This server runs with |cff00ccffmod-companion-questsync|r |cffcccccc- companion quest + level sync|r");
        handler.SendSysMessage("|cff00ff00Companion:|r quests and level follow yours. |cffffff00.companionsync status|r for a report, |cffffff00.companionsync|r to sync now.");
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

        if (!tally.added && !tally.onAccept && !tally.completed && !tally.refused && !tally.reopened && !tally.levelFrom)
            return;

        std::string level;
        if (tally.levelFrom && tally.levelTo)
            level = ", level " + std::to_string(tally.levelFrom) + " -> " + std::to_string(tally.levelTo);

        LOG_INFO("server.loading", "[CompanionQuestSync] session: {} - {} quest(s) added ({} on accept), {} completed, {} reopened, {} refused{}",
                 player->GetName(), tally.added + tally.onAccept, tally.onAccept, tally.completed, tally.reopened, tally.refused, level);
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
