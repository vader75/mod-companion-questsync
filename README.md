# mod-companion-questsync

Keeps a player's **companions** on the same quest log — and at a level that can take
those quests — so they can always quest together. Up to four companions per owner,
each one a real bot character bound, dressed, named and released by this module.

A companion is a body this module bound to an owner in **its own table**,
`cq_bindings`. It does not need `mod-wowlegends` to exist: it claims bodies from the
`mod-playerbots` AddClass pool itself, wipes them, names them, dresses them and
returns them when released. The only thing it can borrow from `mod-wowlegends` is a
read of that module's binding table, so two modules never level the same body — and
that is switchable off (`RespectLegacyBindings = 0`).

> [!NOTE]
> **Companion surplus trading requires an AzerothCore patch.** The
> [trade acceptance guard](patches/azerothcore-trade-offer-guard.patch) applies to
> both automatic and manual surplus offers. It checks exact stacks
> and the companion's reserve at acceptance, rejects duplicate items before transfer,
> and prevents bot trades from being accepted during combat.
>
> Apply it before building worldserver using the command under [Build](#build).
> The module installer does not apply it automatically. Without the patch,
> companion surplus offers are disabled; ordinary trades and the rest of the
> module still work. No patch to `mod-playerbots` or `mod-wowlegends` is required.

Test plan: [`docs/companion-test-plan.md`](docs/companion-test-plan.md) — a 40-minute
in-game checklist covering every command in this file.
State of play: [`docs/state-of-play.md`](docs/state-of-play.md) — where the clones live and
which one to pull.
Lifecycle snapshot: [`docs/companion-lifecycle-report.md`](docs/companion-lifecycle-report.md)
— what was found and fixed at r76 (12–13 November 2026). A dated record, not the design;
the design is described under "What it owns" and "Independence" below.

## Why it is needed

`mod-playerbots` handles two related things, both one-sided:

| Existing behaviour | Where | What it leaves out |
|---|---|---|
| `AiPlayerbot.SyncQuestWithPlayer` | `TalkToQuestGiverAction::ProcessQuest` | completes the *bot's* copy only once the bot next talks to that giver |
| Quest-share auto-accept | `QuestStrategy` → `AcceptQuestShareAction` | only fires when the **player** clicks "Share quest", per quest |

Nothing notices the owner has a quest the companion lacks, and nothing keeps the
companion high enough level to take it: `Player::SatisfyQuestLevel` gates on
`quest_template.MinLevel`, while a companion's level is whatever pool body it was
claimed from — usually level 1.

## What it does

Two entry points, one rule set:

- **instant** — `PlayerScript::OnPlayerQuestAccept`: the moment the owner accepts a
  quest, the companion is given it. No tick wait.
- **sweep** — every `IntervalSeconds` (30 s): anything the hook missed (logins, level
  gates now satisfied, drop-extras, upkeep).

Per pass, for each owner with bindings:

1. **Upkeep, always** — level alignment (`LevelMode`), bags (`ExtraBags`), the starting
   kit on a body that has none, and **wearing upgrades**. This is *not* gated by
   grouping: a companion that slipped out of the party is still kept healthy. Level
   changes and equipping wait for a quiet moment (`SkipInCombat`); quest copying does
   not. Upgrade checks run every three minutes and separately from the bag-pressure
   rescue, so a better item goes on the companion rather than sitting in its pack until
   the pack fills.
2. **Quests** — every quest in the owner's log the companion lacks is added via
   `AddQuestAndCheckCompletion` (or `Player::AddQuest` with `ForceAccept = 1`).
   Eligibility uses the core's own rules; a refusal is reported with the gate that
   caused it. With `RequireGrouped = 1` this step waits until the companion is in the
   owner's party; the default is `0`.
3. **Completion** (`SyncCompletion = 1`) — a quest the owner finished is marked
   complete on the companion so both can hand in together.
4. **Drops** — `DropExtraBotQuests = 1` (default) removes companion quests the owner
   does not have, so the logs match; `DropExtraGraceSeconds` gives a freshly added
   quest time to settle first.
5. **Gathering, on its own clock** — a collection quest whose items lie on the ground
   ("eight handfuls of oats") is not a kill-and-loot quest: the item is in a chest
   standing in a field, and playerbots lists such an object as loot but never walks to
   one. Every two seconds the companion is stepped to a spawned object it needs, takes
   what it holds, and the emptied object is deactivated so it cannot be farmed twice.
   The whole objective clears in one pass, not one object per thirty-second pass, and
   the companion says so once when it finishes
   (*"I picked up the last of '<quest>' off the ground…"*). If the world will not yield
   the items, `CatchUpGroundItems` finishes the quest after `CatchUpIdleSeconds` with
   no progress — but only ground items, and only for a quest the **owner has already
   been rewarded**, so the companion catches up rather than running ahead.
   `.companionsync catchup` (and the window's **Ground quests** button) does the same on
   demand.
6. **Teleports** — a hearthstone, a portal or a summon moves the player, so the companion
   goes too (3 s later, once the destination has loaded). Hooked on the teleport itself:
   walking away never teleports anything, and a companion is never dragged into a
   battleground or arena. `FollowThroughTeleport = 0` turns it off.
7. **Hand-ins happen in reach of the giver** (`HandInMode = 2`, the default) — a finished
   quest rewards itself when the companion is within `HandInNearDistance` (40 yards) of the
   giver: the same core call the gossip window would make, no giver object required, the
   companion still stepping up to the NPC so the turn-in is seen. On a choice of rewards it
   picks the highest item level and says which it kept. Beyond that range nothing is
   rewarded and nothing blinks - the companion whispers to be led closer, and the gold pin
   on the giver's map points the way. `1` rewards from any distance; `0` returns to the
   bot-AI path, which needs the giver in the companion's sight - the shape that produced
   "I cannot talk to the quest giver".
8. **Pins and whispers** — the open quests of companions the owner cannot see are
   pushed to the owner's map (`MapPins`, `PinSyncSeconds`), and anything worth saying
   is said once, in words, naming the next action. The roster line is only re-sent when
   something that matters changes: free bag slots drift on their own and wait for a
   slower clock rather than being broadcast every pass. A quest owing work in several
   areas draws one pin per area (deduped by converted point, capped at four per quest),
   each on its own zone's map; a ready hand-in is one pin at the giver. For a collection
   quest the work pin leads with the creatures that drop the required item (from the loot
   tables, nearest to the companion) - a wanted poster's POI points at the poster and an
   outline's corner is wherever the cartographer started - and falls back to the POI
   vector when no loot source resolves. The module
   publishes the full payload once a minute alongside its `Hello` heartbeat, so a client
   that just `/reloaded` is repopulated without waiting for a change or the 10-minute
   cycle (`SendHeartbeat`).

Class- and race-locked quests are expected for a differently-built companion: they
are counted separately and reported (`NotifySkippedQuests`). Objective counters are
deliberately not copied — group quest credit already reaches the companion — and
timed quests are skipped because their client timer cannot be transferred.


## Companions that grow: spells, talents, professions, and their own two hands

Everything in this section is autonomous or one click; nothing needs a command typed.

- **A raise teaches itself.** A level the module grants - syncing to the owner, or the
  **Level** button - brings its own spells, weapon and profession skills, class spells and
  everything a trainer would sell at the new level (the bot factory's own learn path), and
  the fresh talent points are spent along the spec the companion already has: incrementally,
  never a reset, never a re-roll. The core grants no spells on level-up (3.3.5 puts them
  behind trainers) and playerbots' own auto-learn is gated to random bots, of which this
  realm runs none - without this a raised companion fought on at rank-1 spells with its
  talents unspent.
- **Train all** (window button, `.companionsync learn [name]`) parses every companion in
  the world for spells and talents missed while that fix was not in place. Each companion
  whispers its own verdict; the handler sums the totals in chat.
- **Professions** - every companion takes a class-appropriate pair, and one of the two
  always gathers, because the point of a companion profession is what it picks up while
  walking with you:

  | Class | Pair |
  |---|---|
  | Warrior, Paladin, Death Knight | Mining + Blacksmithing |
  | Hunter, Rogue, Shaman | Skinning + Leatherworking |
  | Druid, Mage, Priest, Warlock | Herbalism + Alchemy |

  The choice is kept for the body's life (playerbots' per-bot value store, key
  `cqProfPair`); re-teaching never re-rolls it, so gathered skill is never thrown away.
  The gathering ceiling follows the level in the classic tiers - 75 at level 1,
  150 (Journeyman) at 10, 225 (Expert) at 20, 300 (Artisan) at 35 - raised automatically
  on every level raise and by the **Professions** button; gathered progress is never reset.
  The gathering tools a pair needs (Mining Pick, Skinning Knife) are handed out with it.
  The module teaches its supported recipes at their trainer's minimum **attained skill**,
  not at the character's skill ceiling. Already-known recipes are not removed.
  Produce is a utility list, not a complete profession-leveling route. If a
  supported recipe goes gray before the next one unlocks, the companion needs
  other recipes for skill-ups or a higher gathering-zone catch-up minimum.
  The playerbots **gather strategy** is armed on every teach: on a timer the companion
  adds only nodes its own skill can work, so it stops at what it can actually use while
  it follows you, and questing carries on normally - the trigger is deliberately
  low-priority.
- **The land asks a minimum of its gatherers** (`GatherCatchUp`, default on) - a companion
  walking into a zone it cannot work yet catches its gathering skill up to what the zone
  asks, once per zone, with one line in chat (*"I caught my Herbalism up to 150 - this
  is what this land asks"*). What the zone asks is read from the world itself, not
  guessed: at boot the module indexes every ground node (gameobject chests with a
  gathering lock) and per zone takes the **highest node requirement its own nodes
  carry** - Bruiseweed asks Herbalism 100, a Copper Vein Mining 1 - resolved separately
  per skill line and cached, because herbs and veins answer differently for the same
  zone. The zone's level in the classic five-points-per-level currency is only the
  fallback when no node resolves (Skinning has no fixed nodes: it walks the mob levels).
  The answer is then clamped to the **level's own tier ceiling** - what the land asks,
  within what the level can hold - so a sloppy `WorldMapArea` bounding box (this realm's
  Westfall row spans half the eastern kingdoms) can never drag a neighbouring zone's
  high node in and hand a level-12 companion a 210 skill. Organically-gathered skill
  above the zone minimum is never touched. The production half of the pair rises with
  the gathering half - what it gathers, it can work - and the catch-up lines ride the
  whisper queue's vital lane, so a fresh-login burst of invitations cannot delay them
  for long. The card in the window shows the skill against its ceiling.
- **Talents and spells arrive when the level does** - organic level-ups included. A
  companion grinding with the party earns its talent points through the core's own
  level-up, which grants nothing; the module teaches at that exact moment (the
  `OnPlayerLevelChanged` hook), so Train all is a backstop for anything raised before
  the hook existed, not the only teacher.
- **Vendors are used** (`VendorRestock`, default on) - standing near a trade or
  repair NPC, the companion tops up the shop-bought reagents its known recipes ask
  (vials, threads, coal) to a small stock - the same shopping-trip stand-in Produce
  makes - and a repairer's hammer mends what the day's work dented, paying the
  usual price. At most once every five minutes per companion, one quiet line either
  way ("I restocked 5x Empty Vial and had my gear repaired.").
- **Crafting is a habit** (`AutoProduce`, default on) - every three minutes per
  companion, outside combat and while the companion is not trading. Mining and
  Blacksmithing smelt bars and make sharpening stones; Leatherworking makes armor
  kits; Alchemy brews healing potions. For each type, the companion picks the best
  known recipe it has the skill and raw materials for, falling back to a lower one
  if needed. Reagents are consumed. Only a small stock of explicit vendor supplies
  such as vials and thread is topped up; raw materials are never granted. Results
  land in bags without a crafting whisper.
- **Produce** (card button, `.companionsync produce <name>`) - the same craft on
  demand, with one chat report: what was made or what to do next. Finish any open
  trade and get out of combat before asking.
- **Background inventory**: companions put on their own upgrades and quietly sell only
  grey items that Playerbots also marks as vendor junk, at a merchant they can interact
  with. Crowded bags never trigger item deletion or bulk mail. Quest items and uncertain
  gear stay with the companion.
- **Curated trade** (`AutoTrade`, default on): when you and a companion are stationary
  and out of combat in a rested area, it can offer a clear armor upgrade or supplies you
  requested. One automatic offer per owner per ten minutes; you still click Accept.
  Gear must be equippable and beat the active-spec score for that slot by more than 10%;
  weapons, jewelry, sets, and gear with ambiguous effects remain manual decisions.
- **Mail**: at a reachable mailbox, the same policy can send up to three whole stacks.
  There is never more than one unclaimed companion parcel at a time, and a 15-minute
  cooldown covers all companions for the owner. Raw materials and vendor junk stay out.
- **Trade** (card button, `.companionsync trade <name>`) is the manual path for both
  useful items and supported profession surplus. It preserves at least five of each
  material or product, and more where a known supported recipe needs it.
- **Revive** (card button on a dead companion, `.companionsync revive <name>`) - within
  forty yards the body rises where it fell, at the half-strength a real resurrection
  would carry; beyond that its spirit releases at the graveyard the way the client's own
  Release button does, its ghost walks home on its own, and the resurrect hook re-arms
  the follow so it rejoins at your side.

## Commands


Player commands are `SEC_PLAYER`: no RBAC row, no database change. They are not
available over SOAP, for the same reason `.companion` is not.

| Command | Effect |
|---|---|
| `.companionsync` | one pass now: counts, locks, skip reasons |
| `.companionsync status` | bindings, both levels, both quest counts |
| `.companionsync level` | level-align only |
| `.companionsync quests` | compare the two logs, quest by quest |
| `.companionsync catchup` | finish a quest the companion cannot gather for itself |
| `.companionsync learn [name]` | teach every companion (or one) its missing spells, spend its unspent talents |
| `.companionsync professions [name]` | teach the class profession pair, re-cap to the level, arm gathering |
| `.companionsync produce <name>` | craft what its own gathering already paid for |
| `.companionsync trade <name>` | open a trade and hand over its surplus |
| `.companionsync delivery preview` | list the curated items ready from summoned companions without moving them |
| `.companionsync delivery collect <name>` | ask a nearby companion for curated items only |
| `.companionsync delivery request <item ID>` | for this login, top up a supported crafted consumable to five including bags, bank and pending mail; `delivery clear` clears requests |
| `.companionsync revive <name>` | raise a fallen companion, or let its spirit walk home |
| `.companionsync pins` | push map pins now |
| `.companionsync refresh` | resend the panel payload now |
| `.companionsync list` | every companion with its state |
| `.companionsync pool` | pool stock for the race/class picker |
| `.companionsync create <race> <class> <name>` | claim a free body and bind it |
| `.companionsync summon <name>` | call a companion to your side |
| `.companionsync dismiss <name>` | send it away, keep the binding |
| `.companionsync release <name>` | wipe it, return it to the pool, forget it |
| `.companionsync bind <bot>` | adopt an existing bot as a companion |
| `.companionsync unbind <name>` | stop tracking it without releasing it |

Administrator commands are `SEC_ADMINISTRATOR` and work from the console and over
SOAP, including for **offline** owners:

| Command | Effect |
|---|---|
| `.companionadmin create <player> <race> <class> <name>` | create a companion for someone |
| `.companionadmin release <player> <name>` | release one of their companions |
| `.companionadmin pool <player>` | send pool stock to that player |
| `.companionadmin integrity [fix]` | audit both tables; `fix` repairs what is safe |
| `.companionadmin gear <player>` | what each bound body is wearing, slot by slot |
| `.companionadmin dress <player>` | run the dressing pass now |
| `.companionadmin catchup <player>` | finish that player's ground-object quests |

## Talking to the client

The module pushes state to two addons; it never asks the client for anything.

- **Channel:** every payload is a system message prefixed `CSYNC|`, sent to the owner's
  session. Not a whisper — a whisper needs a live speaker, and the map and the window have
  to work with no companion in the world.
- **Both consumers register `CHAT_MSG_SYSTEM`, ignore any line without the tag, and strip
  exactly it.** When the channel or the tag changes, both change in the same pass: an
  addon left on the old channel receives nothing and keeps drawing what it last wrote to
  saved variables.
- **Presence:** on login the module sends `CSYNC|Hello <interval>` unconditionally, outside
  every cosmetic switch. That is how the client knows the module is loaded, and the interval
  it carries sets how long the window waits for the first payload before it says anything.
- **Payloads:**

  | Line | Meaning |
  |---|---|
  | `Comp sync N: name,class,race,level,quests,flags,free\|…` | the roster; flags are `o` in world, `g` in party, `s` same level, `b` behind |
  | `Pin sync N: x,y,h=Quest\|x,y,g=Quest\|…` | the map, which is authoritative: anything not listed is removed |
  | `Pool stock N: 1-5=8\|…` | free bodies per race-class, so the picker can offer only combinations that exist |

  Sets longer than one line are chunked on `|` boundaries as `1/3:`, `2/3:`, `3/3:` and
  rebuilt by the client before anything is drawn.

The addons are `addon/CompanionSync` (the window and the buttons — companion roster, create,
summon, dismiss, release, **Ground quests**) and `addon/CompanionPins` (titled **Companion Pins**:
the map markers and the key in the map's corner). Both are client-side only.

## Configuration

All options live under `CompanionQuestSync.` in
`configs/modules/mod_companion_questsync.conf`; the shipped `.conf.dist` documents
each one. The defaults that matter most:

| Option | Default | Meaning |
|---|---|---|
| `Enable` | 1 | master switch; 0 makes the module inert |
| `AutoSync` | 1 | periodic pass; 0 = command only |
| `IntervalSeconds` | 30 | pass cadence (floor 5) |
| `LevelMode` | 1 | 0 off, 1 raise to the owner, 2 exact (raise and lower) |
| `SyncCompletion` | 1 | mark complete when the owner's copy is complete |
| `ForceAccept` | 0 | 1 = bypass `CanTakeQuest` and insert anyway |
| `DropExtraBotQuests` | 1 | make the logs identical (destructive) |
| `DropExtraGraceSeconds` | 120 | grace before a new quest may be dropped |
| `DropFailedBotQuests` | 0 | also drop quests the companion actually failed |
| `RequireGrouped` | 0 | 1 = only sync **quests** while it is in your party |
| `AllowCreate` | 1 | players may create companions themselves |
| `SkipInCombat` | 1 | no level changes while either side fights |
| `SkipTimedQuests` | 1 | never copy timed quests |
| `BindingTable` | `cq_bindings` | bindings this module owns (identifier only) |
| `PoolTable` | `cq_pool` | its own release ledger (identifier only) |
| `RespectLegacyBindings` | 1 | also honour `mod-wowlegends`' table; 0 = never look at it |
| `ExtraArmourSlots` | 1 | belt, bracers, gloves and cloak in the class's armour type |
| `PoolAccountType` | 2 | `playerbots_account_type` value that marks an AddClass body |
| `NotifySkippedQuests` | 2 | 0 silent, 1 class/race only, 2 all refusals |
| `TidyInventory` / `KeepFreeSlots` / `BagMargin` | 1 / 2 / 8 | bag-space warnings and an eligible merchant sale before retrying a blocked quest pickup |
| `MailHome` | 1 | curated delivery at a mailbox; turning this off never destroys anything |
| `SellJunk` | 1 | quietly sell confirmed grey vendor junk at an interactable merchant |
| `ExtraBagItem` / `ExtraBags` | 20400 / 4 | bags it hands a companion that has none |
| `MapPins` / `PinSync` / `PinSyncSeconds` | 1 / 1 / 600 | companion quest pins via CompanionPins; native quest-giver icons are unchanged |
| `GatherCatchUp` | 1 | catch a companion's gathering skill up to what the zone's own nodes ask, once per zone, clamped to its level's tier ceiling; the pair's production half rises with it |
| `VendorRestock` | 1 | near a trade or repair NPC: shop reagents topped up to a small stock, gear mended at the usual price; at most once every five minutes per companion |
| `AutoProduce` | 1 | craft what its gathering paid for every three minutes per companion, outside combat, silently; the Produce button gives the report on demand |
| `AutoTrade` | 1 | open a curated trade in a rested area when stationary and out of combat; you accept |
| `CatchUpGroundItems` / `CatchUpIdleSeconds` | 1 / 120 | finish a collection quest whose items lie on the ground (oats in a field, cloth in a crate) when the world will not yield them: only ground items, only for a quest the owner has already been rewarded |
| `FollowThroughTeleport` | 1 | take the companion through your hearthstone, portal or summon (never into a battleground); walking away never teleports it |
| `HandInMode` / `HandInDelaySeconds` / `HandInNearDistance` | 2 / 20 / 40 | hand-in in reach of the giver (2 = auto within 40 yd, whispers to be led beyond; 1 = server-side anywhere; 0 = bot AI); a finished quest stays visible (gold pin, companion at the giver) 20 s before it rewards |
| `AnnounceAtStartup` / `AnnounceOnLogin` | 1 / 1 | one line, so players know the module is running |
| `Debug` / `HeartbeatSeconds` | 0 / 300 | log detail, and the heartbeat window for `Debug = 2` |

## Other modules' settings this module depends on

The companions are `mod-playerbots` bots: several of playerbots' own settings change what a
companion does, and two of them override this module's logic from outside it. These are
realm-level settings (set in the live `configs/modules/playerbots.conf`, not in this
module's own `.conf`), and each row says what a companion does **differently** when the
value is not the one this module expects. Verified on this realm; the flipped-away values
are what shipped by default.

| Setting | File | Expected | What a companion does differently when it differs |
|---|---|---|---|
| `AiPlayerbot.SyncQuestWithPlayer` | playerbots.conf | **0** | At `1` (the shipped default) playerbots force-completes a bot's copy of any quest the player has complete the moment the bot's AI talks to the giver — **owed quest items ignored** — and a second path force-completes any incomplete quest it discusses. It rewards companions for quests they never finished, overriding this module's owed-items guard, and it silences the companions' own "Cannot talk to quest giver" accept attempts by letting the AI accept remotely. At `0` this module owns completion truth, and the accept hook keeps the party within interaction distance of the giver instead |
| `AiPlayerbot.LootNeedRollLevel` | playerbots.conf | **2** | `1` (shipped default) downgrades every equip-upgrade vote NEED→GREED; the companions then never roll need on their own upgrades |
| `AiPlayerbot.LootGreedRollLevel` | playerbots.conf | **1** | `0` (shipped default) converts every GREED→PASS — so no BoE item ever reaches a companion's bags, gear progression stalls forever beyond the starter kit, and the equip-upgrade dispatcher has nothing to equip. At `1` companions roll NEED on real upgrades and GREED on vendorables, which this module's bag ladder sells |
| `AiPlayerbot.AutoPickReward` | playerbots.conf | **yes** | `no` makes the bot AI list reward choices and ask for help on its own turn-ins — chat noise the module's scored pick already handles; `yes` lets its turn-in pick silently, and the module's own choice (the same stat-weight scoring) covers the hand-ins the module performs |
| `DataDir` | worldserver.conf | the server's own `data` directory | This module parses `DataDir/dbc/WorldMapArea.dbc` at boot for pin zones (the core's own WorldMapArea store is broken twice on this repack). A wrong `DataDir` costs the boot line "cannot open … - pin zones fall back to the terrain probe", and the terrain probe is the quietly wrong layer the module was built to avoid |
| AddClass bodies marked `playerbots_account_type = 2` | `wl_playerbots` database | **2** | `PoolAccountType` reads this to identify pool bodies; unmarked bodies fail the pool checks and are refused as companions |

Two behaviours are **by design on the boundary** between the modules, not config: the
companions' own AI may still accept quests and turn in its own completed copies when it is
in range of a giver (its narration, its choice of reward — both scored the same way this
module scores), and this module's accept hook mirrors the owner's accepts instantly either
way. Whoever reaches a turn-in first performs it; the other sees the copy rewarded and
stays quiet.

## What a new companion arrives wearing

Dressed the moment it spawns, not on the next pass: a class weapon, a chest, legs and
boots from a verified starter set, then belt, bracers, gloves and cloak in the class's
own armour type. Deliberately a short curated list rather than a database query —
selecting by item level produced tuxedos and NPC-only templates, and one hand-picked
item (Tattered Cloth Pants) is Horde-only in this repack, which is how a companion
once ended up with bare legs. `ExtraArmourSlots = 0` keeps the four core slots.

## What it owns

The module is self-contained: it creates its schema, claims bodies from the AddClass
pool, dresses them, releases them back, and keeps its own ledger.

| Table | Purpose |
|---|---|
| `cq_bindings` | owner → body bindings, one row per companion |
| `cq_pool` | its own release ledger (`released_at`, `reason`), pruned after 180 days |

Install the schema before the first start. It is idempotent, it migrates rows out of
the old `wowlegends_companion_*` names, and it repairs tables created before the
primary key existed:

```
mysql -u <user> -p wl_characters < sql/companion-questsync.sql
```

Repair is a command, not a hope. `.companionadmin integrity [fix]` finds dangling
owners, missing bodies, one body bound twice, over-cap owners, ledger orphans, bodies
still carrying a pool name, and party rows left behind by a companion that was
released while nobody was looking — and repairs the ones that are safe to repair.

## Independence

- **No `mod-wowlegends` build dependency**: includes core and `mod-playerbots`
  headers only. Companion surplus offers require the core patch noted above.
- **One optional read**: by default it reads `mod-wowlegends`' binding table so two
  modules never level and quest the same body. `RespectLegacyBindings = 0` removes
  even that, and the module runs on a server where that module is not installed.
- **Bodies come from the AddClass pool** that `mod-playerbots` maintains
  (`PoolAccountType`, excluding the AuctionHouseBot). That is a data convention, not
  an API call; `integrity` reports it when a body stops matching.
- **Its own names**: `cq_bindings` and `cq_pool`, created by its own SQL.

## Safety

- **Its own tables, the core's APIs** for everything else: quests, levels, items and
  group changes go through core and playerbots entry points, so
  `character_queststatus` cannot drift from the live map.
- **Identifier whitelist** on both table names; anything else disables the module
  rather than risking SQL built from a config file.
- **Bot-session rail** — a binding must resolve to a bot session and must not be the
  owner, so a stale row cannot level or re-quest a real character.
- **Removals run inline from the command handler**, never from the update tick and
  never within 20 seconds of a summon. This is not a preference: playerbots tears a
  body down in place, and doing it from the tick is what used to kill the process.
- **Hand-in is untouched** — the module only sets `COMPLETE`; rewards and turn-in stay
  with the bot AI.

## Build

On a compatible, unpatched AzerothCore source tree, apply the core patch once before
building. Run this from the module repository root:

```sh
git -C "/path/to/server-src" apply "$(git rev-parse --show-toplevel)/patches/azerothcore-trade-offer-guard.patch"
```

Part of the worldserver build (AzerothCore static modules):

```
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSCRIPTS=static -DMODULES=static
cmake --build . --config RelWithDebInfo --target worldserver
```

Copy it into a server source tree with the script, which also installs the addon if
you give it a client path:

```
scripts/install-to-server.sh <server-src> [<client>/Interface/AddOns]
mysql -u <user> -p wl_characters < sql/companion-questsync.sql    # first install only
```

Adding the module requires a CMake **re-run** (the module config list is baked in at
configure time), and on an existing install the `.conf.dist` must be copied to
`configs/modules/mod_companion_questsync.conf` before restarting.

The in-game window is part of this repository,
[`addon/CompanionSync`](addon/CompanionSync): companion roster, a button for every
action, a draggable minimap launcher, and quest pins. It is optional — the module works
with commands alone — and it reads the same protocol lines.

## Licence and release

- **Licence**: GNU General Public License, version 2 — see [`LICENSE`](LICENSE). The addon in
  `addon/CompanionSync` carries the same terms.
- **Compatibility note**: this module includes headers from `mod-playerbots`, which is
  licensed AGPL-3.0, and GPL-2.0 cannot be combined with AGPL-3.0. If you redistribute the
  module alongside playerbots — which is the normal deployment — AGPL-3.0 is the correct
  licence for this repository.
- **Release**: module, schema and addon ship together from this repository. The addon's
  `.toc` carries the version (`1.0.8`); the schema is `sql/companion-questsync.sql`, which
  creates `cq_bindings` and `cq_pool` and migrates the pre-decoupling tables.
- **What is not here**: no core, `mod-playerbots` or `mod-wowlegends` source. Those keep
  their own licences and their own repositories.

## Where this code lives

This repository is the single source of truth for the module, the addon and the tests.
The realm source tree holds a clone at `modules/mod-companion-questsync`, which is where
the worldserver build picks it up; that clone is ignored by the realm repository, so
exactly one copy of this code is tracked anywhere.

```
git clone https://github.com/vader75/mod-companion-questsync <server-src>/modules/mod-companion-questsync
```

Edit here, then copy into the build tree before building:

```
scripts/install-to-server.sh D:/Build/WOW_Legends_Server_Source [client-AddOns-dir]
```

The script copies `src conf sql addon` (the whole module, so the clone can never again
be "current" on two files and missing seven), and installs both addons when a client
directory is given. Keeping a tracked copy in two repositories is what let the two
drift a thousand lines apart.
