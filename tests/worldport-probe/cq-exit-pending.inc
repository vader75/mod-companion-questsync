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
