// Included inside the module's private namespace, after the profession tables.
    template <class Visit>
    void VisitCarriedItems(Player* bot, Visit&& visit)
    {
        for (uint8 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
            if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
                visit(item);
        for (uint8 i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; ++i)
            if (Bag* bag = bot->GetBagByPos(i))
                for (uint8 j = 0; j < bag->GetBagSize(); ++j)
                    if (Item* item = bot->GetItemByPos(i, j))
                        visit(item);
    }

    bool QuestNeedsItem(Player* player, uint32 entry)
    {
        for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            if (uint32 id = player->GetQuestSlotQuestId(slot))
                if (Quest const* quest = sObjectMgr->GetQuestTemplate(id))
                {
                    if (quest->GetSrcItemId() == entry)
                        return true;
                    for (uint32 required : quest->RequiredItemId)
                        if (required == entry)
                            return true;
                    for (uint32 source : quest->ItemDrop)
                        if (source == entry)
                            return true;
                }
        return false;
    }

    bool CanReleaseTemplate(Player* bot, Player* owner, ItemTemplate const* proto)
    {
        return proto && !proto->StartQuest && proto->Class != ITEM_CLASS_QUEST &&
               proto->Class != ITEM_CLASS_CONTAINER && proto->Class != ITEM_CLASS_QUIVER &&
               !QuestNeedsItem(bot, proto->ItemId) && !QuestNeedsItem(owner, proto->ItemId);
    }

    bool CanReleaseItem(Player* bot, Player* owner, Item const* item)
    {
        return CanReleaseTemplate(bot, owner, item->GetTemplate());
    }

    bool IsCraftedSupply(uint16 profession, uint32 entry)
    {
        for (CraftLane const& lane : CraftLanes(profession))
            for (CraftRecipe const& recipe : lane.recipes)
                if (recipe.product == entry)
                    return true;
        return false;
    }

    uint32 CraftingReserve(uint16 profession, uint32 entry, Player* bot)
    {
        uint32 reserve = TradeReserveCount;
        for (CraftLane const& lane : CraftLanes(profession))
            for (CraftRecipe const& recipe : lane.recipes)
                if (bot->HasSpell(recipe.spell))
                    if (SpellInfo const* spell = sSpellMgr->GetSpellInfo(recipe.spell))
                        for (uint8 i = 0; i < MAX_SPELL_REAGENTS; ++i)
                            if (uint32(spell->Reagent[i]) == entry)
                                reserve = std::max(reserve, uint32(spell->ReagentCount[i]));
        return reserve;
    }

    uint32 OwnerStock(Player* owner, uint32 entry)
    {
        uint32 count = owner->GetItemCount(entry, true);
        for (Mail const* mail : owner->GetMails())
            if (mail->state != MAIL_STATE_DELETED)
                for (auto const& attachment : mail->items)
                    if (attachment.item_template == entry)
                        if (Item* item = owner->GetMItem(attachment.item_guid))
                            count += item->GetCount();
        return count;
    }

    uint8 OwnerUpgradeSlot(Player* bot, Player* owner, Item* item)
    {
#ifdef MOD_PLAYERBOTS
        ItemTemplate const* proto = item->GetTemplate();
        // Start with armor with an unambiguous slot. Leave weapons, accessory
        // swaps and set/proc tradeoffs to the player.
        if (proto->Class != ITEM_CLASS_ARMOR || proto->Quality < ITEM_QUALITY_UNCOMMON ||
            proto->ItemSet || proto->InventoryType == INVTYPE_FINGER || proto->InventoryType == INVTYPE_TRINKET ||
            proto->InventoryType == INVTYPE_HOLDABLE || OwnerStock(owner, proto->ItemId))
            return NULL_SLOT;
        for (auto const& spell : proto->Spells)
            if (spell.SpellId > 0)
                return NULL_SLOT;
        uint16 ownerPos = 0;
        if (owner->CanEquipItem(NULL_SLOT, ownerPos, item, true) != EQUIP_ERR_OK)
            return NULL_SLOT;
        uint8 const slot = uint8(ownerPos);
        Item* current = owner->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (current)
        {
            ItemTemplate const* worn = current->GetTemplate();
            if (!worn || worn->ItemSet || worn->SubClass != proto->SubClass)
                return NULL_SLOT;
            for (auto const& spell : worn->Spells)
                if (spell.SpellId > 0)
                    return NULL_SLOT;
            // Random-property enchants are already included in CalculateItem.
            for (uint32 enchant = PERM_ENCHANTMENT_SLOT; enchant < PROP_ENCHANTMENT_SLOT_0; ++enchant)
                if (current->GetEnchantmentId(EnchantmentSlot(enchant)))
                    return NULL_SLOT;
        }
        StatsWeightCalculator score(owner);
        score.SetQualityBlend(false);
        float const offered = score.CalculateItem(item->GetEntry(), item->GetItemRandomPropertyId(), slot);
        float const equipped = current ? score.CalculateItem(current->GetEntry(), current->GetItemRandomPropertyId(), slot) : 0.0f;
        if (offered <= 0.0f || offered <= equipped * 1.10f)
            return NULL_SLOT;

        // Use the same ItemUsage decision as Playerbots' equip-upgrade action.
        if (bot->CanUseItem(proto) == EQUIP_ERR_OK)
        {
            uint16 botPos = 0;
            if (bot->CanEquipItem(NULL_SLOT, botPos, item, true) != EQUIP_ERR_OK)
                return NULL_SLOT;  // a transient equip refusal does not mean it is spare
            PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
            if (!ai)
                return NULL_SLOT;
            std::string key = std::to_string(item->GetEntry());
            if (int32 property = item->GetItemRandomPropertyId())
                key += "," + std::to_string(property);
            ItemUsage const usage = ai->GetAiObjectContext()->GetValue<ItemUsage>("item upgrade", key)->Get();
            if (usage == ITEM_USAGE_EQUIP || usage == ITEM_USAGE_REPLACE || usage == ITEM_USAGE_BAD_EQUIP)
                return NULL_SLOT;
        }
        return slot;
#else
        return NULL_SLOT;
#endif
    }

    struct DeliveryRule
    {
        uint32 reserve = 0;
        uint32 limit = 0;
        uint8 gearSlot = NULL_SLOT;
    };

    constexpr uint32 DeliveryQuantity(uint32 stack, uint32 carried, DeliveryRule rule, uint32 planned, bool whole)
    {
        uint32 const cap = carried > rule.reserve ? std::min(rule.limit, carried - rule.reserve) : 0;
        uint32 const available = cap > planned ? std::min(cap - planned, stack) : 0;
        return whole && available != stack ? 0 : available;
    }

    static_assert(DeliveryQuantity(20, 23, {5, 5}, 0, true) == 0);   // mail skips a large stack
    static_assert(DeliveryQuantity(3, 23, {5, 5}, 0, true) == 3);     // a later whole stack fits
    static_assert(DeliveryQuantity(20, 8, {5, 5}, 0, false) == 3);    // trade can split, retain reserve
    static_assert(DeliveryQuantity(3, 23, {5, 5}, 4, false) == 1);    // cap applies across stacks

    constexpr DeliveryRule SupplyCraftRule(uint32 keep, uint32 fresh)
    {
        return { keep, fresh };
    }

    static_assert(DeliveryQuantity(5, 5, SupplyCraftRule(3, 2), 0, false) == 2);
    static_assert(DeliveryQuantity(4, 4, SupplyCraftRule(3, 2), 0, false) == 1);

    DeliveryRule DeliveryRuleFor(Player* bot, Player* owner, Item* item, bool curated, SupplyOrder supply = {})
    {
        if (!CanReleaseItem(bot, owner, item))
            return {};
        if (supply.entry && item->GetEntry() != supply.entry)
            return {};
        if (supply.fresh)
            return SupplyCraftRule(supply.keep, supply.fresh);
        if (uint8 const slot = OwnerUpgradeSlot(bot, owner, item); slot != NULL_SLOT)
            return { 0, 1, slot };

        uint32 const entry = item->GetEntry();
        auto const pair = ClassProfessionPair(bot->getClass());
        bool const product = IsCraftedSupply(pair.second, entry);
        if (curated)
        {
            auto const desired = g_requestedSupplies.find(owner->GetGUID().GetCounter());
            if (!product || desired == g_requestedSupplies.end() || !desired->second.count(entry) ||
                item->GetTemplate()->Class != ITEM_CLASS_CONSUMABLE || owner->CanUseItem(item->GetTemplate()) != EQUIP_ERR_OK)
                return {};
            uint32 const have = OwnerStock(owner, entry);
            return { CraftingReserve(pair.second, entry, bot), have < 5 ? 5 - have : 0 };
        }
        for (uint32 reagent : GatheredReagentsOf(pair.first))
            if (reagent == entry)
                return { CraftingReserve(pair.second, entry, bot), UINT32_MAX };
        return product ? DeliveryRule{ CraftingReserve(pair.second, entry, bot), UINT32_MAX } : DeliveryRule{};
    }

    static std::vector<std::pair<ObjectGuid, uint32>> PlanDelivery(Player* bot, Player* owner, bool curated, bool mail, SupplyOrder supply)
    {
        std::vector<std::pair<ObjectGuid, uint32>> offers;
        std::map<uint32, uint32> planned;
        std::set<uint8> gearSlots;
        Item* incoming[TRADE_SLOT_TRADED_COUNT] = {};
        bool splitSelected = false;
        VisitCarriedItems(bot, [&](Item* item)
        {
            if (supply.sourcePos && item->GetPos() != supply.sourcePos)
                return;
            if (splitSelected || offers.size() >= (mail ? 3 : TRADE_SLOT_TRADED_COUNT) || item->IsInTrade() ||
                !item->CanBeTraded(mail) || item->IsSoulBound() || item->IsRefundable())
                return;
            DeliveryRule const rule = DeliveryRuleFor(bot, owner, item, curated, supply);
            if (!rule.limit || gearSlots.count(rule.gearSlot))
                return;
            uint32 const entry = item->GetEntry();
            uint32 const count = item->GetCount();
            uint32 const spare = DeliveryQuantity(count, bot->GetItemCount(entry, false), rule, planned[entry], mail);
            if (!spare || (spare != count && !FindFreeBagPos(bot, item)))
                return;
            if (!mail && spare != count)
            {
                // Check the exact quantity without creating a speculative split.
                ItemPosCountVec dest;
                if (!offers.empty() || owner->CanStoreItem(NULL_BAG, NULL_SLOT, dest, entry, spare) != EQUIP_ERR_OK)
                    return;
                splitSelected = true;
            }
            else if (!mail)
            {
                uint32 limitCategory = 0;
                incoming[offers.size()] = item;
                if (owner->CanStoreItems(incoming, int(offers.size() + 1), &limitCategory) != EQUIP_ERR_OK)
                {
                    incoming[offers.size()] = nullptr;
                    return;
                }
            }
            offers.emplace_back(item->GetGUID(), spare);
            planned[entry] += spare;
            if (rule.gearSlot != NULL_SLOT)
                gearSlots.insert(rule.gearSlot);
        });
        return offers;
    }

    static bool DeliveryStillValid(Player* bot, Player* owner, PendingTrade const& pending)
    {
        std::map<uint32, uint32> totals;
        std::set<uint8> gearSlots;
        for (auto const& offered : pending.offeredItems)
        {
            Item* item = bot->GetItemByGuid(offered.guid);
            if (!item || !item->CanBeTraded() || item->IsSoulBound() || item->IsRefundable())
                return false;
            DeliveryRule const rule = DeliveryRuleFor(bot, owner, item, pending.curated, pending.supply);
            uint32 const entry = item->GetEntry();
            uint32 const count = (totals[entry] += offered.count);
            if (count > rule.limit || bot->GetItemCount(entry, false) < count + rule.reserve ||
                (rule.gearSlot != NULL_SLOT && !gearSlots.insert(rule.gearSlot).second))
                return false;
        }
        return true;
    }

    static bool IsTownDeliveryStop(Player* owner, Player* bot)
    {
        return owner->HasPlayerFlag(PLAYER_FLAGS_RESTING) && !owner->isMoving() && !bot->isMoving();
    }

    bool CanOfferTownDelivery(Player* owner, Player* bot)
    {
        return CanTradeSurplusNow(bot, owner) && IsTownDeliveryStop(owner, bot) &&
               !bot->GetTradeData() && !owner->GetTradeData();
    }
