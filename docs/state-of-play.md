# State of play

What changed in the layout, and the rules that follow from it. Any session working on this
module should read this before touching files — the copies used to drift and this is how
that was fixed.

*Last updated: 25 November 2026, at `6fcbd15`. If you are picking this module up in a fresh
session, read this page, then `README.md` (install, commands, config, test commands) and
`docs/companion-roles.md` (roles → gear/talents/strategies).*

## Where the code lives

| Location | Role | Rule |
|---|---|---|
| `github.com/vader75/mod-companion-questsync` (`master`) | **Project of record** | Everything lands here |
| `D:\Projects\wow-legends-companion-questsync` | Working clone | Edit, commit, push |
| `D:\Projects\wow-legends\modules\mod-companion-questsync` | Clone inside the realm tree (git-ignored there) | **Pull only** — never edit |
| `D:\Build\WOW_Legends_Server_Source\modules\mod-companion-questsync` | Build tree | Refreshed from a clone, never edited |
| `<client>\Interface\AddOns\` | Installed addons | Installed by `scripts/install-to-server.sh` |

**Both local clones are clones now.** The realm copy used to be a hand-copied folder; it was
found four commits behind (`b3ab88f` + one comment line) while the build tree was current.
That is the drift this layout exists to prevent: never edit in two places, and never copy
files between them by hand.

```bash
# bring any clone up to date
git -C D:/Projects/wow-legends/modules/mod-companion-questsync pull --ff-only
git -C D:/Projects/wow-legends-companion-questsync pull --ff-only

# they must report the same commit
git -C D:/Projects/wow-legends/modules/mod-companion-questsync rev-parse --short HEAD
git -C D:/Projects/wow-legends-companion-questsync rev-parse --short HEAD
```

Refresh the build tree and the client from a clone with
`scripts/install-to-server.sh <server-src> [<client>/Interface/AddOns]` — never file by file.

## The addon rename (v0.9)

The pins addon is **`CompanionPins`**; it was `WLPins` before v0.9.

- Folder `addon/CompanionPins/`, files `CompanionPins.toc` / `CompanionPins.lua`
- Globals and saved variables: `CompanionPinsDB` (was `WLPinsDB`)
- Unchanged: the chat command `/cpins`, the title **Companion Pins**, and the wire protocol
  (`CSYNC|` system lines) — no server rebuild is needed for the rename
- `install-to-server.sh` installs by folder name, so a rename must update it in the same
  commit; otherwise the next install recreates the folder you just retired

Pins already on a map survive the rename: the saved-variable files were migrated in place
(`WTF/Account/<account>/SavedVariables/WLPins.lua` → `CompanionPins.lua`, with the global
renamed inside). The old `WLPinsDB` global is **not** read at runtime — the TOC declares
`SavedVariables: CompanionPinsDB`, so the client never loads the old name.

## Documentation moved

`docs/companion-test-plan.md` and `docs/companion-lifecycle-report.md` used to sit in the
realm repo, linked from here as `../../docs/...`. Those links resolve in the realm tree but
point at nothing in this repository — the one place the README ships. They describe this
module, so they now live in `docs/` here.

- The test plan is a living checklist (T1–T9, about 40 minutes) — use it after any change
  that touches companion behaviour.
- The lifecycle report is a dated snapshot (r76, 12–13 November). History, not design.

## Verified at this revision (`6fcbd15`, 25 Nov 2026)

- `python -B -m unittest discover -s tests` — 369 passed, 13 skipped
- `python -B addon/CompanionSync/test_parse.py` — 14/14 payload cases
- `python -B tests/lua_sandbox.py` — PASS (Core.lua and UI.lua load, launcher built, `/csync` answers)
- Lua syntax compiles for `CompanionPins.lua`, `CompanionSync/Core.lua`, `CompanionSync/UI.lua`
- `src/mod_companion_questsync.cpp` byte-identical across the working clone, the realm clone
  and the build tree (check with `md5sum` on the three paths; CRLF differences alone are not
  drift)
- Both addons installed in the client as `CompanionPins` + `CompanionSync`

## Flows, in the order they run

1. **Change** the module in this clone — never in the realm copy, never in the build tree.
2. **Refresh the build tree**: `scripts/install-to-server.sh D:/Build/WOW_Legends_Server_Source`,
   then `touch` the changed `.cpp`: a build can otherwise report success without recompiling it.
3. **Build** (cmake is not on PATH):
   `"D:/Applications/VisualStudio2022BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"`
   `--build D:/Build/build --config RelWithDebInfo --target worldserver --parallel 12`, and expect
   `0` matches for `error C|error LNK|error MSB`. The `rc.exe` step dies with
   `MSB6003 … The filename or extension is too long` at ~31 KB of include dirs: run
   `python scripts/fix-resource-includes.py` (from this clone), which trims the four
   `ResourceCompile` items in the generated `D:/Build/build/src/server/apps/worldserver.vcxproj`
   to `D:\Build\build;D:\Build\WOW_Legends_Server_Source\src\server\apps`, and rebuild. No source
   changes, and a CMake reconfigure undoes it - rerun the script then.
4. **Prove it compiled**: the module `.obj` must be newer than the source it came from, and any
   change that adds a *string literal* should be checkable in
   `build/bin/RelWithDebInfo/worldserver.exe` (`python -c` over the bytes — comments and static
   function names prove nothing).
5. **Stage** a copy as `D:/AppData/wow-legends/builds/worldserver-<date>-<slug>.exe`. Staged is a
   finished state: the running realm keeps the previous binary until the user restarts.
6. **Deploy only on the user's word**, and only through the portal
   (`http://127.0.0.1:8080/#/server`): Stop the Worldserver card → wait for `:8085` to leave
   netstat *and* the process to exit → copy the exe → Start once. Never signal or spawn the
   worldserver by hand; the portal is its parent.
