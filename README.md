# Companion Quest Sync for AzerothCore

Play with up to four persistent Playerbots companions. QuestSync helps them pick up eligible quests, keep pace with your level, and turn in their own completed quests. **CompanionSync** is the in-game control panel; **CompanionPins** adds their quest markers to the world map.

[Play with companions](#play-with-companions) · [Install the server module](#install-the-server-module) · [Upgrade or roll back](#upgrade-or-roll-back) · [Troubleshooting](#troubleshooting) · [Command reference](#command-reference)

## Play with companions

1. On a realm running this module, download the matching revision (on GitHub, **Code → Download ZIP** downloads the current branch). Copy `addon/CompanionSync` and `addon/CompanionPins` into your WoW **3.3.5a** client's `Interface/AddOns/` folder. Each `.toc` must be directly inside its named addon folder. Enable the addons on the character screen and log in. If you're replacing an already loaded copy, use `/reload`; restart the client if it doesn't list a newly installed addon.
2. Click the CompanionSync minimap button or type `/csync`. Select an existing companion and click **Summon**. To claim one, click **+ Add companion**, choose an available race and class, enter a name, and click **Create**. If no combinations are available, ask your realm operator about reviewed stock. Death Knights aren't available for new claims.
3. Accept a quest and play normally. The selected companion's **Quests** section shows their active quest count. Use **Group quests** for a comparison or **Map** for locations. With CompanionPins enabled, hover a green `!` for objective details or a gold `?` for a ready hand-in. These markers are separate from your own quest markers. Bring the companion within interaction range of the actual NPC or object to turn in; a location hint alone does not mean the giver is usable.
4. Use **Dismiss** to send a companion away without giving it up. **Untrack** pauses tracking but keeps ownership and the occupied roster slot. **Release** requests retirement: it does not wipe the character, free its name, or make it available to other players. An operator must finish retirement offline to free the slot.

Both addons are optional: the module can be used with in-game commands alone. Use the addons from the same checkout as the server module where possible. The checked-in versions are CompanionSync **2.7.0** and CompanionPins **1.2.3**; the [CompanionSync TOC](addon/CompanionSync/CompanionSync.toc) and [CompanionPins TOC](addon/CompanionPins/CompanionPins.toc) are the version sources. DragonUI is optional; see the [CompanionSync guide](addon/CompanionSync/README.md) and [CompanionPins guide](addon/CompanionPins/README.md).

### Ask for supplies

Select a summoned companion, open **Supplies**, then choose the handoff you want:

| Button | What it does |
| --- | --- |
| **Choose supply → Take** | Ask for a specific item the companion can spare from its bags. |
| **Choose supply → Craft** | Ask for one standard batch of a supported recipe. The companion uses its own reagents and keeps any specialization bonus. |
| **Collect delivery** | Ask for curated deliveries, such as clear armor upgrades or previously requested supplies, without choosing a specific stack. |

Stand near the companion with both of you out of combat and no other trade open. Check the items and click **Accept** in the normal trade window. An open or filled window is not a completed trade. If the addon reports an incomplete or unconfirmed transfer, check your bags before trying again. A named **Take** or **Craft** order never substitutes another item, charges trade gold, or falls back to mail. If an item says **Not ready**, use **Refresh supplies** after conditions change.

Keeper stock stays with the companion. The server can offer curated trades automatically at rested stops and, if configured, send eligible curated parcels at a reachable mailbox. It does not mail general bag overflow or vendor junk. The named picker requires both core patches described below.

### What companions do on their own

- **Quests:** The accept hook copies eligible quests; a periodic pass catches missed accepts. Companions earn their own objective credit. Completion rechecks their own objectives, and hand-ins use native reward handling. Class and race locks still apply. Timed quests are skipped by default.
- **Ground objectives:** Companions can loot spawned ground objects. **Ground quests** manually fills missing items that the server indexes as ground loot for summoned companions; it does not grant kill credit. Automatic synthetic catch-up is off by default. If enabled, that background fallback waits for stalled progress and requires the owner to have received the quest reward.
- **Progression:** Companions can receive starter gear, equip upgrades, learn spells on level-up, and spend new talent points along their existing specialization. The **Professions** section shows gathering and crafting skill. Use **Train** for spells and talents or **Teach skills** for professions.
- **Starter bags:** On realms with starter-bag support installed, newly claimed companions get one chance to equip up to four **Portable Holes** (24 slots each) on their first verified summon. Only empty bag slots are filled; existing bags and their contents stay put. Existing companions aren't upgraded, and removed Portable Holes aren't replaced.
- **Professions:** Warriors, paladins and death knights use Mining/Blacksmithing; hunters, rogues and shamans use Skinning/Leatherworking; druids, mages, priests and warlocks use Herbalism/Alchemy. Supported recipes depend on attained skill and aren't a full profession-leveling route. Crafting consumes reagents; trade or repair vendors can restock shop supplies or repair gear. Bag cleanup sells only eligible grey vendor junk and does not delete items.
- **Behaviour you direct:** Each companion has a **role** — `auto`, `tank`, `healer`, `dps` or `support` — that decides which Playerbots strategies it runs; `support` heals without ever off-healing, and `healer` is the switch that does. **Engagement** says when it joins a fight (`auto`, `mine`, `joined`, `never`). **Quiet** trades damage for less threat on a `dps`/`support` companion; it is off by default because it costs damage. Distance is the companion's own formation, not a follow ring: `auto` gives each role a sensible shape, and `close`/`tight`/`loose`/`spread`/`lead`/`off` override it. Out of combat an open trade window or gossip menu, or standing still for a few seconds, brings everyone to arm's length; a companion that is not tanking also has its gap closer withheld, so a `dps` warrior walks to the target instead of charging it. The panel exposes all four; `.companionsync inspect` says what the server believes. See [roles](docs/companion-roles.md) and [distance](docs/companion-formation.md).
- **Travel:** Companions can follow actual teleports, not simply blink when you walk away. The module does not follow you into a battleground or arena. Use **Revive** on a fallen companion nearby; a distant companion can release and recover as a ghost instead.

## Install the server module

### Requirements

- A buildable AzerothCore 3.3.5a source tree with compatible [mod-playerbots](https://github.com/mod-playerbots/mod-playerbots), working realm databases, and extracted client data. Follow the Playerbots installation guide for its required AzerothCore fork and branch; do not assume stock AzerothCore is interchangeable. Test the combination on a staging realm. QuestSync is part of the worldserver build, not a plug-in for a prebuilt executable.
- Access to the **characters** database and permission to configure and rebuild worldserver. Basic QuestSync does not require a Playerbots patch. Starter bags require the Playerbots initialization patch and characters migration below.
- `mod-wowlegends` is **not** required. If your realm also uses its legacy companion allocator, apply the [legacy custody guard](patches/mod-wowlegends-companion-custody.patch) (verified base `523563e`), and deploy both custody-aware modules together before allowing claims. Do not leave an old allocator running against shared stock.

### Build

1. From the AzerothCore source root, clone this repository. For an existing clone, update it with Git rather than copying files over it.

   ```sh
   git clone https://github.com/vader75/mod-companion-questsync.git modules/mod-companion-questsync
   ```

2. For companion trade offers, check and apply the [trade acceptance guard](patches/azerothcore-trade-offer-guard.patch) first, then the [trade commit receipt](patches/azerothcore-trade-offer-commit.patch). The optional [loot reserve patch](patches/azerothcore-companion-loot-reserve.patch) keeps the last eight slots from being used by new stacks of confirmed grey miscellaneous junk from non-skinnable corpses when `SellJunk` is on. It does not change existing inventory or vendor sales. From the AzerothCore source root, in a shell such as Bash or Git Bash:

   ```sh
   git apply --check modules/mod-companion-questsync/patches/azerothcore-trade-offer-guard.patch &&
     git apply modules/mod-companion-questsync/patches/azerothcore-trade-offer-guard.patch
   ```

   Once the guard is in place:

   ```sh
   git apply --check modules/mod-companion-questsync/patches/azerothcore-trade-offer-commit.patch &&
     git apply modules/mod-companion-questsync/patches/azerothcore-trade-offer-commit.patch
   ```

   Skip an already applied patch. If a check fails, reconcile your core revision or existing changes before proceeding; don't force the patch. The receipt patch expects a guard-only baseline and must not be layered over an earlier receipt implementation.

   | Build | Companion delivery support |
   | --- | --- |
   | Neither patch | Quest sync, roster and map pins still work. Companion trade offers are disabled; ordinary player trades and separately configured curated mail do not need the patches. |
   | Guard only | Guarded automatic and manual surplus offers work. **Collect delivery** falls back to the older trade flow without addon receipts. Named picker requests are unavailable. |
   | Guard and receipt | Adds named **Take/Craft** requests and addon delivery status. Completion requires a confirmed save, not just a closed trade window. |

3. For starter bags, apply the [Playerbots initialization patch](patches/mod-playerbots-companion-initialization.patch) from the **mod-playerbots** directory, not the AzerothCore root. Skip it if already applied; if the check fails, reconcile your Playerbots revision rather than forcing it.

   ```sh
   cd modules/mod-playerbots
   git -c core.autocrlf=false apply --check ../mod-companion-questsync/patches/mod-playerbots-companion-initialization.patch &&
     git -c core.autocrlf=false apply ../mod-companion-questsync/patches/mod-playerbots-companion-initialization.patch
   cd ../..
   ```

4. Re-run CMake so it discovers the new module, then build and install worldserver using your usual AzerothCore procedure. For example, from the core root on Windows with Visual Studio 2022:

   ```sh
   cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DSCRIPTS=static -DMODULES=static
   cmake --build build --config RelWithDebInfo --target worldserver
   ```

   Reuse the generator and settings of your existing build if they differ. The [copy helper](scripts/install-to-server.sh) is for staging files into a non-Git module folder and can copy both addons. It does not apply core patches, build the executable, or complete the database setup.

### Prepare the database and start

1. Stop worldserver cleanly, wait for character database writes to drain, and take a restorable backup. For an existing realm, follow the [upgrade checklist](#upgrade-or-roll-back) before changing anything.
2. From the QuestSync repository root, import both SQL files. Replace `acore_characters` and `your_db_user` with your realm's actual database and user. Do not import into the world, auth, or Playerbots database.

   ```sh
   for f in companion-questsync companion-custody companion-roles companion-engage companion-quiet; do
     mysql -u your_db_user -p acore_characters < "sql/$f.sql" || break
   done
   ```

   The four custody files are additive: `companion-roles`, `companion-engage` and `companion-quiet`
   add the `role`, `engage` and `quiet` columns the behaviour controls read. Skip any that are
   already applied — the module probes for each column and says so instead of pretending, and
   `.companionsync role` answers "roles are not set up yet on this realm" when the column is
   missing.

   The SQL adds tables if absent. It does not migrate old bindings, repair old indexes, or populate stock. `cq_bindings` is the roster, `cq_companion_custody` holds reviewed reservations and each companion's `role`, `engage` and `quiet`, and `cq_pool` is historical only. An empty custody table is a safe starting point, not a ready-to-use pool. Use the [custody guide](docs/companion-custody.md) to review and enroll existing owners or clean AddClass stock offline. AddClass account type alone is not proof that a character is safe to claim.

   If you built with the Playerbots initialization patch, apply the [starter-bag migration](sql/companion-starter-bags.sql) **once**, after the custody schema and before starting worldserver:

   ```sh
   mysql -u your_db_user -p acore_characters < sql/companion-starter-bags.sql
   ```

   Without both the working Playerbots hook and this migration, claims still work but don't qualify for the grant, even after a later upgrade. Leave `ExtraBagItem` on its normal fallback (default: Pumpkin Bag); do not set it to Portable Hole (`51809`). See the [custody guide](docs/companion-custody.md) for the grant's save and rollback limits.
3. Install the built binary and copy the [module config template](conf/mod_companion_questsync.conf.dist) to `configs/modules/mod_companion_questsync.conf` in your server config directory. If upgrading, merge settings rather than overwriting your live config. Review the settings below before you start.
4. Start worldserver and check `[CompanionQuestSync]` output for schema or custody errors. On a test character with reviewed custody, check the roster, summon and dismiss, accept an eligible quest, and verify the companion's progress. Walk to a live quest ender and verify the companion's reward separately from yours. If you enabled trade, test a manually accepted delivery and check the received items. The [acceptance checklist](docs/companion-audit-acceptance.md) covers negative cases as well.

## Configure the realm

All module keys below use the `CompanionQuestSync.` prefix. The [config template](conf/mod_companion_questsync.conf.dist) lists the rest.

| Setting | Shipped default | What to check |
| --- | --- | --- |
| `Enable` / `AutoSync` / `IntervalSeconds` | `1` / `1` / `30` | Enable the module and set how often it reconciles existing quests. |
| `LevelMode` | `1` | Raise only, in both modes: `0` leaves levels alone, `2` keeps the companion level with you by raising it. Neither demotes. |
| `SyncCompletion` / `ForceAccept` | `1` / `0` | Recheck companion completion. Leave `ForceAccept=0` to respect eligibility. |
| `DropExtraBotQuests` / `DropExtraGraceSeconds` | `1` / `120` | **Can remove companion-only quests** after the grace period. Set `DropExtraBotQuests=0` if you want to keep those quests. `DropFailedBotQuests` is off by default. |
| `RequireGrouped` | `0` | Set to `1` to limit quest sync to grouped companions, not general upkeep. |
| `HandInMode` / `HandInDelaySeconds` | `1` / `20` | `0` disables module hand-ins; `1` uses native interaction; `2` also gives a travel reminder. `HandInNearDistance` controls the reminder threshold, not native interaction range. |
| `MapPins` / `PinSync` | `1` / `1` | `MapPins` pushes the markers, `PinSync` governs the protocol; both must be on. Install CompanionPins on the client to see them. |
| `Formation` | `auto` | Distance: `auto` by role, or `close`/`tight`/`loose`/`spread`/`lead`/`off`. Presets map to Playerbots' own formations (`close`→`near`, `tight`→`queue`, `loose`→`circle`, `spread`→`far`, `lead`→`shield`), so combat movement stays Playerbots'. Out of combat a trade window, gossip menu or standing still gathers everyone at arm's length. |
| `SuppressGapClosers` | `1` | Mutes Charge for a companion whose role is not `tank` or `auto`, and clears the mute when it tanks again. A warrior's charge is a trigger on its class strategy, not a strategy, so this is the only lever that reaches it. It puts the spell on a long cooldown rather than removing it: removing it leaves the charge action registered, which costs the companion its whole walk into combat. Set to `0` to leave companions alone. |
| `AllowCreate` / `PoolAccountType` | `1` / `2` | Player-side claims need reviewed stock on eligible AddClass accounts. Disabling player creation is not a substitute for guarding all allocators. |
| `AutoProduce` / `AutoTrade` / `MailHome` | `1` / `1` / `1` | Background crafting, guarded town trades, and curated mail are separate switches. `AutoTrade=0` leaves manual offers available on a patched build. |

In `playerbots.conf`, set `AiPlayerbot.SyncQuestWithPlayer = 0` so Playerbots does not force-complete copies independently of QuestSync's objective checks. Review `AiPlayerbot.LootNeedRollLevel = 2`, `AiPlayerbot.LootGreedRollLevel = 1`, and `AiPlayerbot.AutoPickReward = yes` if you want the intended upgrade rolls and automatic reward choice. These settings affect Playerbots realm-wide. In `worldserver.conf`, point `DataDir` at your extracted server data; QuestSync uses `dbc/WorldMapArea.dbc` for pin coordinates.

## Upgrade or roll back

- Start with the [custody migration and retirement guide](docs/companion-custody.md) and [acceptance checklist](docs/companion-audit-acceptance.md). Record your current revisions, binary, config and addon versions. Rehearse the characters restore and startup on an isolated copy before touching production.
- Stop all world processes and confirm saves have drained before schema or custody changes. Preserve existing bindings and progress, review conflicting historical rows offline, and deploy both guarded allocators together if you use the legacy module. Never turn old release rows into free stock or rerun a destructive compatibility migration.
- Keep the previous compatible binary and a verified backup together. Do not start older, custody-unaware allocators against the migrated database. A pre-upgrade restore discards later progress; plan for that before cutover. Do not use `sql/companion-retirement-maintenance.sql` as a routine install step; it is an optional offline tool with the prerequisites in the custody guide.

## Troubleshooting

| Symptom | First check |
| --- | --- |
| No panel or empty roster | Confirm CompanionSync is enabled and the server is running the module, then click **Refresh**. `/csync test` checks the client parser; it does not prove the server is sending data. |
| No available race/class or an old companion won't summon | Check reviewed stock or ownership, current character identity, and custody/schema logs. Don't re-enroll an ambiguous body to bypass a refusal. |
| Released a companion but the slot is still taken | Automatic retirement finishes once the logout is confirmed; if it was not, the row stays `retiring` and `sql/companion-retirement-maintenance.sql` (plus the portal's maintenance preview) is the backup path. |
| A new companion didn't receive Portable Holes | Confirm the patched build and starter-bag migration were in place before the claim. The grant only fills empty slots and will not retry after the attempt has been spent. |
| Quest missing or won't turn in | Check eligibility, quest-log and bag space, and the companion's own objectives. Summon it and bring it to a live, usable ender. Mode `0` disables the module's own hand-ins. |
| No pins | Open the matching **zone** map, check both server pin settings and the addon, then use `/cpins refresh` with the map open. Check `DataDir` if zone mapping looks wrong. |
| A warrior charges although it is not the tank | Check its role is `dps`/`support`/`healer` (not `auto`), that `sql/companion-roles.sql` was imported, and `SuppressGapClosers=1`, then `.companionsync inspect <name>`: a muted Charge is listed. `tank` and `auto` keep the charge by design. |
| Companions stand too far away, or too close | `.companionsync inspect <name>` prints the formation the server believes, the leash it implies and the realm's pitch-in radius. `Formation=off` leaves distance entirely to Playerbots, and a role's `auto` shape follows the role, not the last preset. |
| Supplies unavailable or trade cancels | Confirm server patch support, companion control, range, combat state, and free bag space. For Craft, also check materials and any required station. Refresh the picker before retrying. |

For a bug report, include the core/Playerbots/module revisions, addon versions, relevant settings, exact steps, expected and actual behaviour, and a short log excerpt. Leave out credentials and connection strings.

## Command reference

Player commands run in game, not from the server console. Use the addon for the named supply picker; its catalog and request messages are an internal interface.

| Task | Commands |
| --- | --- |
| Sync and inspect | `.companionsync`, `.companionsync status`, `.companionsync quests`, `.companionsync list`, `.companionsync inspect [name]` |
| Direct behaviour | `.companionsync role <name> auto\|tank\|healer\|dps\|support`, `.companionsync engage <name> auto\|mine\|joined\|never`, `.companionsync quiet <name> on\|off`, `.companionsync formation [name] auto\|close\|tight\|loose\|spread\|lead\|off` |
| Manage companions | `.companionsync pool`, `.companionsync create <race> <class> <name>`, `.companionsync summon <name>`, `.companionsync dismiss <name>`, `.companionsync release <name>` |
| Change tracking without transferring ownership | `.companionsync unbind <name>`, `.companionsync bind <bot>` |
| Progression and recovery | `.companionsync level`, `.companionsync learn [name]`, `.companionsync professions [name]`, `.companionsync revive <name>`, `.companionsync catchup` |
| Supplies | `.companionsync produce <name>`, `.companionsync trade <name>`, `.companionsync delivery preview`, `.companionsync delivery collect <name>` |
| Request a recurring top-up for this login | `.companionsync delivery request <item ID>`, `.companionsync delivery clear` |
| Refresh the addons | `.companionsync refresh`, `.companionsync pins`; see the addon guides for `/csync` and `/cpins` options. |

Administrator commands use `SEC_ADMINISTRATOR` and allow console use: `.companionadmin create <player> <race> <class> <name>`, `release <player> <name>`, `pool <player>`, `integrity [fix]`, `gear <player>`, `dress <player>`, and `catchup <player>` (each with the `.companionadmin` prefix). Some actions need a live player or summoned companion. Run `integrity` without `fix` first and review the findings; it does not prove ownership or replace offline custody review.

## Test and reference material

From the module repository root:

```sh
python -B -m unittest discover -s tests
python -B addon/CompanionSync/test_parse.py
```

Some integration cases skip unless their dependencies or disposable fixtures are available. These checks do not replace a build against your core or a gameplay test. Use the [acceptance checklist](docs/companion-audit-acceptance.md) for staging and the [custody guide](docs/companion-custody.md#local-verification) for isolated SQL checks. Behaviour is documented apart from installation: [roles](docs/companion-roles.md) maps each role to the Playerbots strategies it turns on, [distance](docs/companion-formation.md) covers formations, the interaction and idle clamps and the leash, [the strategy map](docs/companion-strategy-map.md) lists every strategy the module may touch, and [the distance review](docs/companion-distance-review.md) records why the module stopped moving companions itself. The addon guides describe the roster, supply and pin messages separately from companion whispers. Older realm-specific notes are in [the historical test plan](docs/companion-test-plan.md), [lifecycle report](docs/companion-lifecycle-report.md), and [state of play](docs/state-of-play.md); use the current acceptance checklist rather than treating those notes as installation steps.

## Licence

See the repository's [GPL version 2 licence file](LICENSE). AzerothCore, Playerbots, and other modules have their own terms. Check licence compatibility before redistributing a combined build; this README does not relicense any component.
