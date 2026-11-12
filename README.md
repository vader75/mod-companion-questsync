# mod-companion-questsync

Keep playerbots companions on the same quest log — and the same level — as their owner, so they can quest together without the bots falling behind on chains or being locked out of quests by level. One companion, or several: see *Several companions*.

Built for AzerothCore 3.3.5a with [mod-playerbots](https://github.com/liyunfan1223/mod-playerbots) and the WOW Legends realm, works with any module that stores an owner → bot binding in a table.

## What it does

Every 30 seconds (and instantly on quest accept) the module reconciles a player's quest log onto every companion bound to them:

| Behaviour | Detail |
|---|---|
| **Mirror quests** | Adds the owner's quests to the companion through the normal `AddQuest` path — eligibility and class/race locks still apply. Prerequisites the owner has already finished are credited first, so a chained quest is not refused for a link the companion never walked |
| **Instant on accept** | `OnPlayerQuestAccept` mirrors the quest in the same call, no 30 s wait |
| **Mirror completion** | When the owner's copy is `COMPLETE`, the companion's is too — but never for a quest that delivers items the companion does not actually hold (that copy could never be handed in) |
| **Level alignment** | `LevelMode` 0/1/2: off, raise-only, or exactly the owner's level. Quests gate on `quest_template.MinLevel`, so this is what unblocks a low companion. Alignment only ever **raises**: an out-levelled companion keeps the level it earned, because `GiveLevel` downwards strips spell ranks |
| **Self-heal** | A copy left `COMPLETE` that can never be rewarded is reopened, so its own AI collects what it owes and hands in |
| **Hands in the turn-in** | When the companion owes a hand-in it is stepped **beside the player, angled away from the giver** — still inside interaction range whenever the player is at the giver, and a hop the player caused reads as "caught up" where a hop to somebody else's coordinates reads as a bug. The reward (`r`) is asked for in the same pass, because its follow AI walks it back within seconds |
| **Knows what is grey** | A quest 5–8 levels below the companion is dropped by its own quest-log cleaner, so the module declines it once ("I have levelled past 'X' – finish that one without me") instead of handing it back every pass |
| **Bag management** | Sells vendor trash (via playerbots), wears upgrades, fills empty bag slots with a 16-slot bag, and only then bins disposable items — below `ItemKeepQuality`, never quest items, starters, equippable gear or anything a live quest needs |
| **Explains itself** | Whispers the owner what it cannot take and why, what it still owes (`Large Candle 0/8`), where the turn-in NPC is (map coordinates), and keeps asking for help every five minutes while a quest is unfinished. Rate-limited and queued, and re-checked before they are spoken so it never complains about a level it has already reached |
| **Follows you out** | A quest the owner abandons leaves the companion too — instantly through `OnPlayerQuestAbandon`, or after `DropExtraGraceSeconds` when the abandon cannot be seen. A quest the owner has already *handed in* is kept, because the companion is catching up; class- and race-locked quests are kept too, since they were never the owner's |
| **Sweeps on login** | The first pass runs the moment you log in instead of waiting for the timer, so nothing shows yesterday's state |
| **Reports its own failures** | A copy the companion failed on its own (a dead escort, an expired script) cannot be finished; it is reported once, and dropped only when `DropFailedBotQuests` is on |
| **Observability** | Startup banner, login greeting, `.companionsync status` / `quests` / `pins`, a 5-minute heartbeat carrying free bag slots and any failures, and a one-line session summary on logout |

## Prerequisites

`Player::SatisfyQuestPreviousQuest` requires every predecessor to be **rewarded**,
which a copied active log can never satisfy for a companion adopted after the chain
was walked — the normal case: quest first, companion second. Before adding a quest
the module walks the same `prevQuests` chain the core walks and calls
`Player::SetRewardedQuest` for the ancestors **the owner has already rewarded**.

That marks the rewarded set and its save flag only: no XP, no gold, no item, no
reward spell, no quest-log slot. Nothing is credited that the owner has not rewarded,
so a shared log cannot put the companion ahead of its owner. One consequence is
worth knowing: a level-1 companion adopted by a level-60 owner inherits that chain
history silently. Parity is the point, but `character_queststatus_rewarded` for a
companion is not a record of what it played.

## Requirements

- AzerothCore 3.3.5a (built and tested against the WOW Legends 1.7.0 sources, core `82d3bf23`)
- [mod-playerbots](https://github.com/liyunfan1223/mod-playerbots) — optional at compile time, required for the bag/AI features
- A table mapping `owner_guid` → `bot_guid`. The shipped default `wowlegends_companion` allows **one companion per player** (its `owner_guid` is the primary key); for several, use the table in *Several companions* and point `CompanionQuestSync.BindingTable` at it

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
| `.companionsync quests` | Every quest the companion carries: state, what it still owes by item and count, where it hands in, how far, and the objective area in map coordinates |
| `.companionsync pins` | Re-send the map-pin set to the client addon |
| `.companionsync level` | Align the level of **every** bound companion |
| `.companionsync list` | What is bound, and what each companion is doing right now |
| `.companionsync bind <name>` | Make an **online** bot one of your companions |
| `.companionsync unbind <name>` | Stop tracking a companion; the bot stays yours |
| `.companionsync summon [name]` | Bring a companion out, or every one that is not out |
| `.companionsync dismiss [name]` | Send a companion away, or all of them; it stays bound |

All of these are `SEC_PLAYER`; a player can only affect their own companions.

`bind` only accepts a bot that is **in the world**. Guids come from a pool, so a row
written for a body that is not running is a promise the module cannot keep — the body can
be released and re-issued to somebody else, and the row would follow the guid rather than
the companion. Bring it out first (`.playerbots bot add <name>`), then bind it.

## Configuration

See `conf/mod_companion_questsync.conf.dist` — every option is documented there. The ones worth knowing first:

| Key | Default | Meaning |
|---|---|---|
| `Enable` | 1 | Master switch |
| `IntervalSeconds` | 30 | Sweep interval (min 5). The accept hook mirrors instantly, so this is only the safety net |
| `LevelMode` | 1 | 0 off, 1 raise-only, 2 exactly the owner's level. Never lowers a companion |
| `SyncCompletion` | 1 | Mirror the owner's `COMPLETE` status |
| `NotifySkippedQuests` | 2 | 0 silent, 1 class/race only, 2 every refusal |
| `AutoSync` | 1 | Run the sweep on a timer; `.companionsync` still works with it off |
| `RequireGrouped` | 1 | Only sync a companion that is in the player's party. With several companions, an ungrouped one is skipped |
| `HeartbeatSeconds` | 300 | Heartbeat window (`Debug` 2); carries one line per companion |
| `BindingTable` | `wowlegends_companion` | Owner → bot mapping table. On this branch point it at a table whose `owner_guid` is **not** unique, or only one companion is ever tracked |
| `TidyInventory` | 1 | Managed bag space |
| `KeepFreeSlots` | 2 | Slots to keep free |
| `SellJunk` | 1 | Ask the bot's AI to sell vendor trash |
| `TidyVendorJunk` | 1 | At zero free slots, bin what playerbots rates vendor trash |
| `TidySurplus` | 1 | Last resort: spare consumables/materials |
| `ItemKeepQuality` | 2 | Never bin this quality or better (2 = green) |
| `ExtraBagItem` / `ExtraBags` | 20400 / 4 | Fill empty bag slots with this bag item |
| `MapPins` | 1 | Push companion quest areas to the owner's map |
| `PinSync` | 1 | Whisper the pin set to the owner's addon (`MapPins` decides what is in it), split across several whispers when it needs to be |
| `ForceAccept` | 0 | Insert quests bypassing eligibility (for broken chains only) |
| `DropExtraBotQuests` | 1 | Remove companion quests the owner abandoned (handed-in and class/race quests are kept) |
| `DropExtraGraceSeconds` | 120 | How long a copy survives before it counts as abandoned |
| `DropFailedBotQuests` | 0 | Also drop copies the companion failed on its own (reported either way) |
| `SkipTimedQuests` | 1 | Do not mirror timed quests the companion cannot finish on schedule |
| `PinSyncSeconds` | 600 | Longest gap between identical pin-sync lines |
| `Debug` | 0 | 0 silent, 1 changes, 2 + heartbeat |

## Several companions (experimental branch)

`feat/multi-companion` tracks every row in the binding table for an owner, not just
the first.

**The stock table cannot hold two companions.** `wowlegends_companion` has `owner_guid`
as its PRIMARY KEY, so one row per owner is a schema rule — the module can loop all it
likes, there is nothing to loop over. Point `BindingTable` at a table without that
constraint:

```sql
CREATE TABLE wowlegends_companion_multi (
  owner_guid INT UNSIGNED NOT NULL,
  bot_guid   INT UNSIGNED NOT NULL,
  companion_name VARCHAR(24) NOT NULL,
  created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  KEY owner (owner_guid),
  KEY bot (bot_guid)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

INSERT INTO wowlegends_companion_multi (owner_guid, bot_guid, companion_name) VALUES
  (<owner>, <bot 1>, 'Healy'),
  (<owner>, <bot 2>, 'Soak');
```

```ini
CompanionQuestSync.BindingTable = wowlegends_companion_multi
```

then `.reload config`. `sql/multi-companion.sql` does all of that and carries your
existing bindings across, so nothing is lost on the switch.

### Summoning and dismissing one companion at a time

`.companion summon` belongs to mod-wowlegends and only knows its single companion, so a
group had no way to call back one specific bot. `summon`/`dismiss` drive playerbots through
the owner's own session — the same path mod-wowlegends uses — so its login hook still does
the part that matters: join the group, follow, enable combat.

The outcome is **verified**, not assumed: playerbots logs a body in on its own schedule, so
ten seconds later the module checks whether the companion is really in the world and says
so if it is not, with the next step (`unbind` then `bind` once the body is back). A silent
no-op is the most expensive failure this module can have, and a summon that does nothing is
exactly that. With no argument, both verbs act on the whole group.

### Membership, and what happens when a companion goes away

Nothing else in the server writes the binding table — not `mod-wowlegends`, whose
`.companion create` and `.companion forget` only touch its own `wowlegends_companion`. So
the module validates every row whenever it fills its binding cache (once a minute per
player) and drops what it cannot believe, telling the player which companion and why:

| Found | Why it is dropped |
|---|---|
| the character is gone | the body was deleted or wiped; the row points at nothing |
| released after it was bound | the body belongs to the pool again, and the pool re-issues bodies |
| bound to another player | two sweeps would level and re-quest one character against each other |
| the same bot twice | the sweep would act twice on one body |
| a fifth companion | a group is five with the player |

Releasing a companion with `.companion forget` therefore clears itself up: the release is
recorded, and the next sweep drops the row and says so. Everything the module remembered
about that body — memos, latches, "already told them" sets, queued whispers — is erased
with it, because bot guids are recycled and the next companion to inherit one would
otherwise inherit its predecessor's silences.

**Four companions, no more.** A party is five with the owner, so four is the ceiling:
a fifth row is ignored and logged rather than swept on every pass, and the same bot bound
twice is ignored the same way. On this realm a bot bound to two owners used to mean two
sweeps levelling one character against each other. Keep `bot_guid` unique in practice — one bot bound to two owners
means two sweeps levelling and questing it against each other.

What changes:

- the sweep loops per companion, aggregating the pass report;
- the heartbeat writes one line per companion, joined with ` | `, so two bots' quest
  counts are never blended into one row;
- whispers are prefixed with the companion's name when more than one is bound — with a
  single companion the prefix is suppressed, because it would be noise. A held line is
  spoken by the companion that queued it — the prefix is applied at send, not at queue;
- pin entries carry `@Name`, so the addon can tell two companions apart on one map;
- `.companionsync quests` and `.companionsync pins` walk the whole binding;
  `.companionsync status` still describes one companion and says how many are bound.

Also handled:

- the pin payload is built once per owner for every companion, merged by coordinate, so
  two companions needing the same giver produce one marker reading "A + B";
- turn-in markers are collected across companions and applied once, so one companion's
  pass cannot clear another's marker;
- the pass memo is keyed per companion;
- the map's quest-area circles are also built once per owner from every companion's log.
  They used to be pushed per companion while the "already sent" key lived per owner, so
  the packet was re-sent most passes and the client — which replaces its circles on
  receipt — only ever showed whichever companion passed last;
- a whisper held by the rate limit is spoken by the companion that queued it, and the
  window is given to whichever companion has gone longest without being heard, so one
  companion cannot starve the rest;
- the addon filters per companion with `/wlpins only <name>` and `/wlpins all`.

Still open:

- the owner-level **session** tally sums every companion (one line per session) instead
  of naming them, and the **whisper throttle** is shared — deliberately, since the player
  should not hear four bots at once;
- `RequireGrouped` is per companion, so every companion has to be in the party or it is
  skipped. `.playerbots bot add <name>` does not always group what it adds; check
  `group_member` if one companion looks ignored;
- verified with two companions live. Four is the cap and the sweep, the cap warning and
  the merged-marker layout are tested, but four bots questing at once have not been played
  for a session.

Add rows to `wowlegends_companion_multi` (or whichever table `BindingTable` names). The
`.companion` commands in mod-wowlegends still manage one companion at a time.

## Notes and limitations

- **`AiPlayerbot.SyncQuestWithPlayer` should be 0** if you want companions to gather their own quest items. With it on, mod-playerbots "will not loot quest items" (its own wording) and completes the bot's copy the moment the owner hands in — which is the state this module exists to avoid.
- Quests with `RequiredItemId` need those items in the companion's own bags to be rewarded; the module will not pretend otherwise.
- Timed quests are skipped by default (`SkipTimedQuests`), as are quests the companion's class or race cannot take (reported once, not counted as failures).
- The sweep is owner → companion only. A companion that races ahead is left alone unless `DropExtraBotQuests` is enabled, and even then only quests the owner neither holds nor has completed are dropped.
- Map pins are best effort: the 3.3.5 client decides whether to draw POIs for quests outside its own log, so the whisper always carries map coordinates as a fallback.
- **Several companions need the multi table.** The binding lookup returns every row, but the shipped
  `wowlegends_companion` cannot hold more than one row per owner. See *Several companions* above for the
  table and the config switch; with the default table the module behaves exactly as it did before.

## Companion addon: map pins

`addon/WLPins/` is an optional 3.3.5a client addon that puts the companion's work on
your world map. The client decides how quest areas look and will not draw one for a
quest outside your own log, and the quest-POI packet carries no colour, so this
cannot be done server-side.

A marker is a coloured quest glyph in an outlined font, about 25 px, sitting on the
point with the quest name beside it - no plate, no black label bar:

| Glyph | Meaning |
|---|---|
| gold **?** | the companion can hand that quest in here |
| green **!** | an area where it still has work to do |
| blue **?** | an errand only the companion can run |

The module sends one authoritative `Pin sync` line - split across several whispers
when there are more entries than one can hold - on login, whenever the set changes,
every ten minutes, and on demand with `.companionsync pins`. The addon treats it as
the whole truth: any pin it does not mention is removed, so a marker cannot outlive
the work it stood for. Tooltips carry the zone, the exact coordinates and
right-click-to-remove. `/wlpins life <minutes>` sets how long a pin lives (30 by
default), `/wlpins offset X Y` calibrates the anchor, and `/wlpins debug` echoes every
whisper the addon hears.

Client-side only — no server change, no core edit. See `addon/WLPins/README.md`.

## Testing

The behaviour here is verified on a live realm by a simulation harness that lives
with the realm sources rather than in this repo (`tests/companion-sim/` in the
`wow-legends` project). It resets a character to level 1, quests it, adds a
companion mid-chain, and then measures level pinning, quest copy, completion sync,
item debt, class locks, hand-ins and grey-quest declines against the module's own
log:

```
python tests/companion-sim/run.py --fresh-companion --all     # single companion
python tests/companion-sim/multi_companion.py                 # several companions
```

`--fresh-companion` performs the order this module needs after `.companion forget`
(forget → restart the worldserver → create) and rotates the companion class, because
a forgotten companion keeps its rewarded quest history.

`multi_companion.py` seeds the multi table, logs the pilot in, adds both bots through the
client and asserts, from the module's own log, that both are swept, that an accept
reaches both, that an abandon drops from both, that whispers name the speaker, that the
pin payload merges their markers, and that the heartbeat carries one line each. It needs
`BindingTable` pointed at the multi table, and it clears the test quest from the owner and
both companions before each check — a companion that already holds the quest produces no
`+` line and looks like a failure.

## Licence

GPL-2.0, matching AzerothCore and mod-playerbots.
