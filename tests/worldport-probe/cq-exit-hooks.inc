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
