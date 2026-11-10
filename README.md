# mod-companion-questsync

Keep a playerbots companion on the same quest log — and the same level — as its owner, so the two can quest together without the bot falling behind on chains or being locked out of quests by level.

Built for AzerothCore 3.3.5a with [mod-playerbots](https://github.com/liyunfan1223/mod-playerbots) and the WOW Legends realm, works with any module that stores an owner → bot binding in a table.

## What it does

Every 30 seconds (and instantly on quest accept) the module reconciles one player's quest log onto the companion bound to them:

| Behaviour | Detail |
|---|---|
| **Mirror quests** | Adds the owner's quests to the companion, through the normal `AddQuest` path — eligibility, class/race locks and prerequisites still apply |
| **Instant on accept** | `OnPlayerQuestAccept` mirrors the quest in the same call, no 20 s wait |
| **Mirror completion** | When the owner's copy is `COMPLETE`, the companion's is too — but never for a quest that delivers items the companion does not actually hold (that copy could never be handed in) |
| **Level alignment** | `LevelMode` 0/1/2: off, raise-only, or exactly the owner's level. Quests gate on `quest_template.MinLevel`, so this is what unblocks a low companion |
| **Self-heal** | A copy left `COMPLETE` that can never be rewarded is reopened, so its own AI collects what it owes and hands in |
| **Bag management** | Sells vendor trash (via playerbots), wears upgrades, fills empty bag slots with a 16-slot bag, and only then bins disposable items — below `ItemKeepQuality`, never quest items, starters, equippable gear or anything a live quest needs |
| **Explains itself** | Whispers the owner what it cannot take and why, what it still owes (`Large Candle 0/8`), where the turn-in NPC is (map coordinates), and keeps asking for help every five minutes while a quest is unfinished. Rate-limited, queued, never spammy |
| **Observability** | Startup banner, login greeting, `.companionsync` commands, a 5-minute heartbeat in the log, and a one-line session summary on logout |

## Requirements

- AzerothCore 3.3.5a (built and tested against the WOW Legends 1.7.0 sources, core `82d3bf23`)
- [mod-playerbots](https://github.com/liyunfan1223/mod-playerbots) — optional at compile time, required for the bag/AI features
- A table mapping `owner_guid` → `bot_guid` (default `wowlegends_companion`; set `CompanionQuestSync.BindingTable` to yours)

The module's own quest logic needs nothing but the core; without mod-playerbots it compiles and syncs quests, but cannot sell or equip.

## Install

```
cd <azerothcore>/modules
git clone https://github.com/<you>/mod-companion-questsync.git
```

Re-run CMake (the module set changed) and rebuild `worldserver`. Copy `conf/mod_companion_questsync.conf.dist` to `configs/modules/mod_companion_questsync.conf` if your build does not deploy it automatically.

## Commands

| Command | Effect |
|---|---|
| `.companionsync` | Reconcile now, and report what changed |
| `.companionsync status` | Levels, quest counts, and every quest the companion is behind on with the reason |
| `.companionsync level` | Force level alignment now |

All three are `SEC_PLAYER`; a player can only affect their own companion.

## Configuration

See `conf/mod_companion_questsync.conf.dist` — every option is documented there. The ones worth knowing first:

| Key | Default | Meaning |
|---|---|---|
| `Enable` | 1 | Master switch |
| `IntervalSeconds` | 20 | Sweep interval (min 5) |
| `LevelMode` | 1 | 0 off, 1 raise-only, 2 exactly the owner's level |
| `SyncCompletion` | 1 | Mirror the owner's `COMPLETE` status |
| `NotifySkippedQuests` | 2 | 0 silent, 1 class/race only, 2 every refusal |
| `BindingTable` | `wowlegends_companion` | Owner → bot mapping table |
| `TidyInventory` | 1 | Managed bag space |
| `KeepFreeSlots` | 2 | Slots to keep free |
| `SellJunk` | 1 | Ask the bot's AI to sell vendor trash |
| `TidyVendorJunk` | 1 | At zero free slots, bin what playerbots rates vendor trash |
| `TidySurplus` | 1 | Last resort: spare consumables/materials |
| `ItemKeepQuality` | 2 | Never bin this quality or better (2 = green) |
| `ExtraBagItem` / `ExtraBags` | 20400 / 4 | Fill empty bag slots with this bag item |
| `MapPins` | 1 | Push companion quest areas to the owner's map |
| `ForceAccept` | 0 | Insert quests bypassing eligibility (for broken chains only) |
| `DropExtraBotQuests` | 0 | Remove companion quests the owner does not have |
| `Debug` | 0 | 0 silent, 1 changes, 2 + heartbeat |

## Notes and limitations

- **`AiPlayerbot.SyncQuestWithPlayer` should be 0** if you want companions to gather their own quest items. With it on, mod-playerbots "will not loot quest items" (its own wording) and completes the bot's copy the moment the owner hands in — which is the state this module exists to avoid.
- Quests with `RequiredItemId` need those items in the companion's own bags to be rewarded; the module will not pretend otherwise.
- Timed quests are skipped by default (`SkipTimedQuests`), as are quests the companion's class or race cannot take (reported once, not counted as failures).
- The sweep is owner → companion only. A companion that races ahead is left alone unless `DropExtraBotQuests` is enabled.
- Map pins are best effort: the 3.3.5 client decides whether to draw POIs for quests outside its own log, so the whisper always carries map coordinates as a fallback.
- **One companion per player.** `FindCompanionGuid` returns a single `bot_guid` for an owner, and the sweep, heartbeat, whisper funnel and pin sync are all keyed on the owner guid. A second bound companion is not tracked: it will not be levelled, will not have quests mirrored, and will not appear on the map. Supporting more than one companion needs the binding lookup to return a list, a per-bot key through those paths, and a pin format that names the companion — the addon already understands a `@Companion` suffix on pin entries (`43,65,h=Kobold Candles@Beta`) so the wire format can grow without breaking existing clients. Until that work is done, document one companion and keep the binding table to one row per player.

## Companion addon: coloured map pins

`addon/WLPins/` is an optional 3.3.5a client addon that draws the companion's
turn-ins on your world map. The client decides how quest areas look and will not
draw one for a quest outside your own log, and the quest-POI packet carries no
colour, so this cannot be done server-side. The addon reads the coordinates the
module already whispers (`... map 82, 34`) and pins them in colour: gold for a
copy the companion owns but you have finished, green for a quest you share, blue
for a ready turn-in.

Client-side only — no server change, no core edit. See `addon/WLPins/README.md`.

## Licence

GPL-2.0, matching AzerothCore and mod-playerbots.
