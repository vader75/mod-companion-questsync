// Included beside the delivery commands, after the guarded trade and protocol helpers.
    static void SendSupplyResult(Player* owner, uint32 nonce, std::string const& message)
    {
        SendProtocolLine(owner, fmt::format("Supply result {}:{}", nonce, message));
    }

    static void SendSupplyFailure(Player* owner, uint32 nonce, char const* message)
    {
        SendDeliveryProgress(owner, nonce, "failed", message);
        SendSupplyResult(owner, nonce, message); // older clients still parse this line
    }

    static bool ParseSupplyNumber(std::string const& text, uint32& value)
    {
        auto const parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && value != 0;
    }

#ifdef AC_TRADE_OFFER_GUARD
    static bool CanOrderProduct(Player* bot, Player* owner, ItemTemplate const* item)
    {
        return CanReleaseTemplate(bot, owner, item) && item->Bonding != BIND_WHEN_PICKED_UP &&
               item->Bonding != BIND_QUEST_ITEM;
    }

    static CraftPlan FindSupplyRecipe(Player* bot, uint32 entry)
    {
        for (CraftLane const& lane : CraftLanes(ClassProfessionPair(bot->getClass()).second))
            for (CraftRecipe const& recipe : lane.recipes)
                if (recipe.product == entry && bot->HasSpell(recipe.spell) &&
                    bot->GetPureSkillValue(lane.skill) >= recipe.requiredSkill)
                    if (SpellInfo const* spell = sSpellMgr->GetSpellInfo(recipe.spell))
                        return { &recipe, spell };
        return { nullptr, nullptr };
    }

    static uint32 SupplyBatchSize(CraftPlan const& plan, ItemTemplate const* item)
    {
        for (auto const& effect : plan.spell->Effects)
            if (effect.Effect == SPELL_EFFECT_CREATE_ITEM && uint32(effect.ItemType) == item->ItemId)
                return std::clamp<uint32>(effect.CalcValue(), 1u, item->GetMaxStackSize());
        return 0;
    }

    static bool SupplyCraftReady(Player* bot, Player* owner, CraftPlan const& plan)
    {
        if (bot->HasSpellCooldown(plan.recipe->spell) || !CanStoreCraftOutput(bot, plan))
            return false;
        if (plan.spell->RequiresSpellFocus)
        {
            GameObject* focus = nullptr;
            Acore::GameObjectFocusCheck check(bot, plan.spell->RequiresSpellFocus);
            Acore::GameObjectSearcher<Acore::GameObjectFocusCheck> searcher(bot, focus, check);
            Cell::VisitObjects(bot, searcher, bot->GetVisibilityRange());
            if (!focus)
                return false;
        }
        for (auto const& [reagent, need] : CraftReagentRequirements(plan.spell))
            if (UsableCraftReagentCount(bot, owner, reagent) < need)
                return false;
        return true;
    }

    // Offer one standard batch; specialization bonuses stay with the crafter.
    // Use one source stack and leave room to split it even after a maximum proc.
    static uint16 PreflightSupplyCraft(Player* bot, Player* owner, CraftPlan const& plan,
        ItemTemplate const* item, uint32 batch)
    {
        ItemPosCountVec recipient, nominal, maximum;
        if (!batch || owner->CanStoreItem(NULL_BAG, NULL_SLOT, recipient, item->ItemId, batch) != EQUIP_ERR_OK ||
            bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, nominal, item->ItemId, batch) != EQUIP_ERR_OK)
            return 0;

        float chance = 0.0f;
        uint32 perfect = item->ItemId;
        if (CanCreatePerfectItem(bot, plan.recipe->spell, chance, perfect) && chance > 0.0f && perfect != item->ItemId)
            return 0; // an upgraded replacement is not the item that was ordered
        int32 extra = 0;
        if (!canCreateExtraItems(bot, plan.recipe->spell, chance, extra) || chance <= 0.0f)
            extra = 0;
        uint64 const worst = uint64(batch) * (1 + uint64(std::max(0, extra)));
        if (worst > UINT32_MAX ||
            bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, maximum, item->ItemId, uint32(worst)) != EQUIP_ERR_OK)
            return 0;

        for (ItemPosCount const& part : nominal)
        {
            Item* existing = bot->GetItemByPos(part.pos);
            if (!Player::IsInventoryPos(part.pos) || (existing && (!existing->CanBeTraded() || existing->IsInTrade() ||
                existing->IsSoulBound() || existing->IsRefundable())))
                continue;
            uint32 const held = existing ? existing->GetCount() : 0;
            auto const maxPart = std::find_if(maximum.begin(), maximum.end(), [&](ItemPosCount const& value)
                { return value.pos == part.pos; });
            if (held + part.count < batch || maxPart == maximum.end() || held + maxPart->count < batch)
                continue;
            if ((held + maxPart->count > batch || held + part.count > batch) && !FindFreeBagPos(bot, item, &maximum))
                continue;
            return part.pos;
        }
        return 0;
    }

    static void SendSupplyCatalog(Player* owner, Player* bot, uint32 nonce)
    {
        std::set<uint32> entries;
        VisitCarriedItems(bot, [&](Item* item)
        {
            if (!item->IsInTrade() && item->CanBeTraded() && !item->IsSoulBound() && !item->IsRefundable())
                entries.insert(item->GetEntry());
        });
        for (CraftLane const& lane : CraftLanes(ClassProfessionPair(bot->getClass()).second))
            for (CraftRecipe const& recipe : lane.recipes)
                entries.insert(recipe.product);

        uint32 index = 0;
        for (uint32 const entry : entries)
        {
            ItemTemplate const* item = sObjectMgr->GetItemTemplate(entry);
            if (!item)
                continue;
            uint32 quantity = 0;
            for (auto const& offer : PlanDelivery(bot, owner, false, false, { entry }))
                quantity += offer.second;
            char const* state = "bag";
            if (!quantity)
            {
                CraftPlan const plan = FindSupplyRecipe(bot, entry);
                if (!plan.recipe || !CanOrderProduct(bot, owner, item))
                    continue;
                uint32 const batch = SupplyBatchSize(plan, item);
                if (!batch)
                    continue;
                state = SupplyCraftReady(bot, owner, plan) && PreflightSupplyCraft(bot, owner, plan, item, batch) ? "craft" : "missing";
            }

            // The name is display-only; the bounded numeric item ID is the order key.
            std::string name = item->Name1;
            std::replace(name.begin(), name.end(), '\n', ' ');
            std::replace(name.begin(), name.end(), '\r', ' ');
            std::replace(name.begin(), name.end(), '|', ' ');
            if (name.empty() || name.size() > 150)
                name = fmt::format("Item {}", entry);
            SendProtocolLine(owner, fmt::format("Supply row {} {} {} {} {}:{}", nonce, ++index, entry, state, quantity, name));
            if (index == 64)
                break;
        }
        SendProtocolLine(owner, fmt::format("Supply end {} {}", nonce, index));
    }

