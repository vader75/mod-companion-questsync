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
