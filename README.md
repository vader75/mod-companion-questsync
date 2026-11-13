# mod-companion-questsync

Keeps a player's **companions** on the same quest log — and at a level that can take
those quests — so they can always quest together. Up to four companions per owner,
each one a real bot character bound, dressed, named and released by this module.

A companion is a body this module bound to an owner in **its own table**,
`cq_bindings`. It does not need `mod-wowlegends` to exist: it claims bodies from the
`mod-playerbots` AddClass pool itself, wipes them, names them, dresses them and
returns them when released. The only thing it can borrow from `mod-wowlegends` is a
read of that module's binding table, so two modules never level the same body — and
that is switchable off (`RespectLegacyBindings = 0`). Nothing in the core,
`mod-playerbots` or `mod-wowlegends` is patched.

Design notes: [`docs/companion-lifecycle-report.md`](../../docs/companion-lifecycle-report.md).
Test plan: [`docs/companion-test-plan.md`](../../docs/companion-test-plan.md).

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

1. **Upkeep, always** — level alignment (`LevelMode`), bags (`ExtraBags`), and the
   starting kit on a body that has none. This is *not* gated by grouping: a companion
   that slipped out of the party is still kept healthy. Level changes wait for a quiet
   moment (`SkipInCombat`); quest copying does not.
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
5. **Pins and whispers** — the open quests of companions the owner cannot see are
   pushed to the owner's map (`MapPins`, `PinSyncSeconds`), and anything worth saying
   is said once, in words, naming the next action.

Class- and race-locked quests are expected for a differently-built companion: they
are counted separately and reported (`NotifySkippedQuests`). Objective counters are
deliberately not copied — group quest credit already reaches the companion — and
timed quests are skipped because their client timer cannot be transferred.

## Commands

Player commands are `SEC_PLAYER`: no RBAC row, no database change. They are not
available over SOAP, for the same reason `.companion` is not.

| Command | Effect |
|---|---|
| `.companionsync` | one pass now: counts, locks, skip reasons |
| `.companionsync status` | bindings, both levels, both quest counts |
| `.companionsync level` | level-align only |
| `.companionsync quests` | compare the two logs, quest by quest |
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
| `TidyInventory` / `KeepFreeSlots` | 1 / 2 | keep bag room so pickups never stall |
| `SellJunk` / `TidyVendorJunk` / `TidySurplus` | 1 / 1 / 1 | what it may sell or destroy to make room |
| `ItemKeepQuality` | 2 | quality it never destroys (2 = green) |
| `ExtraBagItem` / `ExtraBags` | 20400 / 4 | bags it hands a companion that has none |
| `MapPins` / `PinSync` / `PinSyncSeconds` | 1 / 1 / 600 | quest pins on the owner's map |
| `AnnounceAtStartup` / `AnnounceOnLogin` | 1 / 1 | one line, so players know the module is running |
| `Debug` / `HeartbeatSeconds` | 0 / 300 | log detail, and the heartbeat window for `Debug = 2` |

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

- **No source coupling**: includes core and mod-playerbots headers only — no
  `mod-wowlegends` header, no shared symbol, no build dependency.
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
[`addon/WLCompanionGroup`](addon/WLCompanionGroup): companion roster, a button for every
action, a draggable minimap launcher, and quest pins. It is optional — the module works
with commands alone — and it reads the same protocol lines.

## Licence and release

- **Licence**: GNU General Public License, version 2 — see [`LICENSE`](LICENSE). The addon in
  `addon/WLCompanionGroup` carries the same terms.
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

Edit here, then copy into the build tree before building. Keeping a tracked copy in
both places is what let the two drift a thousand lines apart.
