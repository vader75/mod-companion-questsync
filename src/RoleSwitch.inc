    RoleHands ReadRoleHands(Player* bot)
    {
        RoleHands hands{};
        for (uint8 i = 0; i < hands.size(); ++i)
            if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND + i))
                hands[i] = item->GetGUID();
        return hands;
    }

    Item* FindRoleHand(Player* bot, ObjectGuid guid)
    {
        for (uint8 slot : { EQUIPMENT_SLOT_MAINHAND, EQUIPMENT_SLOT_OFFHAND })
            if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
                item && item->GetGUID() == guid && !item->IsInTrade())
                return item;
        Item* found = nullptr;
        VisitCarriedItems(bot, [&](Item* item)
        {
            if (item && item->GetGUID() == guid && !item->IsInTrade())
                found = item;
        });
        return found;
    }

    bool RestoreRoleHands(Player* bot, RoleHands const& wanted)
    {
        RoleHands const current = ReadRoleHands(bot);
        for (int i = 1; i >= 0; --i) // stow offhand first so a two-hander can go on
        {
            if (current[i] == wanted[i])
                continue;
            uint8 const slot = EQUIPMENT_SLOT_MAINHAND + i;
            if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            {
                ItemPosCountVec dest;
                if (bot->CanUnequipItem(item->GetPos(), false) != EQUIP_ERR_OK ||
                    bot->CanStoreItem(NULL_BAG, NULL_SLOT, dest, item) != EQUIP_ERR_OK)
                    return false;
                bot->RemoveItem(INVENTORY_SLOT_BAG_0, slot, true);
                bot->StoreItem(dest, item, true);
            }
        }
        for (uint8 i = 0; i < wanted.size(); ++i)
            if (wanted[i] != current[i] && !wanted[i].IsEmpty())
                if (Item* item = FindRoleHand(bot, wanted[i]); !item ||
                    !EquipCarriedKitPiece(bot, EQUIPMENT_SLOT_MAINHAND + i, item))
                    return false;
        return ReadRoleHands(bot) == wanted;
    }

    bool CanStowRoleHand(Player* bot, uint8 slot)
    {
        Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        ItemPosCountVec dest;
        return !item || (bot->CanUnequipItem(item->GetPos(), false) == EQUIP_ERR_OK &&
                         bot->CanStoreItem(NULL_BAG, NULL_SLOT, dest, item) == EQUIP_ERR_OK);
    }

    bool RoleHandsFit(Player* bot, std::string const& role)
    {
        Item* main = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
        Item* off = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
        ItemTemplate const* weapon = main ? main->GetTemplate() : nullptr;
        if (KitNeedFor(bot->getClass(), role) == KitNeed::Shield)
            return weapon && weapon->Class == ITEM_CLASS_WEAPON && weapon->InventoryType != INVTYPE_2HWEAPON &&
                   off && off->GetTemplate()->InventoryType == INVTYPE_SHIELD;
        if (KitNeedFor(bot->getClass(), role) == KitNeed::CasterWeapon)
            return !weapon || weapon->InventoryType != INVTYPE_2HWEAPON ||
                   weapon->SubClass == ITEM_SUBCLASS_WEAPON_STAFF;
        return true;
    }

    RoleTalents ReadRoleTalents(Player* bot, uint8 spec)
    {
        RoleTalents result;
        for (auto const& [rankSpell, talent] : bot->GetTalentMap())
            if (talent && talent->State != PLAYERSPELL_REMOVED && talent->IsInSpec(spec))
                if (TalentEntry const* info = ResolveTalentEntry(rankSpell))
                    for (uint8 rank = 0; rank < MAX_TALENT_RANK; ++rank)
                        if (info->RankID[rank] == rankSpell)
                        {
                            uint8& highest = result[info->TalentID];
                            highest = std::max(highest, rank);
                        }
        return result;
    }

    bool RestoreRoleTalents(Player* bot, RoleTalents const& wanted)
    {
        uint8 const spec = bot->GetActiveSpec();
        RoleTalents current = ReadRoleTalents(bot, spec);
        if (current == wanted)
            return true;
        bot->resetTalents(true);
        bot->InitTalentForLevel();
        current = ReadRoleTalents(bot, spec);
        // The core validates tiers and prerequisites. Retry only while learned ranks change.
        while (current != wanted)
        {
            for (auto const& [talent, rank] : wanted)
                bot->LearnTalent(talent, rank);
            RoleTalents next = ReadRoleTalents(bot, spec);
            if (next == current)
                break;
            current = std::move(next);
        }
        return current == wanted;
    }

    bool ReconcileCompanionSpec(Player* bot, int want, RoleTalents const* saved)
    {
        if (want < 0)
            return !saved || RestoreRoleTalents(bot, *saved);
        bot->InitTalentForLevel();
        uint8 const other = bot->GetActiveSpec() == 0 ? 1 : 0;
        if (other < bot->GetSpecsCount() && CurrentSpecTab(bot) != want && SpecTabOf(bot, other) == want &&
            (!saved || ReadRoleTalents(bot, other) == *saved))
            bot->ActivateSpec(other);
        if (saved)
        {
            if (!RestoreRoleTalents(bot, *saved) || CurrentSpecTab(bot) != want)
                return false;
        }
        else if (CurrentSpecTab(bot) != want)
        {
            PlayerbotFactory factory(bot, bot->GetLevel());
            factory.InitTalentsTree(false, true, true, want);
        }
        SpendPointsIntoTree(bot, uint8(want));
        // At high levels a single tree may not have enough legal ranks for all owed points.
        for (uint8 tab = 0; tab < 3 && bot->GetFreeTalentPoints(); ++tab)
            if (tab != want)
                SpendPointsIntoTree(bot, tab);
        return CurrentSpecTab(bot) == want && !bot->GetFreeTalentPoints();
    }

    using StrategySnapshot = std::array<std::vector<std::string>, BOT_STATE_MAX>;

    bool RestoreRoleStrategies(PlayerbotAI* ai, StrategySnapshot const& snapshot, uint8 first = 0)
    {
        bool restored = true;
        for (uint8 i = first; i < BOT_STATE_MAX; ++i)
        {
            BotState const state = BotState(i);
            for (auto const& name : ai->GetStrategies(state))
                if (std::find(snapshot[i].begin(), snapshot[i].end(), name) == snapshot[i].end())
                    ai->ChangeStrategy("-" + name, state);
            for (auto const& name : snapshot[i])
            {
                if (!ai->HasStrategy(name, state))
                    ai->ChangeStrategy("+" + name, state);
                restored = ai->HasStrategy(name, state) && restored;
            }
            restored = ai->GetStrategies(state).size() == snapshot[i].size() && restored;
        }
        return restored;
    }

    // Returns true when the pending request owns this pass, including when it must wait.
    bool ProcessPendingRoleSwitch(Player* owner, Player* bot, uint32 botGuid)
    {
        auto const request = g_roleSwitches.find(botGuid);
        if (request == g_roleSwitches.end() || request->second.pending.empty())
            return false;
        RoleSwitchState& state = request->second;
        uint32 const ownerGuid = owner->GetGUID().GetCounter();
        if (state.ownerGuid != ownerGuid || !HasCurrentCustody(ownerGuid, botGuid, true))
        {
            g_roleSwitches.erase(request);
            return false;
        }
        if (!CanManageBags(owner, bot) || !IsControlledCompanion(bot, owner) ||
            !RoleChangeRefusal(owner, bot, botGuid).empty())
            return true;
        PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
        if (!ai || !owner->GetSession() || bot->IsNonMeleeSpellCast(false))
            return true;

        std::string const role = state.pending;
        std::string const previous = CompanionRole(botGuid);
        auto const refuse = [&](std::string const& reason)
        {
            state.pending.clear();
            ChatHandler(owner->GetSession()).PSendSysMessage("{} could not switch to {} ({}).",
                                                            bot->GetName(), role, reason);
        };
        if (role == previous)
        {
            state.pending.clear();
            return true;
        }
        int const tab = RoleSpecTab(bot->getClass(), role);
        if (tab < 0 && role != "auto")
        {
            refuse("unsupported role");
            return true;
        }

        uint8 const spec = bot->GetActiveSpec();
        uint32 const free = bot->GetFreeTalentPoints();
        RoleHands const hands = ReadRoleHands(bot);
        RoleTalents const talents = ReadRoleTalents(bot, spec);
        auto const saved = state.saved.find(role);
        // Auto may have the old damage weapons, but its talents are not a damage-role snapshot.
        auto savedGear = saved;
        if (savedGear == state.saved.end() && previous == "tank" && (role == "dps" || role == "support"))
            savedGear = state.saved.find("auto");
        bool const hasSavedGear = savedGear != state.saved.end();
        bool const changesHands = (hasSavedGear && hands != savedGear->second.hands) || !RoleHandsFit(bot, role);
        if (changesHands && bot->GetFreeInventorySpace() < hands.size())
        {
            refuse("free two bag slots and try again");
            return true;
        }
        if (hasSavedGear)
            for (ObjectGuid const guid : savedGear->second.hands)
                if (!guid.IsEmpty() && guid != hands[0] && guid != hands[1] && !FindRoleHand(bot, guid))
                {
                    refuse("a saved weapon or offhand is missing");
                    return true;
                }

        bool const talentsEnabled = g_cfg.specFollowRole >= 2 && bot->GetLevel() >= g_cfg.specMinLevel &&
                                    bot->CalculateTalentsPoints();
        int const want = talentsEnabled ? tab : -1;
        RoleTalents const* savedTalents = talentsEnabled && saved != state.saved.end() && !saved->second.talents.empty()
                                            ? &saved->second.talents : nullptr;
        bool const reconcileTalents = want >= 0 || savedTalents;
        // Native reset/activation (including rollback) can dismiss pets, charms and summons.
        // Their original state cannot be reconstructed by the loadout rollback.
        if (reconcileTalents && (!bot->GetPetGUID().IsEmpty() || bot->GetTemporaryUnsummonedPetNumber() ||
            !bot->GetCharmGUID().IsEmpty() || !bot->m_Controlled.empty() ||
            std::any_of(std::begin(bot->m_SummonSlot), std::end(bot->m_SummonSlot),
                        [](ObjectGuid const& guid) { return !guid.IsEmpty(); })))
        {
            refuse("dismiss or release the companion's pet or controlled units, or wait for summons to expire, then apply again");
            return true;
        }
        // Talent reset/activation can auto-unequip an offhand and send it to mail if storage
        // fails. Check a real destination before invoking either native operation.
        if (((changesHands || reconcileTalents) && !CanStowRoleHand(bot, EQUIPMENT_SLOT_OFFHAND)) ||
            (changesHands && !CanStowRoleHand(bot, EQUIPMENT_SLOT_MAINHAND)))
        {
            refuse("the equipped weapons cannot be safely stowed");
            return true;
        }
        if (g_cfg.specFollowRole == 1 && tab >= 0)
            LOG_INFO("server.loading", "[CompanionQuestSync] {} would use {} for {} (SpecFollowRole=1)",
                     bot->GetName(), kSpecTreeNames[bot->getClass()][tab], role);
        uint8 const other = spec == 0 ? 1 : 0;
        RoleTalents const otherTalents = other < bot->GetSpecsCount() ? ReadRoleTalents(bot, other) : RoleTalents{};
        StrategySnapshot strategies;
        for (uint8 i = 0; i < BOT_STATE_MAX; ++i)
            strategies[i] = ai->GetStrategies(BotState(i));
        auto const ledger = g_roleApplied[botGuid];

        std::vector<ObjectGuid> generated;
        bool ready = ReconcileCompanionSpec(bot, want, savedTalents);
        std::string failure = "talents did not verify";
        if (ready && hasSavedGear)
        {
            failure = "the saved weapons could not be equipped";
            ready = RestoreRoleHands(bot, savedGear->second.hands);
        }
        if (ready && !RoleHandsFit(bot, role))
        {
            failure = "the required weapon and offhand did not verify";
            if (KitNeedFor(bot->getClass(), role) == KitNeed::Shield)
            {
                Item* main = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
                if (!main || main->GetTemplate()->Class != ITEM_CLASS_WEAPON ||
                    main->GetTemplate()->InventoryType == INVTYPE_2HWEAPON)
                    ready = FitKitPiece(bot, EQUIPMENT_SLOT_MAINHAND, KitPiece::OneHandedWeapon, &generated);
            }
            if (ready)
                RepairKitShape(bot, role, &generated);
            ready = ready && RoleHandsFit(bot, role);
        }
        if (ready)
        {
            std::string const missedRevert = RevertRoleStrategies(bot);
            ready = missedRevert.empty();
            failure = "AI strategies did not verify";
            if (bot->GetActiveSpec() != spec || ReadRoleTalents(bot, spec) != talents)
            {
                ai->ResetStrategies();
                ready = RestoreRoleStrategies(ai, strategies, BOT_STATE_NON_COMBAT) && ready;
            }
            if (ready)
            {
                std::string const missed = ApplyRoleStrategies(bot, role);
                ready = missed.empty();
                if (!ready)
                    failure += ": " + missed;
            }
        }
        if (ready)
        {
            failure = "the applied role could not be stored";
            ready = SetCompanionRole(ownerGuid, botGuid, role);
        }
        if (!ready)
        {
            bool restored = true;
            // Only items provisioned by this failed attempt are discarded. This also frees any
            // rollback bag space they consumed. The prior and candidate owned kits are untouched.
            for (ObjectGuid const guid : generated)
                if (Item* item = FindRoleHand(bot, guid))
                    bot->DestroyItem(item->GetBagSlot(), item->GetSlot(), true);
            if (reconcileTalents)
            {
                if (bot->GetActiveSpec() != spec)
                {
                    restored = RestoreRoleTalents(bot, otherTalents);
                    bot->ActivateSpec(spec);
                    restored = bot->GetActiveSpec() == spec && restored;
                }
                restored = RestoreRoleTalents(bot, talents) && restored;
                bot->SetFreeTalentPoints(free);
            }
            restored = RestoreRoleHands(bot, hands) && restored;
            restored = RestoreRoleStrategies(ai, strategies) && restored;
            g_roleApplied[botGuid] = ledger;
            if (!restored)
                LOG_ERROR("server.loading", "[CompanionQuestSync] {} role loadout rollback did not verify", bot->GetName());
            refuse(failure + (restored ? "; previous loadout restored" : "; rollback did not verify"));
            return true;
        }

        state.saved[previous] = { hands, talents };
        state.saved.erase(role); // capture the active loadout fresh when next leaving it
        for (ObjectGuid const guid : hands)
            g_staleGear[botGuid].erase(guid.GetRawValue());
        state.pending.clear();
        g_roleNotes.erase(botGuid);
        g_roleApplyAt[botGuid] = uint32(time(nullptr));
        ReconcileRoleSpells(bot, role);
        ReconcilePetTaunt(bot);
        ChatHandler(owner->GetSession()).PSendSysMessage("{} is now {}.", bot->GetName(), role);
        SendRoleAndSpecLines(owner, ownerGuid);
        return true;
    }