7. **Verify the boot**: one pid on `:8085` and `:7878`, `server info` answering with a small
   uptime, `[CompanionQuestSync] enabled (…)` in `Server.log`, no `Missing property
   CompanionQuestSync`, nothing new in `Crashes/`.
8. **Commit per logical change and push**, then close with `HEAD == origin/master`, 0 dirty,
   0 unpushed in both repos. A verified fix does not wait to be asked for.

## Checking the live realm

- `.companionadmin integrity` over SOAP is the read-only health check (owners gone, bodies gone,
  bound twice, also in `wowlegends_companion`, ledger orphans). It works with nobody logged in.
- SOAP authenticates against the account table; the credentials are in
  `D:/AppData/wow-legends/portal.json` (`security.soapUser` / `security.soapPassword`), and the
  envelope needs a bare `<command>` child — `<ns1:command>` returns "Command can not be empty".
- Player commands (`.companionsync …`, `.companion …`) do **not** work over SOAP; the
  `.companionadmin` twins do.
- Module log lines go to `D:/Applications/wow-legends-server/Server.log`, which is truncated on
  every boot — collect evidence before a restart.
- This realm runs `cq_bindings` / `cq_pool` with **four** companions bound to one owner,
  `LevelMode 2`, `SpecFollowRole 2`, `Debug 2`, `IntervalSeconds 30`.

## Still open

- `docs/verification-open.md`: one whisper per pass under contention, and the 300 s nudge
  throttle that may be suppressing the second candidate before the selection rule sees it.
- Pin *payloads* are in the live log (`pin sync -> … 2 part(s)`), but whether the client actually
  draws them after the `CompanionPins` rename is still a player's-eye check.
- Role switching (gear + talents + strategies together) is gated at `SpecMinLevel 10` and has run
  against a level-15 owner, but the switch itself has not been observed in a live session.

## Deep-review builds (21 Nov 2026, three passes over module + addons)

