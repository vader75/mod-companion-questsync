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
                LOG_INFO("server.loading", "[CompanionQuestSync] {} for owner {} expired after {}s ({})",
                         dungeonExit ? "dungeon-exit follow window" : "follow window", ownerGuid, age,
                         travel.holdNote.empty() ? std::string("no companion hold recorded")
                                                 : "last note: " + travel.holdNote);
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
                    itr->second.holdNote = !bot ? std::string("a companion is not in world")
                        : !bot->IsInWorld() ? std::string(bot->GetName()) + ": not in world"
                        : (bot->GetMapId() != travel.sourceMap || bot->GetInstanceId() != travel.sourceInstance)
                            ? std::string(bot->GetName()) + ": left the exit dungeon"
                        : !IsControlledCompanion(bot, owner)
                            ? std::string(bot->GetName()) + ": no longer a controlled companion"
                            : std::string(bot->GetName()) + ": not in the owner's party";
                    itr->second.noteBotGuid = bot ? botGuid : 0;
                    continue;
                }
                if (!bot || !bot->IsInWorld() || !bot->IsAlive() || bot->IsInCombat() || bot->IsBeingTeleported() ||
                    IsOnTaxi(bot) || bot->GetTransport() || bot->GetVehicle() || bot->IsFlying() || bot->IsFalling())
                {
                    itr->second.holdNote = bot ? std::string(bot->GetName()) + ": " + holdReason(bot)
                                               : std::string("a companion is not in world");
                    itr->second.noteBotGuid = bot ? botGuid : 0;
                    if (g_cfg.debug >= 1)
                        LOG_INFO("server.loading", "[CompanionQuestSync] {} is held back ({}s left to follow)",
                                 itr->second.holdNote,
                                 (dungeonExit ? Timing::DungeonExitRetry : Timing::FollowRetry) - age);
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
                    if (itr->second.noteBotGuid == botGuid)
                    {
                        itr->second.noteBotGuid = 0;
                        itr->second.holdNote.clear();
                    }
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

                if (itr->second.noteBotGuid == botGuid)
                {
                    itr->second.noteBotGuid = 0;
                    itr->second.holdNote.clear();
                }

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
                             travel.holdNote.empty() ? std::string("companions caught up")
                                                     : "not followed: " + travel.holdNote);
                itr = g_pendingFollow.erase(itr);
            }
            else
                ++itr;
        }
    }