#ifdef AC_TRADE_OFFER_COMMIT
    static void HandleSupplyAction(Player* owner, Player* bot, uint32 nonce, uint32 entry, bool make)
    {
        if (!CanTradeSurplusNow(bot, owner) || bot->GetTradeData() || owner->GetTradeData())
        {
            SendSupplyFailure(owner, nonce, "Stand beside your companion, both out of combat and not already trading.");
            return;
        }
        SupplyOrder supply{ entry };
        TradeData* opened = nullptr;
        if (make)
        {
            ItemTemplate const* item = sObjectMgr->GetItemTemplate(entry);
            CraftPlan const plan = FindSupplyRecipe(bot, entry);
            if (!plan.recipe || !CanOrderProduct(bot, owner, item))
            {
                SendSupplyFailure(owner, nonce, "I don't know a recipe I can share for that item.");
                return;
            }
            if (!SupplyCraftReady(bot, owner, plan) || bot->IsNonMeleeSpellCast(false) || bot->isMoving())
            {
                SendSupplyFailure(owner, nonce, "I can't craft that now. Check my materials, bag space and crafting station.");
                return;
            }
            uint32 const batch = SupplyBatchSize(plan, item);
            supply.sourcePos = PreflightSupplyCraft(bot, owner, plan, item, batch);
            if (!supply.sourcePos)
            {
                SendSupplyFailure(owner, nonce, "I need a usable stack and room in both our bags before I craft your batch.");
                return;
            }
            opened = InitiateDeliveryTrade(bot, owner);
            if (!opened)
            {
                SendSupplyFailure(owner, nonce, "I couldn't start a trade, so I kept my materials. Try again when trade is available.");
                return;
            }
            // Initiating a trade calls other realm scripts. Re-read the quest
            // reserve after that hook and immediately before consuming reagents.
            if (!SupplyCraftReady(bot, owner, plan) || !HasSupplyAuthority(bot, owner, true))
            {
                if (bot->GetTradeData() == opened)
                    CancelSurplusTrade(bot, owner, nullptr);
                SendSupplyFailure(owner, nonce, "I kept my materials because the crafting requirements changed.");
                return;
            }
            supply.keep = bot->GetItemCount(entry);
            Item* const beforeSource = bot->GetItemByPos(supply.sourcePos);
            ObjectGuid const sourceGuid = beforeSource ? beforeSource->GetGUID() : ObjectGuid::Empty;
            bot->CastSpell(bot, plan.recipe->spell, TriggerCastFlags(TRIGGERED_FULL_MASK &
                ~TRIGGERED_IGNORE_POWER_AND_REAGENT_COST & ~TRIGGERED_IGNORE_SPELL_AND_CATEGORY_CD));
            uint32 const after = bot->GetItemCount(entry);
            if (after < supply.keep || after - supply.keep < batch)
            {
                if (bot->GetTradeData() == opened)
                    CancelSurplusTrade(bot, owner, nullptr);
                SendSupplyFailure(owner, nonce, "I couldn't make that item just now. Check the recipe requirements and try again.");
                return;
            }
            supply.fresh = batch;
            // Taking reagents can free an earlier slot than the tentative new
            // stack's position. Locate the actual source, then lock its GUID
            // and exact count in the normal pending-trade record.
            if (sourceGuid.IsEmpty())
            {
                supply.sourcePos = 0;
                VisitCarriedItems(bot, [&](Item* stack)
                {
                    if (stack->GetEntry() == entry && stack->GetCount() >= batch &&
                        stack->CanBeTraded() && !stack->IsInTrade() && !stack->IsSoulBound() && !stack->IsRefundable() &&
                        (stack->GetCount() == batch || FindFreeBagPos(bot, stack)))
                    {
                        Item* current = supply.sourcePos ? bot->GetItemByPos(supply.sourcePos) : nullptr;
                        if (!current || std::make_tuple(stack->GetCount(), stack->GetGUID()) <
                            std::make_tuple(current->GetCount(), current->GetGUID()))
                            supply.sourcePos = stack->GetPos();
                    }
                });
            }
        }

        if ((!make || supply.sourcePos) && OpenTradeSurplus(bot, owner, /*announce=*/true, /*curated=*/false, supply, opened, nonce))
            SendSupplyResult(owner, nonce, "I'm preparing the trade. Check the items, then click Accept when they're ready.");
        else
        {
            if (opened && bot->GetTradeData() == opened)
                CancelSurplusTrade(bot, owner, nullptr);
            SendSupplyFailure(owner, nonce, make ? "I made the item but couldn't open a trade. It stayed in my bags."
                                              : "That item isn't ready to hand over. Check bag space or refresh my supplies.");
        }
    }
