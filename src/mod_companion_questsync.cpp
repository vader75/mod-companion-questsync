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
 * - Objective credit is not copied. Group credit and the companion's actions
 *   must satisfy the core's objective rules, even when the owner is done.
 * - Timed quests are skipped by default; when enabled, the core shares the
 *   owner's remaining time rather than granting a fresh timer.
 * - Companions are found through the binding table, never through AI internals,
 *   so this file has no dependency on mod-playerbots headers or link order.
 */

#include "Bag.h"
#include "Chat.h"
#include "Config.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include <cmath>
#include <fstream>

// defined in DBCStores.cpp, id-indexed by the AreaTable's own first column
extern DBCStorage<AreaTableEntry> sAreaTableStore;

#include "GossipDef.h"
#include "GameObject.h"
#include "GameObjectAI.h"
#include "GameTime.h"
#include "ItemPackets.h"
#include "Group.h"
#include "Item.h"
#include "Log.h"
#include "Mail.h"
#include "LootMgr.h"
#include "MotionMaster.h"
#include "Map.h"
#include "MapMgr.h"
#include "Cell.h"
#include "CellImpl.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include <atomic>
#include <chrono>
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "Player.h"
#if __has_include("CompanionLootPolicy.h")
#include "CompanionLootPolicy.h"
#define CQ_HAS_COMPANION_LOOT_POLICY
#endif
#include "ItemUsageValue.h"  // playerbots' verdict on what an item is worth
#include "PlayerbotAI.h"     // sell vendor trash through the bot's own AI
#include "AiFactory.h"       // which talent tree a companion is actually in
#include "AttackAction.h"
#include "Engine.h"
#include "ReachTargetActions.h"
#include "Pet.h"             // pet autocasts: Growl and Torment are what taunt for us
#include "SpellMgr.h"        // naming a pet ability we turn off
#include "Playerbots.h"    // GET_PLAYERBOT_AI
#ifdef MOD_PLAYERBOTS
#include "PlayerbotMgr.h"
#include "StatsWeightCalculator.h"  // reward choice scored the way the bot's own AI would
#include "PlayerbotFactory.h"      // teach a hand-raised level what a trainer would
#include "RandomPlayerbotMgr.h"    // the per-bot value store keeps the profession choice
#if __has_include("PlayerbotInitializationPolicy.h")
#include "PlayerbotInitializationPolicy.h"
#endif
#endif
#include "QuestDef.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "SkillExtraItems.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <deque>
#include <future>
#include <memory>
#include <tuple>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <optional>

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
        bool dropFailedBotQuests = false; // a FAILED copy the owner never failed  // keep the addon map reconciled with the companion
        bool announce           = true;
        bool announceLogin      = true;
        bool catchUpGroundItems = false;    // finish a ground-object quest the world will not yield
        bool followThroughTeleport = true; // a hearthstone, portal or summon takes the companion too
        uint8 handInMode         = 1;      // 0 = module hand-ins off, 1 = native turn-in, 2 = native with lead reminder
        float handInNear         = 40.0f;  // HandInMode 2: remind beyond this distance, yards
        uint32 handInDelaySeconds = 20;    // a finished quest is visible this long before it rewards
        uint32 catchUpIdleSeconds = 120;   // how long the count must sit still first
        uint32 intervalSeconds  = 20;
        uint32 heartbeatSeconds = 300;  // Debug >= 2 heartbeat window (0 = off)
        uint32 levelMode        = 1;   // 0 off, 1 raise to owner, 2 exact
        uint32 notifySkipped    = 1;   // 0 off, 1 class/race only, 2 all refusals
        bool tidyInventory     = true;
        bool sellJunk          = true;      // confirmed junk only, at a vendor
        bool trimSurplus       = true;      // spare trade goods, at a vendor, only when bags are tight
        bool levelGear         = true;      // re-gear a kit that lags the companion's level
        bool mapPins           = true;      // push the companion's quest areas onto the owner's map
        bool suppressGapClosers = true;     // take Charge away from a companion that is not tanking
        bool guardFlankAggro    = false;
        bool tankFirst         = false;
        float tankFirstDistance = 10.0f;
        uint32 roleChangeCooldown = 60;     // seconds between role changes for one companion (0 = off)
        uint32 specFollowRole = 2;          // 0 off, 1 log the decision, 2 restore or build talents
        uint32 specMinLevel = 10;           // the first level with native talent points
        std::string formation  = "auto";    // auto | close | tight | loose | spread | lead | off

        bool gatherCatchUp     = true;      // the land asks a minimum of its gatherers
        uint32 professionNoGather = 0;      // 0 gather, 1 log the change, 2 professions at the cap without gathering
        bool vendorRestock     = true;      // near a trade/repair NPC: reagents topped up, gear mended
        bool autoProduce       = true;      // crafting is its own habit; the button is for the report
        uint32 produceInterval  = 180;      // seconds between quiet crafts
        uint32 produceMaxItems  = 4;        // one quiet craft makes at most this many items (0 = uncapped)
        bool produceCombatFirst = true;     // potions and weapon stones before smelted stock
        bool autoTrade         = true;      // beside you with something spare: the trade opens itself
        uint32 extraBagItem    = 20400;     // Pumpkin Bag, 16 slots, no level requirement
        uint32 extraBags       = 4;         // bag slots to fill with it (0 = off)

        uint32 keepFreeSlots   = 2;
        uint32 bagMargin       = 8;  // the tidy ladder starts this many slots before the wall
        bool mailHome          = true;    // curated mailbox fallback
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
    struct BindingSet
    {
        std::vector<uint32> bots;   // every companion bound to one owner
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

        // movement
        constexpr uint32 WalkStep          = 20;  // one giver or gather hop per 20 s
        constexpr uint32 FollowDelay       = 3;   // the destination is still loading
        constexpr float  FollowDistance    = 120.0f;  // yards; closer is not "lost"
        constexpr uint32 FollowRetry       = 60;
        constexpr uint32 DungeonExitRetry  = 300;
        constexpr uint32 AirborneGrace     = 5;   // a real fall ends in seconds; longer is a stuck flag

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
        constexpr uint32 JunkNotice        = 600; // per-owner bag pressure reminder
        constexpr uint32 RewardProd        = 60;  // how often a hand-in attempt is repeated
        constexpr uint32 AcceptWindow      = 60;  // a quest accepted this recently is still settling
        constexpr uint32 MailHome          = 900; // per-owner curated parcel cooldown
        constexpr uint32 TradeDelivery     = 600; // per-owner proactive trade cooldown
        constexpr uint32 MailAfterOffer    = 120; // give a town trade first chance
        constexpr uint32 TownOfferTimeout  = 30;  // don't hold a town window open indefinitely
        constexpr uint32 NamedOfferTimeout = 45;
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
    std::unordered_map<uint32, uint32> g_junkNoticeAt;   // owner guid -> unix time

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
        uint32 sold = 0;        // confirmed junk stacks sold to a real merchant
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
    std::map<uint32, std::string> g_lastSkillSync;        // owner guid -> last profession snapshot
    /// bot guid -> { has an unfinished ground objective, when that was last checked }
    std::map<uint32, std::pair<bool, uint32>> g_gatherPending;

    /// Owner-wide mail cooldown and explicit, per-login supply requests.
    std::map<uint32, uint32> g_ownerDeliveryMailAt;
    std::map<uint32, uint32> g_ownerMailPolicyAt;
    std::map<uint32, std::set<uint32>> g_requestedSupplies;
    std::map<uint32, uint32> g_supplyCatalogAt;
    std::map<uint32, uint32> g_supplyActionAt;

    /// (bot guid, quest id) -> when the copy first became ready to hand in.
    ///
    /// The middle ground between instant and manual: the finished quest is visible as a gold
    /// pin for HandInDelaySeconds, then rewards itself.
    std::map<uint64, uint32> g_readyAt;
    std::map<uint64, uint32> g_handInTravelHintAt;  // (owner, quest) -> last travel hint

    /// Owners whose teleport has just been seen: the companion is brought along a moment
    /// later, once the client has finished loading the destination.
    struct PendingFollow
    {
        uint32 queuedAt;
        bool verifyDestination = false;
        uint32 map = 0;
        float x = 0.0f, y = 0.0f, z = 0.0f;
        uint32 sourceMap = 0;
        uint32 sourceInstance = 0;
        std::map<uint32, std::string> holdNotes; // companion -> why it was last held back
        std::set<uint32> whisperedBots; // already asked the owner not to summon these
        std::map<uint32, uint32> airSince; // companion -> first tick its airborne flag was seen
    };
    std::map<uint32, PendingFollow> g_pendingFollow;
    std::set<uint32> g_taxiInProgress;

    bool IsOnTaxi(Player* player)
    {
        return player->IsInFlight() || player->m_taxi.GetTaxiDestination() != 0;
    }

    void ObserveTaxiLanding(Player* owner, uint32 now)
    {
        if (!g_cfg.followThroughTeleport || !owner->IsInWorld() || owner->IsBeingTeleported())
            return;

        uint32 const id = owner->GetGUID().GetCounter();
        if (IsOnTaxi(owner))
            g_taxiInProgress.insert(id);
        else if (g_taxiInProgress.erase(id) && owner->IsAlive() && !owner->GetMap()->Instanceable())
            g_pendingFollow[id] = { now };
    }


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

    std::map<uint32, std::string> g_lastPoolStock;        // owner guid -> last pool stock line
    std::map<uint32, uint32> g_pinSyncAt;                 // owner guid -> unix time
    struct PinStream
    {
        uint64 epoch = 0;
        uint32 revision = 0;
        bool tagged = false;
        bool handInStates = false;
        bool pinItems = false;
    };
    std::map<uint32, PinStream> g_pinRevisions;           // presence means this client requested v2
    std::map<uint32, uint32> g_pinRequestAt;              // bound repeat requests within a session

    std::map<std::pair<uint32, uint32>, std::string> g_missingCache;  // (bot, quest) -> owed text
    std::map<std::pair<uint32, uint32>, std::string> g_poiCache;      // (zone, quest) -> "map X, Y"
    std::map<std::pair<uint32, uint32>, uint32> g_extraSince;         // (bot, quest) -> first seen extra
    std::set<std::pair<uint32, uint32>> g_failedSeen;                 // (bot, quest) already reported failed
    std::set<uint32> g_deadNoticeAt;                      // bots whose death was announced, one per death
    std::map<uint32, uint32> g_lastGatherZone;            // bot guid -> last zone the gather catch-up ran for
    std::map<uint32, uint32> g_lastRepair;
    std::map<uint32, uint32> g_lastVendorRestock;         // bot guid -> unix time of the last vendor stop
    thread_local uint32 g_commandReplyOwner = 0;          // only while executing a player command
    std::map<uint32, uint32> g_lastAutoProduce;           // bot guid -> unix time of the last quiet craft
    std::map<uint32, uint32> g_lastAutoTrade;             // owner guid -> last applied automatic offer
    std::map<uint32, uint32> g_lastAutoTradeAttempt;      // separate from actual mail precedence
    /// A trade whose items wait for the window to open: the begin round-trip runs on
    /// the bot's own AI tick, and items SetItem'd before it are lost to the client -
    /// the owner saw an empty trade window while the companion insisted it had put
    /// four stacks in. The offer is applied two seconds later, once the window is up.
    struct SupplyOrder
    {
        uint32 entry = 0;   // zero means the normal multi-item delivery
        uint32 keep = 0;    // the product already held before a requested craft
        uint32 fresh = 0;   // only newly crafted units are eligible for that trade
        uint16 sourcePos = 0; // preflighted inventory position for the crafted batch
    };

    struct PendingTrade
    {
        uint32 ownerGuid = 0;
        uint32 at = 0;
        std::vector<std::pair<ObjectGuid, uint32>> offer; // exact source stack and surplus count
        bool applied = false;                           // the window is filled; now guard it
        bool announceFailure = false;                  // automatic probes do not report routine deferrals
        bool curated = false;
        bool automatic = false;
        SupplyOrder supply;
        uint32 nonce = 0;

        uint64 noticeId = 0;                            // identifies this offer, not a later window
        struct OfferedItem { TradeSlots slot; ObjectGuid guid; uint32 count; };
        std::vector<OfferedItem> offeredItems;
    };
    constexpr uint32 TradeReserveCount = 5;
    constexpr uint32 CompanionBagReserve = 8;
    // A companion with room in its pack is not shopped from. Below the comfort line a
    // vendor visit sells the *spare* trade goods back down to a recipe budget, so bags
    // stop being a warehouse without the professions losing what they need to craft.
    constexpr uint32 CompanionBagComfort = 16;
    constexpr uint32 CompanionReagentKeep = 5;   // one recipe's worth of a bought reagent
    constexpr uint32 CompanionMatKeep = 10;      // two stacks of a gathered material
    // A pool body is a level-1 starter; its kit has to catch up with the level the module
    // gives it. Below this gap the kit is left alone, and a fresh pass waits for a step
    // so a companion is not re-geared every time it dings.
    constexpr uint32 CompanionGearLag = 4;
    constexpr uint32 CompanionGearStep = 5;
    constexpr uint32 CompanionConsumableStacks = 2;   // what a player carries, not a pallet
    std::map<uint32, PendingTrade> g_pendingTrade;        // bot guid -> the offer waiting for the window
    uint64 g_nextTradeNoticeId = 0;
    struct DeliveryReceipt
    {
        uint32 ownerGuid;
        uint32 nonce;
        std::chrono::steady_clock::time_point deadline;
        bool transferred;
        std::shared_future<bool> saved;
    };
    std::map<uint64, DeliveryReceipt> g_deliveryReceipts;
    void SendProtocolLine(Player* owner, std::string const& line);
    static bool CanReleaseTemplate(Player* bot, Player* owner, ItemTemplate const* proto);
    static bool IsDeliveryEnabled(PendingTrade const& pending);
    static std::vector<std::pair<ObjectGuid, uint32>> PlanDelivery(Player* bot, Player* owner, bool curated,
        bool mail = false, SupplyOrder supply = {});
    static bool DeliveryStillValid(Player* bot, Player* owner, PendingTrade const& pending);
    static bool IsControlledCompanion(Player* bot, Player* owner);
    static bool HasSupplyAuthority(Player* bot, Player* owner, bool authoritative);
    static bool IsTownDeliveryStop(Player* owner, Player* bot);
    WorldObject* FindReadyQuestEnder(Player* bot, Quest const* quest);
    bool SayToOwner(Player* bot, Player* owner, std::string const& text, uint32 minGap = 45,
                    uint32 validWhileMissing = 0, bool handIn = false, uint64 tradeNoticeId = 0,
                    ObjectGuid readyGiver = ObjectGuid::Empty);
    static uint16 FindFreeBagPos(Player* bot, Item const* item);
    std::map<uint32, std::set<uint32>> g_repairing;       // bot guid -> quests being re-collected
    std::unordered_map<uint32, uint32> g_positionNoticeAt;  // bot guid -> unix time
    std::unordered_map<uint32, uint32> g_lastWhisperAt;     // owner guid -> unix time
    std::unordered_map<uint32, uint32> g_lastWhisperByBot;

    /// What each companion last said, and when. A gear pass re-derives the same re-gear sentence on
    /// every tick, and the queue only deduped against things still waiting to be spoken - so the
    /// player heard the same line three and four times. Repeats inside a quarter of an hour are
    /// dropped now: the world has not changed between them, so the news has not either.
    std::unordered_map<uint32, std::pair<std::string, uint32>> g_lastSpoken;

    /// When each companion's role last actually changed. Two things send roles - the operator's
    /// command and the addon re-sending what it remembers - and a panel showing one role while the
    /// other side insists on the old one made them trade places every few seconds: "is now tank!",
    /// "is now dps!", a retrain and two whispers each time. A repeat of the role already stored is
    /// a no-op, and a fresh flip inside ten seconds is refused rather than obeyed.

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
        uint64 tradeNoticeId = 0;
        ObjectGuid readyGiver = ObjectGuid::Empty;
    };

    struct PendingNotice
    {
        uint32 questId = 0;
        std::string text;
    };

    std::map<uint32, std::vector<PendingNotice>> g_pendingNotices;  // bot guid -> lines
    std::unordered_map<uint32, uint32> g_sellAt;            // bot guid -> unix time
    std::unordered_map<uint32, uint32> g_trimAt;            // bot guid -> unix time
    std::unordered_map<uint32, uint32> g_gearLevel;         // bot guid -> level the kit was built for

    /// Companions whose talent state has been reported this boot: one line each, so the log answers
    /// "did it take?" without a per-tick stream.
    std::set<uint32> g_talentGrantNotes;

    /// Last shape check per companion: the hands can change outside our gear pass, so the watch
    /// is throttled rather than tied to the pass.
    std::unordered_map<uint32, uint32> g_kitWatchAt;

    /// Where talents start to mean something. Below this a tree is a handful of points wide and a
    /// spec is theatre; from here the role's tree is worth filling, template or no template.
    uint8 const CompanionTalentsFrom = 10;

    /// Pieces a re-gear pulled out of a slot and left in the bags, bot guid -> item guid.
    ///
    /// playerbots destroys the item it replaces *in* a slot, so a plain upgrade leaves the bags
    /// alone. An offhand is the exception: equipping a two-hander runs AutoUnequipOffhandIfNeed,
    /// which puts the shield back into the bags - so a warrior flipping tank -> dps sheds a shield
    /// per flip and fills its bags with them. These are sold at the next vendor (never destroyed,
    /// never mailed), and only pieces this module's own pass displaced are ever marked.
    std::unordered_map<uint32, std::set<uint64>> g_staleGear;

    using RoleHands = std::array<ObjectGuid, 2>;
    using RoleTalents = std::map<uint32, uint8>; // talent DBC id -> zero-based rank
    struct SavedRoleLoadout
    {
        RoleHands hands;
        RoleTalents talents;
    };
    struct RoleSwitchState
    {
        uint32 ownerGuid = 0;
        std::string pending;
        std::map<std::string, SavedRoleLoadout> saved;
    };
    std::map<uint32, RoleSwitchState> g_roleSwitches;

    bool IsSavedRoleItem(uint32 botGuid, ObjectGuid guid)
    {
        if (guid.IsEmpty())
            return false;
        auto const state = g_roleSwitches.find(botGuid);
        if (state == g_roleSwitches.end())
            return false;
        for (auto const& [role, loadout] : state->second.saved)
            if (loadout.hands[0] == guid || loadout.hands[1] == guid)
                return true;
        return false;
    }

    bool IsStaleGear(uint32 botGuid, Item* item)
    {
        if (!item)
            return false;
        auto const it = g_staleGear.find(botGuid);
        return it != g_staleGear.end() && it->second.count(item->GetGUID().GetRawValue()) != 0;
    }
    std::unordered_map<uint32, uint32> g_taughtLevel;       // bot guid -> level its spells/professions were synced for
    std::unordered_map<uint32, uint32> g_equipAt;           // bot guid -> unix time
    std::map<uint32, std::deque<QueuedWhisper>> g_whisperQueue;  // owner guid -> waiting texts
    std::map<std::pair<uint32, uint32>, uint32> g_nudgeAt;     // (bot, quest) -> unix time
    std::map<std::pair<uint32, uint32>, uint32> g_turnInHintAt;     // (bot, quest) -> unix time
    std::map<std::pair<uint32, uint32>, uint32> g_rewardProdAt;     // (bot, quest) -> unix time
    std::map<std::pair<uint32, uint32>, uint32> g_addedAt;          // (bot, quest) -> when we added it
    std::map<std::pair<uint32, uint32>, bool> g_ownerCycleSeen; // (bot, quest) -> already held in current owner acceptance
    std::map<std::pair<uint32, uint32>, uint32> g_addedCount;       // (bot, quest) -> log size at add
    std::map<std::pair<uint32, uint32>, uint32> g_vanishLogged;      // (bot, quest) -> unix time
    std::unordered_map<uint32, std::string> g_pinnedQuests;        // owner guid -> pinned set
    std::map<uint32, std::string> g_formation;                     // owner guid -> formation preset
    std::map<uint32, std::string> g_formationApplied;              // bot guid -> formation we set
    struct IdleTrack
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;
        uint32 since = 0;           // when the position last changed
    };
    std::map<uint32, IdleTrack> g_ownerIdle;                        // owner guid -> standing still?
    std::map<uint32, std::string> g_roleNotes;    // bot guid -> strategy changes that did not apply
    std::map<uint32, std::string> g_engageNotes;  // bot guid -> why engagement control is unavailable
    struct AppliedEdit
    {
        std::string name;
        int state;
        bool added;
    };

    std::map<uint32, std::vector<AppliedEdit>> g_roleApplied;   // what this module changed, per bot
    std::set<uint32> g_passiveHeld;               // bots we have put into the passive stance
    std::atomic<int> g_engageColumn{0};           // 0 unknown, 1 present, -1 missing
    std::atomic<int> g_passiveStrategy{0};        // 0 unprobed, 1 usable, -1 not in this build
    std::atomic<int> g_quietColumn{0};            // 0 unknown, 1 present, -1 missing
    std::map<uint32, uint32> g_pinnedTarget;      // bot guid -> owner target we pointed it at
    std::map<uint32, std::vector<uint32>> g_petTauntOff;   // bot guid -> pet autocasts we turned off
    std::map<uint32, std::vector<uint32>> g_spellsWithheld;  // bot guid -> gap closers we muted
    std::map<uint32, uint32> g_roleApplyAt;       // bot guid -> last reconcile
    uint32 constexpr kRoleReassertSeconds = 60;   // playerbots drops strategies on spec/level change

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

    static bool IsDeliveryEnabled(PendingTrade const& pending)
    {
        return g_cfg.enable && g_tableReady && (!pending.automatic || g_cfg.autoTrade);
    }
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
        g_cfg.catchUpGroundItems = sConfigMgr->GetOption<bool>("CompanionQuestSync.CatchUpGroundItems", false);
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
        g_cfg.trimSurplus        = sConfigMgr->GetOption<bool>("CompanionQuestSync.TrimSurplus", true);
        g_cfg.levelGear           = sConfigMgr->GetOption<bool>("CompanionQuestSync.LevelGear", true);

        g_cfg.mapPins            = sConfigMgr->GetOption<bool>("CompanionQuestSync.MapPins", true);
        g_cfg.formation          = sConfigMgr->GetOption<std::string>("CompanionQuestSync.Formation", "auto");
        g_cfg.suppressGapClosers = sConfigMgr->GetOption<bool>("CompanionQuestSync.SuppressGapClosers", true);
        g_cfg.guardFlankAggro = sConfigMgr->GetOption<bool>("CompanionQuestSync.GuardFlankAggro", false);
        g_cfg.tankFirst = sConfigMgr->GetOption<bool>("CompanionQuestSync.TankFirst", false);
        g_cfg.tankFirstDistance = std::clamp(sConfigMgr->GetOption<float>("CompanionQuestSync.TankFirstDistance", 10.0f), 5.0f, 20.0f);
        g_cfg.roleChangeCooldown = sConfigMgr->GetOption<uint32>("CompanionQuestSync.RoleChangeCooldown", 60);
        g_cfg.specFollowRole = sConfigMgr->GetOption<uint32>("CompanionQuestSync.SpecFollowRole", 2);
        g_cfg.specMinLevel = sConfigMgr->GetOption<uint32>("CompanionQuestSync.SpecMinLevel", 10);

        g_cfg.gatherCatchUp      = sConfigMgr->GetOption<bool>("CompanionQuestSync.GatherCatchUp", true);
        g_cfg.professionNoGather = sConfigMgr->GetOption<uint32>("CompanionQuestSync.ProfessionNoGather", 0);
        g_cfg.vendorRestock      = sConfigMgr->GetOption<bool>("CompanionQuestSync.VendorRestock", true);
        g_cfg.autoProduce        = sConfigMgr->GetOption<bool>("CompanionQuestSync.AutoProduce", true);
        g_cfg.produceInterval    = sConfigMgr->GetOption<uint32>("CompanionQuestSync.ProduceIntervalSeconds", 180);
        g_cfg.produceMaxItems    = sConfigMgr->GetOption<uint32>("CompanionQuestSync.ProduceMaxPerInterval", 4);
        g_cfg.produceCombatFirst = sConfigMgr->GetOption<bool>("CompanionQuestSync.ProduceCombatFirst", true);

        if (g_cfg.professionNoGather > 2)
            g_cfg.professionNoGather = 2;

        if (g_cfg.produceInterval < 30)
            g_cfg.produceInterval = 30;

        // A flip into no-gather mode must take effect at the level each companion
        // already is, not at its next one: the per-level teach latch would
        // otherwise hold every skill below the cap until the companion dings.
        // Clearing it makes the next pass re-teach (idempotent, and silent where
        // nothing has to change).
        static uint32 s_prevNoGather = g_cfg.professionNoGather;
        if (g_cfg.professionNoGather != s_prevNoGather)
        {
            s_prevNoGather = g_cfg.professionNoGather;
            if (g_cfg.professionNoGather >= 2)
                g_taughtLevel.clear();
        }
        g_cfg.autoTrade          = sConfigMgr->GetOption<bool>("CompanionQuestSync.AutoTrade", true);
        g_cfg.extraBagItem       = sConfigMgr->GetOption<uint32>("CompanionQuestSync.ExtraBagItem", 20400);
        g_cfg.extraBags          = sConfigMgr->GetOption<uint32>("CompanionQuestSync.ExtraBags", 4);

        g_cfg.keepFreeSlots      = sConfigMgr->GetOption<uint32>("CompanionQuestSync.KeepFreeSlots", 2);
        g_cfg.bagMargin          = sConfigMgr->GetOption<uint32>("CompanionQuestSync.BagMargin", 8);
        g_cfg.mailHome           = sConfigMgr->GetOption<bool>("CompanionQuestSync.MailHome", true);
        g_cfg.bindingTable       = sConfigMgr->GetOption<std::string>("CompanionQuestSync.BindingTable", "cq_bindings");
        g_cfg.poolTable          = sConfigMgr->GetOption<std::string>("CompanionQuestSync.PoolTable", "cq_pool");
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

    void ProbeBindingTable()
    {
        g_bindingHasName = static_cast<bool>(
            CharacterDatabase.Query("SHOW COLUMNS FROM {} LIKE 'companion_name'", g_cfg.bindingTable));
        g_bindingHasCreatedAt = static_cast<bool>(
            CharacterDatabase.Query("SHOW COLUMNS FROM {} LIKE 'created_at'", g_cfg.bindingTable));

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



        g_repairing.erase(botGuid);
        g_notified.erase(botGuid);
        g_pendingNotices.erase(botGuid);
        g_sellAt.erase(botGuid);
        g_lastRepair.erase(botGuid);
        g_lastVendorRestock.erase(botGuid);
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
        EraseByBot(g_turnInHintAt, botGuid);
        EraseByBot(g_rewardProdAt, botGuid);
        EraseByBot(g_addedAt, botGuid);
        EraseByBot(g_ownerCycleSeen, botGuid);
        EraseByBot(g_addedCount, botGuid);
        EraseByBot(g_vanishLogged, botGuid);
        EraseByBot(g_extraSince, botGuid);

        // Level latches live in the same pooled key space: a recycled guid must not
        // inherit "already taught" or "already geared" from the body that had it first,
        // or a fresh companion silently skips its spell, talent and profession catch-up.
        g_roleNotes.erase(botGuid);
        g_engageNotes.erase(botGuid);
        // no Player* here: a purged bot is leaving the world, and playerbots rebuilds an AI -
        // and with it the strategy set - when the bot is next summoned
        g_roleApplied.erase(botGuid);
        g_passiveHeld.erase(botGuid);
        g_pinnedTarget.erase(botGuid);
        g_petTauntOff.erase(botGuid);
        g_spellsWithheld.erase(botGuid);
        g_formationApplied.erase(botGuid);
        g_roleApplyAt.erase(botGuid);
        g_talentGrantNotes.erase(botGuid);
        g_taughtLevel.erase(botGuid);
        g_gearLevel.erase(botGuid);
        g_staleGear.erase(botGuid);
        g_kitWatchAt.erase(botGuid);
        g_trimAt.erase(botGuid);
        g_lastAutoProduce.erase(botGuid);
        g_deadNoticeAt.erase(botGuid);
        EraseByBot(g_missingCache, botGuid);
    }

    bool g_custodyReady = false;
    bool g_starterBagsReady = false;

    bool CharacterTableExists(char const* table)
    {
        return static_cast<bool>(CharacterDatabase.Query(
            "SELECT 1 FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name = '{}'", table));
    }

#ifdef MOD_PLAYERBOTS_INITIALIZATION_POLICY_V1
    bool g_nativeInitializationVetoRegistered = false;
    enum class NativeCustodySchema { Unknown, Absent, Present };
    std::atomic<NativeCustodySchema> g_nativeCustodySchema{NativeCustodySchema::Unknown};

    // The custody reservation is keyed by body, not by the current summoner,
    // tracking, the QuestSync enable flag, or the account's other characters.
    bool DeferNativeBotInitialization(uint32 bodyGuid)
    {
        NativeCustodySchema const schema = g_nativeCustodySchema.load();
        if (schema != NativeCustodySchema::Present)
            return schema != NativeCustodySchema::Absent;

        // COUNT always returns a row on success. Never treat a failed read as
        // "not ours". Even a broken binding cannot release the reservation.
        QueryResult row = CharacterDatabase.Query(
            "SELECT COUNT(*) FROM cq_companion_custody WHERE bot_guid = {} "
            "AND module = 'cq' AND state IN ('owned', 'retiring')", bodyGuid);
        return !row || row->Fetch()[0].Get<uint64>() != 0;
    }

    void ProbeNativeCustodySchema()
    {
        QueryResult row = CharacterDatabase.Query(
            "SELECT (SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = DATABASE() "
            "AND table_name = 'cq_companion_custody'), "
            "(SELECT COUNT(*) FROM information_schema.columns WHERE table_schema = DATABASE() "
            "AND table_name = 'cq_companion_custody' AND column_name IN ('bot_guid', 'module', 'state'))");
        NativeCustodySchema schema = NativeCustodySchema::Unknown;
        if (row)
        {
            uint64 const tables = row->Fetch()[0].Get<uint64>();
            uint64 const columns = row->Fetch()[1].Get<uint64>();
            if (!tables && !columns)
                schema = NativeCustodySchema::Absent;
            else if (tables == 1 && columns == 3)
                schema = NativeCustodySchema::Present;
        }
        g_nativeCustodySchema = schema;
        if (schema == NativeCustodySchema::Unknown)
            LOG_ERROR("server.loading", "[CompanionQuestSync] custody schema unknown; native initialization deferred");
    }
