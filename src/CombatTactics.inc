// Included by the world module after the companion/ownership helpers.
namespace
{
    enum class TacticChoice : uint8 { Default, Off, On };

    TacticChoice TacticAt(uint8 packed, uint8 place)
    {
        return TacticChoice(packed / place % 3);
    }

    bool TacticEnabled(TacticChoice choice, bool fallback)
    {
        return choice == TacticChoice::Default ? fallback : choice == TacticChoice::On;
    }

    std::atomic<int> g_tacticsColumn{0};

    bool CombatTacticsColumnPresent()
    {
        int state = g_tacticsColumn.load();
        if (!state && g_custodyReady)
        {
            auto row = CharacterDatabase.Query(
                "SELECT COUNT(*) FROM information_schema.columns WHERE table_schema = DATABASE() "
                "AND table_name = 'cq_companion_custody' AND column_name = 'combat_tactics'");
            state = row && row->Fetch()[0].Get<uint64>() ? 1 : -1;
            g_tacticsColumn = state;
        }
        return state > 0;
    }


    std::unordered_map<uint32, uint8> CombatTacticsForOwner(uint32 owner)
    {
        std::unordered_map<uint32, uint8> choices;
        if (CombatTacticsColumnPresent())
            if (auto rows = CharacterDatabase.Query("SELECT bot_guid, combat_tactics FROM cq_companion_custody "
                "WHERE owner_guid = {} AND module = 'cq' AND state = 'owned' AND combat_tactics BETWEEN 1 AND 26", owner))
                do
                {
                    Field* row = rows->Fetch();
                    choices.emplace(row[0].Get<uint32>(), row[1].Get<uint8>());
                } while (rows->NextRow());
        return choices;
    }

    bool SetCombatTactic(uint32 owner, uint32 bot, uint8 place, TacticChoice choice)
    {
        if (!CombatTacticsColumnPresent() || !HasCurrentCustody(owner, bot, true))
            return false;
        // One SQL update changes only the chosen digit; concurrent edits to other choices survive.
        CharacterDatabase.DirectExecute("UPDATE cq_companion_custody SET combat_tactics = "
            "combat_tactics - MOD(FLOOR(combat_tactics / {}), 3) * {} + {} "
            "WHERE bot_guid = {} AND owner_guid = {} AND module = 'cq' AND state = 'owned'",
            place, place, uint8(choice) * place, bot, owner);
        auto row = CharacterDatabase.Query("SELECT combat_tactics FROM cq_companion_custody "
            "WHERE bot_guid = {} AND owner_guid = {} AND module = 'cq' AND state = 'owned'", bot, owner);
        return row && TacticAt(row->Fetch()[0].Get<uint8>(), place) == choice;
    }

    void SendCombatTactics(Player* owner, uint32 ownerGuid)
    {
        if (!g_cfg.enable || !CombatTacticsColumnPresent())
        {
            SendProtocolLine(owner, "Tactics unavailable");
            return;
        }
        std::vector<uint32> bots;
        if (!FindCompanions(ownerGuid, bots))
        {
            SendProtocolLine(owner, "Tactics none");
            return;
        }
        auto choices = CombatTacticsForOwner(ownerGuid);
        std::string payload;
        for (uint32 guid : bots)
        {
            std::string const name = CompanionNameFor(guid);
            if (name.empty() || name.find_first_of("|=") != std::string::npos)
                continue;
            uint8 const packed = choices.count(guid) ? choices.at(guid) : 0;
            if (!payload.empty())
                payload += '|';
            payload += fmt::format("{}={}{}{}", name, uint8(TacticAt(packed, 1)), uint8(TacticAt(packed, 3)),
                                   uint8(TacticAt(packed, 9)));
        }
        SendProtocolLine(owner, "Tactics " + (payload.empty() ? "none" : payload));
    }

    bool FlankRisksPull(Player* bot)
    {
        constexpr float margin = 8.0f;
        // Bounded outdoor screening, not a navmesh prediction. Check the live
        // aggro radius for each candidate; the scan intentionally stays local.
        auto risky = [bot](Creature* mob)
        {
            if (!mob->IsAlive() || mob->IsEngaged() || mob->IsInCombat() || !mob->HasReactState(REACT_AGGRESSIVE) ||
                mob->IsCivilian() || mob->IsImmuneToPC() || mob->IsNeutralToAll() || !mob->IsHostileTo(bot))
                return false;
            float const aggro = mob->GetAggroRange(bot) + mob->m_CombatDistance;
            return aggro > 0.0f &&
                (mob->CanFly() || mob->GetDistanceZ(bot) <= CREATURE_Z_ATTACK_RANGE + mob->m_CombatDistance) &&
                mob->IsWithinDistInMap(bot, aggro + margin, true, false, false) && mob->IsWithinLOSInMap(bot);
        };
        Creature* risk = nullptr;
        Acore::CreatureSearcher<decltype(risky)> searcher(bot, risk, risky);
        Cell::VisitObjects(bot, searcher, MAX_AGGRO_RADIUS + 2 * margin);
        return risk != nullptr;
    }