#endif

#endif

    static bool HandleModernCollect(Player* owner, std::string const& text)
    {
        auto const tokens = SplitArgs(text.c_str(), 4);
        uint32 nonce = 0;
        if (tokens.size() < 3 || tokens[0] != "collect" || !ParseSupplyNumber(tokens[2], nonce))
            return false; // legacy .companionsync delivery collect <name>

        SendDeliveryProgress(owner, nonce, "requested", "Checking for spare upgrades and supplies.");
        if (tokens.size() != 3 || !g_cfg.enable || !g_tableReady)
        {
            SendDeliveryProgress(owner, nonce, "failed", "Companion delivery is disabled or the request is invalid.");
            return true;
        }
#ifndef AC_TRADE_OFFER_COMMIT
        SendDeliveryProgress(owner, nonce, "failed", "This realm does not support confirmed companion delivery.");
#else
        uint32 const ownerGuid = owner->GetGUID().GetCounter();
        uint32 const now = uint32(time(nullptr));
        uint32& last = g_supplyActionAt[ownerGuid];
        if (last && now - last < 1u)
        {
            SendDeliveryProgress(owner, nonce, "failed", "Give me a moment, then try again.");
            return true;
        }
        last = now;
        std::vector<uint32> bound;
        uint32 const guid = FindCompanions(ownerGuid, bound) ? ResolveCompanion(owner, tokens[1], bound) : 0;
        Player* bot = guid ? FindCompanionPlayer(guid) : nullptr;
        if (!bot || !HasSupplyAuthority(bot, owner, true) || !CanTradeSurplusNow(bot, owner) ||
            bot->GetTradeData() || owner->GetTradeData())
            SendDeliveryProgress(owner, nonce, "failed", "Stand beside your summoned companion, both out of combat and not already trading.");
        else if (!OpenTradeSurplus(bot, owner, /*announce=*/true, /*curated=*/true, {}, nullptr, nonce))
            SendDeliveryProgress(owner, nonce, "failed", "No eligible delivery could be offered. Check bag space and available supplies.");
#endif
        return true;
    }

    static bool HandleNamedSupply(Player* owner, std::string const& text)
    {
        auto const tokens = SplitArgs(text.c_str(), 5);
        if (tokens.empty() || (tokens[0] != "catalog" && tokens[0] != "take" && tokens[0] != "make"))
            return false;
        bool const catalog = tokens[0] == "catalog";
        uint32 nonce = 0, entry = 0;
        if (tokens.size() < 2 || !ParseSupplyNumber(tokens[1], nonce))
            return true;
        if (!catalog)
            SendDeliveryProgress(owner, nonce, "requested", "Checking the requested supply.");
        auto const fail = [&](char const* message)
        {
            if (catalog)
                SendSupplyResult(owner, nonce, message);
            else
                SendSupplyFailure(owner, nonce, message);
        };
        if (tokens.size() != (catalog ? 3 : 4) || (!catalog && !ParseSupplyNumber(tokens[2], entry)))
        {
            fail("The supply request is invalid.");
            return true;
        }
        if (!g_cfg.enable || !g_tableReady)
        {
            fail("Companion delivery is disabled.");
            return true;
        }
#ifndef AC_TRADE_OFFER_GUARD
        fail("This realm doesn't support guarded companion trades.");
#else
#ifndef AC_TRADE_OFFER_COMMIT
        if (!catalog)
        {
            fail("This realm does not support confirmed companion delivery.");
            return true;
        }
#endif
        uint32 const ownerGuid = owner->GetGUID().GetCounter();
        uint32 const now = uint32(time(nullptr));
        uint32& last = (catalog ? g_supplyCatalogAt : g_supplyActionAt)[ownerGuid];
        if (last && now - last < (catalog ? 2u : 1u))
        {
            fail("Give me a moment, then try again.");
            return true;
        }
        last = now;
        Player* bot = nullptr;
        std::vector<uint32> bound;
        if (FindCompanions(ownerGuid, bound))
            for (uint32 guid : bound)
                if (Player* candidate = FindCompanionPlayer(guid);
                    candidate && candidate->GetName() == tokens.back() && HasSupplyAuthority(candidate, owner, true))
                {
                    bot = candidate;
                    break;
                }
        if (!bot || !bot->IsAlive())
            fail("Summon or revive this companion to see their supplies.");
        else if (catalog)
            SendSupplyCatalog(owner, bot, nonce);
#ifdef AC_TRADE_OFFER_COMMIT
        else
            HandleSupplyAction(owner, bot, nonce, entry, tokens[0] == "make");
#endif
#endif
        return true;
    }