- **r123 safety** (`c8a929e`): MailHome could move the same Item* twice (three tidy passes) — postHome now dedupes by pointer; the mail whisper had SayToOwner(owner, bot) — swapped; vital "Finished" whispers now bypass the queue cap, jump the chooser and drain on clean passes; CompanionPins checks torn syncs key-by-key (no sparse-table `#`); coordinate whispers only from senders named in a sync line; purge covers g_failedSeen/g_walkAt/g_positionNoticeAt and the owner+bot stuck-notice map.
- **r124 togetherness** (`90185cd`): accept/abandon hooks walk every bound companion (abandon = the sweep's full three-call drop, so no orphan source items); death whisper once per death + `d` flag in the comp-sync payload (addon card shows "dead"); ghost-release no longer drags the living companion to a graveyard (teleport hook skips ghosts, executor holds 3 min, OnPlayerResurrect re-arms follow); taxi follow armed by zone/map change only when `!IsInFlight()`; combat-skipped follows retry for 60 s; summon RECALLS in-world companions beyond 120 yd (with dead/combat/instance guards); dungeon/raid maps excluded from follow (TeleportTo would land the bot in its own instance).
- **r125 polish** (`3fac33e`): `.companionsync level` is raise-only (never GiveLevel downwards); gather reached-but-full now whispers why; commands print names, not guids; the grouped-gate message matches what still runs; g_multiCompanion is per owner (speaker map); CompanionSync cards keep their first slot (no reorder under the mouse), releasing expires in 25 s, clicks acknowledge with "asking the server...", notices fade after 8 s, the header subtitle shows "N companions · server answering"; CompanionPins: hand-in pins live 5 min and a g→h transition refreshes the timestamp, lifetime persists, LoadDB prunes expired pins, the legend lists "to finish", label-collision math compares label centres, AddPin dedupes by quest too, and pins draw only on their own zone's map.
- Deferred on purpose: in-window quest log panel, window scale option, per-zone pin calibration.

## The no-gather option set (28 Nov 2026)

- **`ProfessionNoGather` (0/1/2, default 0)** — how a companion's professions are established.
  Mode 0 is today's behaviour (the gathering half supplies the crafts). Mode 2 establishes both
  skills at the level's tier ceiling without walking a node: no gather strategy is armed (the
  per-pass upkeep re-asserts the strategy toward the mode, because playerbots drops strategies on
  spec and level changes), no pick or knife is granted, the zone catch-up stands down, and every
  reagent a known recipe asks for is granted in small stacks at craft time — the vendor-only
  scoping widens to all reagents, scoped to the craft being made, never as a stockpile. A flip
  into mode 2 takes effect at the level each companion already is (the per-level teach latch is
  cleared so the next pass re-teaches). Mode 1 logs what mode 2 would do (at `Debug 2`, and only
  while a raise is pending) and changes nothing of what mode 2 would change — gathering continues
  as in mode 0.
- **`ProduceIntervalSeconds` (180, floor 30) / `ProduceMaxPerInterval` (4, 0 = uncapped) /
  `ProduceCombatFirst` (1)** — the quiet craft's cadence, budget and ordering. The interval
  replaces the hardcoded 180 s; the budget counts items across every product in one pass (the
  manual `.companionsync produce` report stays uncapped); combat-first plans the combat-useful
  lane before raw stock where a profession splits across lanes (Blacksmithing's goods before its
  smelting). The quiet-craft log line only reports a made product, so Debug 2 stays readable.
  The potion ladder
  (118/858/929/1710/3928/13446) was verified against this realm's `item_template` — required
  levels 1/3/12/21/35/45, so the best known recipe is the tier-appropriate one.
- The suite's `s13` scenario (realm repo, `tests/companion-sim/scenarios.py`) flips the live conf
  to mode 2 for its own window and asserts the teach lines (`set <profession> to <cap> without
  gathering`) and a quiet-craft product line, then restores the conf.