    struct TacticsSnapshot
    {
        ObjectGuid owner;
        ObjectGuid tank;
        uint32 botGuid = 0;
        float leash = 10.0f;
        TacticChoice gap = TacticChoice::Default;
        bool tankFirst = false;
        bool guardFlanks = false;
    };

    struct TacticsState
    {
        std::atomic<std::shared_ptr<TacticsSnapshot const>> rules;
    };

    bool TacticsEligible(Player* bot, Player* owner)
    {
        return bot->IsInWorld() && bot->IsAlive() && owner && owner->IsInWorld() &&
            bot->GetGroup() && owner->GetGroup() == bot->GetGroup() && owner->GetMap() == bot->GetMap() &&
            !bot->GetMap()->IsBattlegroundOrArena();
    }

    // The engine owns each listener and deletes it with the AI. Reach the existing
    // protected engine slots via a pointer-to-member; do not downcast the live AI
    // or keep an engine pointer after the bot logs out. No strategies are edited.
    struct TacticsEngineAccess : PlayerbotAI
    {
        static void Add(PlayerbotAI* ai, BotState state, ActionExecutionListener* listener)
        {
            (ai->*(&TacticsEngineAccess::engines))[state]->AddActionExecutionListener(listener);
        }
    };

    class CombatTacticsListener final : public ActionExecutionListener
    {
    public:
        CombatTacticsListener(PlayerbotAI* ai, std::shared_ptr<TacticsState> state) : _ai(ai), _state(std::move(state)) { }

        bool Before(Action* action, Event /*event*/) override
        {
            auto const rules = _state->rules.load();
            if (!action || !rules)
                return true;
            Player* bot = _ai->GetBot();
            std::string const& name = action->getName();
            bool const gap = bot->getClass() == CLASS_WARRIOR &&
                (name == "charge" || name.rfind("intercept", 0) == 0);
            bool const flank = rules->guardFlanks && name == "set behind";
            bool const pull = rules->tankFirst && (name == "pull start" || name == "pull action" ||
                                                   name == "reach pull" || name == "pitch in attack");
            bool const attacks = rules->tankFirst && dynamic_cast<AttackAction*>(action) != nullptr;
            bool const reach = rules->tankFirst && dynamic_cast<ReachMeleeAction*>(action) != nullptr;
            if (!flank && !pull && !attacks && !reach && !(gap && (rules->tankFirst || rules->gap != TacticChoice::Default)))
                return true;
            Player* owner = ObjectAccessor::GetPlayer(*bot, rules->owner);
            if (!TacticsEligible(bot, owner))
                return true;

            if (flank && bot->IsInCombat() &&
                !bot->GetMap()->Instanceable() && bot->IsOutdoors() && FlankRisksPull(bot))
                return false;

            if (gap && rules->gap == TacticChoice::Off)
                return false;
            if (!rules->tankFirst && !(gap && rules->gap == TacticChoice::On))
                return true;
            Player* tank = rules->tank ? ObjectAccessor::GetPlayer(*bot, rules->tank) : nullptr;
            if (!tank || !tank->IsAlive() || tank->GetGroup() != bot->GetGroup() || tank == bot)
                return !gap || rules->gap != TacticChoice::On;

            if (pull)
                return false;
            if (!gap && !attacks && !reach)
                return true;
            Player* master = name == "attack my target" ? _ai->GetMaster() : nullptr;
            Unit* target = name == "attack my target" ? (master ? master->GetSelectedUnit() : nullptr) : action->GetTarget();
            if (!target || !target->IsAlive() || target->GetMap() != bot->GetMap())
                return false;
            if (!gap && attacks && bot->IsWithinMeleeRange(target) &&
                (target->GetVictim() == bot || bot->GetVictim() == target))
                return true; // already fighting or defending, not a new chase
            return target->IsEngagedBy(tank) && bot->IsWithinDistInMap(tank, rules->leash) &&
                tank->IsWithinDistInMap(target, rules->leash) && tank->IsWithinLOSInMap(target);
        }

        bool AllowExecution(Action*, Event) override { return true; }
        void After(Action*, bool, Event) override { }
        bool OverrideResult(Action*, bool executed, Event) override { return executed; }

    private:
        PlayerbotAI* _ai;
        std::shared_ptr<TacticsState> _state;
    };

    std::unordered_map<PlayerbotAI*, std::weak_ptr<TacticsState>> g_tacticsListeners;

    bool HasActiveGapCloserOverride(Player* bot)
    {
        auto const found = g_tacticsListeners.find(GET_PLAYERBOT_AI(bot));
        auto const state = found == g_tacticsListeners.end() ? nullptr : found->second.lock();
        auto const rules = state ? state->rules.load() : nullptr;
        return g_cfg.enable && rules && rules->gap != TacticChoice::Default && bot->IsInWorld() &&
            TacticsEligible(bot, ObjectAccessor::GetPlayer(*bot, rules->owner));
    }