#endif

    bool HasSingleColumnUniqueKey(char const* table, char const* column)
    {
        return static_cast<bool>(CharacterDatabase.Query(
            "SELECT index_name FROM information_schema.statistics WHERE table_schema = DATABASE() "
            "AND table_name = '{}' AND non_unique = 0 GROUP BY index_name "
            "HAVING COUNT(*) = 1 AND MIN(column_name) = '{}' LIMIT 1", table, column));
    }

    bool CustodyTablesTransactional()
    {
        return CharacterDatabase.Query(
            "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = DATABASE() AND engine = 'InnoDB' "
            "AND table_name IN ('characters', 'cq_bindings', 'cq_companion_custody') HAVING COUNT(*) = 3") &&
            HasSingleColumnUniqueKey("cq_companion_custody", "bot_guid") &&
            HasSingleColumnUniqueKey("cq_bindings", "bot_guid");
    }

    std::string ForeignCustodyClause(char const* guid)
    {
        std::string sql;
        for (char const* table : { "wowlegends_companion", "wowlegends_companion_multi" })
        {
            QueryResult found = CharacterDatabase.Query(
                "SELECT (SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name = '{0}'), "
                "(SELECT COUNT(*) FROM information_schema.columns WHERE table_schema = DATABASE() "
                "AND table_name = '{0}' AND column_name = 'bot_guid')", table);
            if (!found || (found->Fetch()[0].Get<uint64>() && !found->Fetch()[1].Get<uint64>()))
                return " AND 1 = 0 ";
            if (found->Fetch()[0].Get<uint64>())
                sql += fmt::format(" AND NOT EXISTS (SELECT 1 FROM {} foreign_b WHERE foreign_b.bot_guid = {}) ", table, guid);
        }
        return sql;
    }

    // The shared, operator-approved custody row is authoritative. Neither a binding
    // alone nor a release timestamp (including ties) proves who owns a reused guid.
    std::string CurrentCustodyClause(bool tracked, std::string const& assignment = {})
    {
        return (assignment.empty() ? " FROM " : "UPDATE ") +
            std::string("cq_bindings b JOIN cq_companion_custody x ON x.bot_guid = b.bot_guid "
                           "JOIN characters c ON c.guid = b.bot_guid AND c.account = x.body_account "
                           "JOIN characters o ON o.guid = b.owner_guid AND o.guid <> c.guid ") + assignment +
            "WHERE x.module = 'cq' AND x.state = 'owned' AND x.owner_guid = b.owner_guid "
            "AND BINARY x.body_name = BINARY c.name AND BINARY b.companion_name = BINARY c.name " +
            (tracked ? " AND x.tracking = 1 " : "") + ForeignCustodyClause("b.bot_guid");
    }

    bool HasCurrentCustody(uint32 ownerGuid, uint32 botGuid, bool tracked)
    {
        return g_custodyReady && static_cast<bool>(CharacterDatabase.Query(
            "SELECT 1" + CurrentCustodyClause(tracked) +
            fmt::format(" AND b.owner_guid = {} AND b.bot_guid = {}", ownerGuid, botGuid)));
    }

    uint32 ResolveOwnedCompanion(uint32 ownerGuid, std::string name)
    {
        if (!g_custodyReady || name.empty())
            return 0;
        CharacterDatabase.EscapeString(name);
        if (QueryResult row = CharacterDatabase.Query("SELECT b.bot_guid" + CurrentCustodyClause(false) +
                fmt::format(" AND b.owner_guid = {} AND c.name = '{}'", ownerGuid, name)))
        {
            uint32 const guid = row->Fetch()[0].Get<uint32>();
            if (!row->NextRow())
                return guid;
        }
        return 0;
    }

    bool SetCompanionTracking(uint32 ownerGuid, uint32 botGuid, bool tracking)
    {
        if (!HasCurrentCustody(ownerGuid, botGuid, false))
            return false;
        CharacterDatabase.DirectExecute(
            "UPDATE cq_companion_custody SET tracking = {} WHERE bot_guid = {} AND owner_guid = {} "
            "AND module = 'cq' AND state = 'owned'", uint32(tracking), botGuid, ownerGuid);
        if (!HasCurrentCustody(ownerGuid, botGuid, false))
            return false;
        return static_cast<bool>(CharacterDatabase.Query(
            "SELECT 1 FROM cq_companion_custody WHERE bot_guid = {} AND tracking = {}", botGuid, uint32(tracking)));
    }

    // ------------------------------------------------------------------ roles
    //
    // A role is the player's statement of intent for one companion. playerbots derives its
    // own BotRoles from the talent spec and offers no setter (AiFactory does the assigning),
    // so this is the module's record - it is what later decides which strategies a companion
    // gets and how it is allowed to engage. "auto" keeps the module's hands off entirely.
    constexpr char const* kCompanionRoles[] = { "auto", "tank", "healer", "dps", "support" };

    std::atomic<int> g_roleColumn{0};   // 0 unknown, 1 present, -1 missing

    /// `role_changed_at` arrived after `role`, so it gets its own probe: a realm that imported
    /// the first migration but not the second still records roles, it just cannot rate-limit
    /// them, and that must be a warning rather than a broken role command.
    std::atomic<int> g_roleLockColumn{ 0 };

    bool RoleLockColumnPresent()
    {
        int state = g_roleLockColumn.load();
        if (state)
            return state > 0;

        QueryResult row = CharacterDatabase.Query(
            "SELECT COUNT(*) FROM information_schema.columns WHERE table_schema = DATABASE() "
            "AND table_name = 'cq_companion_custody' AND column_name = 'role_changed_at'");
        state = (row && row->Fetch()[0].Get<uint64>() != 0) ? 1 : -1;
        g_roleLockColumn = state;

        if (state < 0)
            LOG_WARN("server.loading", "[CompanionQuestSync] cq_companion_custody has no role_changed_at "
                                       "column; role changes are not rate limited until "
                                       "sql/companion-role-lock.sql is imported");
        return state > 0;
    }

    bool RoleColumnPresent()
    {
        int state = g_roleColumn.load();
        if (state)
            return state > 0;

        QueryResult row = CharacterDatabase.Query(
            "SELECT COUNT(*) FROM information_schema.columns WHERE table_schema = DATABASE() "
            "AND table_name = 'cq_companion_custody' AND column_name = 'role'");
        state = (row && row->Fetch()[0].Get<uint64>() != 0) ? 1 : -1;
        g_roleColumn = state;

        if (state < 0)
            LOG_WARN("server.loading", "[CompanionQuestSync] cq_companion_custody has no role column; "
                                       "companion roles stay unavailable until sql/companion-roles.sql is imported");
        return state > 0;
    }

    bool RoleKnown(std::string const& role)
    {
        for (char const* known : kCompanionRoles)
            if (role == known)
                return true;
        return false;
    }

    std::string RoleOrAuto(std::string role)
    {
        std::transform(role.begin(), role.end(), role.begin(),
                       [](unsigned char c) { return char(std::tolower(c)); });
        return RoleKnown(role) ? role : std::string("auto");
    }

    /// The stored role for a body: "auto" when the row predates the column or says nothing.
    std::string CompanionRole(uint32 botGuid)
    {
        if (!RoleColumnPresent())
            return "auto";

        QueryResult row = CharacterDatabase.Query(
            "SELECT role FROM cq_companion_custody WHERE bot_guid = {} AND module = 'cq' LIMIT 1", botGuid);
        if (!row)
            return "auto";

        Field* field = row->Fetch();
        if (field[0].IsNull())
            return "auto";
        return RoleOrAuto(field[0].Get<std::string>());
    }

    /// Store a role, then read it back: a write that did not land must not look like one that did.
    /// Seconds before this companion may be given another role, 0 when it may change now.
    ///
    /// A role is not a checkbox: it drives strategies, and (once the loadout lands) a respec and
    /// a gear swap. Flipping it repeatedly is how a companion ends up respeccing in a doorway,
    /// so one change per window, and the window is reported to the addon so its Apply button can
    /// count down instead of letting the player bang on a dead control.
    uint32 RoleLockRemaining(uint32 botGuid)
    {
        if (!g_cfg.roleChangeCooldown || !RoleLockColumnPresent())
            return 0;

        QueryResult row = CharacterDatabase.Query(
            "SELECT role_changed_at FROM cq_companion_custody WHERE bot_guid = {} "
            "AND module = 'cq' AND state = 'owned' LIMIT 1", botGuid);
        if (!row)
            return 0;

        Field* field = row->Fetch();
        if (field[0].IsNull())
            return 0;

        uint32 const changedAt = field[0].Get<uint32>();
        if (!changedAt)
            return 0;

        uint32 const now = static_cast<uint32>(time(nullptr));
        if (now <= changedAt)
            return 0;

        uint32 const spent = now - changedAt;
        return spent >= g_cfg.roleChangeCooldown ? 0 : g_cfg.roleChangeCooldown - spent;
    }

    /// Why this role change is refused, or empty when it may go ahead.
    ///
    /// Out of combat is the hard rule: the companion must be standing still and not mid-swing
    /// for the change to settle, and the owner being in a fight is the same moment.
    std::string RoleChangeRefusal(Player* owner, Player* bot, uint32 botGuid)
    {
        if (bot && bot->IsInCombat())
            return "it is fighting - a role settles out of combat";

        if (owner && owner->IsInCombat())
            return "you are fighting - a role settles out of combat";

        if (uint32 const remaining = RoleLockRemaining(botGuid); remaining)
            return "it changed roles a moment ago; " + std::to_string(remaining) + "s to go";

        return std::string();
    }

    bool SetCompanionRole(uint32 ownerGuid, uint32 botGuid, std::string const& role)
    {
        if (!RoleColumnPresent() || !HasCurrentCustody(ownerGuid, botGuid, false))
            return false;

        std::string const wanted = RoleOrAuto(role);
        if (RoleLockColumnPresent())
            CharacterDatabase.DirectExecute(
                "UPDATE cq_companion_custody SET role = '{}', role_changed_at = UNIX_TIMESTAMP() "
                "WHERE bot_guid = {} AND owner_guid = {} AND module = 'cq' AND state = 'owned'",
                wanted, botGuid, ownerGuid);
        else
            CharacterDatabase.DirectExecute(
                "UPDATE cq_companion_custody SET role = '{}' WHERE bot_guid = {} AND owner_guid = {} "
                "AND module = 'cq' AND state = 'owned'", wanted, botGuid, ownerGuid);
        return CompanionRole(botGuid) == wanted;
    }

    bool CompanionQuiet(uint32 botGuid);

    struct RoleStrategyEdit
    {
        char const* name;
        BotState state;
        bool add;
    };

    /// One row of the translation table: a class-specific lever for one role.
    ///
    /// The druid form bug is what this table exists for. The flat role table asked every class
    /// the same question and got a wrong answer for druids, whose form comes from a talent aura
    /// rather than from intent. Adding a class now means adding rows, not branches.
    struct ClassRule
    {
        uint8 klass;
        char const* role;        // which role the row applies to
        char const* strategy;
        BotState state;
        bool add;
        uint32 needsSpell;       // 0 = no requirement, otherwise the bot must know this spell
    };

    ClassRule constexpr kClassRules[] = {
        // druids: bear form is chosen from the Thick Hide aura, so it has to be taken away
        // explicitly; install the replacement before checking whether removal would disarm
        { CLASS_DRUID, "tank",    "bear", BOT_STATE_COMBAT, true,  5487 },   // Bear Form
        { CLASS_DRUID, "dps",     "cat",  BOT_STATE_COMBAT, true,  768  },   // Cat Form
        { CLASS_DRUID, "support", "cat",  BOT_STATE_COMBAT, true,  768  },
        { CLASS_DRUID, "healer",  "resto", BOT_STATE_COMBAT, true, 0 },
        { CLASS_DRUID, "dps",     "bear", BOT_STATE_COMBAT, false, 0 },
        { CLASS_DRUID, "support", "bear", BOT_STATE_COMBAT, false, 0 },
        { CLASS_DRUID, "healer",  "bear", BOT_STATE_COMBAT, false, 0 },
        { CLASS_DEATH_KNIGHT, "tank",    "blood", BOT_STATE_COMBAT, true,  0 },
        { CLASS_DEATH_KNIGHT, "dps",     "blood", BOT_STATE_COMBAT, false, 0 },
        { CLASS_DEATH_KNIGHT, "support", "blood", BOT_STATE_COMBAT, false, 0 },
    };

    /// The strategies that *are* a spec, per class. Removing one of these is not a role change,
    /// it is disarming the companion: "tank" is both a role word and the name of a protection
    /// warrior's (or paladin's) whole combat strategy, so `-tank` on a protection companion left
    /// it with no combat actions at all and it stopped engaging.
    constexpr char const* kSpecStrategies[] = {
        "tank", "arms", "fury", "melee", "dps", "cat", "bear", "balance", "resto", "heal",
        "enh", "ele", "surv", "mm", "bm", "frost", "fire", "arcane", "affli", "demo", "destro", "blood", "unholy",
    };

    /// True when taking `removing` away would leave the companion with no combat spec.
    bool WouldDisarm(PlayerbotAI* ai, std::string const& removing)
    {
        if (!ai || !ai->HasStrategy(removing, BOT_STATE_COMBAT))
            return false;

        for (char const* spec : kSpecStrategies)
        {
            if (removing == spec)
                continue;
            if (ai->HasStrategy(spec, BOT_STATE_COMBAT))
                return false;
        }

        return true;
    }

    /// What a role asks of playerbots, as strategy edits: the flat part every class shares,
    /// then whatever this class needs on top, then the quiet flag.
    ///
    /// `policy` is the engagement setting: "pull" is how a companion starts a fight of its own,
    /// so on `auto` it is left alone (a companion that can only assist reads as one that does
    /// not engage) and the player-directed policies take it away.
    std::vector<RoleStrategyEdit> RoleStrategyEdits(std::string const& role, Player* bot, bool quiet,
                                                    std::string const& policy)
    {
        std::vector<RoleStrategyEdit> edits;

        bool const keepPull = (policy == "auto" || policy.empty());
        bool const classTank = bot && (bot->getClass() == CLASS_DRUID || bot->getClass() == CLASS_DEATH_KNIGHT);

        if (role == "tank")
        {
            if (!classTank)
                edits.push_back({ "tank", BOT_STATE_COMBAT, true });
        }
        else if (role == "healer")
        {
            if (!classTank)
            {
                edits.push_back({ "heal", BOT_STATE_COMBAT, true });
                edits.push_back({ "tank", BOT_STATE_COMBAT, false });
            }
            if (!keepPull)
                edits.push_back({ "pull", BOT_STATE_NON_COMBAT, false });
        }
        else if (role == "dps" || role == "support")
        {
            if (!classTank)
                edits.push_back({ "tank", BOT_STATE_COMBAT, false });
            if (!keepPull)
                edits.push_back({ "pull", BOT_STATE_NON_COMBAT, false });
        }
        else
            return edits;   // auto: the module keeps its hands off this companion entirely

        if (bot)
            for (ClassRule const& rule : kClassRules)
                if (bot->getClass() == rule.klass && role == rule.role &&
                    (!rule.needsSpell || bot->HasSpell(rule.needsSpell)))
                    edits.push_back({ rule.strategy, rule.state, rule.add });

        // "quiet" is the threat lever: dropping taunts is not the same as not generating threat
        if (quiet && (role == "dps" || role == "support"))
            edits.push_back({ "threat", BOT_STATE_COMBAT, true });

        return edits;
    }

    std::string RevertRoleStrategies(Player* bot);
    std::string CompanionEngage(uint32 botGuid);
    void ReconcileRoleSpells(Player* bot, std::string const& role);
    bool HasActiveGapCloserOverride(Player* bot);
    void SendCombatTactics(Player* owner, uint32 ownerGuid);
    bool ProcessPendingRoleSwitch(Player* owner, Player* bot, uint32 botGuid);
    // defined with the gear code below
    uint32 RefreshLevelGear(Player* owner, Player* bot, bool specChanged = false);

    /// Apply a companion's role to its AI. Returns the edits that did not take, so the card
    /// can say "no tank strategy for this class" rather than showing a control that lies.
    ///
    /// Removals cannot fail in a way worth reporting: a strategy the class does not have is
    /// already absent, which is what the role asked for.
    std::string ApplyRoleStrategies(Player* bot, std::string const& requested = {})
    {
        std::string missed;
        if (!bot || !bot->IsInWorld())
            return missed;

        uint32 const botGuid = bot->GetGUID().GetCounter();
        std::string const role = requested.empty() ? CompanionRole(botGuid) : requested;
        if (role == "auto")
        {
            // "auto" means "as playerbots had it", so hand back exactly what we took: leaving
            // -tank and +passive in place is what made a companion stop fighting when its
            // setting was put back to auto
            return RevertRoleStrategies(bot);
        }

        PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
        if (!ai)
            return missed;

        std::vector<AppliedEdit>& applied = g_roleApplied[botGuid];
        auto const record = [&](std::string const& name, BotState state, bool added)
        {
            auto const prior = std::find_if(applied.begin(), applied.end(), [&](AppliedEdit const& old)
            {
                return old.name == name && old.state == int(state);
            });
            if (prior == applied.end())
                applied.push_back({ name, int(state), added });
            else if (prior->added != added)
                applied.erase(prior);
        };

        bool const quiet = CompanionQuiet(botGuid);

        auto const edits = RoleStrategyEdits(role, bot, quiet, CompanionEngage(botGuid));
        for (RoleStrategyEdit const& edit : edits)
        {
            // never disarm: a removal that would leave no spec at all is refused and reported,
            // because the companion then stands in a fight doing nothing
            if (!edit.add && WouldDisarm(ai, edit.name))
            {
                if (!missed.empty())
                    missed += ",";
                missed += std::string("kept ") + edit.name;
                continue;
            }

            bool const present = ai->HasStrategy(edit.name, edit.state);
            if (edit.add == present)
                continue;

            auto const before = edit.add ? ai->GetStrategies(edit.state) : std::vector<std::string>{};
            ai->ChangeStrategy(std::string(edit.add ? "+" : "-") + edit.name, edit.state);
            // The engine automatically removes sibling specs when a replacement is added.
            // Record those removals too so Auto can restore the native strategy, not just drop ours.
            for (auto const& name : before)
                if (!ai->HasStrategy(name, edit.state))
                    record(name, edit.state, false);

            // verify the edit landed, in both directions: a "+tank" that leaves the strategy
            // absent means this class has no such strategy, and a "-bear" that leaves the
            // strategy present means the companion is still in that form - the second is the
            // bug a druid reports as "it is still a bear"
            if (ai->HasStrategy(edit.name, edit.state) != edit.add)
            {
                if (!missed.empty())
                    missed += ",";
                missed += edit.add ? edit.name : std::string("-") + edit.name;
            }
            else
                record(edit.name, edit.state, edit.add);
        }

        // Adding another strategy can replace an earlier one in the same strategy group.
        for (RoleStrategyEdit const& edit : edits)
            if (ai->HasStrategy(edit.name, edit.state) != edit.add && missed.find(edit.name) == std::string::npos)
                missed += " " + std::string(edit.name);
        return missed;
    }

    /// Undo the role edits this module made, and only those.
    std::string RevertRoleStrategies(Player* bot)
    {
        if (!bot)
            return "missing companion";

        auto const mine = g_roleApplied.find(bot->GetGUID().GetCounter());
        if (mine == g_roleApplied.end())
            return {};

        PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
        if (!ai)
            return "missing AI";
        std::string missed;
        for (auto it = mine->second.rbegin(); it != mine->second.rend(); ++it)
            ai->ChangeStrategy(std::string(it->added ? "-" : "+") + it->name, BotState(it->state));
        for (AppliedEdit const& edit : mine->second)
            if (ai->HasStrategy(edit.name, BotState(edit.state)) == edit.added)
                missed += " " + edit.name;
        if (missed.empty())
            g_roleApplied.erase(mine);
        return missed;
    }

    // --------------------------------------------------------------- quiet
    //
    // Threat is not taunts. Dropping "tank" stops the companion grabbing aggro deliberately,
    // but its damage still builds threat; playerbots' own "threat" strategy is the lever that
    // makes it quieter. Off by default, because it costs damage.
    bool QuietColumnPresent()
    {
        int state = g_quietColumn.load();
        if (state)
            return state > 0;

        QueryResult row = CharacterDatabase.Query(
            "SELECT COUNT(*) FROM information_schema.columns WHERE table_schema = DATABASE() "
            "AND table_name = 'cq_companion_custody' AND column_name = 'quiet'");
        state = (row && row->Fetch()[0].Get<uint64>() != 0) ? 1 : -1;
        g_quietColumn = state;
        if (state < 0)
            LOG_WARN("server.loading", "[CompanionQuestSync] cq_companion_custody has no quiet column; "
                                       "the quiet flag stays off until sql/companion-quiet.sql is imported");
        return state > 0;
    }

    bool CompanionQuiet(uint32 botGuid)
    {
        if (!QuietColumnPresent())
            return false;

        QueryResult row = CharacterDatabase.Query(
            "SELECT quiet FROM cq_companion_custody WHERE bot_guid = {} AND module = 'cq' LIMIT 1", botGuid);
        if (!row)
            return false;

        Field* field = row->Fetch();
        return !field[0].IsNull() && field[0].Get<uint8>() != 0;
    }

    bool SetCompanionQuiet(uint32 ownerGuid, uint32 botGuid, bool quiet)
    {
        if (!QuietColumnPresent() || !HasCurrentCustody(ownerGuid, botGuid, false))
            return false;

        CharacterDatabase.DirectExecute(
            "UPDATE cq_companion_custody SET quiet = {} WHERE bot_guid = {} AND owner_guid = {} "
            "AND module = 'cq' AND state = 'owned'", quiet ? 1 : 0, botGuid, ownerGuid);
        return CompanionQuiet(botGuid) == quiet;
    }

    // --------------------------------------------------------------- pets
    //
    // A hunter pet or a voidwalker taunts with an ability, not a strategy, so the tank/dps split
    // cannot reach it. The module turns those autocasts off for anything that is not tanking,
    // and only ever turns back on what it turned off.
    bool IsTauntAbility(uint32 spellId)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (!info || !info->SpellName[0])
            return false;

        std::string const name = info->SpellName[0];
        return name == "Growl" || name == "Torment" || name == "Suffering" || name == "Anguish";
    }

    void ReconcilePetTaunt(Player* bot)
    {
        if (!bot)
            return;

        uint32 const botGuid = bot->GetGUID().GetCounter();
        std::string const role = CompanionRole(botGuid);
        std::vector<uint32>& off = g_petTauntOff[botGuid];

        Pet* pet = bot->GetPet();
        if (!pet)
        {
            off.clear();
            return;
        }

        // tanking keeps its taunt; auto means the module never touched it
        if (role == "tank" || role == "auto")
        {
            for (uint32 const spellId : off)
                if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId))
                    pet->ToggleAutocast(info, true);
            off.clear();
            return;
        }

        for (uint8 i = 0; i < pet->GetPetAutoSpellSize(); ++i)
        {
            uint32 const spellId = pet->GetPetAutoSpellOnPos(i);
            if (!IsTauntAbility(spellId) || std::find(off.begin(), off.end(), spellId) != off.end())
                continue;

            if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId))
            {
                pet->ToggleAutocast(info, false);
                off.push_back(spellId);
            }
        }
    }

    // --------------------------------------------------------------- gap closers
    //
    // A warrior charges whatever it can see, in every spec and every role, because the charge
    // is not a strategy: each warrior strategy hangs `enemy out of melee -> charge` on itself,
    // and the action is a root node nobody can add or remove by name. That is why "-tank" left
    // a dps warrior leaping in.
    //
    // The lever below a trigger is what playerbots will cast, and `PlayerbotAI::CanCastSpell`
    // bails on two things worth knowing here: a spell the bot does not know, and a spell on
    // cooldown. A cooldown is the honest one - it leaves the spellbook alone, it is reversible,
    // and it cannot strand the warrior (see ReconcileRoleSpells).
    struct GapCloser
    {
        uint8 klass;
        uint32 spell;
        char const* name;
    };

    GapCloser constexpr kGapClosers[] = {
        { CLASS_WARRIOR, 100,   "Charge" },      // rank 1 unlearns the chain
        { CLASS_WARRIOR, 6178,  "Charge" },      // rank 2
        { CLASS_WARRIOR, 11578, "Charge" },      // rank 3
    };

    /// How long a gap closer is muted for. The reconcile pass re-applies it long before this
    /// runs out, so the number only has to outlast a pass and a fight.
    uint32 constexpr kGapCloserMuteMs = 10 * 60 * 1000;

    /// A gap closer is *muted with a cooldown*, not taken out of the spellbook.
    ///
    /// Removing the spell does not remove the action. The charge trigger stays registered, its
    /// action still counts as possible, and every "enemy out of melee" tick is spent on a charge
    /// that cannot land - so the warrior never walks into the fight, because playerbots' own
    /// "reach melee" sits at a lower priority and never gets its turn. That is what "the warrior
    /// stopped engaging when I set it to dps" looked like. A cooldown is one of the things
    /// PlayerbotAI::CanCastSpell checks, so the charge goes impossible, the walk takes over, and
    /// the spellbook is never touched (nothing to repair after a restart, and nothing to learn
    /// back).
    void ReconcileRoleSpells(Player* bot, std::string const& role)
    {
        if (!bot || bot->getClass() != CLASS_WARRIOR)
            return;

        uint32 const botGuid = bot->GetGUID().GetCounter();
        std::vector<uint32>& withheld = g_spellsWithheld[botGuid];

        // tank and auto keep everything: auto means the module is not steering this companion
        bool const shouldMute = (role != "tank" && role != "auto");
        bool const applyDefault = g_cfg.suppressGapClosers && !HasActiveGapCloserOverride(bot);
        uint32 const now = getMSTime();

        for (GapCloser const& closer : kGapClosers)
        {
            if (bot->getClass() != closer.klass || !bot->HasSpell(closer.spell))
                continue;

            bool const held = std::find(withheld.begin(), withheld.end(), closer.spell) != withheld.end();

            if (shouldMute && applyDefault)
            {
                // re-applied on every pass on purpose: it is cheap, and it survives a cooldown
                // clear (a death, a respec, a client-side reset) without an event hook
                if (bot->GetSpellCooldownDelay(closer.spell) < kGapCloserMuteMs / 2)
                    bot->AddSpellCooldown(closer.spell, 0, now + kGapCloserMuteMs);

                if (!held)
                {
                    withheld.push_back(closer.spell);
                    LOG_INFO("server.loading", "[CompanionQuestSync] {} muted {} (role {})",
                             bot->GetName(), closer.name, role);
                }
            }
            else if (held)
            {
                bot->RemoveSpellCooldown(closer.spell, true);
                withheld.erase(std::remove(withheld.begin(), withheld.end(), closer.spell), withheld.end());
                LOG_INFO("server.loading", "[CompanionQuestSync] {} can use {} again (role {})",
                         bot->GetName(), closer.name, role);
            }
        }
    }

    /// The three trees of each class, in tab order. Kept as words because every line this
    /// produces is read by a person in `inspect` or in Server.log, and "tab 2" tells them nothing.
    constexpr char const* kSpecTreeNames[CLASS_DRUID + 1][3] =
    {
        { "?",               "?",             "?" },                 // 0 unused
        { "arms",            "fury",          "protection" },        // warrior
        { "holy",            "protection",    "retribution" },       // paladin
        { "beast mastery",   "marksmanship",  "survival" },          // hunter
        { "assassination",   "combat",        "subtlety" },          // rogue
        { "discipline",      "holy",          "shadow" },            // priest
        { "blood",           "frost",         "unholy" },            // death knight
        { "elemental",       "enhancement",   "restoration" },       // shaman
        { "arcane",          "fire",          "frost" },             // mage
        { "affliction",      "demonology",    "destruction" },       // warlock
        { "?",               "?",             "?" },                 // 10 unused
        { "balance",         "feral",         "restoration" },       // druid
    };

    /// Which tree a role asks of a class, or -1 when the class cannot hold that role at all.
    ///
    /// `support` rides the damage tree on purpose: support in this module means "fights, never
    /// off-heals", so a support tree would be a healer tree the module then refuses to let heal.
    /// A bear tank and a cat both live in feral - the tree survives, the form does not - so a
    /// druid honours `tank` and `dps` the same way at the talent level.
    int RoleSpecTab(uint8 cls, std::string const& role)
    {
        if (role == "auto")
            return -1;

        bool const tank = role == "tank";
        bool const heal = role == "healer";

        switch (cls)
        {
            case CLASS_WARRIOR:      return tank ? 2 : (heal ? -1 : 1);
            case CLASS_PALADIN:      return tank ? 1 : (heal ? 0 : 2);
            case CLASS_HUNTER:       return (tank || heal) ? -1 : 1;
            case CLASS_ROGUE:        return (tank || heal) ? -1 : 1;
            case CLASS_PRIEST:       return heal ? 1 : (tank ? -1 : 2);
            case CLASS_DEATH_KNIGHT: return tank ? 0 : (heal ? -1 : 1);
            case CLASS_SHAMAN:       return heal ? 2 : 0;
            case CLASS_MAGE:         return (tank || heal) ? -1 : 1;
            case CLASS_WARLOCK:      return (tank || heal) ? -1 : 0;
            case CLASS_DRUID:        return heal ? 2 : 1;
            default:                 return -1;
        }
    }

    /// Points a talent tree holds for one spec slot.
    ///
    /// `specMask` on each talent is what tells the two specs apart; the rank is not stored, it is
    /// the highest RankID spell the companion actually knows, which is also what the talent pane
    /// shows. Summing it per tab is how a slot is identified without asking the client.
    /// A stored talent, however this build spells it.
    ///
    /// `character_talent` carries an id per talent, and different cores store either the talent's own
    /// DBC id or the spell id of the rank it sits at. Looking up only the first convention made every
    /// counter in this module read zero - "still untalented" for talents that were plainly there and
    /// `none` on the card - so both are resolved, rank spell first because that is what this database
    /// holds.
    std::unordered_map<uint32, TalentEntry const*> g_talentByRankSpell;

    TalentEntry const* ResolveTalentEntry(uint32 id)
    {
        if (g_talentByRankSpell.empty())
        {
            for (uint32 i = 1; i < sTalentStore.GetNumRows(); ++i)
                if (TalentEntry const* info = sTalentStore.LookupEntry(i))
                    for (uint8 r = 0; r < MAX_TALENT_RANK; ++r)
                        if (info->RankID[r])
                            g_talentByRankSpell.emplace(info->RankID[r], info);
        }

        auto const byRank = g_talentByRankSpell.find(id);
        if (byRank != g_talentByRankSpell.end())
            return byRank->second;
        return sTalentStore.LookupEntry(id);
    }

    uint32 SpecPoints(Player* bot, uint8 spec, uint8 tab)
    {
        uint32 points = 0;
        for (auto const& entry : bot->GetTalentMap())
        {
            PlayerTalent* talent = entry.second;
            if (!talent || talent->State == PLAYERSPELL_REMOVED || !talent->IsInSpec(spec))
                continue;

            TalentEntry const* info = ResolveTalentEntry(entry.first);
            if (!info)
                continue;
            TalentTabEntry const* tabInfo = sTalentTabStore.LookupEntry(info->TalentTab);
            if (!tabInfo || tabInfo->tabpage != tab)
                continue;

            // The repack's HasTalent ignores the spec argument. Use the keyed rank and mask.
            for (uint8 rank = 0; rank < MAX_TALENT_RANK; ++rank)
                if (info->RankID[rank] == entry.first)
                    points += rank + 1;
        }
        return points;
    }

    /// The tree a spec slot is actually in: the one holding the most points, or -1 with none.
    int SpecTabOf(Player* bot, uint8 spec)
    {
        int best = -1;
        uint32 bestPoints = 0;
        for (uint8 tab = 0; tab < 3; ++tab)
        {
            if (uint32 const points = SpecPoints(bot, spec, tab); points > bestPoints)
            {
                bestPoints = points;
                best = tab;
            }
        }
        return bestPoints ? best : -1;
    }

    int CurrentSpecTab(Player* bot)
    {
        return SpecTabOf(bot, bot->GetActiveSpec());
    }

    /// Spend the companion's free points into a tree, without playerbots' build templates.
    ///
    /// `InitTalentsTree` fills the trees from `parsedSpecLinkOrder[class][spec][level]`, and it
    /// returns having spent nothing when that table has no entry for the level - it wipes the tree,
    /// learns nothing, and reports success. At ten points the tree is worth filling anyway, so when
    /// the template leaves the wanted tree empty the points go in here: lowest row first, one rank at
    /// a time, and the core vetting every pick through `LearnTalent` (prerequisites, tier gates,
    /// rank caps). A pick the core refuses leaves the point count unchanged, which is how the loop
    /// knows to stop rather than spin.
    uint32 SpendPointsIntoTree(Player* bot, uint8 tab)
    {
        if (!bot || tab > 2 || bot->GetLevel() < CompanionTalentsFrom)
            return 0;

        uint32 const classMask = bot->getClassMask();
        std::vector<std::pair<uint32, uint32>> candidates;      // row, then talent id: stable order
        for (uint32 i = 1; i < sTalentStore.GetNumRows(); ++i)
        {
            TalentEntry const* info = sTalentStore.LookupEntry(i);
            if (!info)
                continue;

            TalentTabEntry const* tabInfo = sTalentTabStore.LookupEntry(info->TalentTab);
            if (!tabInfo || tabInfo->tabpage != tab || !(classMask & tabInfo->ClassMask))
                continue;

            candidates.emplace_back(info->Row, i);
        }
        std::sort(candidates.begin(), candidates.end());

        uint32 spent = 0;
        bool progress = true;
        while (progress && bot->GetFreeTalentPoints())
        {
            progress = false;
            for (auto const& candidate : candidates)
            {
                if (!bot->GetFreeTalentPoints())
                    break;

                TalentEntry const* info = sTalentStore.LookupEntry(candidate.second);
                if (!info)
                    continue;

                uint32 rank = 0;
                for (uint8 r = 0; r < MAX_TALENT_RANK; ++r)
                    if (info->RankID[r] && bot->HasTalent(info->RankID[r], bot->GetActiveSpec()))
                        rank = r + 1;
                if (rank == MAX_TALENT_RANK)
                    continue;

                uint32 const before = bot->GetFreeTalentPoints();
                bot->LearnTalent(candidate.second, rank);
                if (bot->GetFreeTalentPoints() < before)
                {
                    ++spent;
                    progress = true;
                }
            }
        }
        return spent;
    }

    /// Make sure the body has the talent points its level owes it.
    ///
    /// Companions arrive at a level rather than levelled into it, so the level-up hook that grants
    /// talent points never fired: the point counter stayed at zero, every tree read empty with
    /// nothing to spend, and the retrain had literally nothing to work with - which is exactly what
    /// `talents: 0 point(s) in fury, 0 unspent` reported on four level-15 companions. The core's own
    /// `InitTalentForLevel` is the authority on how many points a level affords and what is already
    /// spent, so it is asked rather than reimplemented: it tops the free points up to `affords minus
    /// spent` and leaves a body that has spent more than it affords alone rather than guessing.
    bool GrantOwedTalentPoints(Player* bot)
    {
        if (!bot)
            return false;

        uint32 const botGuid = bot->GetGUID().GetCounter();
        uint32 const level = bot->GetLevel();
        uint32 const before = bot->GetFreeTalentPoints();
        uint32 const allowed = bot->CalculateTalentsPoints();
        if (level < CompanionTalentsFrom || before != 0)
        {
            if (g_talentGrantNotes.insert(botGuid).second)
                LOG_INFO("server.loading", "[CompanionQuestSync] {} talent grant check: level {}, core allows {}, "
                                           "free before {}, skipped ({})",
                         bot->GetName(), level, allowed, before,
                         level < CompanionTalentsFrom ? "below talent level" : "points already available");
            return false;
        }

        bot->InitTalentForLevel();
        uint32 const after = bot->GetFreeTalentPoints();
        if (g_talentGrantNotes.insert(botGuid).second)
            LOG_INFO("server.loading", "[CompanionQuestSync] {} talent grant check: level {}, core allows {}, "
                                       "free before {}, free after {}",
                     bot->GetName(), level, allowed, before, after);
        return after != 0;
    }

    // --------------------------------------------------------------- engagement
    //
    // The role decides what a companion may do *in* a fight; the engagement policy decides
    // whether it gets into one at all:
    //
    //   auto    the module keeps out of it (playerbots' own behaviour)
    //   mine    it fights only what you are fighting - you pick the target, it follows
    //   joined  it joins fights you are already in, and never starts one
    //   never   it stays out of combat entirely; nothing here stops it defending itself
    //
    // The lever is playerbots' passive strategy, which keeps a bot following without
    // engaging. This core's headers are mangled in places, so the name is *probed* rather
    // than assumed: if the build has no passive strategy, the card says engagement control
    // is unavailable instead of drawing a control that does nothing.
    constexpr char const* kEngagePolicies[] = { "auto", "mine", "joined", "never" };

    bool EngagePolicyKnown(std::string const& policy)
    {
        for (char const* known : kEngagePolicies)
            if (policy == known)
                return true;
        return false;
    }

    std::string EngageOrAuto(std::string policy)
    {
        std::transform(policy.begin(), policy.end(), policy.begin(),
                       [](unsigned char c) { return char(std::tolower(c)); });
        return EngagePolicyKnown(policy) ? policy : std::string("auto");
    }

    bool EngageColumnPresent()
    {
        int state = g_engageColumn.load();
        if (state)
            return state > 0;

        QueryResult row = CharacterDatabase.Query(
            "SELECT COUNT(*) FROM information_schema.columns WHERE table_schema = DATABASE() "
            "AND table_name = 'cq_companion_custody' AND column_name = 'engage'");
        state = (row && row->Fetch()[0].Get<uint64>() != 0) ? 1 : -1;
        g_engageColumn = state;
        if (state < 0)
            LOG_WARN("server.loading", "[CompanionQuestSync] cq_companion_custody has no engage column; "
                                       "engagement policies stay unavailable until sql/companion-engage.sql is imported");
        return state > 0;
    }

    std::string CompanionEngage(uint32 botGuid)
    {
        if (!EngageColumnPresent())
            return "auto";

        QueryResult row = CharacterDatabase.Query(
            "SELECT engage FROM cq_companion_custody WHERE bot_guid = {} AND module = 'cq' LIMIT 1", botGuid);
        if (!row)
            return "auto";

        Field* field = row->Fetch();
        return field[0].IsNull() ? std::string("auto") : EngageOrAuto(field[0].Get<std::string>());
    }

    bool SetCompanionEngage(uint32 ownerGuid, uint32 botGuid, std::string const& policy)
    {
        if (!EngageColumnPresent() || !HasCurrentCustody(ownerGuid, botGuid, false))
            return false;

        std::string const wanted = EngageOrAuto(policy);
        CharacterDatabase.DirectExecute(
            "UPDATE cq_companion_custody SET engage = '{}' WHERE bot_guid = {} AND owner_guid = {} "
            "AND module = 'cq' AND state = 'owned'", wanted, botGuid, ownerGuid);
        return CompanionEngage(botGuid) == wanted;
    }

    /// Is playerbots' passive strategy usable here? Probed once, with the answer remembered:
    /// a build without it cannot be given engagement control, and pretending otherwise would
    /// be the kind of silent no-op this module keeps out.
    bool PassiveStrategyUsable(Player* bot)
    {
        int state = g_passiveStrategy.load();
        if (!state)
        {
            PlayerbotAI* ai = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
            bool usable = false;
            if (ai)
            {
                ai->ChangeStrategy("+passive", BOT_STATE_NON_COMBAT);
                usable = ai->HasStrategy("passive", BOT_STATE_NON_COMBAT);
                if (usable)
                    ai->ChangeStrategy("-passive", BOT_STATE_NON_COMBAT);
            }
            state = usable ? 1 : -1;
            g_passiveStrategy = state;
            if (state < 0)
                LOG_WARN("server.loading", "[CompanionQuestSync] this playerbots build has no passive strategy; "
                                           "companion engagement policies are unavailable");
        }
        return state > 0;
    }

    /// Take the bot out of the passive stance if, and only if, we are the ones who put it there.
    void ReleasePassive(Player* bot)
    {
        if (!bot)
            return;

        uint32 const botGuid = bot->GetGUID().GetCounter();
        if (!g_passiveHeld.erase(botGuid))
            return;                     // not ours to undo

        if (PlayerbotAI* ai = GET_PLAYERBOT_AI(bot))
        {
            ai->ChangeStrategy("-passive", BOT_STATE_COMBAT);
            ai->ChangeStrategy("-passive", BOT_STATE_NON_COMBAT);
        }
    }

    /// Hold a companion to its engagement policy. Runs on the fast tick; every branch is a
    /// no-op for "auto", which is what keeps the setting honest.
    std::string EffectiveFormation(Player* owner, Player* bot);
    float FormationLeash(std::string const& formation);

    void ApplyEngagement(Player* owner, Player* bot)
    {
        if (!owner || !bot || !EngageColumnPresent())
            return;

        uint32 const botGuid = bot->GetGUID().GetCounter();
        std::string const policy = CompanionEngage(botGuid);
        if (policy == "auto")
        {
            // putting the setting back to auto must not leave the bot in the stance we put it
            // in: a stuck passive is a companion that ignores every order to fight
            ReleasePassive(bot);
            g_pinnedTarget.erase(botGuid);
            return;
        }

        if (!PassiveStrategyUsable(bot))
        {
            g_engageNotes[bot->GetGUID().GetCounter()] = "no passive strategy in this build";
            return;
        }

        g_engageNotes.erase(bot->GetGUID().GetCounter());

        if (!bot->IsAlive() || !owner->IsAlive() || bot->GetMapId() != owner->GetMapId())
            return;

        PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
        if (!ai)
            return;

        Unit* ownerTarget = owner->GetSelectedUnit();
        bool const ownerFighting = owner->IsInCombat();
        bool const ownerHitsSomething = ownerTarget && ownerTarget->IsAlive() &&
                                        owner->IsValidAttackTarget(ownerTarget);

        // Passive is an idle stance, not a combat order: PassiveMultiplier returns a value of
        // 0 for every action except co/nc/reset/mount/lfg and the follow/stay/chat-shortcut
        // families, so a passive bot acknowledges "attack my target" and then does nothing.
        // That is why "mine" used to leave the companion standing there: it was held passive
        // while being told to attack.
        //
        // So passive means "you are not fighting, so neither am I". The moment the owner is in
        // combat it comes off, and "mine" re-points the bot at the owner's target every tick.
        bool const wantPassive = policy == "never" || (!ownerFighting && policy != "auto");

        bool const passive = ai->HasStrategy("passive", BOT_STATE_COMBAT);
        if (wantPassive != passive)
        {
            ai->ChangeStrategy(wantPassive ? "+passive" : "-passive", BOT_STATE_COMBAT);
            ai->ChangeStrategy(wantPassive ? "+passive" : "-passive", BOT_STATE_NON_COMBAT);
        }

        if (wantPassive)
            g_passiveHeld.insert(botGuid);
        else
            g_passiveHeld.erase(botGuid);

        // "mine" is the only policy where the module names the target. It does so on *change*,
        // not on every tick: "attack my target" persists its intent in the AI's prioritized
        // targets, so repeating it is noise that fights the AI's own targeting.
        if (policy == "mine")
        {
            // the leash: never order a companion in from further away than its formation works
            if (owner->GetDistance(bot) > FormationLeash(EffectiveFormation(owner, bot)) + 5.0f)
                return;

            uint32 const wantTarget = (ownerFighting && ownerHitsSomething)
                                          ? ownerTarget->GetGUID().GetCounter()
                                          : 0;
            uint32& pinned = g_pinnedTarget[botGuid];
            if (wantTarget != pinned)
            {
                pinned = wantTarget;
                if (wantTarget)
                    ai->DoSpecificAction("attack my target");
            }
        }
        else if (g_pinnedTarget.erase(botGuid))
        {
            // the policy changed away from mine: stop pinning anything
        }
    }

    bool FindCompanions(uint32 ownerGuid, std::vector<uint32>& bots)
    {
        bots.clear();
        if (!g_custodyReady)
            return false;
        uint32 const now = static_cast<uint32>(time(nullptr));
        auto itr = g_bindingCache.find(ownerGuid);
        if (itr != g_bindingCache.end() && itr->second.expires > now)
        {
            bots = itr->second.bots;
            return !bots.empty();
        }

        BindingSet set;
        set.expires = now + kBindingCacheSeconds;
        if (QueryResult rows = CharacterDatabase.Query("SELECT b.bot_guid" + CurrentCustodyClause(true) +
                fmt::format(" AND b.owner_guid = {} ORDER BY b.bot_guid LIMIT {}", ownerGuid, kMaxCompanions)))
        {
            do
                set.bots.push_back(rows->Fetch()[0].Get<uint32>());
            while (rows->NextRow());
        }
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
        Player* body = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(botGuid));
        return body && body->GetSession() && body->GetSession()->IsBot() ? body : nullptr;
    }

    /// `Roles Vorrin=tank|Branros=auto` - the addon's view of what each companion is for.
    std::string RoleLine(uint32 ownerGuid)
    {
        std::vector<uint32> bots;
        if (!RoleColumnPresent() || !FindCompanions(ownerGuid, bots))
            return std::string();

        std::string line;
        for (uint32 const botGuid : bots)
        {
            Player* bot = FindCompanionPlayer(botGuid);
            std::string name = bot ? bot->GetName() : std::string();
            if (name.empty())
            {
                QueryResult row = CharacterDatabase.Query("SELECT name FROM characters WHERE guid = {}", botGuid);
                if (!row)
                    continue;
                name = row->Fetch()[0].Get<std::string>();
            }
            if (name.find('|') != std::string::npos || name.find('=') != std::string::npos)
                continue;                        // a separator in a name would split the line
            if (!line.empty())
                line += "|";
            line += name + "=" + CompanionRole(botGuid);
            if (std::string const policy = CompanionEngage(botGuid); policy != "auto")
                line += "/" + policy;
            if (CompanionQuiet(botGuid))
                line += ":q";
            auto const note = g_roleNotes.find(botGuid);
            if (note != g_roleNotes.end() && !note->second.empty())
                line += ":na";       // this class cannot honour part of the role
            if (uint32 const remaining = RoleLockRemaining(botGuid); remaining)
                line += ":l" + std::to_string(remaining);   // the addon counts this down
        }
        return line;
    }

    /// What each companion's talents are, and what its role is asking of them.
    ///
    /// `name=fury>protection@40` and then a suffix for why it has not happened yet: `!low` below
    /// the level gate, `!off` when the server is only logging. A companion whose role asks nothing
    /// of its class is sent as just `name=fury`, so the addon can say "talents left alone" rather
    /// than inventing an intent.
    std::string SpecLine(uint32 ownerGuid)
    {
        std::vector<uint32> bots;
        if (!FindCompanions(ownerGuid, bots))
            return std::string();

        std::string line;
        for (uint32 const botGuid : bots)
        {
            Player* bot = FindCompanionPlayer(botGuid);
            if (!bot || !bot->IsInWorld())
                continue;                    // no live body, no talents worth reporting

            std::string const name = bot->GetName();
            if (name.find('|') != std::string::npos || name.find('=') != std::string::npos)
                continue;

            uint8 const cls = bot->getClass();
            if (cls < 1 || cls >= std::size(kSpecTreeNames) || cls == 10)
                continue;

            int const have = SpecTabOf(bot, bot->GetActiveSpec());
            int const want = RoleSpecTab(cls, CompanionRole(botGuid));

            if (!line.empty())
                line += "|";
            line += name + "=" + (have >= 0 ? kSpecTreeNames[cls][have] : "none");
            if (want < 0)
                continue;                    // the role does not ask this class for a tree

            line += ">" + std::string(kSpecTreeNames[cls][want]);
            line += "@" + std::to_string(g_cfg.specMinLevel);
            if (g_cfg.specFollowRole < 2)
                line += "!off";
            else if (bot->GetLevel() < g_cfg.specMinLevel)
                line += "!low";
        }
        return line;
    }

    /// Roles and talents travel together: the role is what the player asked for, the talents are
    /// what actually happened, and a card that shows one without the other is how a companion
    /// keeps an old tree for a week without anyone noticing.
    void SendRoleAndSpecLines(Player* target, uint32 ownerGuid)
    {
        if (!target)
            return;

        if (std::string const roles = RoleLine(ownerGuid); !roles.empty())
            SendProtocolLine(target, "Roles " + roles);
        if (std::string const specs = SpecLine(ownerGuid); !specs.empty())
            SendProtocolLine(target, "Specs " + specs);
        SendCombatTactics(target, ownerGuid);
    }

    /// Re-assert every owned companion's role, throttled.
    ///
    /// A one-shot apply silently lapses: playerbots resets strategies when the spec or level
    /// changes, and the companion then quietly goes back to taunting. Re-asserting on a slow
    /// cadence is what makes the role a standing instruction instead of a gesture.
    void ReconcileRoles(Player* owner)
    {
        if (!owner || !owner->IsInWorld() || !RoleColumnPresent())
            return;

        std::vector<uint32> bots;
        if (!FindCompanions(owner->GetGUID().GetCounter(), bots))
            return;

        uint32 const now = static_cast<uint32>(time(nullptr));
        for (uint32 const botGuid : bots)
        {
            Player* bot = FindCompanionPlayer(botGuid);
            if (ProcessPendingRoleSwitch(owner, bot, botGuid))
                continue;
            if (!bot || !bot->IsInWorld() || !IsControlledCompanion(bot, owner))
                continue;

            uint32& at = g_roleApplyAt[botGuid];
            if (at && now - at < kRoleReassertSeconds)
                continue;

            at = now;
            ReconcilePetTaunt(bot);
            ReconcileRoleSpells(bot, CompanionRole(bot->GetGUID().GetCounter()));
            if (!owner->IsInCombat() && !bot->IsInCombat() && !owner->GetTradeData() && !bot->GetTradeData())
                GrantOwedTalentPoints(bot);
            std::string const missed = ApplyRoleStrategies(bot);
            if (missed.empty())
                g_roleNotes.erase(botGuid);
            else
                g_roleNotes[botGuid] = missed;
        }
    }

    /// Hold every owned companion to its engagement policy. Cheap: one map lookup, and
    /// nothing at all for "auto".
    void ReconcileEngagement(Player* owner)
    {
        if (!owner || !owner->IsInWorld() || !EngageColumnPresent())
            return;

        std::vector<uint32> bots;
        if (!FindCompanions(owner->GetGUID().GetCounter(), bots))
            return;

        for (uint32 const botGuid : bots)
            if (Player* bot = FindCompanionPlayer(botGuid))
                ApplyEngagement(owner, bot);
    }

    static Player* FindDeliveryOwner(uint32 ownerGuid)
    {
        if (!ownerGuid)
            return nullptr;
        Player* owner = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(ownerGuid));
        return owner && owner->GetSession() && !owner->GetSession()->IsBot() ? owner : nullptr;
    }

    static bool CanTradeSurplusNow(Player* bot, Player* owner)
    {
        return bot->IsInWorld() && owner->IsInWorld() && bot->IsAlive() && owner->IsAlive() &&
               bot->GetSession() && owner->GetSession() && !bot->IsInCombat() && !owner->IsInCombat() &&
               bot->IsWithinDistInMap(owner, TRADE_DISTANCE, false);
    }

    static bool MatchesTradeOffer(Player* bot, TradeData const* trade, PendingTrade const& pending)
    {
        if (pending.offeredItems.empty() || trade->GetMoney() || trade->GetSpell())
            return false;

        TradeData const* receiver = trade->GetTrader()->GetTradeData();
        if (!receiver || receiver->GetMoney() || receiver->GetSpell())
            return false;
        for (uint8 slot = 0; slot < TRADE_SLOT_COUNT; ++slot)
            if (receiver->GetItem(TradeSlots(slot)))
                return false;

        for (auto const& [slot, guid, count] : pending.offeredItems)
        {
            Item const* item = trade->GetItem(slot);
            if (!item || item->GetGUID() != guid || item->GetCount() != count)
                return false;

        }
        for (uint8 slot = uint8(pending.offeredItems.size()); slot < TRADE_SLOT_COUNT; ++slot)
            if (trade->GetItem(TradeSlots(slot)))
                return false;
        return DeliveryStillValid(bot, trade->GetTrader(), pending);
    }

    static void CancelSurplusTrade(Player* bot, Player* owner, char const* reason)
    {
        WorldPacket packet;
        if (WorldSession* session = bot->GetSession())
            session->HandleCancelTradeOpcode(packet);

        if (reason)
            SayToOwner(bot, owner, reason, 30);
    }

    static void SendDeliveryProgress(Player* owner, uint32 nonce, char const* state, char const* text)
    {
        if (nonce)
            SendProtocolLine(owner, fmt::format("Delivery progress {} {}:{}", nonce, state, text));
    }

    static void ApplyDeliveryReceipts()
    {
        for (auto it = g_deliveryReceipts.begin(); it != g_deliveryReceipts.end();)
        {
            DeliveryReceipt const& receipt = it->second;
            bool const ready = receipt.saved.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
            if (!ready && std::chrono::steady_clock::now() < receipt.deadline)
            {
                ++it;
                continue;
            }
            bool committed = false;
            if (ready)
                try { committed = receipt.saved.get(); }
                catch (std::future_error const&) {} // shutdown/broken promise is not success
            bool const completed = committed && receipt.transferred;
            SendDeliveryProgress(FindDeliveryOwner(receipt.ownerGuid), receipt.nonce,
                completed ? "completed" : committed ? "failed" : "unconfirmed", completed ? "Delivery completed." :
                    committed ? "The trade did not move all the offered items. Check your bags before retrying." :
                    "The trade closed but its save could not be confirmed. Check your bags before retrying.");
            it = g_deliveryReceipts.erase(it);
        }
    }

    static void CancelPendingDelivery(uint32 botGuid, PendingTrade const& pending, char const* reason)
    {
        Player* owner = FindDeliveryOwner(pending.ownerGuid);
#ifdef AC_TRADE_OFFER_GUARD
        if (Player* bot = FindCompanionPlayer(botGuid))
            if (TradeData const* trade = bot->GetTradeData(); trade && trade->GetOfferToken() == pending.noticeId &&
                trade->GetTrader() && trade->GetTrader()->GetGUID().GetCounter() == pending.ownerGuid &&
                trade->GetTrader()->GetTrader() == bot && trade->GetTrader()->GetSession() && bot->GetSession())
                CancelSurplusTrade(bot, trade->GetTrader(), nullptr);
#endif
        SendDeliveryProgress(owner, pending.nonce, "cancelled", reason);
    }

    static void CancelBoundDelivery(uint32 botGuid, Player* owner)
    {
        auto const it = g_pendingTrade.find(botGuid);
        if (!owner || it == g_pendingTrade.end() || it->second.ownerGuid != owner->GetGUID().GetCounter())
            return;
        CancelPendingDelivery(botGuid, it->second, "The companion is no longer available for this delivery.");
        g_pendingTrade.erase(it);
    }

    static void CancelPendingDeliveries()
    {
        for (auto it = g_pendingTrade.begin(); it != g_pendingTrade.end();)
        {
            if (IsDeliveryEnabled(it->second))
            {
                ++it;
                continue;
            }
            CancelPendingDelivery(it->first, it->second, "Companion delivery was disabled.");
            it = g_pendingTrade.erase(it);
        }
    }

    /// Fill after the bot has had time to answer the begin-trade round trip.
    void ApplyPendingTrades()
    {
        for (auto itr = g_pendingTrade.begin(); itr != g_pendingTrade.end();)
        {
            PendingTrade& pending = itr->second;
            Player* bot = FindCompanionPlayer(itr->first);
            Player* owner = FindDeliveryOwner(pending.ownerGuid);

            TradeData* trade = bot ? bot->GetTradeData() : nullptr;
            TradeData* other = owner ? owner->GetTradeData() : nullptr;
            if (!trade || !other || trade->GetTrader() != owner || other->GetTrader() != bot
#ifdef AC_TRADE_OFFER_GUARD
                || trade->GetOfferToken() != pending.noticeId
#endif
                )
            {
                SendDeliveryProgress(owner, pending.nonce, "cancelled", "The trade window closed without a delivery.");
                itr = g_pendingTrade.erase(itr);
                continue;
            }

            uint32 const elapsed = uint32(time(nullptr)) - pending.at;
            if (!IsDeliveryEnabled(pending) || !CanTradeSurplusNow(bot, owner) || !HasSupplyAuthority(bot, owner, false) ||
                (pending.automatic && (!IsTownDeliveryStop(owner, bot) || elapsed >= Timing::TownOfferTimeout)) ||
                (pending.nonce && elapsed >= Timing::NamedOfferTimeout))
            {
                CancelSurplusTrade(bot, owner, nullptr);
                SendDeliveryProgress(owner, pending.nonce, "cancelled", "The delivery timed out or trading is no longer possible.");
                itr = g_pendingTrade.erase(itr);
                continue;
            }

            if (pending.applied)
            {
                // A missing or replaced stack invalidates what the player was offered.
                if (!trade->IsAccepted() || !MatchesTradeOffer(bot, trade, pending))
                {
                    CancelSurplusTrade(bot, owner, pending.announceFailure
                        ? "My offer changed. Ask me again when you're ready." : nullptr);
                    SendDeliveryProgress(owner, pending.nonce, "cancelled", "The offer changed. Request it again when you are ready.");
                    itr = g_pendingTrade.erase(itr);
                }
                else
                    ++itr;                      // the guard is live; the trade ends, the record goes
                continue;
            }

            if (uint32(time(nullptr)) - pending.at < 2)
            {
                ++itr;
                continue;
            }

#ifdef AC_TRADE_OFFER_COMMIT
            if (!trade->IsOfferWindowOpen())
            {
                if (elapsed < 10)
                {
                    ++itr;
                    continue;
                }
                CancelSurplusTrade(bot, owner, nullptr);
                SendDeliveryProgress(owner, pending.nonce, "failed", "The trade window did not open.");
                itr = g_pendingTrade.erase(itr);
                continue;
            }
#endif
            // Never overwrite a slot added by another trade path.
            bool empty = true;
            for (uint8 slot = 0; slot < TRADE_SLOT_COUNT; ++slot)
                empty = empty && !trade->GetItem(TradeSlots(slot));
            if (!empty || trade->GetMoney() || trade->GetSpell())
            {
                CancelSurplusTrade(bot, owner, pending.announceFailure
                    ? "The trade changed before I could fill it. Please try again when you're ready." : nullptr);
                SendDeliveryProgress(owner, pending.nonce, "failed", "The trade changed before I could fill it.");
                itr = g_pendingTrade.erase(itr);
                continue;
            }

            // Recalculate the reserve before touching bags. Never substitute a stack
            // with the same item entry: it may already be offered or held in reserve.
            if (PlanDelivery(bot, owner, pending.curated, false, pending.supply) != pending.offer)
            {
                CancelSurplusTrade(bot, owner, pending.supply.entry
                    ? "My supplies changed before the trade. Refresh my list and try again." : nullptr);
                SendDeliveryProgress(owner, pending.nonce, "failed", "The available supplies changed. Refresh the list and try again.");
                itr = g_pendingTrade.erase(itr);
                continue;
            }

            uint32 offered = 0;
            for (auto const& [guid, want] : pending.offer)
            {
                Item* tradeItem = bot->GetItemByGuid(guid);
                if (!tradeItem || !tradeItem->CanBeTraded() || tradeItem->GetCount() < want ||
                    trade->HasItem(tradeItem->GetGUID()))
                    continue;

                if (tradeItem->GetCount() > want)
                {
                    uint16 const dstPos = FindFreeBagPos(bot, tradeItem);
                    if (!dstPos)
                        continue;
                    bot->SplitItem(tradeItem->GetPos(), dstPos, want);
                    tradeItem = bot->GetItemByPos(dstPos);
                    if (!tradeItem || !tradeItem->CanBeTraded() || tradeItem->GetCount() != want ||
                        trade->HasItem(tradeItem->GetGUID()))
                        continue;
                }

                uint8 const slot = uint8(offered);
                trade->SetItem(TradeSlots(slot), tradeItem);
                pending.offeredItems.push_back({ TradeSlots(slot), tradeItem->GetGUID(), want });
                ++offered;
            }

            // Arm only the companion's side. SetItem clears both sides' acceptance;
            // the owner must still review the filled offer and click Accept.
            if (offered && offered == pending.offer.size() && MatchesTradeOffer(bot, trade, pending))
            {
                pending.applied = true;
                trade->SetAccepted(true);
#ifdef AC_TRADE_OFFER_COMMIT
                if (pending.automatic)
                    g_lastAutoTrade[pending.ownerGuid] = uint32(time(nullptr));
#endif
                SendDeliveryProgress(owner, pending.nonce, "ready", "Your items are in the trade window. Review them and click Accept.");
                if (pending.announceFailure && !pending.nonce && !pending.supply.entry)
                    SayToOwner(bot, owner, "I put " + std::to_string(offered) +
                               " stack(s) in the trade - take them, they are yours.", 30, 0, false, pending.noticeId);
                ++itr;                            // the record stays: the guard is live now
            }
            else
            {
                CancelSurplusTrade(bot, owner, pending.announceFailure
                    ? "I couldn't prepare any spare stacks. I'll check again when there's room." : nullptr);
                SendDeliveryProgress(owner, pending.nonce, "failed", "I could not prepare the complete offer.");
                itr = g_pendingTrade.erase(itr);
            }
        }
    }

    /// Bring companions along when the owner teleports.
    ///
    /// A hearthstone, a portal or a summon moves the player and leaves the companion where it
    /// stood - the one thing in the game that should never be left behind in another zone.
    /// Hooked on the teleport rather than inferred from distance: walking away must never
    /// teleport anything, and a hearth must always.
    void FollowOwnerThroughTeleport()
    {
        if (!g_cfg.followThroughTeleport || g_pendingFollow.empty())
            return;

        constexpr uint32 kFollowDelaySeconds = Timing::FollowDelay;
        constexpr float kFollowDistanceYards = Timing::FollowDistance;

        uint32 const now = uint32(time(nullptr));

        for (auto itr = g_pendingFollow.begin(); itr != g_pendingFollow.end();)
        {
            uint32 const ownerGuid = itr->first;

            PendingFollow const& travel = itr->second;
            uint32 const age = now - travel.queuedAt;
            bool const dungeonExit = travel.sourceInstance && !travel.verifyDestination;
            bool const extendedOnly = age > Timing::FollowRetry;
            if (age > (dungeonExit ? Timing::DungeonExitRetry : Timing::FollowRetry))
            {
                std::string holds;
                for (auto const& [guid, note] : travel.holdNotes)
                    holds += (holds.empty() ? "" : ", ") + note;
                LOG_INFO("server.loading", "[CompanionQuestSync] {} for owner {} expired after {}s ({})",
                         dungeonExit ? "dungeon-exit follow window" : "follow window", ownerGuid, age,
                         travel.holdNotes.empty() ? std::string("no companion hold recorded")
                                                  : "last holds: " + holds);
                itr = g_pendingFollow.erase(itr);
                continue;
            }

            // Far teleports remove the owner from world before the client acknowledges.
            // Keep the exit intent until OnPlayerMapChanged confirms it (or it expires).
            if (age < kFollowDelaySeconds || (travel.sourceInstance && travel.verifyDestination))
            {
                ++itr;
                continue;
            }

            Player* owner = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(ownerGuid));
            std::vector<uint32> bots;

            if (!owner || !FindCompanions(ownerGuid, bots))
            {
                if (g_cfg.debug >= 1)
                    LOG_INFO("server.loading",
                             "[CompanionQuestSync] follow intent for owner {} dropped (owner or companions not in world)",
                             ownerGuid);
                itr = g_pendingFollow.erase(itr);
                continue;
            }

            if (!owner->IsInWorld() || owner->IsBeingTeleported() || !owner->IsAlive() ||
                owner->IsInCombat() || IsOnTaxi(owner) || owner->GetTransport() || owner->GetVehicle() ||
                owner->IsFlying() || owner->IsFalling())
            {
                ++itr;
                continue;
            }
            if (owner->GetMap()->Instanceable() || (dungeonExit && owner->GetMapId() != travel.map))
            {
                LOG_INFO("server.loading", "[CompanionQuestSync] follow intent for owner {} dropped - owner {}",
                         ownerGuid,
                         owner->GetMap()->Instanceable() ? "entered an instanceable map" : "left the exit map");
                itr = g_pendingFollow.erase(itr);
                continue;
            }
            if (travel.verifyDestination && (owner->GetMapId() != travel.map ||
                owner->GetDistance(travel.x, travel.y, travel.z) > 40.0f))
            {
                ++itr; // BeforeTeleport reported intent, not successful arrival.
                continue;
            }

            bool anySkipped = false;

            auto holdReason = [](Player* bot) -> std::string
            {
                if (bot->IsInCombat()) return "in combat";
                if (!bot->IsAlive()) return "dead";
                if (bot->IsBeingTeleported()) return "teleporting";
                if (IsOnTaxi(bot)) return "on a taxi";
                if (bot->GetTransport()) return "on a transport";
                if (bot->GetVehicle()) return "in a vehicle";
                if (bot->IsFlying()) return "flying";
                return "falling";
            };

            for (uint32 const botGuid : bots)
            {
                Player* bot = FindCompanionPlayer(botGuid);
                if (extendedOnly && !(bot && bot->IsInWorld() &&
                    bot->GetMapId() == travel.sourceMap && bot->GetInstanceId() == travel.sourceInstance &&
                    IsControlledCompanion(bot, owner) && owner->GetGroup() && bot->GetGroup() == owner->GetGroup()))
                {
                    itr->second.holdNotes[botGuid] = !bot ? std::string("a companion is not in world")
                        : !bot->IsInWorld() ? std::string(bot->GetName()) + ": not in world"
                        : (bot->GetMapId() != travel.sourceMap || bot->GetInstanceId() != travel.sourceInstance)
                            ? std::string(bot->GetName()) + ": left the exit dungeon"
                        : !IsControlledCompanion(bot, owner)
                            ? std::string(bot->GetName()) + ": no longer a controlled companion"
                            : std::string(bot->GetName()) + ": not in the owner's party";
                    continue;
                }
                // A bot clears its fall flag only when its client sends a landing update,
                // and an idle bot sends none - so a flag picked up as the owner left can
                // stick for the whole window. A real fall ends within seconds; give every
                // airborne hold that long, then let the follow proceed despite the flag.
                auto& airSince = itr->second.airSince;
                bool const airborneClean = bot && bot->IsInWorld() && bot->IsAlive() && !bot->IsInCombat() &&
                                           !bot->IsBeingTeleported() && !IsOnTaxi(bot) &&
                                           !bot->GetTransport() && !bot->GetVehicle();
                bool const airborne = airborneClean && (bot->IsFlying() || bot->IsFalling());
                if (!airborne)
                    airSince.erase(botGuid);
                bool airborneGraceSpent = false;
                if (airborne)
                {
                    uint32& seen = airSince[botGuid];
                    if (!seen)
                        seen = now;
                    airborneGraceSpent = now - seen >= Timing::AirborneGrace;
                }
                if (!bot || !bot->IsInWorld() || !bot->IsAlive() || bot->IsInCombat() || bot->IsBeingTeleported() ||
                    IsOnTaxi(bot) || bot->GetTransport() || bot->GetVehicle() ||
                    ((bot->IsFlying() || bot->IsFalling()) && !airborneGraceSpent))
                {
                    std::string const note = bot ? std::string(bot->GetName()) + ": " + holdReason(bot)
                                                 : std::string("a companion is not in world");
                    std::string& last = itr->second.holdNotes[botGuid];
                    if (g_cfg.debug >= 1 && last != note)
                    {
                        LOG_INFO("server.loading", "[CompanionQuestSync] {} is held back ({}s left to follow)",
                                 note,
                                 (dungeonExit ? Timing::DungeonExitRetry : Timing::FollowRetry) - age);
                        last = note;
                    }
                    else if (last != note)
                        last = note;
                    if (dungeonExit && extendedOnly && bot && !itr->second.whisperedBots.count(botGuid))
                    {
                        itr->second.whisperedBots.insert(botGuid);   // one ask per companion per exit
                        SayToOwner(bot, owner,
                                   "I'm still tied up in here - I'll follow you out on my own, no need to summon me.",
                                   300, 0, false);
                    }
                    anySkipped = true;   // held for the retry window, not abandoned
                    continue;
                }

                bool const elsewhere = bot->GetMap() != owner->GetMap();
                float const away = elsewhere ? 999999.0f : bot->GetDistance(owner);

                if (away <= kFollowDistanceYards)
                {
                    itr->second.holdNotes.erase(botGuid);
                    continue;
                }

                float x = owner->GetPositionX() + 2.0f;
                float y = owner->GetPositionY();
                float z = owner->GetPositionZ();

                owner->UpdateGroundPositionZ(x, y, z);
                bot->StopMoving();

                if (elsewhere)
                {
                    if (!bot->TeleportTo(owner->GetMapId(), x, y, z, owner->GetOrientation()))
                    {
                        anySkipped = true;
                        continue;
                    }
                }
                else
                    bot->NearTeleportTo(x, y, z, owner->GetOrientation());

                itr->second.holdNotes.erase(botGuid);

                LOG_INFO("server.loading",
                         "[CompanionQuestSync] {} followed {} through a teleport - map {}, {} yd behind",
                         bot->GetName(), owner->GetName(), owner->GetMapId(),
                         elsewhere ? 0 : uint32(away));
                if (extendedOnly)
                    LOG_INFO("server.loading", "[CompanionQuestSync] {} requested delayed dungeon exit teleport ({}s)",
                             bot->GetName(), age);
            }

            // Unsafe or offline companions hold the entry until the retry window expires.
            if (!anySkipped)
            {
                if (dungeonExit)
                    LOG_INFO("server.loading",
                             "[CompanionQuestSync] delayed exit follow for owner {} ended after {}s ({})",
                             ownerGuid, age,
                             [&]
                             {
                                 std::string holds;
                                 for (auto const& [guid, note] : travel.holdNotes)
                                     holds += (holds.empty() ? "" : ", ") + note;
                                 return travel.holdNotes.empty() ? std::string("companions caught up")
                                                                 : "not followed: " + holds;
                             }());
                itr = g_pendingFollow.erase(itr);
            }
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
    bool SayToOwner(Player* bot, Player* owner, std::string const& text, uint32 minGap,
                    uint32 validWhileMissing, bool handIn, uint64 tradeNoticeId, ObjectGuid readyGiver)
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

        // Replies on the command stack can speak immediately. Ambient hand-in
        // notices stay paced even if the command synchronously runs a quest pass.
        bool const commandReply = g_commandReplyOwner == ownerGuid && !handIn;

        // asked-for replies answer every time; ambient news that has not changed does not repeat
        if (!commandReply && !handIn && tradeNoticeId == 0 && !text.empty())
        {
            auto const memory = g_lastSpoken.find(botGuid);
            if (memory != g_lastSpoken.end() && memory->second.first == text && now >= memory->second.second &&
                now - memory->second.second < 900)
                return false;
            g_lastSpoken[botGuid] = { text, now };
        }
        if (!commandReply && last && now - last < minGap)
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
                if (entry.botGuid == botGuid && entry.text == text && entry.tradeNoticeId == tradeNoticeId &&
                    entry.readyGiver == readyGiver)
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

            if (!dup && (vital || tradeNoticeId || queue.size() < 4))
            {
                if (vital)
                    queue.insert(queue.begin(), { text, botGuid, validWhileMissing, now, true, handIn });
                else
                    queue.push_back({ text, botGuid, validWhileMissing, now, false, handIn, tradeNoticeId, readyGiver });
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
        // "it is a class quest my class cannot take" is not an ask the player can answer, so it is
        // never whispered: it goes to the log once and stays there. Once per quest, not once per
        // pass - a class or race lock is permanent for this body, so the same line every 30 s is
        // pure noise (measured: 84 lines in 16 minutes for one paladin's class quest across four
        // companions). The latch is g_notified, which the pass already prunes against the owner's
        // log, so a quest that leaves and comes back is reported again.
        if (refusal.expected)
        {
            bool const first = g_notified[bot->GetGUID().GetCounter()].insert(quest->GetQuestId()).second;
            if (first && g_cfg.debug >= 1)
                LOG_INFO("server.loading", "[CompanionQuestSync] {} could not take '{}' ({}): {}",
                         bot->GetName(), quest->GetTitle(), quest->GetQuestId(), refusal.reason);
            return;
        }

        bool const wanted = g_cfg.notifySkipped >= 2;
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
        uint32 phase = 1;
        bool gameObject = false;
    };

    uint32 ZoneAt(uint32 mapId, uint32 phaseMask, float x, float y, float z)
    {
        Map* map = sMapMgr->FindMap(mapId, 0);
        if (!map)
            return 0;

        return map->GetZoneId(phaseMask, x, y, z);
    }

    struct EnderPin
    {
        float x, y;
        std::string zone;
    };

    std::optional<EnderPin> PlotEnder(EnderSpot const& spot);

    // Cache immutable spawn candidates, not the decision of which spawn is
    // nearest a moving companion. These records are hints, not live enders.
    std::unordered_map<uint32, std::vector<EnderSpot>> g_enderSpawns;

    EnderSpot FindQuestEnder(Player* bot, uint32 questId, std::optional<EnderPin>* forPin = nullptr)
    {
        if (forPin)
            forPin->reset();
        auto [it, load] = g_enderSpawns.try_emplace(questId);
        if (load)
        {
            QueryResult result = WorldDatabase.Query(
                "SELECT ct.name, c.position_x, c.position_y, c.position_z, c.map, ce.id, c.zoneId, c.phaseMask, 0 "
                "FROM creature_questender ce JOIN creature_template ct ON ct.entry = ce.id "
                "JOIN creature c ON c.id1 = ce.id WHERE ce.quest = {} "
                "UNION ALL "
                "SELECT gt.name, g.position_x, g.position_y, g.position_z, g.map, ge.id, g.zoneId, g.phaseMask, 1 "
                "FROM gameobject_questender ge JOIN gameobject_template gt ON gt.entry = ge.id "
                "JOIN gameobject g ON g.id = ge.id WHERE ge.quest = {} ORDER BY 5, 6, 2, 3, 4",
                questId, questId);
            if (result)
            {
                do
                {
                    Field* f = result->Fetch();
                    it->second.push_back({ true, f[5].Get<uint32>(), f[0].Get<std::string>(),
                        f[1].Get<float>(), f[2].Get<float>(), f[3].Get<float>(), f[4].Get<uint32>(),
                        f[6].Get<uint32>(), f[7].Get<uint32>(), f[8].Get<uint32>() != 0 });
                } while (result->NextRow());
            }
        }

        EnderSpot const* nearest = nullptr;
        float best = 0.0f;
        uint32 const botMap = bot->GetMapId();
        for (EnderSpot const& candidate : it->second)
        {
            // Map pins also exclude phase-incompatible remote spawns; preserve the
            // existing cross-map direction fallback for whispers.
            bool const sameMap = candidate.map == botMap;
            if ((forPin || sameMap) && !(candidate.phase & bot->GetPhaseMask()))
                continue;
            // Maps are incomparable; the first eligible remote spawn is the fallback.
            float const dist = sameMap ? bot->GetDistance(candidate.x, candidate.y, candidate.z) : 0.0f;
            if (nearest && (!sameMap || (nearest->map == botMap && !(dist < best))))
                continue;
            if (forPin)
            {
                auto pin = PlotEnder(candidate);
                if (!pin)
                    continue;
                *forPin = std::move(pin);
            }
            nearest = &candidate;
            best = dist;
        }
        EnderSpot spot = nearest ? *nearest : EnderSpot{};
        if (!forPin && spot.found && !spot.zone)
            spot.zone = ZoneAt(spot.map, spot.phase, spot.x, spot.y, spot.z);
        return spot;
    }

    /// "map 52, 34" for a world position, in the client's own 0-100 scale, so the
    /// player can plot the spot themselves even when the map will not show a pin for
    /// a quest that is not in their own log. Nothing parses this text: the markers
    /// come from the CSYNC pin payload, and CompanionPins treats whispers as
    /// narrative only. It is a hint for the player's own map, nothing more.
    std::string MapCoords(float x, float y, uint32 zoneId)
    {
        float mapX = x;
        float mapY = y;

        // Map2ZoneCoordinates is the world -> map-percentage direction; the
        // similarly named Zone2MapCoordinates converts the other way, which is why
        // every coordinate this module produced came back out of range and was
        // dropped by the guard below - no "map X, Y" in any spoken hint.
        Map2ZoneCoordinates(mapX, mapY, zoneId);

        if (mapX < 0.0f || mapX > 100.0f || mapY < 0.0f || mapY > 100.0f)
            return std::string();

        return "map " + std::to_string(uint32(mapX)) + ", " + std::to_string(uint32(mapY));
    }
    /// "map X, Y" for a quest's objective area, taken from its POI points, so a
    /// spoken hint names a spot the player can plot. The marker for the same area
    /// travels in the pin payload instead; the coordinate text is only ever read by
    /// the player, never by the addons. Every branch that speaks a location carries
    /// one, so the hint reads the same whichever branch fires.
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

    std::string FormationLower(std::string name)
    {
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return char(std::tolower(c)); });
        return name;
    }

    std::string ZoneName(uint32 zoneId, char const* fallback = "an unknown zone")
    {
        // Direct ID lookup. GetAreaEntryByAreaID on this build round-trips through the
        // area's exploration bit and looks THAT up as an entry id - an arbitrary row -
        // which is how a pin standing in Elwynn introduced itself as Tkashi Ruins. The
        // store is id-indexed, so the honest path is one LookupEntry.
        if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(zoneId))
            if (zone->area_name[0])
                return zone->area_name[0];

        return fallback;
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
    bool WmaConvert(float& x, float& y, int32 areaId, std::optional<uint32> mapId = std::nullopt)
    {
        for (WorldMapAreaRow const& row : g_worldMapAreas)
        {
            if (row.areaId != areaId || (mapId && row.mapId != *mapId))
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

    std::optional<EnderPin> PlotEnder(EnderSpot const& spot)
    {
        if (!spot.found)
            return std::nullopt;
        uint32 const zone = spot.zone ? spot.zone : ZoneAtHeight(spot.map, spot.phase, spot.x, spot.y);
        float gx = spot.x, gy = spot.y;
        if (!zone || !WmaConvert(gx, gy, int32(zone), spot.map) ||
            !(gx >= 0.0f && gx <= 100.0f && gy >= 0.0f && gy <= 100.0f))
            return std::nullopt;
        std::string zoneName = ZoneName(zone, "");
        if (zoneName.empty())
            return std::nullopt;
        return EnderPin{ gx, gy, std::move(zoneName) };
    }


    /// content-keyed, bounded by the quests that exist (same policy as g_poiCache)
    // One quest can owe work in several areas: quest_poi holds a row per objective,
    // each with its own points, so the map should show one pin per area, not just the
    // first. Capped at four so a sprawling quest cannot carpet the map; the whisper
    // (`.companionsync quests`) still names every area.
    std::map<std::tuple<uint32, uint32, uint32>, std::vector<std::pair<std::string, std::string>>> g_poiPinCache;

    /// Where the work actually is: the creatures that drop the item the quest requires.
    ///
    /// A wanted poster's POI points at the poster, and an outline's first corner is
    /// wherever the cartographer started the path - neither is the mob. The loot table
    /// is: for every required item, the creatures whose loot holds it, spawned where
    /// they spawn. Stable hotspots are capped like every other pin; a quest
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

        // Group spawn coordinates into fixed map cells, not nearest-to-bot spots.
        // Cache by quest/map/phase so walking neither rearranges pins nor redoes
        // terrain queries. Use an actual spawn near each cell centre as the anchor.
        using Coordinates = std::vector<std::pair<std::string, std::string>>;
        static std::map<std::tuple<uint32, uint32, uint32>, Coordinates> cache;
        uint32 const map = bot->GetMapId();
        uint32 const phase = bot->GetPhaseMask();
        auto const key = std::make_tuple(questId, map, phase);
        if (auto it = cache.find(key); it != cache.end())
        {
            out = it->second;
            return !out.empty();
        }

        struct Area
        {
            uint32 count = 0;
            std::tuple<int32, int32, int32> anchor; // squared distance to centre, x, y
        };
        constexpr int32 kCellSize = 8;
        using CellKey = std::tuple<uint32, int32, int32>;
        std::map<CellKey, Area> areas;
        for (auto const& [spawnMap, sx, sy] : spawns->second)
        {
            if (spawnMap != map)
                continue;
            uint32 const zone = ZoneAtHeight(map, phase, sx, sy);
            if (!zone)
                continue;
            float x = sx, y = sy;
            if (!WmaConvert(x, y, int32(zone)) && zone != 12)
                continue;
            if (!(x >= 0.0f && x <= 100.0f && y >= 0.0f && y <= 100.0f))
                continue;

            int32 const gx = int32(x), gy = int32(y);
            int32 const cx = gx / kCellSize, cy = gy / kCellSize;
            int32 const dx = gx - cx * kCellSize - kCellSize / 2;
            int32 const dy = gy - cy * kCellSize - kCellSize / 2;
            auto const candidate = std::make_tuple(dx * dx + dy * dy, gx, gy);
            Area& area = areas[{ zone, cx, cy }];
            if (!area.count || candidate < area.anchor)
                area.anchor = candidate;
            ++area.count;
        }

        std::vector<std::pair<CellKey, Area>> ranked(areas.begin(), areas.end());
        std::sort(ranked.begin(), ranked.end(), [](auto const& a, auto const& b)
        {
            if (a.second.count != b.second.count)
                return a.second.count > b.second.count;
            return a.first < b.first;
        });
        // Cover distinct zones before adding a second hotspot in any one zone.
        std::map<uint32, uint8> perZone;
        for (uint8 pass = 0; pass < 2 && out.size() < 4; ++pass)
        {
            std::set<uint32> seen;
            for (auto const& [cell, area] : ranked)
            {
                uint32 const zone = std::get<0>(cell);
                if (pass == 1 && seen.insert(zone).second)
                    continue; // the first pass already selected this cell
                if (perZone[zone] != pass)
                    continue;
                ++perZone[zone];
                out.emplace_back("map " + std::to_string(std::get<1>(area.anchor)) + ", " +
                    std::to_string(std::get<2>(area.anchor)), ZoneName(zone));
                if (out.size() >= 4)
                    break;
            }
        }
        cache.emplace(key, out);
        return !out.empty();
    }

    bool PoiPins(Player* bot, Quest const* quest, std::vector<std::pair<std::string, std::string>>& out)
    {
        auto const key = std::make_tuple(quest->GetQuestId(), bot->GetMapId(), bot->GetPhaseMask());
        auto const cached = g_poiPinCache.find(key);
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

        g_poiPinCache[key] = out;
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

    void StepBesidePoint(Player* bot, float x, float y, float z, float radius);

    bool OpenGroundQuestChest(Player* bot, GameObject* go)
    {
        if (go->GetGOInfo()->GetLockId())
        {
            // Opening - No Text is the playerbots default. Cast normally so
            // core lock requirements, cooldowns and cast checks still apply.
            uint32 constexpr openingSpell = 6477;
            return bot->CastSpell(go, openingSpell, false) == SPELL_CAST_OK;
        }
        if (go->HasGameObjectFlag(GO_FLAG_LOCKED))
            return false;

        // Spell::CheckCast rejects lockless targets. Match Spell::SendLoot's
        // chest dispatch before opening instance loot; playerbots handles the
        // native response, storage checks, and release (including chest events).
        if (sScriptMgr->OnGossipHello(bot, go) || go->AI()->GossipHello(bot, false))
            return true; // a script handled the interaction
        if (uint32 trap = go->GetGOInfo()->chest.linkedTrapId)
            go->TriggeringLinkedGameObject(trap, bot);
        bot->SendLoot(go->GetGUID(), LOOT_SKINNING);
        return true;
    }

    bool GatherFromGround(Player* owner, Player* bot, std::string& what)
    {
        if (!owner || !bot || !bot->IsAlive() || bot->IsInCombat() || bot->IsInFlight() ||
            bot->IsBeingTeleported() || bot->GetTransport() || bot->GetVehicle() ||
            bot->GetMap() != owner->GetMap() || bot->GetLootGUID() ||
            bot->HasUnitState(UNIT_STATE_CASTING) || g_itemGroundObjects.empty())
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
                auto const ground = g_itemGroundObjects.find(itemId);
                if (ground == g_itemGroundObjects.end() || entry.second.ItemCount[i] >= quest->RequiredItemCount[i])
                    continue;

                ++g_gridSearches;
                std::list<GameObject*> objects;
                bot->GetGameObjectListWithEntryInGrid(objects, ground->second, 60.0f);
                objects.sort([bot](GameObject* a, GameObject* b) { return bot->GetDistance(a) < bot->GetDistance(b); });
                GameObject* approach = nullptr;
                for (GameObject* go : objects)
                {
                    if (!go->isSpawned() || go->getLootState() != GO_READY || go->GetGoState() != GO_STATE_READY ||
                        go->GetGoType() != GAMEOBJECT_TYPE_CHEST || !go->GetGOInfo()->GetLootId() ||
                        !go->ActivateToQuest(bot) ||
                        go->HasGameObjectFlag(GO_FLAG_NOT_SELECTABLE) || go->HasGameObjectFlag(GO_FLAG_INTERACT_COND))
                        continue;

                    if (!go->IsAtInteractDistance(bot))
                    {
                        if (!approach)
                            approach = go;
                        continue;
                    }

                    // Stop after requesting one open. Never roll the template again
                    // or deactivate an object ourselves while its loot is pending.
                    std::string const source = go->GetName();
                    std::string const title = quest->GetTitle();
                    bot->StopMoving();
                    if (OpenGroundQuestChest(bot, go))
                    {
                        what = source + " for " + title;
                        return true;
                    }
                }

                if (approach)
                {
                    StepBesidePoint(bot, approach->GetPositionX(), approach->GetPositionY(), approach->GetPositionZ(), 2.0f);
                    return false; // wait for positioning before opening
                }
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

            if (quest->IsRepeatable() || quest->IsSeasonal() || quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_TIMED) ||
                !owner->IsQuestRewarded(entry.first))
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

    EnderSpot DescribeQuestEnder(WorldObject* giver);

    // Navigation is one errand for the player, even if several companions owe it.
    bool TakeHandInTravelHint(Player* owner, uint32 questId, uint32 now)
    {
        uint64 const key = (uint64(owner->GetGUID().GetCounter()) << 32) | questId;
        uint32& last = g_handInTravelHintAt[key];
        if (last && now - last < 600)
            return false;
        last = now;
        return true;
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

            WorldObject* giver = FindReadyQuestEnder(bot, quest);
            EnderSpot const spot = giver ? DescribeQuestEnder(giver) : FindQuestEnder(bot, questId);
            if (!spot.found)
                continue;

            if (spot.map != bot->GetMapId())
            {
                if (!TakeHandInTravelHint(owner, questId, now))
                    continue;
                last = now;
                SayToOwner(bot, owner, "'" + quest->GetTitle() + "' gets handed in at " + spot.name +
                                      ", and that is on another map from here - we would have to travel there together.",
                           45, questId, true);
                return;
            }

            // Spawn coordinates are directions, not evidence that the giver is usable.
            if (giver)
            {
                uint32& hintAt = g_turnInHintAt[{botGuid, questId}];
                if (hintAt && now - hintAt < 600)
                    continue;
                last = now;
                hintAt = now;

                std::string const here = MapCoords(spot.x, spot.y, bot->GetZoneId());
                SayToOwner(bot, owner, "I am at " + spot.name + " with '" + quest->GetTitle() +
                                      "' ready to turn in" +
                                      (here.empty() ? "." : " (" + here + ")."),
                           45, questId, true, 0, giver->GetGUID());
                return;
            }

            float const dist = bot->GetDistance(spot.x, spot.y, spot.z);
            if (dist > 60.0f)
            {
                if (!TakeHandInTravelHint(owner, questId, now))
                    continue;
                last = now;
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

    // Share current bag counts between the hand-in check, whispers and map details.
    template <typename Visitor>
    void ForEachMissingRequiredItem(Player* bot, Quest const* quest, Visitor visit)
    {
        for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
        {
            uint32 const required = quest->RequiredItemCount[i];
            if (!required)
                continue;

            uint32 const itemId = quest->RequiredItemId[i];
            uint32 const have = bot->GetItemCount(itemId);
            if (have < required)
                visit(itemId, have, required);
        }
    }

    std::string RequiredItemName(uint32 itemId)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
        return proto && !proto->Name1.empty() ? proto->Name1 : "item " + std::to_string(itemId);
    }

    /// "Large Candle 0/8" - what the companion still owes for a quest, spelled
    /// out. A bare "missing items" whisper told the player nothing they could act
    /// on: not which item, not how many are already in the bags.
    std::string DescribeMissingItemsUncached(Player* bot, Quest const* quest)
    {
        std::string out;
        ForEachMissingRequiredItem(bot, quest, [&](uint32 itemId, uint32 have, uint32 required)
        {
            if (!out.empty())
                out += ", ";
            out += RequiredItemName(itemId) + " " + std::to_string(have) + "/" + std::to_string(required);
        });
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
    bool IsLiveTradeNotice(Player* bot, Player* owner, uint64 noticeId)
    {
        auto const pending = g_pendingTrade.find(bot->GetGUID().GetCounter());
        if (pending == g_pendingTrade.end() || pending->second.noticeId != noticeId ||
            pending->second.ownerGuid != owner->GetGUID().GetCounter() || !pending->second.applied)
            return false;

        TradeData const* trade = bot->GetTradeData();
        TradeData const* other = owner->GetTradeData();
        if (!trade || !other || trade->GetTrader() != owner || other->GetTrader() != bot || !trade->IsAccepted())
            return false;
#ifdef AC_TRADE_OFFER_GUARD
        if (trade->GetOfferToken() != noticeId)
            return false;
#endif

        return MatchesTradeOffer(bot, trade, pending->second);
    }

    /// Send whatever was held back by the rate limit, one per pass.
    void FlushWhisperQueue(Player* owner, Player* /*fallback*/)
    {
        uint32 const ownerGuid = owner->GetGUID().GetCounter();
        auto itr = g_whisperQueue.find(ownerGuid);
        if (itr == g_whisperQueue.end() || itr->second.empty())
            return;

        uint32 const now = static_cast<uint32>(time(nullptr));
        uint32& last = g_lastWhisperAt[ownerGuid];
        if (last && now - last < Timing::WhisperGap)
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
                if (entry->tradeNoticeId)
                    stale = !IsLiveTradeNotice(speaker, owner, entry->tradeNoticeId);
                else if (entry->questId && entry->handIn)
                {
                    // An invitation to hand in is only true while the quest is still
                    // complete: the moment it is rewarded (or gone), a held-back
                    // "close to the giver" line would arrive describing a past event.
                    // Truth wins over age here - no expiry window can save a lie.
                    stale = speaker->GetQuestStatus(entry->questId) != QUEST_STATUS_COMPLETE;
                    if (!stale && entry->readyGiver)
                    {
                        Quest const* quest = sObjectMgr->GetQuestTemplate(entry->questId);
                        WorldObject* giver = FindReadyQuestEnder(speaker, quest);
                        stale = !giver || giver->GetGUID() != entry->readyGiver;
                    }
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

        auto const liveTrade = std::find_if(queue.begin(), queue.end(), [](QueuedWhisper const& entry)
        {
            return entry.tradeNoticeId != 0;
        });
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
        else if (liveTrade != queue.end())
            chosen = liveTrade;
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

    /// The tool items a companion's two professions depend on. A bag cleanup must never
    /// sell them: a pick and a knife are "outgrown" by every measure except usefulness.
    static std::set<uint32> ProfessionToolItems(uint8 cls)
    {
        std::set<uint32> items;
        auto const pair = ClassProfessionPair(cls);
        for (uint16 const skill : { pair.first, pair.second })
            if (uint32 const tool = ProfessionOf(skill).tool)
                items.insert(tool);
        return items;
    }

    /// The skill ceiling a trainer would sell at this level - the classic tiers.
    static uint32 ProfessionCap(uint8 level)
    {
        if (level >= 35) return 300;
        if (level >= 20) return 225;
        if (level >= 10) return 150;
        return 75;
    }

    struct CraftRecipe
    {
        uint32 spell;
        uint32 product;
        uint16 requiredSkill;
    };

    struct CraftLane
    {
        uint16 skill;
        std::vector<CraftRecipe> recipes;
    };

    // Trainer skill floors, not SkillLineAbility.MinSkillLineRank: several advanced
    // WotLK recipes have a DBC floor of 1. Keep each lane ordered best to starter.
    static std::vector<CraftLane> const& CraftLanes(uint16 profession)
    {
        static std::vector<CraftLane> const smith = {
            { SKILL_MINING, { { 16153, 12359, 230 }, { 10097, 3860, 175 }, { 3307, 3575, 125 },
                              { 3304, 3576, 65 }, { 2657, 2840, 1 } } },
            { SKILL_BLACKSMITHING, { { 16641, 12404, 250 }, { 9918, 7964, 200 }, { 2674, 2871, 125 },
                                     { 2665, 2863, 65 }, { 2660, 2862, 1 } } },
        };
        static std::vector<CraftLane> const leather = {
            { SKILL_LEATHERWORKING, { { 19058, 15564, 250 }, { 10487, 8173, 200 }, { 3780, 4265, 150 },
                                      { 2165, 2313, 100 }, { 2152, 2304, 1 } } },
        };
        static std::vector<CraftLane> const alchemy = {
            { SKILL_ALCHEMY, { { 17556, 13446, 275 }, { 11457, 3928, 215 }, { 7181, 1710, 155 },
                               { 3447, 929, 110 }, { 2337, 858, 55 }, { 2330, 118, 1 } } },
        };
        static std::vector<CraftLane> const none;
        switch (profession)
        {
            case SKILL_BLACKSMITHING: return smith;
            case SKILL_LEATHERWORKING: return leather;
            case SKILL_ALCHEMY: return alchemy;
            default: return none;
        }
    }

    static void TeachCraftRecipes(Player* bot, uint16 profession)
    {
        for (CraftLane const& lane : CraftLanes(profession))
        {
            uint16 const attained = bot->GetPureSkillValue(lane.skill);
            for (CraftRecipe const& recipe : lane.recipes)
            {
                if (attained < recipe.requiredSkill || bot->HasSpell(recipe.spell) || !sSpellMgr->GetSpellInfo(recipe.spell))
                    continue;

                bot->learnSpell(recipe.spell);
                if (g_cfg.debug >= 2 && bot->HasSpell(recipe.spell))
                    LOG_INFO("server.loading", "[CompanionQuestSync] {} learned craft spell {} at skill {}:{}",
                             bot->GetName(), recipe.spell, uint32(lane.skill), attained);
            }
        }
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
            // no-gather mode does not pick nodes up, so no pick or knife is owed
            if (entry.tool && g_cfg.professionNoGather < 2 && !bot->GetItemCount(entry.tool))
            {
                ItemPosCountVec dest;
                if (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, entry.tool, 1) == EQUIP_ERR_OK)
                    if (Item* tool = bot->StoreNewItem(dest, entry.tool, true))
                        bot->SendNewItem(tool, 1, true, false, true);
            }
            // raise the ceiling only; gathered progress is never reset
            uint16 const cur = bot->GetSkillValue(skill);
            bot->SetSkill(skill, 0, std::min<uint16>(cur, uint16(cap)), uint16(cap));

            // ProfessionNoGather 2: the profession is established at the level's own
            // tier ceiling without walking a node - the current value rises to the cap
            // it can already hold. Mode 1 says what it would do (at Debug 2) and
            // touches nothing.
            if (cur < cap)
            {
                if (g_cfg.professionNoGather == 1)
                {
                    if (g_cfg.debug >= 2)
                        LOG_INFO("server.loading",
                                 "[CompanionQuestSync] {} would set {} to {} (ProfessionNoGather = 1: logging only)",
                                 bot->GetName(), entry.name, cap);
                }
                else if (g_cfg.professionNoGather >= 2)
                {
                    bot->SetSkill(skill, 0, uint16(cap), uint16(cap));
                    LOG_INFO("server.loading",
                             "[CompanionQuestSync] {} set {} to {} without gathering (ProfessionNoGather = 2)",
                             bot->GetName(), entry.name, cap);
                }
            }
        }
        TeachCraftRecipes(bot, pair.second);

        if (PlayerbotAI* ai = GET_PLAYERBOT_AI(bot))
        {
            // the strategy follows the mode: a no-gather companion is not asked to work
            // its own nodes, and the upkeep slot re-asserts either side after playerbots
            // drops strategies on a spec or level change
            if (g_cfg.professionNoGather >= 2)
                ai->ChangeStrategy("-gather", BOT_STATE_NON_COMBAT);
            else
                ai->ChangeStrategy("+gather", BOT_STATE_NON_COMBAT);
        }

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
                       (g_cfg.professionNoGather >= 2
                           ? " - the materials find their way to me."
                           : " - I will gather what I pass from now on."), 30);

        return tookUp;
    }

    /// Distance: presets are a friendly face for playerbots' own formations.
    ///
    /// The module used to walk bots onto a ring itself, out of combat, with its own spacing
    /// maths. playerbots ships nine formations that already move bots with combat priority
    /// (`CombatStrategy` named "formation"), persist per bot, and understand geometry
    /// (a shield wall, a wedge, single file). Re-implementing that was the wrong shape: ours
    /// could not position anybody during a fight, which is when positioning matters.
    ///
    /// So a preset now means "put this companion in that formation", and `auto` means "let its
    /// role choose": a tank leads, a healer keeps its distance, melee closes.
    char const* kFormationPresets[] = { "off", "auto", "close", "tight", "loose", "spread", "lead" };

    /// The playerbots formation an explicit preset stands for, or "" when the preset is not one.
    std::string FormationForPreset(std::string const& preset)
    {
        if (preset == "close")
            return "near";
        if (preset == "tight")
            return "queue";
        if (preset == "loose")
            return "circle";
        if (preset == "spread")
            return "far";
        if (preset == "lead")
            return "shield";
        return std::string();
    }

    /// How far, roughly, a formation lets a companion work from its owner.
    ///
    /// This is the leash. The module will not pin a target for a companion further behind than
    /// this: ordering a bot into a fight it is nowhere near is how companions sprint across a
    /// zone at a mob that is already dead.
    float FormationLeash(std::string const& formation)
    {
        if (formation == "near")
            return 5.0f;
        if (formation == "queue")
            return 7.0f;
        if (formation == "melee" || formation == "shield" || formation == "arrow")
            return 9.0f;
        if (formation == "circle")
            return 15.0f;
        if (formation == "far")
            return 25.0f;
        return 12.0f;                    // chaos, or a formation we have not measured
    }

    /// A preset name the module accepts, or an empty string for a typo.
    std::string FormationPreset(std::string const& name)
    {
        for (char const* preset : kFormationPresets)
            if (name == preset)
                return name;
        return std::string();
    }

    /// The owner's current preset, seeded from the config the first time it is asked for.
    std::string FormationFor(Player* owner)
    {
        uint32 const guid = owner->GetGUID().GetCounter();
        auto it = g_formation.find(guid);
        if (it == g_formation.end())
        {
            std::string const configured = FormationPreset(FormationLower(g_cfg.formation));
            it = g_formation.emplace(guid, configured.empty() ? std::string("auto") : configured).first;
        }
        return it->second;
    }

    /// How long a player has to stand still before the party may gather.
    ///
    /// Long enough that walking between two mobs does not flip the formation back and forth,
    /// short enough that stopping at a vendor or a body to loot feels immediate.
    uint32 constexpr kIdleGatherSeconds = 3;

    /// Is the player in the middle of something distance could spoil?
    ///
    /// A gossip menu or a trade window on *either* side. Only the bot's own window was checked
    /// before, which missed the case that actually bites: the player opens the trade.
    bool OwnerIsInteracting(Player* owner)
    {
        if (!owner)
            return false;

        if (owner->GetTradeData())
            return true;

        return owner->PlayerTalkClass &&
               owner->PlayerTalkClass->GetGossipMenu().GetMenuItemCount() > 0;
    }

    /// Has the player been on the same spot for a moment?
    ///
    /// Tracked rather than asked of the client: an unchanged position is the same signal the
    /// movement flags give, and it survives sitting, casting, looting and reading a quest text.
    bool OwnerStandingStill(Player* owner)
    {
        if (!owner)
            return false;

        uint32 const now = static_cast<uint32>(time(nullptr));
        IdleTrack& track = g_ownerIdle[owner->GetGUID().GetCounter()];

        float const dx = track.x - owner->GetPositionX();
        float const dy = track.y - owner->GetPositionY();
        float const dz = track.z - owner->GetPositionZ();

        if (!track.since || dx * dx + dy * dy + dz * dz > 1.0f)      // moved a yard or more
        {
            track.x = owner->GetPositionX();
            track.y = owner->GetPositionY();
            track.z = owner->GetPositionZ();
            track.since = now;
            return false;
        }

        return now - track.since >= kIdleGatherSeconds;
    }

    /// The formation a companion should be in right now.
    ///
    /// Three things, in order: an interaction you opened (trade on either side, a gossip menu)
    /// puts the party at your elbow; otherwise the preset wins if you set one, else the role
    /// decides; and out of a fight, if you have stopped moving, the shape gathers one step
    /// tighter. Nothing here moves anybody - playerbots' formation strategy does the walking.
    ///
    /// "" means the module is not managing this companion's distance: the old "off", kept as an
    /// escape hatch for anyone who would rather playerbots' default `chaos` stood.
    std::string EffectiveFormation(Player* owner, Player* bot)
    {
        if (!owner || !bot)
            return std::string();

        std::string const preset = FormationFor(owner);
        if (preset == "off")
            return std::string();

        // the bot's own trade window, and yours: both mean "hand it over, not a walk"
        if (bot->GetTradeData() || OwnerIsInteracting(owner))
            return "near";

        std::string shape = FormationForPreset(preset);      // "" for "auto"

        if (shape.empty())
        {
            if (owner->IsInCombat())
            {
                uint32 const botGuid = bot->GetGUID().GetCounter();
                std::string const role = CompanionRole(botGuid);
                std::string const policy = CompanionEngage(botGuid);

                // a companion that refuses the fight also refuses to stand in it
                if (policy == "never")
                    return "far";

                if (role == "tank")
                    shape = "shield";    // between you and the mob: what tanking means
                else if (role == "healer")
                    shape = "far";       // inside heal range, outside cleave
                else if (role == "support")
                    shape = "circle";
                else if (role == "dps")
                    shape = PlayerbotAI::IsMelee(bot) ? "melee" : "far";
                else
                    shape = "chaos";     // auto: exactly what playerbots would have done
            }
            else
                shape = "chaos";         // out of a fight, an auto companion is unmanaged
        }

        // Out of a fight, a player who has stopped moving gets the party at arm's length: if
        // nothing is happening, distance is only ever a way to be out of reach of a hand-over,
        // a vendor or a body to loot. Moving again restores the shape the role asked for.
        if (!owner->IsInCombat() && OwnerStandingStill(owner))
            return "near";

        return shape;
    }

    /// Bring a companion's spells, talents and professions up to the level it carries.
    ///
    /// GiveLevel fires OnPlayerLevelChanged, and that hook syncs all three - but a body can
    /// be levelled outside that path: the pool raises it before it is owned, a character is
    /// restored or hand-granted, or the custody row lands after the level change. Then a
    /// level-14 companion stands there with a level-1 spellbook and unspent talent points.
    /// This is the idempotent catch-up - once per level, at claim and on the periodic pass.
    void EnsureLevelProgress(Player* owner, Player* bot)
    {
        if (!bot || !bot->IsInWorld() || !bot->IsAlive() || !bot->GetSession())
            return;
        uint8 const level = bot->GetLevel();
        if (level < 5)
            return;                       // nothing a trainer sells below five

        uint32 const botGuid = bot->GetGUID().GetCounter();
        if (g_taughtLevel[botGuid] == level)
            return;

        TeachTrainerContent(bot, owner);  // weapon skills, class spells, talent points
        TeachProfessions(bot, owner);     // both professions at the tier cap, tools, recipes

        // Latch only once it has run: a pass that could not teach (a trade open, the
        // trainer sync refused) must get another try at this level, not the next one.
        g_taughtLevel[botGuid] = level;
    }

    /// Gathered materials eligible for surplus trade. Crafting checks its actual
    /// spell reagents separately; this is not a list of what can be bought.
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

    static bool IsCraftVendorReagent(uint32 item)
    {
        // Vials, threads and the existing vendor stock. Never infer that an
        // unlisted herb, hide, ore or intermediate is vendor stock.
        switch (item)
        {
            case 3371: case 3372: case 8925: // vials
            case 2320: case 2321: case 4291: // thread
            case 2604: case 3857: case 2605:
                return true;
            default:
                return false;
        }
    }

    /// Raw gathering stock: an herb, ore, stone or hide the gathering half of the
    /// pair would have collected. In no-gather mode the companion will never
    /// collect more and its crafts are supplied at craft time, so whatever it
    /// still carries is dead weight the vendor takes off its hands.
    static bool IsGatheredStock(uint32 item)
    {
        for (uint16 skill : { SKILL_HERBALISM, SKILL_MINING, SKILL_SKINNING })
            for (uint32 gathered : GatheredReagentsOf(skill))
                if (gathered == item)
                    return true;
        return false;
    }

    /// Item name for the report, or "?" when the template is gone.
    static std::string ItemNameFor(uint32 item)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
        return proto ? std::string(proto->Name1) : std::string("?");
    }

    struct CraftPlan
    {
        CraftRecipe const* recipe;
        SpellInfo const* spell;
        uint16 skill;
    };

    // Triggered casts skip the core's CREATE_ITEM space check. Do it before the
    // spell consumes any reagents, even when the previous batch fitted.
    static bool CanStoreCraftOutput(Player* bot, CraftPlan const& plan)
    {
        ItemTemplate const* item = sObjectMgr->GetItemTemplate(plan.recipe->product);
        if (!item)
            return false;
        for (auto const& effect : plan.spell->Effects)
        {
            if (effect.Effect != SPELL_EFFECT_CREATE_ITEM || uint32(effect.ItemType) != plan.recipe->product)
                continue;
            uint32 const count = std::clamp<uint32>(effect.CalcValue(), 1u, item->GetMaxStackSize());
            ItemPosCountVec dest;
            if (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, plan.recipe->product, count) != EQUIP_ERR_OK)
                return false;
            return true;
        }
        return false;
    }

    // Combat-useful crafts outrank raw stock when the interval budget can only
    // fund one: a potion feeds the fight and a weapon stone edges it, while a
    // smelted bar is only ever an intermediate for the next craft.
    static uint8 CraftLanePriority(uint16 skill)
    {
        if (skill == SKILL_ALCHEMY)
            return 0;
        if (skill == SKILL_BLACKSMITHING || skill == SKILL_LEATHERWORKING)
            return 1;
        return 2;
    }

    // Is a reagent the craft may be supplied with beyond what the companion
    // gathered? The shop carries only the listed stock - unless the profession
    // mode says the shopping trip is a stand-in entirely (ProfessionNoGather 2).
    static bool CraftReagentGrantable(Player* bot, uint32 item, uint32 missing)
    {
        if (g_cfg.professionNoGather < 2 && !IsCraftVendorReagent(item))
            return false;

        ItemPosCountVec dest;
        return bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, item, missing) == EQUIP_ERR_OK;
    }

    // Prefer the best eligible recipe with raw materials and bag room. If none
    // can run, keep the best eligible recipe so a manual request explains why.
    static std::vector<CraftPlan> PlanCrafts(Player* bot, uint16 profession)
    {
        std::vector<CraftPlan> plans;
        for (CraftLane const& lane : CraftLanes(profession))
        {
            uint16 const attained = bot->GetPureSkillValue(lane.skill);
            CraftPlan chosen = { nullptr, nullptr, 0 };
            for (CraftRecipe const& recipe : lane.recipes)
            {
                if (attained < recipe.requiredSkill || !bot->HasSpell(recipe.spell))
                    continue;
                SpellInfo const* spell = sSpellMgr->GetSpellInfo(recipe.spell);
                if (!spell)
                    continue;
                CraftPlan const candidate = { &recipe, spell, lane.skill };
                if (!chosen.recipe)
                    chosen = candidate;

                bool ready = CanStoreCraftOutput(bot, candidate);
                for (uint8 r = 0; r < MAX_SPELL_REAGENTS && ready; ++r)
                {
                    uint32 const item = uint32(spell->Reagent[r]);
                    uint32 const need = spell->ReagentCount[r];
                    if (!item || !need)
                        continue;
                    uint32 const have = bot->GetItemCount(item);
                    if (have >= need)
                        continue;
                    ready = CraftReagentGrantable(bot, item, need - have);
                }
                if (ready)
                {
                    chosen = candidate;
                    break;
                }
            }
            if (chosen.recipe)
                plans.push_back(chosen);
        }

        if (g_cfg.produceCombatFirst)
            std::stable_sort(plans.begin(), plans.end(), [](CraftPlan const& a, CraftPlan const& b)
            {
                return CraftLanePriority(a.skill) < CraftLanePriority(b.skill);
            });
        return plans;
    }

    static std::map<uint32, uint32> CraftReagentRequirements(SpellInfo const* spell)
    {
        std::map<uint32, uint32> reagents;
        for (uint8 r = 0; r < MAX_SPELL_REAGENTS; ++r)
            if (spell->Reagent[r] > 0 && spell->ReagentCount[r])
                reagents[uint32(spell->Reagent[r])] += spell->ReagentCount[r];
        return reagents;
    }

    // The core consumes by entry, not by stack. Leave all quest-related units
    // alone (including completed-but-unrewarded objectives and quest source items).
    static uint32 UsableCraftReagentCount(Player* bot, Player* owner, uint32 entry)
    {
        return CanReleaseTemplate(bot, owner, sObjectMgr->GetItemTemplate(entry)) ? bot->GetItemCount(entry) : 0;
    }

    /// The quiet craft (and the manual Produce report). `maxItems` caps the items a
    /// single call may create across every product - the interval habit passes the
    /// configured budget; the manual button passes 0, an explicit ask needs no cap.
    static std::vector<std::string> ProduceForCompanion(Player* bot, Player* owner, uint32 maxItems = 0)
    {
        std::vector<std::string> report;
        uint16 const profession = ClassProfessionPair(bot->getClass()).second;
        uint32 made = 0;
        for (CraftPlan const& plan : PlanCrafts(bot, profession))
        {
            if (maxItems && made >= maxItems)
                break;

            uint32 const product = plan.recipe->product;
            std::string missing;
            auto noteMissing = [&](uint32 item, uint32 have, uint32 need)
            {
                if (!missing.empty())
                    missing += ", ";
                missing += ItemNameFor(item) + " " + std::to_string(have) + "/" + std::to_string(need);
            };
            auto const reagents = CraftReagentRequirements(plan.spell);
            for (auto const& [item, need] : reagents)
            {
                uint32 const have = UsableCraftReagentCount(bot, owner, item);
                bool const grantable = IsCraftVendorReagent(item) || g_cfg.professionNoGather >= 2;
                if (have < need && (!grantable ||
                    !CanReleaseTemplate(bot, owner, sObjectMgr->GetItemTemplate(item))))
                    noteMissing(item, have, need);
            }
            if (!missing.empty())
            {
                if (g_cfg.professionNoGather == 1 && g_cfg.debug >= 2)
                    LOG_INFO("server.loading",
                             "[CompanionQuestSync] {} would grant the missing reagents for {} (ProfessionNoGather = 1: logging only)",
                             bot->GetName(), ItemNameFor(product));
                report.push_back("I need " + missing + " for " + ItemNameFor(product));
                continue;
            }

            if (!CanStoreCraftOutput(bot, plan))
            {
                report.push_back("I need bag room for " + ItemNameFor(product));
                continue;
            }

            // Only the explicit shop supplies may be topped up - or, in no-gather
            // mode, every reagent the recipe asks: the shopping trip is a stand-in.
            // The core then consumes the reagents even though the craft runs
            // immediately.
            for (auto const& [item, need] : reagents)
            {
                if (!IsCraftVendorReagent(item) && g_cfg.professionNoGather < 2)
                    continue;
                uint32 const have = UsableCraftReagentCount(bot, owner, item);
                if (have >= need)
                    continue;
                uint32 const give = need - have;
                ItemPosCountVec dest;
                if (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, item, give) == EQUIP_ERR_OK)
                    if (Item* bought = bot->StoreNewItem(dest, item, true))
                        bot->SendNewItem(bought, give, true, false, true);
                uint32 const restocked = UsableCraftReagentCount(bot, owner, item);
                if (restocked < need)
                    noteMissing(item, restocked, need);
            }
            if (!missing.empty())
            {
                report.push_back("I need bag room for " + missing + " before I can make " + ItemNameFor(product));
                continue;
            }

            uint32 const before = bot->GetItemCount(product);
            for (uint8 batch = 0; batch < 2; ++batch)
            {
                if (maxItems && made >= maxItems)
                    break;

                bool const ready = std::all_of(reagents.begin(), reagents.end(), [&](auto const& pair)
                {
                    auto const& [item, need] = pair;
                    return UsableCraftReagentCount(bot, owner, item) >= need;
                });
                if (!ready || !CanStoreCraftOutput(bot, plan))
                    break;

                uint32 const prior = bot->GetItemCount(product);
                bot->CastSpell(bot, plan.recipe->spell,
                    TriggerCastFlags(TRIGGERED_FULL_MASK & ~TRIGGERED_IGNORE_POWER_AND_REAGENT_COST));
                if (bot->GetItemCount(product) > prior)
                    made += bot->GetItemCount(product) - prior;
                else
                    break;
            }

            uint32 const after = bot->GetItemCount(product);
            if (after > before)
                report.push_back("I made " + std::to_string(after - before) + "x " + ItemNameFor(product));
            else
                report.push_back("I couldn't make " + ItemNameFor(product) + " just now; check my bag space and crafting station");
        }
        if (report.empty())
            report.push_back("I don't know a recipe I can make yet; check my profession skill");
        return report;
    }


    /// A free bag position for a split stack, packed the way SplitItem wants it.
    static uint16 FindFreeBagPos(Player* bot, ItemTemplate const* item, ItemPosCountVec const* occupied)
    {
        auto freeAfterCraft = [&](uint8 bag, uint8 slot)
        {
            uint16 const pos = uint16((bag << 8) | slot);
            return !bot->GetItemByPos(bag, slot) && (!occupied || std::none_of(occupied->begin(), occupied->end(),
                [pos](ItemPosCount const& destination) { return destination.pos == pos; }));
        };
        for (uint16 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
            if (freeAfterCraft(INVENTORY_SLOT_BAG_0, uint8(i)))
                return uint16((INVENTORY_SLOT_BAG_0 << 8) | i);
        for (uint16 i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; ++i)
            if (Bag* bag = bot->GetBagByPos(i); bag && ItemCanGoIntoBag(item, bag->GetTemplate()))
                for (uint8 j = 0; j < bag->GetBagSize(); ++j)
                    if (freeAfterCraft(uint8(i), j))
                        return uint16((i << 8) | j);
        return 0;
    }

    static uint16 FindFreeBagPos(Player* bot, Item const* item)
    {
        return FindFreeBagPos(bot, item->GetTemplate(), nullptr);
    }

#include "CuratedDelivery.inc"

    static TradeData* InitiateDeliveryTrade(Player* bot, Player* owner)
    {
        WorldPacket packet(CMSG_INITIATE_TRADE);
        packet << owner->GetGUID();
        bot->GetSession()->HandleInitiateTradeOpcode(packet);
        return bot->GetTradeData();
    }

    static uint32 OpenTradeSurplus(Player* bot, Player* owner, bool announce, bool curated, SupplyOrder supply = {},
        TradeData* opened = nullptr, uint32 nonce = 0)
    {
#ifndef AC_TRADE_OFFER_GUARD
        if (announce)
            SayToOwner(bot, owner, "I can't prepare a safe trade on this realm. Check with the realm administrator.");
        return 0;
#else
        if (!g_cfg.enable || !g_tableReady || !HasSupplyAuthority(bot, owner, false) ||
            !CanTradeSurplusNow(bot, owner) || bot->GetTradeData() != opened ||
            (opened ? (opened->GetTrader() != owner || opened->GetOfferToken() || !owner->GetTradeData() ||
                owner->GetTradeData()->GetTrader() != bot) : owner->GetTradeData() != nullptr))
            return 0;

        auto offers = PlanDelivery(bot, owner, curated, false, supply);
        if (supply.fresh)
        {
            uint32 count = 0;
            for (auto const& offer : offers)
                count += offer.second;
            if (count != supply.fresh)
                return 0;  // don't present part of a freshly crafted batch as the whole order
        }

        if (offers.empty())
        {
            if (announce && !supply.entry)
                SayToOwner(bot, owner, "I have nothing spare to hand you - I keep what I gather for the road.", 30);
            return 0;
        }

        TradeData* td = opened ? opened : InitiateDeliveryTrade(bot, owner);
        if (!td)
        {
            if (announce && !supply.entry)
                SayToOwner(bot, owner, "The trade did not open - try again in a moment.", 30);
            return 0;
        }

        // the offer is a WISH for now: the window takes one bot tick to come up, and
        // what is SetItem'd before that round-trip is lost to the display. The applier
        // finds the items by value two seconds later, fills the window, and only then
        // speaks - no pointers cross the tick, and nothing is split until a window
        // exists to put the split in.
        PendingTrade& pending = g_pendingTrade[bot->GetGUID().GetCounter()];
        pending = PendingTrade{};
        pending.ownerGuid = owner->GetGUID().GetCounter();
        pending.at = uint32(time(nullptr));
        pending.noticeId = ++g_nextTradeNoticeId;
        pending.announceFailure = announce;
        pending.curated = curated;
        pending.automatic = !announce;
        pending.supply = supply;
        pending.nonce = nonce;
        pending.offer = std::move(offers);

        uint32 const botGuid = bot->GetGUID().GetCounter();
        uint64 const noticeId = pending.noticeId;
        td->SetOfferGuard(noticeId, [botGuid, noticeId](Player* source, Player* target, TradeData const* trade)
        {
            auto const it = g_pendingTrade.find(botGuid);
            return g_cfg.enable && g_tableReady && it != g_pendingTrade.end() &&
                   it->second.noticeId == noticeId && it->second.applied && IsDeliveryEnabled(it->second) &&
                   source->GetGUID().GetCounter() == botGuid &&
                   target->GetGUID().GetCounter() == it->second.ownerGuid &&
                   CanTradeSurplusNow(source, target) &&
                   HasSupplyAuthority(source, target, true) &&
                   (!it->second.nonce || uint32(time(nullptr)) - it->second.at < Timing::NamedOfferTimeout) &&
                   (!it->second.automatic || (IsTownDeliveryStop(target, source) &&
                    uint32(time(nullptr)) - it->second.at < Timing::TownOfferTimeout)) &&
                   MatchesTradeOffer(source, trade, it->second);
        });

#ifdef AC_TRADE_OFFER_COMMIT
        if (nonce)
        {
            uint32 const ownerGuid = pending.ownerGuid;
            td->SetOfferCommit([botGuid, noticeId, ownerGuid, nonce](bool transferred, std::shared_future<bool> saved)
            {
                auto const it = g_pendingTrade.find(botGuid);
                if (it == g_pendingTrade.end() || it->second.noticeId != noticeId)
                    return;
                g_deliveryReceipts.emplace(noticeId, DeliveryReceipt{ownerGuid, nonce,
                    std::chrono::steady_clock::now() + std::chrono::seconds(25), transferred, std::move(saved)});
                g_pendingTrade.erase(it);
            });
        }
#endif
        return uint32(pending.offer.size());
#endif
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

    static bool RepairNearVendor(Player* bot, uint32 now)
    {
        uint32& last = g_lastRepair[bot->GetGUID().GetCounter()];
        if (last && now - last < 300)
            return false;

        std::list<Unit*> units;
        float const range = INTERACTION_DISTANCE;
        Acore::AnyUnitInObjectRangeCheck check(bot, range);
        Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(bot, units, check);
        Cell::VisitObjects(bot, searcher, range);
        Creature* repairer = nullptr;
        for (Unit* unit : units)
            if (Creature* c = unit->ToCreature(); c && c->IsAlive() &&
                bot->GetNPCIfCanInteractWith(c->GetGUID(), UNIT_NPC_FLAG_REPAIR))
            {
                if (!repairer || std::make_tuple(bot->GetDistance(c), c->GetGUID()) <
                    std::make_tuple(bot->GetDistance(repairer), repairer->GetGUID()))
                    repairer = c;
            }
        if (!repairer)
            return false;

        last = now;
        uint32 const before = bot->GetMoney();
        WorldPacket request(CMSG_REPAIR_ITEM);
        request << repairer->GetGUID() << ObjectGuid::Empty << uint8(0); // no guild funds
        bot->GetSession()->HandleRepairItemOpcode(request);
        return bot->GetMoney() < before; // report only a repair we saw the bot pay for
    }

    /// Near a trade or repair NPC the module stands in for the shopping trip a bot
    /// cannot walk: the reagents its known recipes ask that no node yields are
    /// topped up to a small stock (the same stand-in Produce makes when it crafts),
    /// and a repairer's hammer mends what the day's work dented, paying the same
    /// price a player would. One line says what happened; the restock runs at most
    /// once every five minutes per companion, so standing at the vendor is quiet.
    void RestockNearVendor(Player* owner, Player* bot)
    {
        if (!g_cfg.vendorRestock || !bot->IsInWorld() || !bot->IsAlive() || !bot->GetSession() || bot->GetTradeData())
            return;

        uint32 const botGuid = bot->GetGUID().GetCounter();
        uint32 const now = uint32(time(nullptr));
        bool const repaired = RepairNearVendor(bot, now);
        uint32& last = g_lastVendorRestock[botGuid];
        std::string topped;
        // A pack that is already tight buys nothing: a player would not restock into a full
        // bag. The trim runs just before this call, so there is space by the next pass.
        if (bot->GetFreeInventorySpace() < CompanionBagComfort)
            return;
        if ((!last || now - last >= 300) && VendorNearby(bot, 40.0f))
        {
            last = now;
            uint16 const profession = ClassProfessionPair(bot->getClass()).second;
            std::set<uint32> reagents;
            for (CraftPlan const& plan : PlanCrafts(bot, profession))
                for (uint8 r = 0; r < MAX_SPELL_REAGENTS; ++r)
                {
                    uint32 const item = uint32(plan.spell->Reagent[r]);
                    if (IsCraftVendorReagent(item) && CanReleaseTemplate(bot, owner, sObjectMgr->GetItemTemplate(item)))
                        reagents.insert(item);
                }

            for (uint32 const item : reagents)
            {
                uint32 const want = CompanionReagentKeep;
                uint32 const have = bot->GetItemCount(item);
                if (have >= want)
                    continue;
                uint32 const give = std::min<uint32>(want - have, 10);
                ItemPosCountVec dest;
                if (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, item, give) != EQUIP_ERR_OK)
                    continue;
                uint32 newSlots = 0;
                for (auto const& slot : dest)
                    if (!bot->GetItemByPos(slot.pos))
                        ++newSlots;
                if (newSlots > 0 && bot->GetFreeInventorySpace() < CompanionBagReserve + newSlots)
                    continue;
                if (Item* bought = bot->StoreNewItem(dest, item, true))
                {
                    bot->SendNewItem(bought, give, true, false, true);
                    if (!topped.empty())
                        topped += ", ";
                    topped += std::to_string(give) + "x " + ItemNameFor(item);
                }
            }
        }

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
        if (!g_cfg.gatherCatchUp || !owner || !bot->IsInWorld() || !bot->IsAlive())
            return;

        // a no-gather companion already sits at its level's cap; the land asks
        // nothing of it and the catch-up's whisper would be a lie
        if (g_cfg.professionNoGather >= 2)
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

        // in no-gather mode nothing is ever picked up, so no tool is owed
        if (g_cfg.professionNoGather >= 2)
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

    constexpr uint32 kStarterBagItem = 51809; // Portable Hole, 24 general-purpose slots

    bool ConsumeStarterBags(std::string const& identity)
    {
        // DirectExecute does not return the affected row count. Read back this
        // attempt's unique token, not merely a "consumed" bit another call could set.
        QueryResult id = CharacterDatabase.Query("SELECT UUID()");
        if (!id)
            return false;
        std::string const token = id->Fetch()[0].Get<std::string>();
        std::string const match = fmt::format(" AND x.starter_bag_attempt = '{}'", token);
        CharacterDatabase.DirectExecute(CurrentCustodyClause(true, fmt::format("SET x.starter_bag_attempt = '{}' ", token)) +
            identity + " AND x.starter_bag_attempt = ''");
        return static_cast<bool>(CharacterDatabase.Query("SELECT 1" + CurrentCustodyClause(true) + identity + match));
    }

    // True means do not run the ordinary bag fallback on this visit (pending,
    // attempted, or authority uncertain). Both verified summon and upkeep can
    // be the first to see a controlled session, including after a restart.
    bool HandleStarterBags(Player* owner, Player* bot)
    {
#ifdef MOD_PLAYERBOTS_INITIALIZATION_POLICY_V1
        if (!g_starterBagsReady)
            return false;
        if (!IsControlledCompanion(bot, owner))
            return true;
        uint32 const botGuid = bot->GetGUID().GetCounter();
        std::string name = bot->GetName();
        CharacterDatabase.EscapeString(name);
        std::string const identity = fmt::format(
            " AND b.owner_guid = {} AND b.bot_guid = {} AND c.account = {} AND BINARY c.name = '{}'",
            owner->GetGUID().GetCounter(), botGuid, bot->GetSession()->GetAccountId(), name);
        QueryResult row = CharacterDatabase.Query("SELECT x.starter_bag_attempt" + CurrentCustodyClause(true) + identity);
        if (!row)
            return true;
        if (row->Fetch()[0].IsNull() || !row->Fetch()[0].Get<std::string_view>().empty())
            return false;

        // Spend first, including when every slot was already occupied. A crash
        // between this write and the inventory save can lose bags, never reissue them.
        if (!ConsumeStarterBags(identity))
            return true;

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(kStarterBagItem);
        if (!proto || proto->Class != ITEM_CLASS_CONTAINER || proto->SubClass != ITEM_SUBCLASS_CONTAINER ||
            proto->ContainerSlots != 24 || proto->BagFamily != 0)
        {
            LOG_ERROR("server.loading", "[CompanionQuestSync] starter bags spent for {} but item {} is not a standard 24-slot bag",
                      botGuid, kStarterBagItem);
            return true;
        }

        PlayerbotFactory factory(bot, bot->GetLevel());
        uint32 empty = 0;
        uint32 given = 0;
        for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END; ++slot)
        {
            if (bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                continue;
            ++empty;
            if (factory.EquipEmptyBagSlot(slot, kStarterBagItem))
                ++given;
        }
        if (given)
        {
            auto trans = CharacterDatabase.BeginTransaction();
            bot->SaveInventoryAndGoldToDB(trans);
            CharacterDatabase.CommitTransaction(trans);
        }
        if (given != empty)
            LOG_ERROR("server.loading", "[CompanionQuestSync] starter bags spent for {} ({}): equipped {} of {}",
                      bot->GetName(), botGuid, given, empty);
        return true;
#else
        return false;
#endif
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
        if (HandleStarterBags(owner, bot) || !g_cfg.extraBags || !g_cfg.extraBagItem || g_cfg.extraBagItem == kStarterBagItem)
            return;

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(g_cfg.extraBagItem);
        if (!proto || proto->Class != ITEM_CLASS_CONTAINER || !proto->ContainerSlots)
            return;

        uint32 given = 0;

        for (uint8 i = 0; i < g_cfg.extraBags && i < (INVENTORY_SLOT_BAG_END - INVENTORY_SLOT_BAG_START); ++i)
        {
            uint8 const slot = INVENTORY_SLOT_BAG_START + i;
            if (bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                continue;                                   // already has one

            // Never store an intermediate item or write to a different/occupied slot.
            uint16 dest = 0;
            uint16 const expected = (uint16(INVENTORY_SLOT_BAG_0) << 8) | slot;
            if (bot->CanEquipNewItem(slot, dest, g_cfg.extraBagItem, false) == EQUIP_ERR_OK && dest == expected &&
                !bot->GetItemByPos(expected) && bot->EquipNewItem(dest, g_cfg.extraBagItem, true))
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
    /// Choose the native reward on behalf of a companion at a live quest ender.
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


    void LogHandInWait(Player* owner, Player* bot, Quest const* quest, EnderSpot const& spot,
                       float distance, char const* reason)
    {
        if (g_cfg.debug < 2)
            return;

        uint32 const questId = quest->GetQuestId();
        LOG_INFO("server.loading",
                 "[CompanionQuestSync] hand-in waiting: {} quest '{}' ({}) at {} ({}) {:.0f} yd; reason={}, bot_status={}, owner_status={}, owner_rewarded={}",
                 bot->GetName(), quest->GetTitle(), questId, spot.name, spot.entry, distance, reason,
                 uint32(bot->GetQuestStatus(questId)), owner ? int32(owner->GetQuestStatus(questId)) : -1,
                 owner && owner->IsQuestRewarded(questId));
    }

    WorldObject* FindInteractableQuestEnder(Player* bot, uint32 questId)
    {
        // Spawn records are directions, never authorization. Check every loaded
        // candidate in reach, including moving/temporary creatures and quest GOs.
        float constexpr radius = 20.0f;
        auto valid = [bot, questId](WorldObject* candidate)
        {
            GameObject* object = candidate->ToGameObject();
            if (object && (!object->isSpawned() || object->HasGameObjectFlag(GO_FLAG_NOT_SELECTABLE)))
                return false;
            if (!candidate->hasInvolvedQuest(questId) || !bot->CanInteractWithQuestGiver(candidate))
                return false;
            // The client uses per-player quest activation for conditional questgivers.
            return !object || !object->HasGameObjectFlag(GO_FLAG_INTERACT_COND) || object->ActivateToQuest(bot);
        };
        std::list<Creature*> creatures;
        Acore::CreatureListSearcher<decltype(valid)> creatureSearcher(bot, creatures, valid);
        Cell::VisitObjects(bot, creatureSearcher, radius);
        std::list<GameObject*> objects;
        Acore::GameObjectListSearcher<decltype(valid)> objectSearcher(bot, objects, valid);
        Cell::VisitObjects(bot, objectSearcher, radius);

        WorldObject* nearest = nullptr;
        auto consider = [&](WorldObject* candidate)
        {
            if (!nearest || bot->GetDistance(candidate) < bot->GetDistance(nearest))
                nearest = candidate;
        };
        for (Creature* creature : creatures)
            consider(creature);
        for (GameObject* object : objects)
            consider(object);
        return nearest;
    }

    EnderSpot DescribeQuestEnder(WorldObject* giver)
    {
        return { true, giver->GetEntry(), giver->GetName(), giver->GetPositionX(), giver->GetPositionY(),
                 giver->GetPositionZ(), giver->GetMapId(), giver->GetZoneId(), giver->GetPhaseMask(), giver->IsGameObject() };
    }

    WorldObject* FindReadyQuestEnder(Player* bot, Quest const* quest)
    {
        if (!quest || bot->GetQuestStatus(quest->GetQuestId()) != QUEST_STATUS_COMPLETE)
            return nullptr;
        WorldObject* giver = FindInteractableQuestEnder(bot, quest->GetQuestId());
        if (!giver || !bot->CanRewardQuest(quest, ChooseRewardIndex(bot, quest), false))
            return nullptr;
        return giver;
    }

    void DeliverHandIns(Player* owner, Player* bot)
    {
        if (!IsControlledCompanion(bot, owner) || !bot->IsAlive() || bot->IsInCombat() || bot->IsInFlight() ||
            bot->IsBeingTeleported() || bot->GetTransport() || bot->GetVehicle() || owner->IsInCombat() ||
            g_cfg.handInMode == 0)
            return;

        uint32 const nowTs = static_cast<uint32>(time(nullptr));
        uint32 const botGuid = bot->GetGUID().GetCounter();
        uint32 const ownerGuid = owner->GetGUID().GetCounter();

        // Newly-ready invitations, staged and flushed at the end of the pass: the
        // closest quest on the companion's own map speaks first, not whichever quest
        // happened to sit in the first log slot.
        struct Invite { uint32 dist; bool sameZone; Quest const* quest; };
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

            WorldObject* giver = FindInteractableQuestEnder(bot, questId);
            EnderSpot const spot = giver ? DescribeQuestEnder(giver) : FindQuestEnder(bot, questId);
            if (!spot.found || spot.map != bot->GetMapId())
                continue;

            float const dist = bot->GetDistance(spot.x, spot.y, spot.z);
            if (!giver && g_cfg.handInMode == 2 && dist > g_cfg.handInNear)
            {
                // The companion can finish the hand-in itself, but only where a player
                // could too: in reach of the giver, not across half the map. Beyond the
                // range it asks to be led; nothing is rewarded and nothing blinks.
                if (TakeHandInTravelHint(owner, questId, nowTs))
                {
                    LogHandInWait(owner, bot, quest, spot, dist, "out-of-range");
                    // Name the place and give map coordinates: the pin may be on another
                    // zone's map than the one the player is looking at.
                    std::string where = "at " + spot.name;
                    if (spot.zone)
                        where += " in " + ZoneName(spot.zone);
                    std::string const coords = spot.zone ? MapCoords(spot.x, spot.y, spot.zone) : std::string();
                    if (!coords.empty())
                        where += " (" + coords + ")";
                    SayToOwner(bot, owner, "I can hand in '" + quest->GetTitle() + "' " + where +
                               " myself. Lead me up to the giver and I will do the rest.", 90,
                               quest->GetQuestId(), true);
                }
                continue;
            }

            if (!giver)
            {
                // Static spawn coordinates are for navigation hints only. Neither
                // absence nor failed native interaction permits moving the bot.
                LogHandInWait(owner, bot, quest, spot, dist, "no-interactable-ender");
                continue;
            }

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
                bool const ownerAtGiver = owner->GetMap() == bot->GetMap() &&
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
                    invites.push_back({ uint32(dist), spot.zone == bot->GetZoneId(), quest });

                    continue;
                }

                if (!ownerAtGiver && nowTs - readyAt < g_cfg.handInDelaySeconds)
                    continue;
                // Deliberately NOT erased here. Clearing the marker before the reward
                // attempt made a blocked reward (full bags) re-announce "ready to hand
                // in" on every pass, forever. It is cleared once the quest leaves the log.
            }

            uint32& prodAt = g_rewardProdAt[{ botGuid, questId }];
            if (prodAt && nowTs - prodAt < Timing::RewardProd)
                continue;

            prodAt = nowTs;

            uint32 const reward = ChooseRewardIndex(bot, quest);

            if (!bot->CanRewardQuest(quest, reward, false))
            {
                LogHandInWait(owner, bot, quest, spot, dist, "reward-check");
                continue;
            }

            // Use the native reward handler for reward scripts, giver AI, and
            // auto-accepted follow-ups, not merely the low-level payout.
            // The sweep's cached roster is not authority to consume a quest.
            if (!IsControlledCompanion(bot, owner) || !HasCurrentCustody(ownerGuid, botGuid, true))
                return;
            WorldPacket request(CMSG_QUESTGIVER_CHOOSE_REWARD);
            request << giver->GetGUID() << questId << reward;
            bot->GetSession()->HandleQuestgiverChooseRewardOpcode(request);
            if (bot->FindQuestSlot(questId) < MAX_QUEST_LOG_SIZE)
                continue;

            g_readyAt.erase(readyKey);   // handed in: the invitation has served its purpose
            g_readyAt.erase((uint64(botGuid) << 32) | questId);
            ++g_session[ownerGuid].handedIn;

            if (g_cfg.debug >= 1)
                LOG_INFO("server.loading",
                         "[CompanionQuestSync] {} handed in '{}' ({}) server-side (giver {}, {:.0f} yd)",
                         bot->GetName(), quest->GetTitle(), questId,
                         spot.name, dist);

            // The player should never wonder who chose the reward: when the quest offers
            // one, the companion says what it kept.
            if (quest->GetRewChoiceItemsCount() > 1)
            {
                if (ItemTemplate const* kept = sObjectMgr->GetItemTemplate(quest->RewardChoiceItemId[reward]))
                    SayToOwner(bot, owner, "For '" + quest->GetTitle() + "' I kept the " +
                               std::string(kept->Name1) + " - the choice was mine to make.", 90);
            }
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
            WorldObject* giver = FindReadyQuestEnder(bot, invite.quest);
            if (!giver)
                continue;
            SayToOwner(bot, owner,
                       "I am ready to hand in '" + invite.quest->GetTitle() + "' at " +
                       giver->GetName() + " - about " +
                       std::to_string(uint32(bot->GetDistance(giver))) + " yards from me. Want to come?", 45,
                       invite.quest->GetQuestId(), true, 0, giver->GetGUID());
        }
    }


    bool CanManageBags(Player* owner, Player* bot)
    {
        return owner && bot && owner->IsInWorld() && bot->IsInWorld() &&
               owner->IsAlive() && bot->IsAlive() && bot->GetSession() &&
               !owner->IsInCombat() && !bot->IsInCombat() &&
               !owner->GetTradeData() && !bot->GetTradeData() &&
               !g_pendingTrade.count(bot->GetGUID().GetCounter());
    }

#if defined(CQ_HAS_COMPANION_LOOT_POLICY) && defined(MOD_PLAYERBOTS)
    bool ReserveCompanionLootSlot(Player* bot, uint32 entry)
    {

        if (!g_cfg.enable || !g_cfg.sellJunk || !bot->GetSession() || !bot->GetSession()->IsBot() ||
            bot->GetFreeInventorySpace() > CompanionBagReserve)
            return false;

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
        // Restrict intake to the explicit junk class, not grey equipment or trade goods.
        if (!proto || proto->Quality != ITEM_QUALITY_POOR || !proto->SellPrice ||
            proto->Class != ITEM_CLASS_MISC || proto->SubClass != ITEM_SUBCLASS_JUNK)
            return false;

        PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
        Player* owner = ai ? ai->GetMaster() : nullptr;
        if (!IsControlledCompanion(bot, owner) || !owner->IsInWorld() ||
            !bot->GetGroup() || bot->GetGroup() != owner->GetGroup() || !CanReleaseTemplate(bot, owner, proto))
            return false;

        auto* context = ai->GetAiObjectContext();
        if (context->GetValue<std::set<uint32>&>("always loot list")->Get().count(entry) ||
            context->GetValue<ItemUsage>("item usage", entry)->Get() != ITEM_USAGE_VENDOR)
            return false;

        return HasCurrentCustody(owner->GetGUID().GetCounter(), bot->GetGUID().GetCounter(), true);
    }
#endif

    uint32 SellManagedJunk(Player* owner, Player* bot)
    {
        if (!g_cfg.sellJunk || !CanManageBags(owner, bot) || bot->isMoving())
            return 0;
        uint32 const now = uint32(time(nullptr));
        uint32& last = g_sellAt[bot->GetGUID().GetCounter()];
        if (last && now - last < Timing::SellJunk)
            return 0;
        std::list<Unit*> units;
        Acore::AnyUnitInObjectRangeCheck check(bot, INTERACTION_DISTANCE);
        Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(bot, units, check);
        Cell::VisitObjects(bot, searcher, INTERACTION_DISTANCE);
        Creature* vendor = nullptr;
        for (Unit* unit : units)
            if ((vendor = bot->GetNPCIfCanInteractWith(unit->GetGUID(), UNIT_NPC_FLAG_VENDOR)) &&
                !vendor->HasFlagsExtra(CREATURE_FLAG_EXTRA_NO_SELL_VENDOR))
                break;
            else
                vendor = nullptr;
        if (!vendor)
            return 0;
        last = now;

        uint32 const botGuid = bot->GetGUID().GetCounter();
        uint32 sold = 0;
        uint32 displacedSold = 0;
        VisitCarriedItems(bot, [&](Item* item)
        {
            ItemTemplate const* proto = item->GetTemplate();
            // A piece this module's own re-gear put down is sold whatever its quality: it is gear
            // the companion has already replaced, and it is here because an offhand came back off
            // the arm rather than being destroyed. Ordinary junk keeps its stricter tests.
            bool const displaced = IsStaleGear(botGuid, item);
            if (!proto || !proto->SellPrice || item->IsInTrade() || item->IsRefundable() ||
                !CanReleaseItem(bot, owner, item))
                return;
            if (!displaced)
            {
                if (proto->Quality != ITEM_QUALITY_POOR)
                    return;
#ifdef MOD_PLAYERBOTS
                PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
                if (!ai || ai->GetAiObjectContext()->GetValue<ItemUsage>("item usage", proto->ItemId)->Get() != ITEM_USAGE_VENDOR)
                    return;
#else
                return;
#endif
            }
            auto const guid = item->GetGUID();
            uint16 const pos = item->GetPos();
            WorldPackets::Item::SellItem request{ WorldPacket(CMSG_SELL_ITEM) };
            request.VendorGuid = vendor->GetGUID();
            request.ItemGuid = guid;
            bot->GetSession()->HandleSellItemOpcode(request);
            Item* remaining = bot->GetItemByPos(pos);
            if (!remaining || remaining->GetGUID() != guid)
            {
                ++sold;
                if (displaced)
                {
                    ++displacedSold;
                    g_staleGear[botGuid].erase(guid.GetRawValue());
                }
            }
        });
        if (sold && g_cfg.debug >= 1)
            LOG_INFO("server.loading", "[CompanionQuestSync] {} sold {} confirmed junk stack(s) to {}{}",
                     bot->GetName(), sold, vendor->GetName(),
                     displacedSold ? fmt::format(" ({} displaced by a re-gear)", displacedSold) : "");
        if (sold)
            g_session[owner->GetGUID().GetCounter()].sold += sold;
        return sold;
    }

    uint32 MailHome(Player* owner, Player* bot, ObjectGuid mailbox, std::vector<Item*>& items);  // defined below
    ObjectGuid MailboxNearby(Player* bot);                                                    // defined below

    /// Sell the *spare* trade goods when the bags are tight.
    ///
    /// Restocking and gathering made a companion's pack a warehouse: every recipe the bot
    /// knows contributed a stack, and nothing ever left. Selling the greys did not help,
    /// because the hoard was trade goods, which the professions genuinely need. The budget
    /// below is what a craft consumes (one recipe of a bought reagent, two stacks of a
    /// material), so the trim frees slots without taking the profession's supply with it.
    /// It runs only below CompanionBagComfort, so a companion with room is never sold from,
    /// and never touches quest items, containers, bound gear or anything the player is
    /// about to trade.
    uint32 SellBagSurplus(Player* owner, Player* bot)
    {
        if (!g_cfg.trimSurplus || !CanManageBags(owner, bot) || bot->isMoving() || bot->IsInCombat())
            return 0;
        // A pack with room is left alone: this is a bag rescue, not an economy pass.
        if (bot->GetFreeInventorySpace() >= CompanionBagComfort)
            return 0;

        uint32 const now = uint32(time(nullptr));
        uint32& last = g_trimAt[bot->GetGUID().GetCounter()];
        if (last && now - last < 300)
            return 0;

        uint8 const level = bot->GetLevel();
        std::set<uint32> const tools = ProfessionToolItems(bot->getClass());

        // What a real player keeps: one recipe of a bought reagent, two stacks of a
        // material, two stacks of a consumable, and no loose bags or outgrown gear.
        std::map<uint32, std::vector<Item*>> carried;
        std::vector<Item*> spare;            // consumables, spare containers, outgrown gear
        VisitCarriedItems(bot, [&](Item* item)
        {
            ItemTemplate const* proto = item->GetTemplate();
            if (!proto || !proto->SellPrice)
                return;
            if (item->IsInTrade() || item->IsRefundable())
                return;

            // A loose bag is worth nothing: four are worn and CanReleaseItem refuses
            // containers by design, so this rule owns the class itself. Only an EMPTY
            // bag is spare - one holding things is carrying them for the companion.
            if (proto->Class == ITEM_CLASS_CONTAINER && proto->SubClass == ITEM_SUBCLASS_CONTAINER)
            {
                Bag* bag = item->ToBag();
                if (!bag || bag->IsEmpty())
                    spare.push_back(item);
                return;
            }

            if (!CanReleaseItem(bot, owner, item))
                return;

            if (proto->Class == ITEM_CLASS_TRADE_GOODS)
            {
                if (item->IsSoulBound())     // the companion's own kit, never surplus
                    return;
                carried[proto->ItemId].push_back(item);
                return;
            }
            // Consumables stack up quietly; two stacks is what a player carries.
            if (proto->Class == ITEM_CLASS_CONSUMABLE && !item->IsSoulBound())
            {
                spare.push_back(item);
                return;
            }
            // Gear it has outgrown. Playerbots auto-stores the piece it replaces, so every
            // upgrade would otherwise leave the old armour in the pack for good.
            if (proto->Class != ITEM_CLASS_WEAPON && proto->Class != ITEM_CLASS_ARMOR)
                return;
            if (proto->Quality > ITEM_QUALITY_UNCOMMON)
                return;                      // a lucky blue is not bag filler
            if (tools.count(item->GetEntry()))
                return;                      // the pick and the knife are never outgrown
            if (uint32(proto->RequiredLevel) + 2 >= level)
                return;                      // still this level's gear, or the owner's gift
            spare.push_back(item);
        });

        // Trade goods over budget, keeping the fullest stacks so the budget costs few slots.
        std::vector<Item*> spareGoods;
        for (auto& [entry, stacks] : carried)
        {
            // In no-gather mode the gathering half of the pair is retired: raw stock
            // is dead weight, so none of it is kept - the vendor (or the mail) takes
            // the lot when the bags get tight, like any other displaced item.
            uint32 const keep = IsCraftVendorReagent(entry) ? CompanionReagentKeep
                : (g_cfg.professionNoGather >= 2 && IsGatheredStock(entry) ? 0 : CompanionMatKeep);
            std::sort(stacks.begin(), stacks.end(),
                [](Item* a, Item* b) { return a->GetCount() > b->GetCount(); });
            uint32 kept = 0;
            for (Item* item : stacks)
            {
                if (kept < keep)
                {
                    kept += item->GetCount();
                    continue;
                }
                spareGoods.push_back(item);
            }
        }

        // Two stacks of a consumable, not the eight that a vendor restock leaves behind.
        std::map<uint32, std::vector<Item*>> consumables;
        for (Item* item : spare)
            if (item->GetTemplate()->Class == ITEM_CLASS_CONSUMABLE)
                consumables[item->GetTemplate()->ItemId].push_back(item);
        std::vector<Item*> spareConsumables;
        for (auto& [entry, stacks] : consumables)
        {
            std::sort(stacks.begin(), stacks.end(),
                [](Item* a, Item* b) { return a->GetCount() > b->GetCount(); });
            uint32 keptStacks = 0;
            for (Item* item : stacks)
                if (keptStacks++ >= CompanionConsumableStacks)
                    spareConsumables.push_back(item);
        }
        spare.erase(std::remove_if(spare.begin(), spare.end(), [&](Item* item)
        {
            ItemTemplate const* proto = item->GetTemplate();
            return proto->Class == ITEM_CLASS_CONSUMABLE &&
                   std::find(spareConsumables.begin(), spareConsumables.end(), item) != spareConsumables.end();
        }), spare.end());

        if (spareGoods.empty() && spare.empty())
            return 0;

        // Sell what a merchant will take; without one, post it home like a player would.
        std::list<Unit*> units;
        Acore::AnyUnitInObjectRangeCheck check(bot, INTERACTION_DISTANCE);
        Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(bot, units, check);
        Cell::VisitObjects(bot, searcher, INTERACTION_DISTANCE);
        Creature* vendor = nullptr;
        for (Unit* unit : units)
            if ((vendor = bot->GetNPCIfCanInteractWith(unit->GetGUID(), UNIT_NPC_FLAG_VENDOR)) &&
                !vendor->HasFlagsExtra(CREATURE_FLAG_EXTRA_NO_SELL_VENDOR))
                break;
            else
                vendor = nullptr;

        if (!vendor)
        {
            ObjectGuid const mailbox = MailboxNearby(bot);
            if (!mailbox)
                return 0;

            std::vector<Item*> attachments;
            for (Item* item : spareGoods)
                if (item->CanBeTraded(true) && !item->IsSoulBound() && !item->IsRefundable())
                    attachments.push_back(item);
            for (Item* item : spare)
                if (item->CanBeTraded(true) && !item->IsSoulBound() && !item->IsRefundable() &&
                    item->GetTemplate()->Class != ITEM_CLASS_CONTAINER)
                    attachments.push_back(item);
            if (attachments.empty())
                return 0;

            // Every attempt is throttled, so a mailbox that refuses cannot be retried
            // once per pass. A letter carries twelve stacks; several letters are fine -
            // demanding a single small letter was why a full bag could never be rescued.
            last = now;
            constexpr size_t kMailAttachments = 12;
            uint32 posted = 0;
            for (size_t i = 0; i < attachments.size(); i += kMailAttachments)
            {
                std::vector<Item*> letter(attachments.begin() + i,
                    attachments.begin() + std::min(i + kMailAttachments, attachments.size()));
                if (!MailHome(owner, bot, mailbox, letter))
                    break;
                posted += uint32(letter.size());
            }
            if (!posted)
                return 0;
            if (auto itr = g_session.find(owner->GetGUID().GetCounter()); itr != g_session.end())
                itr->second.mailed += posted;
            if (g_cfg.debug >= 1)
                LOG_INFO("server.loading", "[CompanionQuestSync] {} posted {} spare stack(s) home (no merchant nearby)",
                    bot->GetName(), posted);
            SayToOwner(bot, owner,
                "My pack was nearly full and no merchant was at hand, so I posted " +
                std::to_string(posted) + " spare stack(s) to you - pick them up at the mailbox.", 30);
            return posted;
        }

        // Set before the first sale: a merchant that refuses every item must not be
        // re-searched on every pass for the same result.
        last = now;
        auto sell = [&](Item* item) -> bool
        {
            auto const guid = item->GetGUID();
            uint16 const pos = item->GetPos();
            WorldPackets::Item::SellItem request{ WorldPacket(CMSG_SELL_ITEM) };
            request.VendorGuid = vendor->GetGUID();
            request.ItemGuid = guid;
            bot->GetSession()->HandleSellItemOpcode(request);
            Item* remaining = bot->GetItemByPos(pos);
            return !remaining || remaining->GetGUID() != guid;
        };

        uint32 sold = 0;
        std::string names;
        for (Item* item : spareGoods)
        {
            std::string const name = item->GetTemplate()->Name1;
            if (!sell(item))
                continue;
            ++sold;
            if (names.find(name) == std::string::npos)
                names += (names.empty() ? "" : ", ") + name;
        }
        uint32 gearSold = 0;
        for (Item* item : spare)
            if (sell(item))
                ++gearSold;

        if (!sold && !gearSold)
            return 0;
        if (auto itr = g_session.find(owner->GetGUID().GetCounter()); itr != g_session.end())
            itr->second.sold += sold + gearSold;
        if (g_cfg.debug >= 1)
            LOG_INFO("server.loading", "[CompanionQuestSync] {} sold {} spare stack(s) and {} piece(s) to {} (free slots now {})",
                bot->GetName(), sold, gearSold, vendor->GetName(), bot->GetFreeInventorySpace());

        std::string said = "My pack was nearly full, so I sold ";
        if (sold)
            said += std::to_string(sold) + " spare stack(s) of " + names;
        if (sold && gearSold)
            said += " and ";
        if (gearSold)
            said += std::to_string(gearSold) + " piece(s) of gear and goods I had no use for";
        said += " at the vendor - I kept enough for our recipes.";
        SayToOwner(bot, owner, said, 30);
        return sold + gearSold;
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

    uint32 DescribeGearItemLevel(Player* bot);   // defined below, read before the re-gear

    /// Bring a companion's kit up to the level the module gave it.
    ///
    /// A pool body arrives as a level-1 starter, and raising its level left the gear behind:
    /// a level-14 companion was still swinging a Worn Battleaxe in Rusted Chain, because the
    /// dress pass only fills empty slots out of its own bag and `equip upgrade` only swaps
    /// what the bag happens to hold. Playerbots already knows how to dress a bot of a given
    /// level, so a kit that lags is rebuilt incrementally - slots are upgraded, nothing is
    /// re-rolled wholesale, and the bag, quest items and profession tools are untouched.
    // ------------------------------------------------------- the shape a role insists on
    //
    // The factory picks by score for the spec tab, which is the right answer for damage and
    // support - and the wrong one for the two roles that imply a shape rather than a number: a
    // tank holding a two-hander has given up its shield, and a healer holding a melee two-hander
    // has given up its caster weapon. Score cannot see that; a player does. So the pass is
    // followed by one check and, at most, one refit.

    enum class KitNeed
    {
        None,
        Shield,          // tank on a class that can carry one
        CasterWeapon     // healer whose hands are full of a melee two-hander
    };

    /// What the role asks of the kit's shape, as opposed to its score.
    KitNeed KitNeedFor(uint8 cls, std::string const& role)
    {
        if (role == "tank")
        {
            // a druid is a bear and a death knight has no shield slot: for them the tank shape
            // *is* the two-hander, so asking for a shield would fight the class
            bool const canShield = cls == CLASS_WARRIOR || cls == CLASS_PALADIN || cls == CLASS_SHAMAN;
            return canShield ? KitNeed::Shield : KitNeed::None;
        }
        if (role == "healer")
            return KitNeed::CasterWeapon;
        return KitNeed::None;
    }

    enum class KitPiece
    {
        Shield,
        OneHandedWeapon,     // what a shield needs in the other hand
        CasterOneHand
    };

    bool MatchesKitPiece(KitPiece piece, ItemTemplate const* proto)
    {
        if (!proto || proto->Class != (piece == KitPiece::Shield ? ITEM_CLASS_ARMOR : ITEM_CLASS_WEAPON) ||
            proto->ItemLevel < 5)
            return false;

        switch (piece)
        {
            case KitPiece::Shield:
                return proto->SubClass == ITEM_SUBCLASS_ARMOR_SHIELD;
            case KitPiece::OneHandedWeapon:
                return proto->InventoryType == INVTYPE_WEAPON &&
                       (proto->SubClass == ITEM_SUBCLASS_WEAPON_MACE || proto->SubClass == ITEM_SUBCLASS_WEAPON_SWORD ||
                        proto->SubClass == ITEM_SUBCLASS_WEAPON_AXE || proto->SubClass == ITEM_SUBCLASS_WEAPON_FIST);
            case KitPiece::CasterOneHand:
                return proto->InventoryType == INVTYPE_WEAPON &&
                       (proto->SubClass == ITEM_SUBCLASS_WEAPON_MACE || proto->SubClass == ITEM_SUBCLASS_WEAPON_DAGGER);
        }
        return false;
    }

    std::unordered_map<uint32, std::vector<uint32>> g_kitCandidates;   // piece * 100 + level band

    /// The best piece of its kind this body can use, from the template store.
    ///
    /// Scanned once per piece and level band: the store holds tens of thousands of templates, the
    /// answer barely moves inside five levels, and the cache is filtered again on the way out
    /// because a second companion may be a different class.
    uint32 FindKitItem(Player* bot, KitPiece piece)
    {
        uint8 const level = bot->GetLevel();
        uint32 const key = uint32(piece) * 100 + level / 5;

        auto const cached = g_kitCandidates.find(key);
        std::vector<uint32> const* ids = cached == g_kitCandidates.end() ? nullptr : &cached->second;

        std::vector<uint32> scanned;
        if (!ids)
        {
            for (ItemTemplate* proto : *sObjectMgr->GetItemTemplateStoreFast())
            {
                if (!proto || proto->RequiredLevel > level || !MatchesKitPiece(piece, proto))
                    continue;
                scanned.push_back(proto->ItemId);
            }

            // the best a level can actually reach first, then stable by id so the choice is the
            // same on every realm and every rerun
            std::sort(scanned.begin(), scanned.end(), [&](uint32 a, uint32 b)
            {
                ItemTemplate const* pa = sObjectMgr->GetItemTemplate(a);
                ItemTemplate const* pb = sObjectMgr->GetItemTemplate(b);
                if (!pa || !pb)
                    return a < b;
                if (pa->ItemLevel != pb->ItemLevel)
                    return pa->ItemLevel > pb->ItemLevel;
                return a < b;
            });

            g_kitCandidates[key] = std::move(scanned);
            ids = &g_kitCandidates[key];
        }

        for (uint32 const id : *ids)
            if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(id))
                if (bot->CanUseItem(proto) == EQUIP_ERR_OK)
                    return id;
        return 0;
    }

    /// The best piece of its kind the companion already carries.
    ///
    /// A shield looted five minutes ago is what the tank should be holding; handing it a fresh one
    /// instead would leave that drop to rot in the bags and give the companion gear it did not earn.
    Item* FindCarriedKitPiece(Player* bot, KitPiece piece)
    {
        Item* best = nullptr;
        if (!bot)
            return best;

        VisitCarriedItems(bot, [&](Item* item)
        {
            if (!item || item->IsEquipped() || item->IsInTrade() || item->IsRefundable())
                return;
            ItemTemplate const* proto = item->GetTemplate();
            if (!MatchesKitPiece(piece, proto) || bot->CanUseItem(proto) != EQUIP_ERR_OK)
                return;
            if (!best || proto->ItemLevel > best->GetTemplate()->ItemLevel)
                best = item;
        });
        return best;
    }

    bool EquipCarriedKitPiece(Player* bot, uint8 slot, Item* item)
    {
        uint16 dest = 0;
        if (bot->CanEquipItem(slot, dest, item, /*swap=*/true) != EQUIP_ERR_OK)
            return false;
        bot->SwapItem(item->GetPos(), dest);
        return bot->GetItemByPos(dest) == item;
    }

    bool EquipKitPiece(Player* bot, uint8 slot, uint32 itemId, std::vector<ObjectGuid>* generated = nullptr)
    {
        uint16 equipDest = 0;
        if (bot->CanEquipNewItem(slot, equipDest, itemId, /*swap=*/true) != EQUIP_ERR_OK)
            return false;
        ItemPosCountVec bagDest;
        if (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, bagDest, itemId, 1) != EQUIP_ERR_OK)
            return false;
        Item* item = bot->StoreNewItem(bagDest, itemId, true);
        if (item && generated)
            generated->push_back(item->GetGUID());
        // Outside a switch, a refused swap leaves the piece for the next carried-item attempt.
        return item && EquipCarriedKitPiece(bot, slot, item);
    }

    /// Fit the best piece of a kind this companion can wear: what it carries first, a new one only
    /// when it owns nothing suitable.
    bool FitKitPiece(Player* bot, uint8 slot, KitPiece piece, std::vector<ObjectGuid>* generated = nullptr)
    {
        if (Item* carried = FindCarriedKitPiece(bot, piece))
            return EquipCarriedKitPiece(bot, slot, carried);
        uint32 const fresh = FindKitItem(bot, piece);
        return fresh && EquipKitPiece(bot, slot, fresh, generated);
    }

    /// The hands, in words - for the log and the card, so "refitted" is not the only evidence.
    std::string KitShapeText(Player* bot)
    {
        auto describe = [&](uint16 slot) -> std::string
        {
            Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!item)
                return "empty";
            ItemTemplate const* proto = item->GetTemplate();
            if (!proto)
                return "a piece";
            switch (proto->InventoryType)
            {
                case INVTYPE_2HWEAPON:
                    return proto->SubClass == ITEM_SUBCLASS_WEAPON_STAFF ? "staff" : "two-hander";
                case INVTYPE_SHIELD:
                    return "shield";
                case INVTYPE_HOLDABLE:
                    return "held offhand";
                case INVTYPE_WEAPON:
                    return "one-hander";
                default:
                    return "mainhand";
            }
        };
        return describe(EQUIPMENT_SLOT_MAINHAND) + " + " + describe(EQUIPMENT_SLOT_OFFHAND);
    }

    /// The equipment slot an inventory type goes in, or -1 when it is not worn at all.
    int SlotForInventoryType(uint8 type)
    {
        switch (type)
        {
            case INVTYPE_HEAD: return EQUIPMENT_SLOT_HEAD;
            case INVTYPE_NECK: return EQUIPMENT_SLOT_NECK;
            case INVTYPE_SHOULDERS: return EQUIPMENT_SLOT_SHOULDERS;
            case INVTYPE_BODY: return EQUIPMENT_SLOT_BODY;
            case INVTYPE_CHEST: case INVTYPE_ROBE: return EQUIPMENT_SLOT_CHEST;
            case INVTYPE_WAIST: return EQUIPMENT_SLOT_WAIST;
            case INVTYPE_LEGS: return EQUIPMENT_SLOT_LEGS;
            case INVTYPE_FEET: return EQUIPMENT_SLOT_FEET;
            case INVTYPE_WRISTS: return EQUIPMENT_SLOT_WRISTS;
            case INVTYPE_HANDS: return EQUIPMENT_SLOT_HANDS;
            case INVTYPE_FINGER: return EQUIPMENT_SLOT_FINGER1;      // either ring slot will do
            case INVTYPE_TRINKET: return EQUIPMENT_SLOT_TRINKET1;
            case INVTYPE_CLOAK: return EQUIPMENT_SLOT_BACK;
            case INVTYPE_WEAPON: case INVTYPE_2HWEAPON: case INVTYPE_WEAPONMAINHAND: return EQUIPMENT_SLOT_MAINHAND;
            case INVTYPE_SHIELD: case INVTYPE_HOLDABLE: case INVTYPE_WEAPONOFFHAND: return EQUIPMENT_SLOT_OFFHAND;
            case INVTYPE_RANGED: case INVTYPE_RANGEDRIGHT: case INVTYPE_THROWN: case INVTYPE_RELIC:
                return EQUIPMENT_SLOT_RANGED;
            default: return -1;
        }
    }

    std::set<uint64> WornGuids(Player* bot)
    {
        std::set<uint64> worn;
        if (!bot)
            return worn;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                worn.insert(item->GetGUID().GetRawValue());
        return worn;
    }

    /// Anything that was worn and is now carried was put down by the change just made.
    uint32 MarkKitDisplaced(Player* bot, std::set<uint64> const& wornBefore)
    {
        uint32 displaced = 0;
        if (!bot || wornBefore.empty())
            return displaced;

        uint32 const botGuid = bot->GetGUID().GetCounter();
        VisitCarriedItems(bot, [&](Item* item)
        {
            if (!item || item->IsEquipped())
                return;                             // still on the body: the change kept it
            uint64 const raw = item->GetGUID().GetRawValue();
            if (wornBefore.count(raw) && !IsSavedRoleItem(botGuid, item->GetGUID()) &&
                g_staleGear[botGuid].insert(raw).second)
                ++displaced;
        });
        return displaced;
    }

    /// A world upgrade moves the piece it beat into the bags, where nothing in the pass ever sees
    /// it again. A carried armour or weapon piece that the worn piece in its own slot beats on item
    /// level is that cast-off, and it goes to the vendor like any other - but a profession tool, a
    /// curated delivery, anything bound or in trade is left exactly where it is.
    void MarkSupersededGear(Player* owner, Player* bot)
    {
        if (!bot || !owner)
            return;

        uint32 const botGuid = bot->GetGUID().GetCounter();
        std::set<uint32> const tools = ProfessionToolItems(bot->getClass());
        VisitCarriedItems(bot, [&](Item* item)
        {
            if (!item || item->IsEquipped() || item->IsInTrade() || item->IsRefundable())
                return;
            ItemTemplate const* proto = item->GetTemplate();
            if (!proto || (proto->Class != ITEM_CLASS_ARMOR && proto->Class != ITEM_CLASS_WEAPON) ||
                tools.count(proto->ItemId) || !CanReleaseItem(bot, owner, item))
                return;

            int const slot = SlotForInventoryType(proto->InventoryType);
            if (slot < 0)
                return;

            Item* worn = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, uint8(slot));
            if (!worn || !worn->GetTemplate() || worn->GetTemplate()->ItemLevel <= proto->ItemLevel)
                return;                             // a spare, not a cast-off

            g_staleGear[botGuid].insert(item->GetGUID().GetRawValue());
        });
    }

    /// Fit the piece the role insists on, when the score-driven kit contradicts it.
    ///
    /// Returns whether anything was refitted. Damage and support roles are left alone on purpose:
    /// for them the best score *is* the answer, and a second opinion would only be taste.
    bool RepairKitShape(Player* bot, std::string const& role, std::vector<ObjectGuid>* generated = nullptr)
    {
        if (!bot || !bot->IsInWorld() || bot->IsInCombat() || role == "auto" ||
            !bot->IsAlive() || bot->GetTradeData())
            return false;

        KitNeed const need = KitNeedFor(bot->getClass(), role);
        if (need == KitNeed::None)
            return false;

        // a refit swaps one piece out, so it needs somewhere to put that piece - not the room the
        // whole pass wants. Holding it to the pass's reserve meant the refit was refused precisely
        // when it was most needed: right after a pass that had just displaced several pieces.
        if (bot->GetFreeInventorySpace() < 1)
            return false;

        Item* mainhand = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
        Item* offhand = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
        ItemTemplate const* mainProto = mainhand ? mainhand->GetTemplate() : nullptr;

        bool changed = false;
        if (need == KitNeed::Shield)
        {
            // the shape is already right, or the shield slot is not this class's problem
            if (offhand && offhand->GetTemplate() && offhand->GetTemplate()->SubClass == ITEM_SUBCLASS_ARMOR_SHIELD &&
                offhand->GetTemplate()->Class == ITEM_CLASS_ARMOR)
                return false;

            // a two-hander holds both hands, so it has to give way before a shield can go on
            if (mainProto && mainProto->InventoryType == INVTYPE_2HWEAPON)
            {
                if (!FitKitPiece(bot, EQUIPMENT_SLOT_MAINHAND, KitPiece::OneHandedWeapon, generated))
                    return false;                   // no shield without a hand free: say nothing, try next pass
                changed = true;
            }

            changed = FitKitPiece(bot, EQUIPMENT_SLOT_OFFHAND, KitPiece::Shield, generated) || changed;
        }
        else if (need == KitNeed::CasterWeapon)
        {
            if (!mainProto || mainProto->InventoryType != INVTYPE_2HWEAPON)
                return false;

            // a staff is a caster's two-hander and is exactly what a healer wants; only a melee
            // two-hander costs it the offhand it could be casting from
            bool const meleeTwoHand = mainProto->SubClass == ITEM_SUBCLASS_WEAPON_AXE2 ||
                                      mainProto->SubClass == ITEM_SUBCLASS_WEAPON_MACE2 ||
                                      mainProto->SubClass == ITEM_SUBCLASS_WEAPON_SWORD2 ||
                                      mainProto->SubClass == ITEM_SUBCLASS_WEAPON_POLEARM;
            if (!meleeTwoHand)
                return false;

            if (!FitKitPiece(bot, EQUIPMENT_SLOT_MAINHAND, KitPiece::CasterOneHand, generated))
                return false;
            changed = true;
        }

        if (changed)
            LOG_INFO("server.loading", "[CompanionQuestSync] {} refitted its hands for the {} role: {}",
                     bot->GetName(), role, KitShapeText(bot));
        return changed;
    }

    void RefitRoleHands(Player* bot, std::string const& role)
    {
        if (KitNeedFor(bot->getClass(), role) == KitNeed::None)
            return;

        std::set<uint64> const worn = WornGuids(bot);
        if (RepairKitShape(bot, role))
            if (uint32 const displaced = MarkKitDisplaced(bot, worn))
                LOG_INFO("server.loading", "[CompanionQuestSync] {} set aside {} displaced piece(s) for the vendor",
                         bot->GetName(), displaced);
    }

