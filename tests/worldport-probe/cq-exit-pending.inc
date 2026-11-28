    struct PendingFollow
    {
        uint32 queuedAt;
        bool verifyDestination = false;
        uint32 map = 0;
        float x = 0.0f, y = 0.0f, z = 0.0f;
        uint32 sourceMap = 0;
        uint32 sourceInstance = 0;
        std::string holdNote;   // why a companion was last held back from following
        uint32 noteBotGuid = 0; // the companion holdNote is about
        std::set<uint32> whisperedBots; // already asked the owner not to summon these
    };