    Player* TacticsTank(Player* owner, std::vector<uint32> const& bots)
    {
        ObjectGuid marked;
        for (auto const& slot : owner->GetGroup()->GetMemberSlots())
            if (slot.flags & MEMBER_FLAG_MAINTANK)
            {
                marked = slot.guid;
                break;
            }
        Player* roleTank = nullptr;
        Player* classTank = nullptr;
        for (GroupReference* ref = owner->GetGroup()->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || !member->IsInWorld() || !member->IsAlive() || member->GetMap() != owner->GetMap())
                continue;
            uint32 const guid = member->GetGUID().GetCounter();
            if (member->GetGUID() == marked)
                return member;
            if (!roleTank && std::find(bots.begin(), bots.end(), guid) != bots.end() && CompanionRole(guid) == "tank")
                roleTank = member;
            if (!classTank && PlayerbotAI::IsTank(member))
                classTank = member;
        }
        return roleTank ? roleTank : classTank;
    }

    void UpdateCombatTactics()
    {
        std::unordered_set<PlayerbotAI*> active;
        if (g_cfg.enable && g_custodyReady && (CombatTacticsColumnPresent() || g_cfg.tankFirst || g_cfg.guardFlankAggro))
            for (auto const& entry : sWorldSessionMgr->GetAllSessions())
            {
                WorldSession* session = entry.second;
                Player* owner = session && !session->IsBot() ? session->GetPlayer() : nullptr;
                if (!owner || !owner->IsInWorld() || !owner->GetGroup())
                    continue;
                std::vector<uint32> bots;
                if (!FindCompanions(owner->GetGUID().GetCounter(), bots))
                    continue;
                auto choices = CombatTacticsForOwner(owner->GetGUID().GetCounter());
                Player* tank = nullptr;
                bool tankResolved = false;
                for (uint32 guid : bots)
                {
                    Player* bot = FindCompanionPlayer(guid);
                    if (!IsControlledCompanion(bot, owner) || !TacticsEligible(bot, owner))
                        continue;
                    PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
                    if (!ai)
                        continue;
                    uint8 const packed = choices.count(guid) ? choices.at(guid) : 0;
                    auto const gap = TacticAt(packed, 3);
                    bool const flank = TacticEnabled(TacticAt(packed, 9), g_cfg.guardFlankAggro);
                    bool const tankFirst = TacticEnabled(TacticAt(packed, 1), g_cfg.tankFirst) && PlayerbotAI::IsMelee(bot);
                    auto const previousEntry = g_tacticsListeners.find(ai);
                    auto state = previousEntry == g_tacticsListeners.end() ? nullptr : previousEntry->second.lock();
                    auto const previous = state ? state->rules.load() : nullptr;
                    if (previous && previous->gap != TacticChoice::Default && gap == TacticChoice::Default)
                    {
                        state->rules.store(nullptr);
                        ReconcileRoleSpells(bot, CompanionRole(guid));
                    }
                    if (!flank && !tankFirst && gap == TacticChoice::Default)
                        continue;
                    if (!tankResolved && (tankFirst || gap == TacticChoice::On))
                    {
                        tank = TacticsTank(owner, bots);
                        tankResolved = true;
                    }
                    if (!state)
                    {
                        state = std::make_shared<TacticsState>();
                        g_tacticsListeners[ai] = state;
                        TacticsEngineAccess::Add(ai, BOT_STATE_COMBAT, new CombatTacticsListener(ai, state));
                        TacticsEngineAccess::Add(ai, BOT_STATE_NON_COMBAT, new CombatTacticsListener(ai, state));
                    }
                    bool const nonTank = bot != tank && CompanionRole(guid) != "tank";
                    auto snapshot = std::make_shared<TacticsSnapshot>();
                    snapshot->owner = owner->GetGUID();
                    snapshot->tank = tank ? tank->GetGUID() : ObjectGuid::Empty;
                    snapshot->botGuid = guid;
                    snapshot->leash = g_cfg.tankFirstDistance;
                    snapshot->gap = gap;
                    snapshot->tankFirst = tankFirst && nonTank;
                    snapshot->guardFlanks = flank;
                    state->rules.store(std::move(snapshot));
                    // Replace the realm-default Charge mute only after the explicit
                    // action guard is in place; never open an unguarded transition.
                    if (auto held = g_spellsWithheld.find(guid); gap != TacticChoice::Default &&
                        held != g_spellsWithheld.end() && !held->second.empty())
                        ReconcileRoleSpells(bot, CompanionRole(guid));
                    active.insert(ai);
                }
            }
        for (auto it = g_tacticsListeners.begin(); it != g_tacticsListeners.end();)
        {
            if (auto state = it->second.lock())
            {
                if (!active.count(it->first))
                    if (auto const previous = state->rules.exchange(nullptr);
                        previous && previous->gap != TacticChoice::Default && g_cfg.enable)
                        if (Player* bot = FindCompanionPlayer(previous->botGuid);
                            bot && GET_PLAYERBOT_AI(bot) == it->first && HasCurrentCustody(previous->owner.GetCounter(), previous->botGuid, true))
                            ReconcileRoleSpells(bot, CompanionRole(previous->botGuid));
                ++it;
            }
            else
                it = g_tacticsListeners.erase(it);
        }
    }
}