#include "RoleSwitch.inc"

    /// The hands can change outside our pass: playerbots equips a world drop the moment it scores
    /// better, and a two-hander picked up by a tank puts the shield back in the bags. So the shape
    /// is also checked on the slow pass, throttled, and the role's shape comes back within a minute
    /// instead of waiting for the next level.
    void WatchKitShape(Player* owner, Player* bot)
    {
        if (!bot || !bot->IsInWorld() || bot->IsInCombat())
            return;

        uint32 const botGuid = bot->GetGUID().GetCounter();
        std::string const role = CompanionRole(botGuid);
        if (role == "auto")
            return;                                 // nothing was asked of this body at all

        uint32& last = g_kitWatchAt[botGuid];
        uint32 const now = uint32(time(nullptr));
        if (last && now - last < 60)
            return;
        last = now;

        // the hands, for the roles that imply a shape - and the bag hygiene for every role, because
        // a world upgrade leaves a cast-off behind whoever the companion is
        auto const requested = g_roleSwitches.find(botGuid);
        if (requested == g_roleSwitches.end() || requested->second.pending.empty())
            RefitRoleHands(bot, role);
        MarkSupersededGear(owner, bot);

    }

    uint32 RefreshLevelGear(Player* owner, Player* bot, bool specChanged)
    {
        if (!g_cfg.levelGear || !bot || !bot->IsInWorld() || !bot->IsAlive() ||
            bot->IsInCombat() || !bot->GetSession() || bot->GetTradeData())
            return 0;

        uint8 const level = bot->GetLevel();
        if (level < 5)
            return 0;                       // under five the starter outfit *is* the level

        uint32 pieces = 0;
        uint32 itemLevelSum = 0;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            {
                ++pieces;
                itemLevelSum += item->GetTemplate()->ItemLevel;
            }
        uint32 const average = pieces ? itemLevelSum / pieces : 0;
        // A kit that already matches the level is left exactly as it is - unless the spec just
        // changed, in which case the kit's *shape* is wrong even though its item level is fine
        // (a protection warrior in the fury gear it was handed is dressed for the wrong fight).
        if (!specChanged && pieces && average + CompanionGearLag >= level)
            return 0;
        // The factory unequips before it equips, so it needs room to work in.
        if (bot->GetFreeInventorySpace() < CompanionBagReserve)
            return 0;

        uint32 const botGuid = bot->GetGUID().GetCounter();
        auto const roleSwitch = g_roleSwitches.find(botGuid);
        if (roleSwitch != g_roleSwitches.end() && !roleSwitch->second.pending.empty())
            return 0;
        for (ObjectGuid const guid : ReadRoleHands(bot))
            if (IsSavedRoleItem(botGuid, guid))
                return 0; // the level factory can destroy a replaced item shared with a saved kit
        uint32 const lastLevel = g_gearLevel[botGuid];
        if (!specChanged && lastLevel && level < lastLevel + CompanionGearStep)
            return 0;

        uint32 const before = DescribeGearItemLevel(bot);

        // what it was wearing, so the pass can tell what it put down
        std::set<uint64> const wornBefore = WornGuids(bot);

#ifdef MOD_PLAYERBOTS
        PlayerbotFactory factory(bot, level);
        factory.InitEquipment(/*incremental=*/true);
        if (PlayerbotAI* ai = GET_PLAYERBOT_AI(bot))
            ai->HandleCommand(CHAT_MSG_WHISPER, "equip upgrade", owner);
#endif

        // the factory picked by score; the role may still insist on one piece of shape
        RepairKitShape(bot, CompanionRole(botGuid));

        // anything that was worn and is now carried was displaced by this pass: the vendor's, not
        // the bags'. playerbots destroys the piece it replaces in a slot, so in the ordinary case
        // there is nothing here at all - an offhand swapped for a two-hander is what shows up.
        uint32 const displaced = MarkKitDisplaced(bot, wornBefore);
        // Latched here, after the attempt but before the verdict: the preconditions
        // above (bags, trade, combat) still retry on the next pass, while a generator
        // that produced nothing does not run again for another five levels.
        g_gearLevel[botGuid] = level;

        uint32 const after = DescribeGearItemLevel(bot);
        if (after == before && !displaced)
        {
            // Not an error, and not invisible either: a spec re-gear that found the kit already
            // fitting is the answer to "did it work?" - the alternative is silence, which reads
            // the same as a pass that never ran.
            LOG_INFO("server.loading", "[CompanionQuestSync] {} re-geared for level {}{}: no slot needed changing "
                                       "(item level {})",
                     bot->GetName(), uint32(level), specChanged ? " after a spec change" : "", after);
            return 0;
        }

        if (displaced)
            LOG_INFO("server.loading", "[CompanionQuestSync] {} set aside {} displaced piece(s) for the vendor",
                     bot->GetName(), displaced);

        LOG_INFO("server.loading", "[CompanionQuestSync] {} re-geared for level {}{} (item level {} -> {})",
                 bot->GetName(), uint32(level), specChanged ? " after a spec change" : "", before, after);
        // only worth saying when the kit actually moved: "17 to 17" is a receipt for nothing, and a
        // spec change that keeps the same average said it on every attempt
        if (after != before)
            SayToOwner(bot, owner,
                       "I traded my starter kit for what a level " + std::to_string(level) +
                       " can carry - average item level " + std::to_string(before) + " to " + std::to_string(after) + ".",
                       30);
        return after - before;
    }

    /// Average item level of what the companion wears, over the pieces it actually has.
    uint32 DescribeGearItemLevel(Player* bot)
    {
        uint32 pieces = 0;
        uint32 sum = 0;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            {
                ++pieces;
                sum += item->GetTemplate()->ItemLevel;
            }
        return pieces ? sum / pieces : 0;
    }

    std::string DescribeMissingItems(Player* bot, Quest const* quest);  // defined below
    uint32 TidyInventory(Player* owner, Player* bot);   // defined below, used by the accept path

    /// Send only stacks already approved by the delivery policy at a mailbox.
    uint32 MailHome(Player* owner, Player* bot, ObjectGuid mailbox, std::vector<Item*>& items)
    {
        if (items.empty())
            return 0;

        // The body lists what is inside: a letter worth opening is the point.
        std::string body = "I set these aside for you:\n";
        for (Item* item : items)
        {
            if (ItemTemplate const* proto = item->GetTemplate())
                body += "- " + std::string(proto->Name1) + " x" + std::to_string(item->GetCount()) + "\n";
        }

        std::string subject = std::string("Companion delivery: ") + bot->GetName();
        for (Item* item : items)
            if (!sScriptMgr->OnPlayerCanSendMail(bot, owner->GetGUID(), mailbox, subject, body, 0, 0, item))
                return 0;

        // Keep the identity prefix server-owned even if another module rewrites subjects.
        MailDraft draft(std::string("Companion delivery: ") + bot->GetName(), body);
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

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

        // Same-character normal mail cannot be authored by a client. Route any
        // standard expiry/return to the recipient, never to a recyclable bot.
        draft.SendMailTo(trans, MailReceiver(owner),
                         MailSender(MAIL_NORMAL, owner->GetGUID().GetCounter()),
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
    ObjectGuid MailboxNearby(Player* bot)
    {
        std::list<GameObject*> objects;
        Acore::GameObjectInRangeCheck check(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), INTERACTION_DISTANCE);
        Acore::GameObjectListSearcher<Acore::GameObjectInRangeCheck> searcher(bot, objects, check);
        Cell::VisitObjects(bot, searcher, INTERACTION_DISTANCE);
        for (GameObject* object : objects)
            if (object->GetGoType() == GAMEOBJECT_TYPE_MAILBOX &&
                bot->GetGameObjectIfCanInteractWith(object->GetGUID(), GAMEOBJECT_TYPE_MAILBOX))
                return object->GetGUID();
        return ObjectGuid::Empty;
    }

    void TryCuratedMail(Player* owner, Player* bot)
    {
        if (!g_cfg.mailHome || !CanManageBags(owner, bot) || owner->isMoving() || bot->isMoving())
            return;
        uint32 const ownerGuid = owner->GetGUID().GetCounter();
        uint32 const now = uint32(time(nullptr));
        uint32& lastMail = g_ownerDeliveryMailAt[ownerGuid];
        if ((lastMail && now - lastMail < Timing::MailHome) || owner->GetMailSize() >= 90)
            return;
#ifdef AC_TRADE_OFFER_GUARD
        auto const lastOffer = g_lastAutoTrade.find(ownerGuid);
        if (g_cfg.autoTrade && lastOffer != g_lastAutoTrade.end() && now - lastOffer->second < Timing::MailAfterOffer)
            return;
#endif
        ObjectGuid const mailbox = MailboxNearby(bot);
        if (!mailbox)
            return;
        auto const mailNow = GameTime::GetGameTime().count();
        for (Mail const* mail : owner->GetMails())
            if (IsDeliveryMail(mail, ownerGuid) && mail->expire_time >= mailNow && !mail->items.empty())
                return;

        std::vector<Item*> attachments;
        for (auto const& [guid, quantity] : PlanDelivery(bot, owner, true, /*mail=*/true))
        {
            Item* item = bot->GetItemByGuid(guid);
            if (item && item->CanBeTraded(true) && !item->IsSoulBound() && !item->IsRefundable() && item->GetCount() == quantity)
                attachments.push_back(item); // no bag splits just to create mail
            if (attachments.size() == 3)
                break;
        }
        if (attachments.empty())
            return;
        uint32& lastPolicyCheck = g_ownerMailPolicyAt[ownerGuid];
        if (lastPolicyCheck && now - lastPolicyCheck < Timing::MailAfterOffer)
            return;
        lastPolicyCheck = now;
        if (MailHome(owner, bot, mailbox, attachments))
        {
            lastMail = now;
            SayToOwner(bot, owner, "I left a parcel of upgrades or supplies for you. Pick it up at the mailbox when we stop.", 120);
        }
    }

    /// Credit the companion for prerequisites its owner already finished.
    ///
    /// `CanTakeQuest` -> `SatisfyQuestPreviousQuest` checks rewarded history
    /// for positive predecessors, with special rules for exclusive groups. Copying the owner's active log can never
    /// satisfy that for a companion adopted after the chain was walked, which is
    /// the normal case: quest first, companion second. This walks the same
    /// `prevQuests` chain the core walks and marks the ancestors the owner has
    /// already rewarded - nothing more. `SetRewardedQuest` touches only the
    /// rewarded set and its save flag: no XP, no item, no spell, no log slot, so
    /// a companion cannot be handed loot or a level it did not earn.
    ///
    /// Negative entries in `prevQuests` require an active prerequisite, not
    /// rewarded history, so they are never stamped.
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

            // History must not overwrite a copy the companion still needs to
            // play, failed copies included, or fabricate a cycle/reset marker.
            if (prev->IsRepeatable() || prev->IsSeasonal() ||
                bot->FindQuestSlot(prevId) < MAX_QUEST_LOG_SIZE || bot->GetQuestStatus(prevId) != QUEST_STATUS_NONE ||
                !owner->IsQuestRewarded(prevId) || bot->IsQuestRewarded(prevId))
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

    bool CompleteEligibleCompanionQuest(Player* bot, Quest const* quest, SyncReport* report)
    {
        uint32 const questId = quest->GetQuestId();
        if (!g_cfg.syncCompletion || bot->GetQuestStatus(questId) != QUEST_STATUS_INCOMPLETE ||
            !bot->CanCompleteQuest(questId) || !DescribeMissingItemsUncached(bot, quest).empty())
            return false;

        bot->CompleteQuest(questId);
        if (bot->GetQuestStatus(questId) == QUEST_STATUS_INCOMPLETE)
            return false; // a core script vetoed completion
        if (report)
            ++report->completed;
        if (g_cfg.debug >= 1)
            LOG_INFO("server.loading", "[CompanionQuestSync] {} = {} ({}) completed", bot->GetName(), quest->GetTitle(), questId);
        return true;
    }

    bool AbandonCompanionQuest(Player* bot, uint16 slot, uint32 questId)
    {
        if (!bot->GetSession() || slot >= MAX_QUEST_LOG_SIZE || bot->GetQuestSlotQuestId(slot) != questId)
            return false;
        WorldPacket request(CMSG_QUESTLOG_REMOVE_QUEST);
        request << uint8(slot);
        bot->GetSession()->HandleQuestLogRemoveQuest(request);
        return bot->FindQuestSlot(questId) >= MAX_QUEST_LOG_SIZE;
    }

    /// Mirror one quest onto one companion. Shared by the periodic sweep and the
    /// accept hook, so the "slow" and "instant" paths can never drift apart.
    /// `report` is optional: the hook path only needs the notifications/log.
    void SyncQuestToBot(Player* owner, Player* bot, Quest const* quest, SyncReport* report)
    {
        uint32 const questId = quest->GetQuestId();

        std::pair<uint32, uint32> const cycleKey{ bot->GetGUID().GetCounter(), questId };
        QuestStatus botStatus = bot->GetQuestStatus(questId);
        QuestStatus const ownerStatus = owner->GetQuestStatus(questId);
        bool const timed = quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_TIMED);
        bool const repeatable = quest->IsRepeatable() || quest->IsSeasonal();

        bool const singleCycle = timed || repeatable;

        if (botStatus == QUEST_STATUS_REWARDED)
        {
            g_addedAt.erase(cycleKey);
            return;
        }
        if (ownerStatus != QUEST_STATUS_INCOMPLETE && ownerStatus != QUEST_STATUS_COMPLETE)
            return;

        if (timed && g_cfg.skipTimedQuests)
        {
            if (report)
                AddSkipped(*report, quest, "timed quest not copied");
            return;
        }
        if (singleCycle)
        {
            if (botStatus != QUEST_STATUS_NONE)
                g_ownerCycleSeen[cycleKey] = true;
            else if (g_ownerCycleSeen.contains(cycleKey))
                return; // do not accept the same owner cycle a second time
        }

        if (botStatus == QUEST_STATUS_NONE)
        {
            if (timed)
            {
                // AddQuest shares the remaining whole seconds from the owner.
                auto const& active = owner->getQuestStatusMap();
                auto const ownerCopy = active.find(questId);
                if (ownerCopy == active.end() || ownerCopy->second.Timer < IN_MILLISECONDS)
                    return;
            }

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

            bool const force = g_cfg.forceAccept && !singleCycle;
            if (!force)
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
                if (!bot->SatisfyQuestPreviousQuest(quest, false))
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

            if (force)
                bot->AddQuest(quest, owner);
            else
                bot->AddQuestAndCheckCompletion(quest, owner);

            botStatus = bot->GetQuestStatus(questId);
            if (singleCycle && (botStatus != QUEST_STATUS_NONE || quest->HasFlag(QUEST_FLAGS_TRACKING)))
                g_ownerCycleSeen[cycleKey] = true;
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

        if (ownerStatus == QUEST_STATUS_COMPLETE)
            CompleteEligibleCompanionQuest(bot, quest, report);
    }

    /// Quest pickup may retry after a real vendor sale, never after deletion or mail.
    uint32 TidyInventory(Player* owner, Player* bot)
    {
        if (!g_cfg.tidyInventory || bot->GetFreeInventorySpace() >= g_cfg.keepFreeSlots + g_cfg.bagMargin)
            return 0;
        return SellManagedJunk(owner, bot);
    }

    /// Resolve the owner's companion and check it is a live bot we may touch.
    /// Returns nullptr (with what we learned recorded in `report`) if not.
    Player* ResolveCompanion(Player* owner, SyncReport& report, uint32 botGuid)
    {
        if (!botGuid)
            return nullptr;
        report.hasCompanion = true;
        Player* bot = FindCompanionPlayer(botGuid);
        if (!IsControlledCompanion(bot, owner))
            return nullptr;

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
    SyncReport SyncCompanionFor(Player* owner, uint32 botGuid)
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
                LOG_INFO("server.loading", "[CompanionQuestSync] requested ground loot from {}", gathered);
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

                if (!AbandonCompanionQuest(bot, slot, questId))
                    continue;
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
        if (g_cfg.dropExtraBotQuests || g_cfg.syncCompletion)
        {
            uint32 const nowTs = static_cast<uint32>(time(nullptr));
            uint32 const botGuid = bot->GetGUID().GetCounter();

            std::set<uint32> stillExtra;

            for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            {
                uint32 const questId = bot->GetQuestSlotQuestId(slot);
                if (!questId)
                    continue;

                if (std::find(ownerQuests.begin(), ownerQuests.end(), questId) != ownerQuests.end())
                    continue;

                // Preserve catch-up copies after the owner hands in, but never
                // infer objective credit from historical repeatable rewards.
                if (owner->IsQuestRewarded(questId))
                {
                    Quest const* rewarded = sObjectMgr->GetQuestTemplate(questId);
                    if (rewarded && !rewarded->IsRepeatable() && !rewarded->IsSeasonal())
                        CompleteEligibleCompanionQuest(bot, rewarded, &report);
                    continue;
                }

                if (!g_cfg.dropExtraBotQuests)
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

                if (!AbandonCompanionQuest(bot, slot, questId))
                    continue;
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
        // the gather strategy follows the profession mode: a no-gather companion is
        // not asked to work its own nodes. Re-asserted in this keep-trying slot
        // because playerbots drops strategies on spec and level changes, and a stale
        // strategy would have the mode's answer depend on which teach ran last.
        if (PlayerbotAI* ai = GET_PLAYERBOT_AI(bot))
        {
            bool const wantGather = g_cfg.professionNoGather < 2;
            if (ai->HasStrategy("gather", BOT_STATE_NON_COMBAT) != wantGather)
                ai->ChangeStrategy(wantGather ? "+gather" : "-gather", BOT_STATE_NON_COMBAT);
        }
        // Learn new supported recipes when attained skill rises, without a button.
        TeachCraftRecipes(bot, ClassProfessionPair(bot->getClass()).second);
        // Bags first: a pack that is nearly full is trimmed back to the recipe budget
        // before anything else is put into it, so restocking never spends into a wall.
        if (CanManageBags(owner, bot))
        {
            // Wear what the pack already holds before judging it spare: an upgrade
            // sold by the trim is worse than one worn, and the trim runs first here.
            TryEquipUpgrades(owner, bot);
            SellBagSurplus(owner, bot);
            RestockNearVendor(owner, bot);
        }
        // crafting is the companion's own habit, not a request: on the configured
        // cadence it turns what its gathering paid for (or, in no-gather mode, what
        // the stand-in trip supplies) into the useful thing, silently - the products
        // land in bags; curated surplus may be delivered later.
        // The Produce button stays for the moment the report itself is wanted.
        if (g_cfg.autoProduce && CanManageBags(owner, bot))
        {
            uint32& lastAuto = g_lastAutoProduce[botGuid];
            uint32 const now = uint32(time(nullptr));
            if (!lastAuto || now - lastAuto >= g_cfg.produceInterval)
            {
                lastAuto = now;
                auto const crafted = ProduceForCompanion(bot, owner, g_cfg.produceMaxItems);
                // only a made product is worth a line: the pass runs every interval,
                // and the "I need / I couldn't" repeats would drown the log at Debug 2
                if (g_cfg.debug >= 2)
                    for (std::string const& line : crafted)
                        if (line.rfind("I made", 0) == 0)
                            LOG_INFO("server.loading", "[CompanionQuestSync] {} quiet craft: {}",
                                     bot->GetName(), line);
            }
        }
        if (CanManageBags(owner, bot))
        {
            // A kit that never caught up with the level is corrected here, so companions
            // claimed before this existed heal themselves instead of staying in starters.
            RefreshLevelGear(owner, bot);
            WatchKitShape(owner, bot);
            // Spells, talents and both professions once per level, so a body hired
            // high or levelled outside the normal path still arrives complete.
            EnsureLevelProgress(owner, bot);
            SellManagedJunk(owner, bot);
            if (g_cfg.tidyInventory && bot->GetFreeInventorySpace() < g_cfg.keepFreeSlots)
            {
                uint32& lastWarning = g_junkNoticeAt[owner->GetGUID().GetCounter()];
                uint32 const now = uint32(time(nullptr));
                if (!lastWarning || now - lastWarning >= Timing::JunkNotice)
                {
                    lastWarning = now;
                    SayToOwner(bot, owner, "My bags are nearly full. I'll sell what I can at the next merchant; ask me for a trade if you need to make room now.", 120);
                }
            }
        }
#ifdef AC_TRADE_OFFER_GUARD
        if (g_cfg.autoTrade && CanOfferTownDelivery(owner, bot))
        {
            uint32& lastAttempt = g_lastAutoTradeAttempt[owner->GetGUID().GetCounter()];
            uint32 const now2 = uint32(time(nullptr));
            if ((!lastAttempt || now2 - lastAttempt >= Timing::TradeDelivery) &&
                OpenTradeSurplus(bot, owner, /*announce=*/false, /*curated=*/true))
                lastAttempt = now2; // an unfilled attempt must not starve mail on subsequent passes
        }
#endif
        TryCuratedMail(owner, bot);
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

        // Newly completed quests can hand in this pass without reacquiring a body
        // through a less restrictive lookup in the outer sweep.
        if (report.completed)
            DeliverHandIns(owner, bot);

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
    /// h = can hand in here now, g = objective work here. Capable clients also get
    /// p = eventual hand-in with work left, u = hand-in not currently available.
    /// A pin that is not in this line is not real, which lets the map clean up after a
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
    /// Cut a string to at most maxBytes without splitting a UTF-8 character. The pin
    /// grammar and the chat frame both render bytes, and half a character is visible.
    inline std::string TruncateUtf8(std::string const& text, size_t maxBytes)
    {
        if (text.size() <= maxBytes)
            return text;
        std::string out = text.substr(0, maxBytes);
        while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80)
            out.pop_back();                                  // drop continuation bytes
        if (!out.empty() && (static_cast<unsigned char>(out.back()) & 0x80))
            out.pop_back();                                  // drop a dangling lead byte
        return out;
    }

    /// A companion name as the pin grammar can survive it.
    ///
    /// Keep actual character names intact on pins: the core limits characters, not
    /// bytes. The roster's existing short-field contract remains the default.
    std::string SafePinName(std::string const& name, bool shortField = true)
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

        if (shortField && out.size() > 12)
            out = TruncateUtf8(out, 12);

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

    // The per-copy suffix is ~i=have/required/hexname;... . Hex keeps chat escapes
    // and every pin delimiter inert, including when a revision splits the suffix.
    std::string PinMissingItems(Player* bot, Quest const* quest)
    {
        std::string out;
        ForEachMissingRequiredItem(bot, quest, [&](uint32 itemId, uint32 have, uint32 required)
        {
            constexpr size_t kMaxNameBytes = 48;
            std::string name = RequiredItemName(itemId);
            name.erase(std::remove_if(name.begin(), name.end(), [](unsigned char c) { return c < 32 || c == 127; }), name.end());
            if (name.empty())
                name = "item " + std::to_string(itemId);
            if (name.size() > kMaxNameBytes)
            {
                size_t cut = kMaxNameBytes - 3;
                while ((uint8(name[cut]) & 0xC0) == 0x80)
                    --cut;
                name.replace(cut, std::string::npos, "...");
            }
            out += out.empty() ? "~i=" : ";";
            out += std::to_string(have) + "/" + std::to_string(required) + "/";
            constexpr char hex[] = "0123456789abcdef";
            for (unsigned char c : name)
            {
                out += hex[c >> 4];
                out += hex[c & 15];
            }
        });
        return out;
    }

    std::string BuildPinLine(Player* owner, bool allowItems = true)
    {
        struct Entry
        {
            std::string numbers;
            char style;
            std::string title;
            uint32 questId;
            std::string zone;   // the giver's zone, named by the SERVER: the client's own
                                // location is not the pin's location, and guessing it was
                                // how pins ended up stamped with the wrong map
        };

        std::vector<Entry> entries;
        auto const stream = g_pinRevisions.find(owner->GetGUID().GetCounter());
        bool const versioned = stream != g_pinRevisions.end();
        bool const handInStates = versioned && stream->second.handInStates;
        bool const pinItems = allowItems && handInStates && stream->second.pinItems;

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

                QuestStatus const status = bot->GetQuestStatus(questId);
                if (status != QUEST_STATUS_INCOMPLETE && status != QUEST_STATUS_COMPLETE)
                    continue;

                // A completed log entry can still be missing turn-in items. Only
                // a live interaction AND the native reward check can make it gold.
                bool const ready = status == QUEST_STATUS_COMPLETE && DescribeMissingItemsUncached(bot, quest).empty();
                WorldObject* giver = ready ? FindReadyQuestEnder(bot, quest) : nullptr;
                std::vector<std::tuple<std::string, char, std::string>> spots;
                // A ready hand-in is pinned wherever its giver stands: requiring a live
                // interaction left a far hand-in with NO pin at all - ready suppresses the
                // objective pins, and no giver meant no ender pin. The 'u' style is only
                // sent to a client that has negotiated the pin protocol, so a far hand-in
                // stays invisible to an addon that cannot read the newer styles.
                if (giver || handInStates || (ready && versioned))
                {
                    std::optional<EnderPin> pin;
                    if (giver)
                        pin = PlotEnder(DescribeQuestEnder(giver));
                    else
                        FindQuestEnder(bot, questId, &pin);
                    char const style = giver ? 'h' : ready ? 'u' : 'p';
                    if (pin)
                    {
                        // Legacy parsers require integer percentages. Keep distinct
                        // nearby giver positions on capable clients. Reverse bounds
                        // can produce -0 at a map edge.
                        std::string const numbers = handInStates ? fmt::format("{:.4f}, {:.4f}", pin->x + 0.0f, pin->y + 0.0f) :
                            std::to_string(uint32(pin->x)) + ", " + std::to_string(uint32(pin->y));
                        spots.emplace_back("map " + numbers, style, pin->zone);
                    }
                }

                if (!ready)
                {
                    // Keep objective markers even when a future hand-in spot is known.
                    std::vector<std::pair<std::string, std::string>> workLeftSpots;
                    if (!LootSpots(bot, quest, workLeftSpots))
                        PoiPins(bot, quest, workLeftSpots);
                    for (auto const& spot : workLeftSpots)
                        spots.push_back({ spot.first, 'g', spot.second });
                }

                for (auto const& [coords, style, zoneName] : spots)
                {
                    // "map 43, 65" -> "43,65"
                    std::string numbers = coords.substr(4);
                    numbers.erase(std::remove_if(numbers.begin(), numbers.end(),
                                                 [](char c) { return c == ' '; }), numbers.end());

                    if (numbers.empty())
                        continue;

                    // A missing template title must not delete the pin: the addon needs a
                    // non-empty label, and "quest 65" is more useful than nothing.
                    std::string title = quest->GetTitle();
                    if (title.empty())
                        title = "quest " + std::to_string(questId);
                    if (title.size() > 22)
                        title = TruncateUtf8(title, 20) + "..";

                    // a title cannot carry the separators, or the addon would split it
                    std::replace(title.begin(), title.end(), '|', '/');
                    std::replace(title.begin(), title.end(), '=', ':');

                    std::replace(title.begin(), title.end(), '+', '/');
                    if (versioned || MultiCompanionSpeech(bot))
                        title += "@" + SafePinName(bot->GetName(), false);

                    // what this companion still owes, as the quest log counts it -
                    // "3/8" after the name, nothing when the quest is ready to hand in
                    if (!ready)
                    {
                        std::string owed = OwedText(bot, quest);
                        if (!owed.empty())
                            title += ":" + owed;
                    }
                    if (pinItems && style == 'p')
                        title += PinMissingItems(bot, quest);

                    bool merged = false;
                    for (Entry& existing : entries)
                    {
                        if (existing.numbers != numbers || existing.zone != zoneName)
                            continue;

                        // The same quest at the same place is one marker, not two: two
                        // companions, one ready to hand in and one short of items, used to
                        // draw a gold pin and a grey pin on the same tile. Keep the strongest
                        // state (h > u > p) and let the names share the label.
                        if (existing.questId == questId)
                        {
                            auto const rank = [](char c) { return c == 'h' ? 3 : c == 'u' ? 2 : c == 'p' ? 1 : 0; };
                            if (rank(style) > rank(existing.style))
                                existing.style = style;
                            if (existing.title.find(title) == std::string::npos)
                                existing.title += " + " + title;
                            merged = true;
                            break;
                        }

                        if (existing.style != style)
                            continue;

                        // Every owner reads on the pin: the client gives the names their
                        // own line under the quest, so a full party of four fits where the
                        // old one-line marker forced counting past the second name.
                        existing.title += " + " + title;

                        merged = true;
                        break;
                    }

                    if (!merged)
                        entries.push_back({ numbers, style, title, questId, zoneName });
                }
            }
        }

        std::sort(entries.begin(), entries.end(), [](Entry const& a, Entry const& b)
        {
            return std::tie(a.zone, a.numbers, a.style, a.title) < std::tie(b.zone, b.numbers, b.style, b.title);
        });
        std::string line;
        for (Entry const& entry : entries)
        {
            if (!line.empty())
                line += "|";

            line += entry.numbers + "," + entry.style + "=" + entry.title;

            // The zone is the only other field written to the wire: it goes through the
            // same separator filter as names, or a comma in a zone name re-parses as the
            // next field and the pin lands on the wrong map.
            if (!entry.zone.empty())
                line += ",z=" + SafePinName(entry.zone, false);
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

    constexpr uint32 kAddonHeartbeatSeconds = 60;

    void SendSyncHello(Player* owner)
    {
        // Keep Hello compatible with older CompanionSync and CompanionPins.
        SendProtocolLine(owner, "Hello " + std::to_string(g_cfg.intervalSeconds));
        SendProtocolLine(owner, fmt::format("Sync capabilities 1 {} {}",
            kAddonHeartbeatSeconds, g_cfg.allowCreate ? 1 : 0));
        SendProtocolLine(owner, "Formation " + FormationFor(owner));
        SendRoleAndSpecLines(owner, owner->GetGUID().GetCounter());
#ifdef AC_TRADE_OFFER_COMMIT
        SendProtocolLine(owner, "Delivery receipts 1");
#endif
    }

    /// Presence is independent of quest auto-sync, summoned companions and map pins.
    void SendHeartbeat(Player* owner)
    {
        if (!owner || !owner->IsInWorld() || !owner->GetSession())
            return;

        uint32 const ownerGuid = owner->GetGUID().GetCounter();
        uint32 const now = static_cast<uint32>(time(nullptr));
        uint32& last = g_heartbeatAt[ownerGuid];

        if (last && now - last < kAddonHeartbeatSeconds)
            return;

        last = now;
        SendSyncHello(owner);

        if (g_cfg.debug >= 1)
            LOG_INFO("server.loading", "[CompanionQuestSync] heartbeat -> {}: Hello {}",
                     owner->GetName(), g_cfg.intervalSeconds);

        SendCompSync(owner, true);
    }

    uint64 NextPinEpoch()
    {
        // Time gives a new server process a new range; the counter also orders multiple
        // negotiations within one tick and protects against clock adjustments in-process.
        using namespace std::chrono;
        uint64 const now = duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
        static std::atomic<uint64> previous{0};
        uint64 seen = previous.load(std::memory_order_relaxed);
        uint64 next;
        do
        {
            next = std::max(now, seen + 1);
        } while (!previous.compare_exchange_weak(seen, next, std::memory_order_relaxed));
        return next;
    }

    void SendPinSync(Player* owner, bool force = false)
    {
        // MapPins is the switch the config documents: PinSync governs the protocol,
        // MapPins whether the markers are pushed at all. Reading it nowhere made the
        // documented setting a no-op.
        if (!g_cfg.pinSync || !g_cfg.mapPins || !owner)
            return;

        uint32 const ownerGuid = owner->GetGUID().GetCounter();
        std::string payload = BuildPinLine(owner);
        constexpr size_t kChunkBytes = 150;
        // Optional detail must not push the snapshot past the reader's 512-part cap.
        // Each non-final chunk can lose at most three bytes to a UTF-8 boundary.
        if (payload.size() > 512 * (kChunkBytes - 3))
            payload = BuildPinLine(owner, false);
        std::string& last = g_lastPinSync[ownerGuid];
        uint32 const now = static_cast<uint32>(time(nullptr));
        uint32 const sentAt = g_pinSyncAt[ownerGuid];

        if (!force && payload == last && (sentAt && now - sentAt < g_cfg.pinSyncSeconds))
            return;

        if (auto revision = g_pinRevisions.find(ownerGuid); revision != g_pinRevisions.end())
        {
            std::vector<std::string_view> chunks;
            for (size_t start = 0; start < payload.size(); )
            {
                size_t end = std::min(start + kChunkBytes, payload.size());
                // Avoid splitting a UTF-8 character across chat messages.
                while (end < payload.size() && (uint8(payload[end]) & 0xC0) == 0x80)
                    --end;
                chunks.emplace_back(payload.data() + start, end - start);
                start = end;
            }
            if (chunks.empty())
                chunks.emplace_back("");
            PinStream& stream = revision->second;
            uint32 const current = ++stream.revision;
            std::string const version = stream.tagged ? fmt::format("{}:{}", stream.epoch, current)
                                                      : std::to_string(current);
            for (size_t i = 0; i < chunks.size(); ++i)
                SendProtocolLine(owner, fmt::format("Pin set {} {}/{}:{}", version, i + 1, chunks.size(), chunks[i]));
            last = payload;
            g_pinSyncAt[ownerGuid] = now;
            if (g_cfg.debug >= 1)
                LOG_INFO("server.loading", "[CompanionQuestSync] pin v2 -> {}: {} parts, {} bytes, revision {}",
                    owner->GetName(), chunks.size(), payload.size(), current);
            return;
        }

        // Legacy clients still read whole entries joined on '|'.
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

    /// Run playerbots commands in the owner's session, using normal access checks.
    void DispatchPlayerbots(Player* owner, std::string const& sub)
    {
        if (owner && owner->GetSession())
            ChatHandler(owner->GetSession()).ParseCommands(".playerbots bot " + sub);
    }

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

    bool IsCompanionLoginPending(ObjectGuid guid)
    {
#ifdef MOD_PLAYERBOTS
        // Playerbots owns the async query holder. Our UI timer is not its lifetime.
        struct LoadingView : PlayerbotHolder
        {
            static bool Contains(ObjectGuid key) { return botLoading.count(key) != 0; }
        };
        return LoadingView::Contains(guid);
#else
        return true;
#endif
    }

    std::string FreeCustodyClause()
    {
        // The ordinary level-one stock contract does not cover the heroic start.
        return " AND x.module = 'cq' AND x.state = 'free' AND x.owner_guid = 0 AND x.tracking = 0 "
               " AND c.online = 0 AND c.level = 1 AND c.at_login = 0 AND c.account = x.body_account "
               " AND BINARY c.name = BINARY x.body_name "
               " AND NOT EXISTS (SELECT 1 FROM guild_member gm WHERE gm.guid = c.guid) "
               " AND NOT EXISTS (SELECT 1 FROM group_member gm WHERE gm.memberGuid = c.guid) "
               " AND NOT EXISTS (SELECT 1 FROM cq_bindings b WHERE b.bot_guid = c.guid) " +
               ForeignCustodyClause("c.guid") + fmt::format(" AND c.class <> {} AND c.guid <> {} AND c.account <> {} ",
                   uint32(CLASS_DEATH_KNIGHT),
                   sConfigMgr->GetOption<uint32>("AuctionHouseBot.GUID", 0),
                   sConfigMgr->GetOption<uint32>("AuctionHouseBot.Account", 0));
    }

    uint32 PickPoolBody(uint8 race, uint8 clazz)
    {
        std::string const accounts = AddClassAccountCsv();
        if (!g_custodyReady || accounts.empty())
            return 0;
        if (QueryResult rows = CharacterDatabase.Query(
                "SELECT c.guid FROM characters c JOIN cq_companion_custody x ON x.bot_guid = c.guid "
                "WHERE c.account IN (" + accounts + ")" + fmt::format(" AND c.race = {} AND c.class = {} ",
                uint32(race), uint32(clazz)) + FreeCustodyClause() + " ORDER BY c.guid"))
        {
            do
            {
                uint32 const guid = rows->Fetch()[0].Get<uint32>();
                ObjectGuid const key = ObjectGuid::Create<HighGuid::Player>(guid);
                if (!ObjectAccessor::FindConnectedPlayer(key) && !IsCompanionLoginPending(key))
                    return guid;
            } while (rows->NextRow());
        }
        return 0;
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

    /// How many roster slots the owner already holds. A released companion keeps its
    /// binding until its retirement is finalized, so it still counts here; naming that
    /// is the difference between "the pool is broken" and "finish the release".
    uint32 CompanionSlotCount(uint32 ownerGuid)
    {
        if (QueryResult row = CharacterDatabase.Query(
                "SELECT COUNT(*) FROM cq_bindings WHERE owner_guid = {}", ownerGuid))
            return row->Fetch()[0].Get<uint32>();

        return 0;
    }

    /// How many free bodies the pool has per race and class: `race-class=count`, `|`
    /// separated. The picker greys out what the pool cannot serve, which is the difference
    /// between a dropdown that works and one that fails after the player has chosen.
    std::string BuildPoolStockLine()
    {
        std::string const accounts = AddClassAccountCsv();
        if (!g_custodyReady || accounts.empty())
            return {};

        std::string const sql = "SELECT c.race, c.class, COUNT(*) FROM characters c "
            "JOIN cq_companion_custody x ON x.bot_guid = c.guid WHERE c.account IN (" + accounts + ")" +
            FreeCustodyClause() + " GROUP BY c.race, c.class ORDER BY c.race, c.class";
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

    /// Publish a revisioned snapshot, then the original entry-chunked format for older addons.
    void SendChunkedPayload(Player* owner, std::string_view label, std::string_view legacy,
                            std::string const& payload, uint32 entries)
    {
        constexpr size_t kChunkBytes = 150;
        constexpr size_t kMaxParts = 32;  // CompanionSync's assembly limit
        std::vector<std::string_view> slices;
        for (size_t start = 0; start < payload.size(); )
        {
            size_t end = std::min(start + kChunkBytes, payload.size());
            while (end > start && end < payload.size() && (uint8(payload[end]) & 0xC0) == 0x80)
                --end;
            if (end == start)
                return;  // invalid UTF-8; don't publish a partial snapshot
            slices.emplace_back(payload.data() + start, end - start);
            start = end;
        }
        if (slices.empty())
            slices.emplace_back("");
        if (slices.size() > kMaxParts)
        {
            LOG_WARN("server.loading", "[CompanionQuestSync] {} snapshot too large; not sending a partial set", label);
            return;
        }

        // No per-owner lifetime to clean up; Lua 5.1 represents these integers exactly.
        // An epoch-based seed also covers ordinary restarts without resetting revisions to 1.
        static std::atomic<uint64> next{uint64(time(nullptr)) * 1'000'000};
        uint64 const revision = next.fetch_add(1, std::memory_order_relaxed);
        for (size_t i = 0; i < slices.size(); ++i)
            SendProtocolLine(owner, fmt::format("{} set {} {}/{}:{}", label, revision, i + 1, slices.size(), slices[i]));

        std::vector<std::string> chunks;
        std::string current;
        size_t start = 0;

        while (start < payload.size())
        {
            size_t const bar = payload.find('|', start);
            std::string entry = payload.substr(start, bar == std::string::npos ? std::string::npos : bar - start);

            if (!current.empty() && current.size() + entry.size() + 1 > kChunkBytes)
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

        for (size_t i = 0; i < chunks.size(); ++i)
        {
            std::string line(legacy);
            line += ' ';

            if (chunks.size() > 1)
                line += std::to_string(i + 1) + "/" + std::to_string(chunks.size()) + ": ";
            else
                line += std::to_string(entries) + ": ";

            line += chunks[i];

            SendProtocolLine(owner, line);
        }
    }

    void SendPoolStock(Player* owner, bool force = false)
    {
        if (!owner || !owner->GetSession() || !g_cfg.allowCreate)
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

        SendChunkedPayload(owner, "Pool", "Pool stock", payload, entries);
        last = payload;

        if (g_cfg.debug >= 2)
            LOG_INFO("server.loading", "[CompanionQuestSync] pool stock -> {}: {} combos: {}",
                     owner->GetName(), entries, payload);
    }

    /// Claim and name a reviewed clean body; the caller may summon after commit.
    ///
    /// Returns false with `error` set to something a player can act on. Every failure is
    /// named, because "it did not work" from a button is useless.
    bool CreateCompanion(uint32 ownerGuid, std::string const& ownerName, uint8 race, uint8 clazz,
                         std::string const& name, uint32& botGuid, std::string& error)
    {
        botGuid = 0;
        if (clazz == CLASS_DEATH_KNIGHT)
        {
            error = "Death Knight companions are unavailable for new claims";
            return false;
        }
        if (!g_custodyReady)
        {
            error = "the verified companion pool is not configured";
            return false;
        }

        if (!ValidCompanionName(name) || sObjectMgr->CheckPlayerName(name) != CHAR_NAME_SUCCESS)
        {
            error = "choose a valid two-to-twelve letter character name";
            return false;
        }
        if (NameTaken(name))
        {
            error = "the name " + name + " is already taken";
            return false;
        }
        if (CompanionSlotCount(ownerGuid) >= kMaxCompanions)
        {
            error = fmt::format("all {} companion slots are in use - release a companion and finalize "
                                "its retirement to free one", uint32(kMaxCompanions));
            return false;
        }
        std::string const accounts = AddClassAccountCsv();
        uint32 const candidate = PickPoolBody(race, clazz);
        if (accounts.empty() || !candidate)
        {
            error = "there is no verified free body for that race and class";
            return false;
        }

        std::string escaped = name;
        CharacterDatabase.EscapeString(escaped);
        std::string const starterAssignment = g_starterBagsReady ?
            ", x.starter_bag_attempt = COALESCE(x.starter_bag_attempt, '') " : " ";
        auto trans = CharacterDatabase.BeginTransaction();
        // One synchronous character-DB transaction. No reset on claim: only bodies
        // independently prepared offline and enrolled as free may enter the pool.
        trans->Append("UPDATE cq_companion_custody x JOIN characters c ON c.guid = x.bot_guid "
            "JOIN characters o ON o.guid = {} AND o.guid <> c.guid "
            "LEFT JOIN characters named ON named.name = '{}' "
            "SET x.owner_guid = o.guid, x.body_name = '{}', x.state = 'owned', x.tracking = 1, c.name = '{}'" + starterAssignment +
            "WHERE x.bot_guid = {} AND c.account IN ({}) AND c.race = {} AND c.class = {} AND named.guid IS NULL " +
            FreeCustodyClause() + " AND (SELECT COUNT(*) FROM cq_bindings slots WHERE slots.owner_guid = {}) < {}",
            ownerGuid, escaped, escaped, escaped, candidate, accounts, uint32(race), uint32(clazz), ownerGuid, kMaxCompanions);
        trans->Append("SET @cq_claim_changed = ROW_COUNT()");
        trans->Append("INSERT INTO cq_bindings (owner_guid, bot_guid, companion_name) "
            "SELECT {}, {}, '{}' WHERE @cq_claim_changed > 0", ownerGuid, candidate, escaped);
        CharacterDatabase.DirectCommitTransaction(trans);

        if (!HasCurrentCustody(ownerGuid, candidate, true) || CompanionNameFor(candidate) != name)
        {
            error = "the claim was not confirmed (that body went out of stock or the name was taken); try again";
            return false;
        }
        botGuid = candidate;
        EnsureCharacterCache(botGuid, name);
        InvalidateCompanionGuid(ownerGuid);
        LOG_INFO("server.loading", "[CompanionQuestSync] {} claimed verified companion {} ({})", ownerName, name, botGuid);
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
    std::string BuildCompLine(Player* owner, std::string& skillPayload)
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

                // No-gather mode: the card marks the companion as producing on its
                // own - the badge beside the gathering track reads this flag.
                if (g_cfg.professionNoGather >= 2)
                    flags += "p";
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
            // Keep the legacy gathering field unchanged for older clients. The
            // optional snapshot adds both actual skills/caps without altering it.
            if (online)
            {
                auto const pair = ClassProfessionPair(bot->getClass());
                uint16 const skill = pair.first;
                line += ",y=" + std::string(ProfessionOf(skill).name) + " " +
                        std::to_string(bot->GetSkillValue(skill)) + "/" + std::to_string(ProfessionCap(bot->GetLevel()));
                if (!skillPayload.empty())
                    skillPayload += "|";
                skillPayload += SafePinName(name);
                for (uint16 const id : { pair.first, pair.second })
                {
                    skillPayload += "," + std::to_string(id) + "," + std::to_string(bot->GetPureSkillValue(id)) +
                                    "," + std::to_string(bot->GetPureMaxSkillValue(id));
                }
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

            // Free slots are field seven, followed by the optional legacy skill.
            size_t slots = 0;
            for (uint8 field = 0; field < 6; ++field)
            {
                size_t const comma = entry.find(',', slots);
                if (comma == std::string::npos)
                    break;
                slots = comma + 1;
                if (field == 5)
                    entry.erase(slots, entry.find(',', slots) - slots);
            }

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
        if (!owner || !owner->GetSession())
            return;

        uint32 const ownerGuid = owner->GetGUID().GetCounter();
        std::string skillPayload;
        std::string const payload = BuildCompLine(owner, skillPayload);
        auto sendSkills = [&]()
        {
            // The roster is capped at four, so even maximum-width skill values
            // fit in one system message. A changed empty snapshot clears the old one.
            std::string& sent = g_lastSkillSync[ownerGuid];
            if ((force && !skillPayload.empty()) || skillPayload != sent)
            {
                SendProtocolLine(owner, "Skill sync: " + skillPayload);
                sent = skillPayload;
            }
        };

        std::string& last = g_lastCompSync[ownerGuid];
        uint32& lastAt = g_lastCompSyncAt[ownerGuid];
        uint32 const now = static_cast<uint32>(time(nullptr));

        // A change that is only the free-slot column waits for the slower clock, so the
        // client is still told (the number is in the payload) but not every 30 seconds.
        constexpr uint32 kSlotOnlyGraceSeconds = Timing::SlotOnlyGrace;

        if (!force && lastAt && (payload == last || (CompSyncSignature(payload) == CompSyncSignature(last)
            && now - lastAt < kSlotOnlyGraceSeconds)))
        {
            sendSkills();
            return;
        }

        uint32 const entries = payload.empty() ? 0 : uint32(std::count(payload.begin(), payload.end(), '|')) + 1;
        SendChunkedPayload(owner, "Comp", "Comp sync", payload, entries);

        last = payload;
        lastAt = now;
        sendSkills();

        if (g_cfg.debug >= 2)
            LOG_INFO("server.loading", "[CompanionQuestSync] comp sync -> {}: {} entries: {}",
                     owner->GetName(), entries, payload);
    }

    uint32 ResolveCompanion(Player* player, std::string const& name, std::vector<uint32> const& bound)
    {
        uint32 const guid = ResolveOwnedCompanion(player->GetGUID().GetCounter(), name);
        return std::find(bound.begin(), bound.end(), guid) != bound.end() ? guid : 0;
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

            if (!HasCurrentCustody(pending.ownerGuid, pending.botGuid, true))
                continue;
            Player* bot = FindCompanionPlayer(pending.botGuid);
            bool const online = bot && bot->IsInWorld();
            Player* owner = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(pending.ownerGuid));

            if (pending.expectOnline)
            {
                if (online && IsControlledCompanion(bot, owner))
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
                        // A starter body arrives dressed for level 1; give it the kit its
                        // level has earned before the owner ever sees it standing there.
                        RefreshLevelGear(master, fresh);
                        // A body hired at a high level must arrive knowing what that level
                        // knows: class spells and talents a trainer would have sold, and both
                        // of its professions at the ceiling for its level, tools included.
                        EnsureLevelProgress(master, fresh);

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
    // Durable retirement; the old pool ledger is kept only for diagnostics.
    // -----------------------------------------------------------------------

    bool g_poolLedgerReady = false;

    void EnsurePoolLedger()
    {
        if (g_poolLedgerReady || g_cfg.poolTable != "cq_pool")
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

    struct PendingRelease
    {
        uint32 ownerGuid;
        uint32 botGuid;
        std::string name;
        std::chrono::steady_clock::time_point deadline;
    };
    std::vector<PendingRelease> g_pendingRelease;
    std::chrono::steady_clock::time_point g_nextReleasePoll;

    bool MarkCompanionRetiring(uint32 ownerGuid, uint32 botGuid)
    {
        if (!HasCurrentCustody(ownerGuid, botGuid, false))
            return false;
        CharacterDatabase.DirectExecute(
            "UPDATE cq_companion_custody x JOIN cq_bindings b ON b.bot_guid = x.bot_guid AND b.owner_guid = x.owner_guid "
            "JOIN characters c ON c.guid = x.bot_guid AND c.account = x.body_account "
            "SET x.state = 'retiring', x.tracking = 0 WHERE x.module = 'cq' AND x.state = 'owned' "
            "AND x.owner_guid = {} AND x.bot_guid = {} AND BINARY x.body_name = BINARY c.name "
            "AND BINARY b.companion_name = BINARY c.name " + ForeignCustodyClause("x.bot_guid"), ownerGuid, botGuid);
        return static_cast<bool>(CharacterDatabase.Query(
            "SELECT 1 FROM cq_companion_custody WHERE bot_guid = {} AND owner_guid = {} "
            "AND module = 'cq' AND state = 'retiring'", botGuid, ownerGuid));
    }

    static bool IsControlledCompanion(Player* bot, Player* owner)
    {
#ifdef MOD_PLAYERBOTS
        if (!bot || !owner || bot == owner || !bot->IsInWorld() || !bot->GetSession() || !bot->GetSession()->IsBot())
            return false;
        // The personal manager is assigned at bot login. Group leadership and
        // the AI's current follow target do not establish control of the body.
        PlayerbotMgr* manager = PlayerbotsMgr::instance().GetPlayerbotMgr(owner);
        return manager && manager->GetPlayerBot(bot->GetGUID()) == bot;
#else
        return false;
#endif
    }

    static bool HasSupplyAuthority(Player* bot, Player* owner, bool authoritative)
    {
        if (!IsControlledCompanion(bot, owner))
            return false;
        uint32 const botGuid = bot->GetGUID().GetCounter();
        uint32 const ownerGuid = owner->GetGUID().GetCounter();
        if (std::any_of(g_pendingRelease.begin(), g_pendingRelease.end(), [botGuid](PendingRelease const& pending)
            { return pending.botGuid == botGuid; }))
            return false;
        if (!authoritative)
        {
            std::vector<uint32> bound;
            return FindCompanions(ownerGuid, bound) && std::find(bound.begin(), bound.end(), botGuid) != bound.end();
        }
        return HasCurrentCustody(ownerGuid, botGuid, true);
    }

    /// Finish a release that was already committed as `retiring`: mark the reservation a
    /// tombstone and drop its binding so the owner's slot is free without an offline
    /// window. The body itself is untouched - never reset, renamed, deleted or recycled -
    /// and the whole thing is refused unless the row is still `retiring`, still matches the
    /// verified body, and that body is offline.
    bool FinalizeRetirementInline(uint32 ownerGuid, uint32 botGuid, std::string const& name)
    {
        std::string escaped = name;
        CharacterDatabase.EscapeString(escaped);
        auto trans = CharacterDatabase.BeginTransaction();
        trans->Append("UPDATE cq_companion_custody x JOIN characters c ON c.guid = x.bot_guid "
            "JOIN characters o ON o.guid = x.owner_guid AND o.guid <> c.guid "
            "SET x.state = 'retired' "
            "WHERE x.module = 'cq' AND x.state = 'retiring' AND x.tracking = 0 "
            "AND x.bot_guid = {} AND x.owner_guid = {} AND c.account = x.body_account "
            "AND c.online = 0 AND BINARY x.body_name = BINARY c.name",
            botGuid, ownerGuid);
        trans->Append("SET @cq_retire_changed = ROW_COUNT()");
        trans->Append("DELETE FROM cq_bindings WHERE bot_guid = {} AND owner_guid = {} "
            "AND BINARY companion_name = BINARY '{}' AND @cq_retire_changed > 0",
            botGuid, ownerGuid, escaped);
        CharacterDatabase.DirectCommitTransaction(trans);

        return static_cast<bool>(CharacterDatabase.Query(
            "SELECT 1 FROM cq_companion_custody WHERE bot_guid = {} AND owner_guid = {} "
            "AND module = 'cq' AND state = 'retired'", botGuid, ownerGuid)) &&
            !CharacterDatabase.Query("SELECT 1 FROM cq_bindings WHERE bot_guid = {}", botGuid);
    }

    void CheckPendingReleases()
    {
        auto const now = std::chrono::steady_clock::now();
        if (g_pendingRelease.empty() || now < g_nextReleasePoll)
            return;
        g_nextReleasePoll = now + std::chrono::seconds(1);
        for (auto it = g_pendingRelease.begin(); it != g_pendingRelease.end();)
        {
            PendingRelease const& pending = *it;
            ObjectGuid const key = ObjectGuid::Create<HighGuid::Player>(pending.botGuid);
            bool const busy = ObjectAccessor::FindConnectedPlayer(key) || IsCompanionLoginPending(key) ||
                !CharacterDatabase.Query("SELECT 1 FROM characters WHERE guid = {} AND online = 0", pending.botGuid);
            if (busy && now < pending.deadline)
            {
                ++it;
                continue;
            }
            // Retiring is a tombstone, not a recycle: the body keeps its name and is never
            // reset or handed to another owner. Finishing it here is what frees the owner's
            // slot without an offline maintenance window, so it is gated on an observed
            // logout rather than on a timer alone.
            bool const retired = !busy && FinalizeRetirementInline(pending.ownerGuid, pending.botGuid, pending.name);
            if (retired)
            {
                LOG_INFO("server.loading", "[CompanionQuestSync] {} ({}) retired on release; the slot is free",
                         pending.name, pending.botGuid);
                if (Player* owner = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(pending.ownerGuid));
                    owner && owner->GetSession())
                    ChatHandler(owner->GetSession()).PSendSysMessage(
                        "{} has retired and its slot is free again. Ask me for a new companion whenever you are ready.",
                        pending.name);
                InvalidateCompanionGuid(pending.ownerGuid);
                it = g_pendingRelease.erase(it);
                continue;
            }
            char const* why = busy ? "logout was not confirmed before timeout" : "retirement could not be confirmed";
            LOG_INFO("server.loading", "[CompanionQuestSync] {} ({}) remains quarantined: {}", pending.name, pending.botGuid, why);
            if (Player* owner = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(pending.ownerGuid));
                owner && owner->GetSession())
                ChatHandler(owner->GetSession()).PSendSysMessage("{}: {}. The body stays reserved for offline maintenance.", pending.name, why);
            it = g_pendingRelease.erase(it);
        }
    }

    bool BeginRelease(uint32 ownerGuid, std::string const& ownerName, uint32 botGuid, std::string& error)
    {
        ObjectGuid const key = ObjectGuid::Create<HighGuid::Player>(botGuid);
        if (!HasCurrentCustody(ownerGuid, botGuid, false))
        {
            error = "current ownership and pool provenance could not be verified";
            return false;
        }
        if (g_pendingSummon.count(botGuid) || IsCompanionLoginPending(key))
        {
            error = "a summon or dismiss is still in flight - wait and retry";
            return false;
        }
        Player* bot = ObjectAccessor::FindConnectedPlayer(key);
        Player* owner = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(ownerGuid));
        uint32 const now = static_cast<uint32>(time(nullptr));
        auto const upAt = g_botUpAt.find(botGuid);
        if (bot && (!IsControlledCompanion(bot, owner) ||
                    upAt == g_botUpAt.end() || now - upAt->second < Timing::BotRemoveSettle))
        {
            error = "the body is not safely dismissible by its verified owner - dismiss it and retry";
            return false;
        }
        std::string const name = CompanionNameFor(botGuid);
        if (!MarkCompanionRetiring(ownerGuid, botGuid))
        {
            error = "the durable release reservation was not confirmed";
            return false;
        }
        if (owner)
            CancelBoundDelivery(botGuid, owner);
        g_roleSwitches.erase(botGuid);
        PurgeCompanionState(botGuid);
        InvalidateCompanionGuid(ownerGuid);
        g_pendingRelease.push_back({ ownerGuid, botGuid, name, std::chrono::steady_clock::now() + std::chrono::seconds(15) });
        if (bot)
            DispatchPlayerbots(owner, "remove " + name);
        LOG_INFO("server.loading", "[CompanionQuestSync] {} quarantined {} ({}) for offline retirement", ownerName, name, botGuid);
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

        // Speech drains at the owner level too: a line queued while a companion was out
        // must not wait for a companion to come back out before it may speak or be
        // dropped as stale - each entry resolves its own speaker, and stale entries drop.
        FlushWhisperQueue(owner, nullptr);

        if (bots.empty())
            return aggregate;

        // A companion that is not in the world must not hold a party slot. This has to run for
        // *every* bound companion before anything tries to resolve one - the resolver returns
        // early for an offline body, which is exactly the case that needs tidying, so putting
        // it behind the resolver meant it never ran at all.
        for (uint32 const botGuid : bots)
        {
            Group* group = owner->GetGroup();
            if (!group)
                break;

            ObjectGuid const guid = ObjectGuid::Create<HighGuid::Player>(botGuid);
            if (!group->IsMember(guid) || ObjectAccessor::FindConnectedPlayer(guid) || IsCompanionLoginPending(guid) ||
                !HasCurrentCustody(owner->GetGUID().GetCounter(), botGuid, true))
                continue;

            group->RemoveMember(guid);

            LOG_INFO("server.loading", "[CompanionQuestSync] companion {} was not in the world - "
                                       "removed it from {}'s group", botGuid, owner->GetName());
        }

        aggregate.hasCompanion = true;
        g_multiCompanion[owner->GetGUID().GetCounter()] = bots.size() > 1;

        for (uint32 const botGuid : bots)
            g_multiCompanionBySpeaker[botGuid] = bots.size() > 1;

        for (uint32 const botGuid : bots)
        {
            SyncReport const one = SyncCompanionFor(owner, botGuid);

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

        // Publish a single addon snapshot after all companion hand-in attempts.
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
    // that cannot be summoned, and two owners can end up pointed at one body. Treat all of
    // that as evidence for an operator, not permission to delete a reservation or reset a body.
    // -----------------------------------------------------------------------

    struct IntegrityReport
    {
        uint32 rows = 0;
        uint32 danglingOwner = 0;
        uint32 danglingBot = 0;
        uint32 duplicate = 0;
        uint32 doubleBound = 0;      // bound in this table and in mod-wowlegends' table
        uint32 ledgerOrphans = 0;
        std::vector<std::string> notes;
    };

    /// Report suspicious references; no automatic repair can turn them into free stock.
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
        if (CharacterTableExists("wowlegends_companion"))
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

        }

        // Release-history rows are diagnostic only. Never auto-delete reservations,
        // rename a body, or mutate guild/party state from historical pool identity.
        if (g_cfg.poolTable == "cq_pool" && CharacterTableExists("cq_pool"))
        {
            if (QueryResult ledger = CharacterDatabase.Query(
                    "SELECT bot_guid FROM cq_pool WHERE bot_guid NOT IN (SELECT guid FROM characters)"))
            {
                do
                {
                    ++out.ledgerOrphans;
                    out.notes.push_back("ledger entry for missing body " + std::to_string(ledger->Fetch()[0].Get<uint32>()));
                } while (ledger->NextRow());
            }
        }
        if (fix)
            out.notes.push_back("No automatic repairs: review reservations and history offline.");

        return out;
    }

    /// Periodic custody diagnostics. Historical reservations need offline review.
    void SweepIntegrity()
    {
        static uint32 nextRun = 0;

        uint32 const now = static_cast<uint32>(time(nullptr));
        if (nextRun && now < nextRun)
            return;

        nextRun = now + 600;

        IntegrityReport const report = RunIntegrity(false);

        if (g_cfg.debug && (report.danglingOwner || report.danglingBot || report.duplicate || report.doubleBound))
            LOG_WARN("server.loading", "[CompanionQuestSync] integrity: review {} binding rows offline "
                     "({} owner gone, {} body gone, {} duplicate, {} shared)",
                     report.rows, report.danglingOwner, report.danglingBot, report.duplicate, report.doubleBound);
    }

    /// Keep the owner's summoned companions at the preset spacing.
    /// Puts each companion into the formation its role asks for, and speaks only on change.
    ///
    /// No motion is generated here any more. playerbots' formation strategy owns movement -
    /// including in combat, which is the point - and re-asserting a formation a bot already has
    /// would only cost a whisper. It is re-sent on change, after a summon, and on the slow
    /// reconcile, because a bot can be recreated with its own persisted formation.
    void ApplyFormation(Player* owner)
    {
        if (!owner || !owner->IsInWorld() || !owner->IsAlive())
            return;

        std::vector<uint32> bots;
        if (!FindCompanions(owner->GetGUID().GetCounter(), bots))
            return;

        for (uint32 const botGuid : bots)
        {
            Player* bot = FindCompanionPlayer(botGuid);
            if (!bot || !bot->IsInWorld() || !IsControlledCompanion(bot, owner))
                continue;

            std::string const wanted = EffectiveFormation(owner, bot);
            std::string const target = wanted.empty() ? std::string("chaos") : wanted;
            auto const applied = g_formationApplied.find(botGuid);
            if (applied != g_formationApplied.end() && applied->second == target)
                continue;

            PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
            if (!ai)
                continue;

            ai->HandleCommand(CHAT_MSG_WHISPER, "formation " + target, owner);
            g_formationApplied[botGuid] = target;
        }
    }

#include "CombatTactics.inc"

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
        g_tacticsColumn = 0;
#ifdef MOD_PLAYERBOTS_INITIALIZATION_POLICY_V1
        if (g_nativeInitializationVetoRegistered)
            ProbeNativeCustodySchema();
#endif
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
        ApplyDeliveryReceipts(); // a core-accepted transaction may finish after disable
        _tacticsAccumulator += diff;
        if (_tacticsAccumulator >= 2000)
        {
            _tacticsAccumulator = 0;
            UpdateCombatTactics();
        }
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
            ApplyPendingTrades();

            for (auto const& pair : sWorldSessionMgr->GetAllSessions())
            {
                WorldSession* session = pair.second;
                if (!session || session->IsBot() || !session->GetPlayer())
                    continue;

                Player* player = session->GetPlayer();

                // Transport has its own cadence even when quest auto-sync is disabled.
                SendHeartbeat(player);
                ApplyFormation(player);   // spacing only; combat hands movement back
                ReconcileRoles(player);
                ReconcileEngagement(player);

                std::vector<uint32> bots;
                if (!FindCompanions(player->GetGUID().GetCounter(), bots))
                    continue;

                uint32 const nowTs = static_cast<uint32>(time(nullptr));
                ObserveTaxiLanding(player, nowTs);

                for (uint32 const botGuid : bots)
                {
                    Player* bot = FindCompanionPlayer(botGuid);
                    if (!IsControlledCompanion(bot, player))
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

                    if (GatherFromGround(player, bot, gathered) && g_cfg.debug >= 2)
                        LOG_INFO("server.loading", "[CompanionQuestSync] requested ground loot from {}", gathered);
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
        g_custodyReady = false;
        g_starterBagsReady = false;
        g_tableReady = g_cfg.enable && CheckBindingTable();
        CancelPendingDeliveries();
        if (!g_cfg.enable)
            return;

        g_custodyReady = g_tableReady && g_cfg.bindingTable == "cq_bindings" && CustodyTablesTransactional() &&
            static_cast<bool>(CharacterDatabase.Query("SHOW COLUMNS FROM cq_bindings LIKE 'companion_name'"));
        if (g_tableReady && !g_custodyReady)
            LOG_WARN("server.loading", "[CompanionQuestSync] lifecycle disabled: install custody schema and use cq_bindings");
#ifdef MOD_PLAYERBOTS_INITIALIZATION_POLICY_V1
        g_starterBagsReady = g_custodyReady && g_nativeInitializationVetoRegistered &&
            g_nativeCustodySchema == NativeCustodySchema::Present && static_cast<bool>(CharacterDatabase.Query(
            "SELECT 1 FROM information_schema.columns WHERE table_schema = DATABASE() "
            "AND table_name = 'cq_companion_custody' AND column_name = 'starter_bag_attempt'"));
#endif
        if (g_custodyReady && !g_starterBagsReady && (!_announced || reload))
            LOG_WARN("server.loading", "[CompanionQuestSync] starter bags disabled: require Playerbots initialization policy and companion-starter-bags.sql; claims remain available");
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
    uint32 _tacticsAccumulator = 0;

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

    class CommandReplyScope
    {
    public:
        explicit CommandReplyScope(ChatHandler* handler) : previous(g_commandReplyOwner)
        {
            Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
            g_commandReplyOwner = player ? player->GetGUID().GetCounter() : 0;
        }

        ~CommandReplyScope() { g_commandReplyOwner = previous; }

        CommandReplyScope(CommandReplyScope const&) = delete;
        CommandReplyScope& operator=(CommandReplyScope const&) = delete;

    private:
        uint32 const previous;
    };

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
    /// mod-wowlegends, and ledger rows for bodies that are gone. This is read-only;
    /// the old `fix` argument is accepted but never deletes historical reservations.
    static bool HandleAdminIntegrity(ChatHandler* handler, char const* args)
    {
        std::string const mode = args ? TrimArg(args) : std::string();
        bool const fix = (mode == "fix");

        IntegrityReport const report = RunIntegrity(fix);

        handler->PSendSysMessage("|cff20ff20Companion quest sync|r: integrity over {} binding row(s), read only", report.rows);

        handler->PSendSysMessage("  |cffaaaaaaowners gone: {}   bodies gone: {}   bound twice: {}   "
                                 "also in wowlegends_companion: {}   ledger orphans: {}|r",
                                 report.danglingOwner, report.danglingBot, report.duplicate,
                                 report.doubleBound, report.ledgerOrphans);

        for (std::string const& note : report.notes)
            handler->PSendSysMessage("  |cffff8000-|r {}", note);

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

        uint32 const botGuid = ResolveOwnedCompanion(ownerGuid, a[1]);
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

        handler->PSendSysMessage("|cff20ff20Companion quest sync|r: {} (body {}) quarantined for offline maintenance for {}.",
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

        if (Player* player = (handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr))
        {
            Player* speaker = nullptr;
            for (uint32 const botGuid : bots)
                if (Player* b = FindCompanionPlayer(botGuid))
                    if (!speaker || b->GetDistance(player) < speaker->GetDistance(player))
                        speaker = b;
            if (speaker)
                SayToOwner(speaker, player,
                           total ? ("I gathered the rest of what lies on the ground - " +
                                    std::to_string(total) + " item(s); the hand-in runs on the next pass.")
                                 : "Nobody owes anything that lies on the ground right now.", 30);
        }
    }

    /// .companionsync role <companion> [auto|tank|healer|dps|support]
    ///
    /// Queue one verified loadout switch; the previous role is authoritative until it lands.
    static bool HandleRoleCommand(ChatHandler* handler, char const* args)
    {
        CommandReplyScope replies(handler);
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        uint32 const ownerGuid = player->GetGUID().GetCounter();
        std::vector<std::string> const a = SplitArgs(args, 2);

        if (a.size() < 2)
        {
            std::string const line = RoleLine(ownerGuid);
            handler->PSendSysMessage("|cffff8000role|r: {}. Use role <companion> auto | tank | healer | dps | support.",
                                     line.empty() ? "no companions with roles yet" : line);
            return true;
        }

        std::string const role = FormationLower(a[1]);
        if (!RoleKnown(role))
        {
            handler->PSendSysMessage("|cffff8000role|r: '{}' is not one of auto, tank, healer, dps, support.", a[1]);
            return true;
        }

        uint32 const botGuid = ResolveOwnedCompanion(ownerGuid, a[0]);
        if (!botGuid)
        {
            handler->PSendSysMessage("|cffff8000role|r: you have no companion called '{}'.", a[0]);
            return true;
        }

        if (!RoleColumnPresent())
        {
            handler->PSendSysMessage("|cffff8000role|r: roles are not set up yet on this realm "
                                     "(import sql/companion-roles.sql).");
            return true;
        }

        if (!HasCurrentCustody(ownerGuid, botGuid, true))
            return true;

        RoleSwitchState& state = g_roleSwitches[botGuid];
        if (state.ownerGuid != ownerGuid)
            state = RoleSwitchState{ ownerGuid };
        if (CompanionRole(botGuid) == role)
        {
            state.pending.clear();             // choosing the applied role cancels a queued switch
            return true;
        }
        if (state.pending == role)
            return true;

        state.pending = role;
        ProcessPendingRoleSwitch(player, FindCompanionPlayer(botGuid), botGuid);
        if (auto const queued = g_roleSwitches.find(botGuid);
            queued != g_roleSwitches.end() && !queued->second.pending.empty())
            handler->PSendSysMessage("{} will switch to {} when ready; the current role is unchanged.", a[0], role);
        return true;
    }

    /// .companionsync quiet <companion> [on|off]
    ///
    /// Threat is not taunts: a companion that cannot taunt still builds threat with its damage.
    /// "quiet" enables playerbots' own threat reduction, at the price of damage, so it is off
    /// unless asked for.
    static bool HandleQuietCommand(ChatHandler* handler, char const* args)
    {
        CommandReplyScope replies(handler);
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        uint32 const ownerGuid = player->GetGUID().GetCounter();
        std::vector<std::string> const a = SplitArgs(args, 2);

        if (a.size() < 2)
        {
            handler->PSendSysMessage("|cffff8000quiet|r: use quiet <companion> on | off.");
            return true;
        }

        std::string const wanted = FormationLower(a[1]);
        if (wanted != "on" && wanted != "off")
        {
            handler->PSendSysMessage("|cffff8000quiet|r: '{}' is not on or off.", a[1]);
            return true;
        }

        uint32 const botGuid = ResolveOwnedCompanion(ownerGuid, a[0]);
        if (!botGuid)
        {
            handler->PSendSysMessage("|cffff8000quiet|r: you have no companion called '{}'.", a[0]);
            return true;
        }

        bool const on = wanted == "on";
        if (!SetCompanionQuiet(ownerGuid, botGuid, on))
        {
            handler->PSendSysMessage("|cffff8000quiet|r: could not store that (is sql/companion-quiet.sql imported?).");
            return true;
        }

        if (Player* bot = FindCompanionPlayer(botGuid))
        {
            if (std::string const missed = ApplyRoleStrategies(bot); !missed.empty())
                g_roleNotes[botGuid] = missed;
            g_roleApplyAt[botGuid] = static_cast<uint32>(time(nullptr));
        }

        handler->PSendSysMessage("|cff20ff20{}|r is {}quiet.{}", a[0], on ? "|cffffffff" : "",
                                 on ? "|r It will work at holding threat down, and hit softer."
                                    : "|r It fights at full strength.");
        SendRoleAndSpecLines(player, ownerGuid);
        return true;
    }

    /// .companionsync inspect [companion]
    ///
    /// Everything this module believes about a companion, in one place: the setting, the
    /// strategies that are actually active, what it has edited, and why anything is missing.
    /// Written because every bug so far was diagnosed by reading playerbots' source.
    static std::string StrategySnapshot(PlayerbotAI* ai)
    {
        struct Probe
        {
            char const* name;
            BotState state;
        };

        Probe const probes[] = {
            { "tank",    BOT_STATE_COMBAT },
            { "pull",    BOT_STATE_NON_COMBAT },
            { "heal",    BOT_STATE_COMBAT },
            { "threat",  BOT_STATE_COMBAT },
            { "passive", BOT_STATE_COMBAT },
            { "bear",    BOT_STATE_COMBAT },
            { "cat",     BOT_STATE_COMBAT },
        };

        std::string out;
        for (Probe const& probe : probes)
        {
            if (!out.empty())
                out += "  ";
            out += std::string(probe.name) + (ai->HasStrategy(probe.name, probe.state) ? "=on" : "=off");
        }
        return out;
    }

    static std::string LedgerSnapshot(uint32 botGuid)
    {
        auto const mine = g_roleApplied.find(botGuid);
        if (mine == g_roleApplied.end() || mine->second.empty())
            return "none";

        std::string out;
        for (AppliedEdit const& edit : mine->second)
        {
            if (!out.empty())
                out += ", ";
            out += std::string(edit.added ? "+" : "-") + edit.name;
        }
        return out;
    }

    static bool HandleInspectCommand(ChatHandler* handler, char const* args)
    {
        CommandReplyScope replies(handler);
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        uint32 const ownerGuid = player->GetGUID().GetCounter();
        std::vector<std::string> const a = SplitArgs(args, 1);

        std::vector<uint32> bots;
        if (!FindCompanions(ownerGuid, bots))
        {
            handler->PSendSysMessage("|cffff8000inspect|r: you have no companions.");
            return true;
        }

        uint32 onlyGuid = 0;
        if (!a.empty() && !a[0].empty())
        {
            onlyGuid = ResolveOwnedCompanion(ownerGuid, a[0]);
            if (!onlyGuid)
            {
                handler->PSendSysMessage("|cffff8000inspect|r: you have no companion called '{}'.", a[0]);
                return true;
            }
        }

        for (uint32 const botGuid : bots)
        {
            if (onlyGuid && botGuid != onlyGuid)
                continue;

            Player* bot = FindCompanionPlayer(botGuid);
            std::string const name = bot ? bot->GetName() : std::string("(not summoned)");
            handler->PSendSysMessage("|cffffd200{}|r  role |cffffffff{}|r  engage |cffffffff{}|r  quiet {}",
                                     name, CompanionRole(botGuid), CompanionEngage(botGuid),
                                     CompanionQuiet(botGuid) ? "|cffffffffon|r" : "off");

            // Distance is the one setting with no other visible record: a bad role shows up in
            // the strategies below, a bad formation only shows up as companions standing in the
            // wrong place. Name it, with the leash it implies and the realm-wide pitch-in radius
            // it competes with.
            if (bot)
            {
                std::string const formation = EffectiveFormation(player, bot);
                uint32 const pitchIn = sConfigMgr->GetOption<uint32>("WowLegends.BotPitchIn.Radius", 20);

                if (formation.empty())
                    handler->PSendSysMessage("  formation: off (distance left to playerbots)");
                else
                    handler->PSendSysMessage("  formation: {} (leash {:.0f} yd, pitch-in {} yd)",
                                             formation, FormationLeash(formation), pitchIn);
            }

            PlayerbotAI* ai = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
            if (ai)
                handler->PSendSysMessage("  active: {}", StrategySnapshot(ai));
            else
                handler->PSendSysMessage("  active: no AI (not summoned, or not a bot)");

            if (bot && g_cfg.specFollowRole && CompanionRole(botGuid) != "auto")
            {
                int const want = RoleSpecTab(bot->getClass(), CompanionRole(botGuid));
                int const have = CurrentSpecTab(bot);
                if (want >= 0)
                {
                    std::string const haveName = have >= 0 ? kSpecTreeNames[bot->getClass()][have]
                                                           : std::string("no points yet");
                    std::string mode = g_cfg.specFollowRole >= 2 ? "" : " (dry run)";
                    handler->PSendSysMessage("  spec: {} |cff9d9d9drole {} wants {}{}|r",
                                             haveName, CompanionRole(botGuid),
                                             kSpecTreeNames[bot->getClass()][want], mode);
                }
            }

            if (bot && CompanionRole(botGuid) != "auto")
                handler->PSendSysMessage("  kit: {} |cff9d9d9d({})|r", KitShapeText(bot),
                                         KitNeedFor(bot->getClass(), CompanionRole(botGuid)) == KitNeed::None
                                             ? "the role asks nothing of its shape"
                                             : "the shape this role needs");

            handler->PSendSysMessage("  edits: {}", LedgerSnapshot(botGuid));

            auto const roleNote = g_roleNotes.find(botGuid);
            if (roleNote != g_roleNotes.end())
                handler->PSendSysMessage("  |cffffd200could not establish:|r {}", roleNote->second);

            auto const engageNote = g_engageNotes.find(botGuid);
            if (engageNote != g_engageNotes.end())
                handler->PSendSysMessage("  |cffffd200engagement:|r {}", engageNote->second);

            auto const withheldSpells = g_spellsWithheld.find(botGuid);
            if (withheldSpells == g_spellsWithheld.end() || withheldSpells->second.empty())
            {
                // A muted spell is a cooldown, so the silence here is honest - but a spell that
                // an older build removed from the spellbook is not, and it has to be named
                // because only a retrain puts it back.
                std::string absent;
                for (GapCloser const& closer : kGapClosers)
                {
                    if (bot->getClass() != closer.klass || bot->HasSpell(closer.spell))
                        continue;
                    if (!absent.empty())
                        absent += ", ";
                    absent += closer.name;
                }

                if (!absent.empty())
                    handler->PSendSysMessage("  gap closers: {} missing from the spellbook (an older "
                                             "build removed it) - retrain to restore", absent);
            }
            else
            {
                std::string names;
                for (uint32 const spell : withheldSpells->second)
                {
                    char const* label = "a spell";
                    for (GapCloser const& closer : kGapClosers)
                        if (closer.spell == spell)
                            label = closer.name;
                    if (!names.empty())
                        names += ", ";
                    names += label;
                }
                handler->PSendSysMessage("  gap closers withheld: {}", names);
            }

            auto const petOff = g_petTauntOff.find(botGuid);
            if (petOff != g_petTauntOff.end() && !petOff->second.empty())
                handler->PSendSysMessage("  pet taunts off: {} ability(ies)", uint32(petOff->second.size()));
        }

        handler->PSendSysMessage("|cff808080role sets what it may do, engage when it joins, quiet how "
                                 "much threat it makes.|r");
        return true;
    }

    /// .companionsync engage <companion> [auto|mine|joined|never]
    ///
    /// Whether the companion gets into a fight at all. "mine" hands target selection to the
    /// player: the bot only ever attacks what the player is attacking.
    static bool HandleEngageCommand(ChatHandler* handler, char const* args)
    {
        CommandReplyScope replies(handler);
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        uint32 const ownerGuid = player->GetGUID().GetCounter();
        std::vector<std::string> const a = SplitArgs(args, 2);

        if (a.size() < 2)
        {
            std::string line;
            std::vector<uint32> bots;
            if (FindCompanions(ownerGuid, bots))
                for (uint32 const botGuid : bots)
                {
                    Player* bot = FindCompanionPlayer(botGuid);
                    std::string name = bot ? bot->GetName() : std::string("a companion");
                    if (!line.empty())
                        line += "|";
                    line += name + "=" + CompanionEngage(botGuid);
                }
            handler->PSendSysMessage("|cffff8000engage|r: {}. Use engage <companion> auto | mine | joined | never.",
                                     line.empty() ? "auto everywhere" : line);
            return true;
        }

        std::string const policy = EngageOrAuto(a[1]);
        if (FormationLower(a[1]) != policy)
        {
            handler->PSendSysMessage("|cffff8000engage|r: '{}' is not one of auto, mine, joined, never.", a[1]);
            return true;
        }

        uint32 const botGuid = ResolveOwnedCompanion(ownerGuid, a[0]);
        if (!botGuid)
        {
            handler->PSendSysMessage("|cffff8000engage|r: you have no companion called '{}'.", a[0]);
            return true;
        }

        if (!SetCompanionEngage(ownerGuid, botGuid, policy))
        {
            handler->PSendSysMessage("|cffff8000engage|r: could not store that (is sql/companion-engage.sql imported?).");
            return true;
        }

        switch (policy[0])
        {
            case 'a':
                handler->PSendSysMessage("|cff20ff20{}|r fights as its own AI sees fit again.", a[0]);
                break;
            case 'm':
                handler->PSendSysMessage("|cff20ff20{}|r will fight only what |cffffffffyou|r are fighting.", a[0]);
                break;
            case 'j':
                handler->PSendSysMessage("|cff20ff20{}|r joins fights you are already in, and starts none.", a[0]);
                break;
            default:
                handler->PSendSysMessage("|cff20ff20{}|r stays out of combat (it will still defend itself).", a[0]);
                break;
        }

        if (Player* bot = FindCompanionPlayer(botGuid))
        {
            ApplyEngagement(player, bot);
            auto const note = g_engageNotes.find(botGuid);
            if (note != g_engageNotes.end())
                handler->PSendSysMessage("  |cffffd200{}|r - this build cannot hold companions out of fights.",
                                         note->second);
        }

        SendRoleAndSpecLines(player, ownerGuid);
        return true;
    }

    static bool HandleTacticsCommand(ChatHandler* handler, char const* args)
    {
        CommandReplyScope replies(handler);
        Player* owner = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!owner)
            return false;
        auto const parts = SplitArgs(args, 3);
        uint32 const ownerGuid = owner->GetGUID().GetCounter();
        if (parts.empty())
        {
            SendCombatTactics(owner, ownerGuid);
            return true;
        }
        uint8 const place = parts.size() == 3 && parts[1] == "tank" ? 1 :
                            parts.size() == 3 && parts[1] == "gap" ? 3 :
                            parts.size() == 3 && parts[1] == "flank" ? 9 : 0;
        if (!place || (parts[2] != "default" && parts[2] != "off" &&
                      parts[2] != (place == 3 ? "safe" : "on")))
        {
            handler->PSendSysMessage("Use tactics <companion> tank|gap|flank default|off|on (use safe for gap).");
            return true;
        }
        uint32 const guid = ResolveOwnedCompanion(ownerGuid, parts[0]);
        auto const choice = parts[2] == "default" ? TacticChoice::Default :
                            parts[2] == "off" ? TacticChoice::Off : TacticChoice::On;
        if (!g_cfg.enable || !guid || !SetCombatTactic(ownerGuid, guid, place, choice))
        {
            handler->PSendSysMessage("Combat tactic not saved. Check the companion and the combat tactics migration.");
            return true;
        }
        SendCombatTactics(owner, ownerGuid);
        return true;
    }

    /// .companionsync formation [off|tight|loose|spread]
    ///
    /// Per owner per session: the preset is spacing behind *you*, so it is not a property
    /// of any one companion, and it resets to the config default at the next login.
    static bool HandleFormationCommand(ChatHandler* handler, char const* args)
    {
        CommandReplyScope replies(handler);
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        std::string const current = FormationFor(player);
        std::vector<std::string> const a = SplitArgs(args, 1);

        if (a.empty())
        {
            handler->PSendSysMessage("|cffff8000formation|r: {} (leash {:.0f} yd). Use auto | close | "
                                     "tight | loose | spread | lead | off.",
                                     current, FormationLeash(FormationForPreset(current)));
            return true;
        }

        std::string const wanted = FormationPreset(FormationLower(a[0]));
        if (wanted.empty())
        {
            handler->PSendSysMessage("|cffff8000formation|r: '{}' is not one of auto, close, tight, loose, "
                                     "spread, lead, off.", a[0]);
            return true;
        }

        g_formation[player->GetGUID().GetCounter()] = wanted;
        ApplyFormation(player);              // take effect now, not on the next tick

        if (wanted == "off")
            handler->PSendSysMessage("Formation off - distance is left entirely to playerbots.");
        else if (wanted == "auto")
            handler->PSendSysMessage("Formation |cff20ff20auto|r - each companion takes the shape its role "
                                     "asks for: a tank leads, a healer keeps its distance, melee closes.");
        else
            handler->PSendSysMessage("Formation |cff20ff20{}|r - every companion takes playerbots' "
                                     "|cffffffff{}|r formation, and keeps its own movement in combat.",
                                     wanted, FormationForPreset(wanted));

        SendProtocolLine(player, "Formation " + wanted);
        return true;
    }

    static bool HandleCatchUpCommand(ChatHandler* handler, char const* /*args*/)
    {
        CommandReplyScope replies(handler);
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
            { "formation", HandleFormationCommand, SEC_PLAYER, Console::No },
            { "role",      HandleRoleCommand,      SEC_PLAYER, Console::No },
            { "engage",    HandleEngageCommand,    SEC_PLAYER, Console::No },
            { "tactics",   HandleTacticsCommand,   SEC_PLAYER, Console::No },
            { "quiet",     HandleQuietCommand,     SEC_PLAYER, Console::No },
            { "inspect",   HandleInspectCommand,   SEC_PLAYER, Console::No },
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
            { "delivery", HandleDeliveryCommand, SEC_PLAYER, Console::No },
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

    /// Re-enable tracking for an existing, verified companion. Playerbot control alone
    /// includes ordinary alts and is not evidence of ownership by this module.
    static bool HandleBindCommand(ChatHandler* handler, char const* args)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;
        uint32 const ownerGuid = player->GetGUID().GetCounter();
        uint32 const botGuid = ResolveOwnedCompanion(ownerGuid, TrimArg(args));
        if (!g_cfg.enable || !botGuid || !SetCompanionTracking(ownerGuid, botGuid, true))
        {
            handler->SendSysMessage("Only a verified companion you already own can be tracked. Ordinary alts and unreviewed bindings are not eligible.");
            return true;
        }
        InvalidateCompanionGuid(ownerGuid);
        SendCompSync(player, true);
        handler->SendSysMessage("Companion quest tracking enabled.");
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

        uint32 const botGuid = ResolveOwnedCompanion(ownerGuid, name);
        if (!g_cfg.enable || !botGuid || !SetCompanionTracking(ownerGuid, botGuid, false))
        {
            handler->SendSysMessage("No verified companion by that name; the reservation was not changed.");
            return true;
        }
        CancelBoundDelivery(botGuid, player);
        g_roleSwitches.erase(botGuid);
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

    /// .companionsync release <name> - quarantine a verified companion for offline retirement.
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

        uint32 const botGuid = g_cfg.enable ? ResolveOwnedCompanion(player->GetGUID().GetCounter(), name) : 0;
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

        SendCompSync(player, true);
        handler->PSendSysMessage("{} is retiring. Its slot frees once it has left the world.", name);
        return true;
    }

    /// .companionsync summon [name] - bring a companion out, or every companion that is not
    /// out. No argument means "the whole group", which is what a player wants after a wipe.
    static bool HandleSummonCommand(ChatHandler* handler, char const* args)
    {
        CommandReplyScope replies(handler);
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
            if (!IsControlledCompanion(bot, player) || !HasCurrentCustody(player->GetGUID().GetCounter(),
                    bot->GetGUID().GetCounter(), true))
                return "not under your verified control";
            if (!bot->IsAlive())
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
            ObjectGuid const key = ObjectGuid::Create<HighGuid::Player>(botGuid);
            if (!HasCurrentCustody(ownerGuid, botGuid, true) || g_pendingSummon.count(botGuid) ||
                ObjectAccessor::FindConnectedPlayer(key) || IsCompanionLoginPending(key))
            {
                handler->SendSysMessage("A companion is not available to summon right now.");
                continue;
            }
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
    /// .companionsync trade <name> - the card's Trade button.
    ///
    /// The companion opens a trade with the owner and fills it with its surplus: every
    /// gathered mat and produced item above the five it keeps for itself, biggest stacks
    /// first, six slots (the trade window's own limit). The owner accepts; the bot's
    /// accept-trade trigger answers on its side. Nothing else in its bags - gear, quest
    /// items, food - is ever offered.
#include "SupplyPicker.inc"

    static bool HandleDeliveryCommand(ChatHandler* handler, char const* args)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;
        std::string const text = TrimArg(args);
        if (HandleNamedSupply(player, text) || HandleModernCollect(player, text))
            return true;
        if (!g_cfg.enable || !g_tableReady)
            return false;
        uint32 const ownerGuid = player->GetGUID().GetCounter();
        std::vector<uint32> bound;
        if (!FindCompanions(ownerGuid, bound))
        {
            handler->SendSysMessage("No companions bound.");
            return true;
        }
        if (text == "clear")
        {
            g_requestedSupplies.erase(ownerGuid);
            handler->SendSysMessage("Companion supply requests cleared.");
            return true;
        }
        if (text.rfind("request ", 0) == 0)
        {
            std::string const value = text.substr(8);
            uint32 entry = 0;
            auto const parsed = std::from_chars(value.data(), value.data() + value.size(), entry);
            ItemTemplate const* item = parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size()
                ? sObjectMgr->GetItemTemplate(entry) : nullptr;
            bool supported = false;
            if (item && item->Class == ITEM_CLASS_CONSUMABLE && player->CanUseItem(item) == EQUIP_ERR_OK)
                for (uint32 guid : bound)
                    if (Player* bot = FindCompanionPlayer(guid))
                        supported = supported || IsCraftedSupply(ClassProfessionPair(bot->getClass()).second, entry);
            if (!supported)
            {
                handler->SendSysMessage("Use an item ID for a consumable you can use that one of your summoned companions can make.");
                return true;
            }
            g_requestedSupplies[ownerGuid].insert(entry);
            handler->PSendSysMessage("Your companions will set aside spare {} for this login, up to five including what you already have.", item->Name1);
            return true;
        }
        if (text.rfind("collect ", 0) == 0)
        {
            uint32 const guid = ResolveCompanion(player, text.substr(8), bound);
            Player* bot = guid ? FindCompanionPlayer(guid) : nullptr;
            if (!bot || !CanTradeSurplusNow(bot, player) || bot->GetTradeData() || player->GetTradeData())
                handler->SendSysMessage("Stand beside a summoned companion when both of you are out of combat and not trading.");
            else
                OpenTradeSurplus(bot, player, /*announce=*/true, /*curated=*/true);
            return true;
        }
        if (!text.empty() && text != "preview")
        {
            handler->SendSysMessage("Usage: .companionsync delivery [preview|collect <name>|request <item ID>|clear]");
            return true;
        }
        bool found = false;
        for (uint32 guid : bound)
        {
            Player* bot = FindCompanionPlayer(guid);
            if (!bot || !bot->IsInWorld())
                continue;
            for (auto const& [stack, quantity] : PlanDelivery(bot, player, true))
                if (Item* item = bot->GetItemByGuid(stack))
                {
                    found = true;
                    handler->PSendSysMessage("{}: {} x{} ready for handoff.", bot->GetName(), item->GetTemplate()->Name1, quantity);
                }
        }
        if (!found)
            handler->SendSysMessage("No clear armor upgrades or requested spare supplies are ready.");
        return true;
    }

    static bool HandleTradeCommand(ChatHandler* handler, char const* args)
    {
        CommandReplyScope replies(handler);
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

        OpenTradeSurplus(bot, player, /*announce=*/true, /*curated=*/false);
#endif

        return true;
    }

    /// .companionsync produce [name] - the card's Produce button.
    ///
    /// One click makes the companion work its own gathering: smelt its ore, cut its
    /// stones, brew what its herbs are for. Shop-bought reagents are granted in small
    /// stacks (the module standing in for the shopping trip); gathered reagents are
    /// waited for. Products are used in play and requested surplus can be delivered.
    static bool HandleProduceCommand(ChatHandler* handler, char const* args)
    {
        CommandReplyScope replies(handler);
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

        if (bot->IsInCombat() || player->IsInCombat())
        {
            handler->PSendSysMessage("|cffc94d{}|r: Ask me to craft once we're out of combat.", bot->GetName());
            return true;
        }
        if (bot->GetTradeData() || player->GetTradeData())
        {
            handler->PSendSysMessage("|cffc94d{}|r: Finish the trade first, then ask me to craft.", bot->GetName());
            return true;
        }

#ifdef MOD_PLAYERBOTS
        TeachCraftRecipes(bot, ClassProfessionPair(bot->getClass()).second);
        auto const report = ProduceForCompanion(bot, player);
        std::string summary;
        for (std::string const& line : report)
            summary += (summary.empty() ? "" : ". ") + line;
        handler->PSendSysMessage("|cffc94d{}|r: {}.", bot->GetName(), summary);
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
        CommandReplyScope replies(handler);
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
        CommandReplyScope replies(handler);
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
            if (!HasCurrentCustody(ownerGuid, botGuid, true) || !IsControlledCompanion(FindCompanionPlayer(botGuid), player) ||
                IsCompanionLoginPending(ObjectGuid::Create<HighGuid::Player>(botGuid)) || g_pendingSummon.count(botGuid))
                continue;
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

    /// .companionsync refresh [roster] - refresh cached addon state silently.
    static bool HandleRefreshCommand(ChatHandler* handler, char const* args)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player || !g_cfg.enable)
            return false;

        bool const rosterOnly = TrimArg(args) == "roster";
        if (!rosterOnly)
            SendPinSync(player, false); // legacy panel request
        uint32 const ownerGuid = player->GetGUID().GetCounter();
        uint32 const now = uint32(time(nullptr));
        auto sent = g_lastCompSyncAt.find(ownerGuid);
        bool const force = rosterOnly && (sent == g_lastCompSyncAt.end() || now - sent->second >= 5);
        SendCompSync(player, force);
        return true;
    }

    /// .companionsync pins [v2 [epoch [handin [items]]]] - recover a map snapshot, including an empty one.
    static bool HandlePinsCommand(ChatHandler* handler, char const* args)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        std::string const protocol = TrimArg(args);
        bool const pinItems = protocol == "v2 epoch handin items";
        if (protocol == "v2" || protocol == "v2 epoch" || protocol == "v2 epoch handin" || pinItems)
        {
            uint32 const ownerGuid = player->GetGUID().GetCounter();
            uint32& last = g_pinRequestAt[ownerGuid];
            uint32 const now = uint32(time(nullptr));
            if (last && now - last < 8)
                return true;
            last = now;
            if (!g_cfg.enable || !g_cfg.pinSync)
            {
                SendProtocolLine(player, "Pin set unavailable");
                return true;
            }
            PinStream& stream = g_pinRevisions[ownerGuid];
            if (!stream.epoch)
                stream.epoch = NextPinEpoch();
            stream.tagged = protocol != "v2";
            stream.pinItems = pinItems;
            stream.handInStates = protocol == "v2 epoch handin" || pinItems;
            if (!stream.handInStates)
                SendProtocolLine(player, "Pin set handin 1");
            if (!stream.pinItems)
                SendProtocolLine(player, "Pin set items 1");
            if (!stream.tagged)
                SendProtocolLine(player, fmt::format("Pin set session {}", stream.epoch));
            SendPinSync(player, true);
            return true;
        }

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
        CommandReplyScope replies(handler);
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

                EnderSpot const spot = FindQuestEnder(bot, questId);
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

            // a press is a conversation: the chat list carries the detail, the whisper
            // carries the summary the player actually asked for
            {
                uint32 ready = 0, owing = 0;
                for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
                {
                    uint32 const qid = bot->GetQuestSlotQuestId(slot);
                    if (!qid)
                        continue;
                    Quest const* quest = sObjectMgr->GetQuestTemplate(qid);
                    if (!quest)
                        continue;
                    if (bot->GetQuestStatus(qid) == QUEST_STATUS_COMPLETE)
                        ++ready;
                    else if (!DescribeMissingItems(bot, quest).empty())
                        ++owing;
                }
                SayToOwner(bot, player,
                           ("I carry " + std::to_string(CountQuestLog(bot)) + " quest(s): " +
                            std::to_string(ready) + " ready to hand in, " +
                            std::to_string(owing) + " still owing items - the list is in chat."), 30);
            }
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
                tally.gathered + tally.mailed + tally.sold > 0)
            {
                handler->PSendSysMessage("|cff20ff20Session|r: {} quest(s) accepted, {} completed ({} handed in), {} item(s) gathered, {} posted home in {} letter(s), {} junk stack(s) sold.",
                                         tally.added + tally.onAccept,
                                         tally.completed + tally.handedIn, tally.handedIn,
                                         tally.gathered, tally.mailed, tally.letters, tally.sold);
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
            handler->PSendSysMessage("  |cffffd200level mode 1|r lets the companion sit above you; quests capped below its level ({} - against {} now) are refused. |cffffff00LevelMode 2|r would have kept them level - it raises, and never demotes.",
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
          PLAYERHOOK_ON_LEVEL_CHANGED, PLAYERHOOK_ON_PLAYER_RESURRECT, PLAYERHOOK_ON_MAP_CHANGED }) { }

    /// A hearthstone, a portal or a summon is about to move the player: note it, and let the
    /// fast tick bring the companion along once the destination has finished loading.
    bool OnPlayerBeforeTeleport(Player* player, uint32 mapid, float x, float y, float z,
                                float /*orientation*/, uint32 /*options*/, Unit* /*target*/) override
    {
        if (!g_cfg.enable || !g_cfg.followThroughTeleport || !player || !player->GetSession())
            return true;

        if (player->GetSession()->IsBot())      // a bot's own teleports are not the player's
            return true;

        MapEntry const* destination = sMapStore.LookupEntry(mapid);
        if (!destination || destination->Instanceable() || !player->IsAlive() || IsOnTaxi(player))
            return true;

        std::vector<uint32> bots;
        if (!FindCompanions(player->GetGUID().GetCounter(), bots))
            return true;

        PendingFollow follow{ uint32(time(nullptr)), true, mapid, x, y, z };
        if (player->IsInWorld() && player->GetMap()->IsDungeon())
        {
            follow.sourceMap = player->GetMapId();
            follow.sourceInstance = player->GetInstanceId();
        }
        g_pendingFollow[player->GetGUID().GetCounter()] = follow;

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
        if (!g_custodyReady)
            return;
        QueryResult row = CharacterDatabase.Query("SELECT b.owner_guid" + CurrentCustodyClause(true) +
            fmt::format(" AND b.bot_guid = {}", player->GetGUID().GetCounter()));
        if (!row)
            return;

        Player* owner = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>((*row)[0].Get<uint32>()));
        if (!IsControlledCompanion(player, owner))
            return;

        TeachTrainerContent(player, owner);   // spells, talents - as they unlock
        TeachProfessions(player, owner);      // ceiling, tools, recipes
    }

    void OnPlayerLogin(Player* player) override
    {
        if (!g_cfg.enable || !g_tableReady)
            return;

        if (!player->GetSession() || player->GetSession()->IsBot())
            return;

        ChatHandler handler(player->GetSession());

        // Login state does not depend on chat announcements or map pins.
        SendSyncHello(player);
        g_heartbeatAt[player->GetGUID().GetCounter()] = static_cast<uint32>(time(nullptr));

        if (g_cfg.pinSync)
            SendPinSync(player, true);
        SendCompSync(player, true);
        SendPoolStock(player, true);

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
        if (!player)
            return;

        // Only owners get a tally (the sweep skips bot sessions), so a companion
        // logging out finds nothing and stays quiet.
        uint32 const ownerGuid = player->GetGUID().GetCounter();
        // Nonces are client-session scoped; never replay an old terminal event
        // to the next login. Dropping the future does not cancel the DB task.
        for (auto it = g_deliveryReceipts.begin(); it != g_deliveryReceipts.end();)
            it = it->second.ownerGuid == ownerGuid ? g_deliveryReceipts.erase(it) : std::next(it);
        if (!g_cfg.enable)
            return;

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
        g_taxiInProgress.erase(ownerGuid);
        g_lastPinSync.erase(ownerGuid);
        g_heartbeatAt.erase(ownerGuid);
        g_lastCompSync.erase(ownerGuid);
        g_lastCompSyncAt.erase(ownerGuid);
        g_lastSkillSync.erase(ownerGuid);
        g_lastPoolStock.erase(ownerGuid);
        g_pinSyncAt.erase(ownerGuid);
        g_formation.erase(ownerGuid);
        g_ownerIdle.erase(ownerGuid);
        g_pinRevisions.erase(ownerGuid);
        g_pinRequestAt.erase(ownerGuid);
        g_requestedSupplies.erase(ownerGuid);
        g_supplyCatalogAt.erase(ownerGuid);
        g_supplyActionAt.erase(ownerGuid);
        g_ownerDeliveryMailAt.erase(ownerGuid);
        g_ownerMailPolicyAt.erase(ownerGuid);
        g_lastAutoTrade.erase(ownerGuid);
        g_lastAutoTradeAttempt.erase(ownerGuid);
        g_junkNoticeAt.erase(ownerGuid);

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
            !tally.gathered && !tally.handedIn && !tally.mailed && !tally.sold && !tally.levelFrom)
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
        if (tally.sold)
            summary += ", " + std::to_string(tally.sold) + " junk stack(s) sold";
        if (level.size())
            summary += level;

        g_lastSessionSummary[ownerGuid] = summary;
        CharacterDatabase.Execute(
            "REPLACE INTO cq_digest (owner_guid, summary, stamp) VALUES ({}, '{}', {})",
            ownerGuid, summary, static_cast<uint32>(time(nullptr)));

        LOG_INFO("server.loading",
                 "[CompanionQuestSync] session: {} - {} quest(s) added ({} on accept), {} completed, {} reopened, {} refused, {} item(s) gathered off the ground, {} handed in, {} mailed ({} letter(s)), {} junk stack(s) sold{}",
                 player->GetName(), tally.added + tally.onAccept, tally.onAccept, tally.completed, tally.reopened,
                 tally.refused, tally.gathered, tally.handedIn, tally.mailed, tally.letters, tally.sold, level);
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

        g_pendingFollow[player->GetGUID().GetCounter()] = { uint32(time(nullptr)) };
    }

    void OnPlayerMapChanged(Player* player) override
    {
        if (!g_cfg.enable || !g_cfg.followThroughTeleport || !player || !player->GetSession())
            return;

        if (player->GetSession()->IsBot())
            return;

        uint32 const ownerGuid = player->GetGUID().GetCounter();
        PendingFollow arrival{ uint32(time(nullptr)) };
        if (auto pending = g_pendingFollow.find(ownerGuid); pending != g_pendingFollow.end())
        {
            PendingFollow const& follow = pending->second;
            if (follow.verifyDestination && follow.map == player->GetMapId())
            {
                arrival.map = follow.map;
                arrival.sourceMap = follow.sourceMap;
                arrival.sourceInstance = follow.sourceInstance;
            }
            g_pendingFollow.erase(pending);
        }

        // An actual outdoor arrival confirms a dungeon exit. Any other map
        // transition invalidates the previous follow, including entering an instance.
        if (!player->IsInWorld() || !player->IsAlive() || player->GetMap()->Instanceable() || IsOnTaxi(player))
            return;

        std::vector<uint32> bots;
        if (!FindCompanions(ownerGuid, bots))
            return;

        g_pendingFollow[ownerGuid] = arrival;
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
            if (!IsControlledCompanion(bot, player))
                continue;

            if (bot->IsQuestRewarded(questId))
                continue;

            uint16 slot = UINT16_MAX;
            for (uint16 s = 0; s < MAX_QUEST_LOG_SIZE; ++s)
                if (bot->GetQuestSlotQuestId(s) == questId) { slot = s; break; }

            if (slot == UINT16_MAX)
                continue;

            if (!AbandonCompanionQuest(bot, slot, questId))
                continue;
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
            if (quest->IsRepeatable() || quest->IsSeasonal() || quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_TIMED))
            {
                g_ownerCycleSeen.erase({ botGuid, questId });
                g_addedAt.erase({ botGuid, questId });
                g_rewardProdAt.erase({ botGuid, questId });
                g_readyAt.erase((uint64(botGuid) << 32) | questId);
            }
            Player* bot = FindCompanionPlayer(botGuid);
            if (!IsControlledCompanion(bot, player))
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
#if defined(CQ_HAS_COMPANION_LOOT_POLICY) && defined(MOD_PLAYERBOTS)
    CompanionLootPolicy::VetoNewStack = ReserveCompanionLootSlot;
#endif
#ifdef MOD_PLAYERBOTS_INITIALIZATION_POLICY_V1
    g_nativeInitializationVetoRegistered = Playerbots::RegisterInitializationVeto(DeferNativeBotInitialization);
    if (!g_nativeInitializationVetoRegistered)
        LOG_ERROR("server.loading", "[CompanionQuestSync] Playerbots initialization veto registration failed; custody is not protected");
#endif
    new CompanionQuestSyncWorldScript();
    new CompanionQuestSyncCommandScript();
    new CompanionQuestSyncPlayerScript();
    new CompanionQuestSyncCreatureScript();
}
