# Multi-companion module — lifecycle report

Written overnight, 12–13 November 2026. No game client was used: everything below was proven
over SOAP and SQL against the live server, or is explicitly marked as waiting on an in-game
check.

---

## 1. Where it stands

The full loop — create, summon, level, quest, dismiss, release, delete — now has a defined
owner for every step, and the module repairs its own records instead of assuming the core
will. Three crashes and two silent no-ops were found and fixed; two design faults were
restructured rather than patched.

| | |
|---|---|
| Deployed | `worldserver.exe` = r76 (per-slot gear with read-back, bag recovery and a creation throttle; offline admin create/release; integrity sweep; ledger retention) |
| Pool | **604** free bodies; every race/class has ≥ 8 |
| Bindings | 1 (`Brandan`, human priest, level 1, offline, bound to Pilot) |
| Integrity | module: 0 problems · core+module audit: **clean** (all checks 0) |
| Automated | `admin_cycle.py` **9/9**, `lifecycle_cascade.py` **8/8**, `integrity_audit.py` clean |
| In-game | waiting on you — `docs/companion-test-plan.md` |

---

## 2. Who owns what

The rule that came out of tonight: **the module owns every row it writes, and owns cleaning
them.** The core is never asked to know about `cq_bindings` or the pool ledger.

| Stage | Owner | What actually happens |
|---|---|---|
| Pick a body | module | AddClass account, offline, unbound in either companion table, not in a guild. Pool counts pushed to the addon as a `pool stock` payload. |
| Claim a body | module | Reset (items, inventory, quest log, level, money, `at_login`), rename `Zz<guid>` → chosen name, register the name in `sCharacterCache`. |
| Bind | module | One row in `cq_bindings`. Cap of 4 enforced at read time; duplicates refused. |
| Summon | module → playerbots | `bot add <name>` dispatched through the owner's own session, **from the update tick**. Verified 10 s later against the body's real online state. |
| Dress | module | A body missing a weapon or chest piece is issued the core's starting list, or the module's fallback kit (cloth + a class-appropriate weapon) when the core has none. |
| Bags | module | Up to 4 × 16-slot bags in empty bag slots, never touching a bag the player gave it. |
| Level | module | `LevelMode 2`: the companion is brought to the owner's level, deferred while either is in combat. |
| Quests | module | Owner's log copied in, completions mirrored, hand-ins delivered when the companion is at the giver, owner's abandons mirrored back **only** when the owner's own class/race can take the quest. |
| Dismiss | module → playerbots | `bot remove` **queued to the update tick** (doing it inline killed the server twice). |
| Release | module | Dismiss if out → wait until the body is really offline → wipe → rename `Zz<guid>` → unbind → ledger row. |
| Character deleted | core | Core removes its rows. **Module repairs its own** within 10 minutes, or immediately with `companionadmin integrity fix`. |
| Account deleted | core | Same as above: our rows go dangling, the sweep removes them. |
| Addon state | module → addon | Authoritative `WLCG|…` system messages; the panel polls `.companionsync refresh` every 8 s while open, so no reopen is needed after a change. |

---

## 3. Faults found and fixed

Each of these was a real failure I could reproduce from the server side.

**1. Dismiss crashed the worldserver.** The handler ran playerbots' `bot remove` inline; that
deletes the body synchronously under the session running the command. Release had already
been fixed this way; dismiss had not. *Fix:* queue it, remove from the update tick, log
`dismiss from the update tick`.

**2. A newly created companion could not be summoned.** pool bodies live only in the
database, so they are absent from `sCharacterCache`; playerbots' `AddPlayerBot` looks the
account id up there and **returns silently** when it finds nothing. *Fix:* `EnsureCharacterCache`
registers account, gender, race, class and level before naming or summoning a body.

**3. Companions arrived naked, then only partly dressed.** Three causes, all fixed. The upkeep
step sat *below* the digest short-circuit, so once a companion's quest logs settled the pass
returned before dressing it. The repack's `playercreateinfo_item` holds a single unrelated row,
so the core's own list is empty. And the dress step asked the core to equip and trusted the
answer — "equip" can fall back to *storing* the item in a bag, so a companion came out wearing a
weapon and boots while its chest and legs sat in the backpack, counted as dressed. *Fix:* every
slot is filled individually and then **read back**; anything that landed in the bag is moved onto
the body by naming the slot explicitly; loose armour in the bags is worn into empty slots; and a
slot that still cannot be filled is an error in the log with the equip error code, naming the slot.

**4. `RequireGrouped = 1` silently disabled everything.** The resolver returned `nullptr` for
an ungrouped companion, so it got no bags, no gear, no level alignment — and nothing was
logged. *Fix:* grouping now gates quest syncing only; upkeep always runs, and a skip logs why.

**5. A released body was unusable for 15 minutes.** Pool selection carried over
mod-wowlegends' re-claim cooldown, which exists because *its* cleanup is asynchronous. This
module resets and renames before it records the release. *Fix:* cooldown removed.

**6. Module messages printed literal `%s`.** printf placeholders where this core wants `{}`.
*Fix:* all converted; failures now show the name or reason.

**7. The panel did not reflect the world.** The server only pushed on change and on a
five-minute heartbeat, and a client cannot ask. *Fix:* `.companionsync refresh` plus an 8 s
poll while the panel is open.

---

## 4. Integrity: what changed, and the evidence

New: **`companionadmin integrity [fix]`**, callable from the console (so from SOAP), plus a
repair pass every 10 minutes. It checks and, with `fix`, removes:

- bindings whose **owner** character no longer exists
- bindings whose **body** character no longer exists
- one body bound **twice**
- a body also bound in **`wowlegends_companion`** (reported, never silently resolved)
- **ledger** rows for bodies that are gone
- ledger rows older than 180 days (retention — nothing reads them after the release)

Evidence, from `tests/companion-sim/lifecycle_cascade.py` (no client needed):

```
[PASS] healthy binding            integrity reports nothing wrong
[PASS] healthy binding kept       row survived the repair
[PASS] owner really deleted       deleted through .character erase (Player::DeleteFromDB)
[PASS] owner deletion detected    owners gone: 1
[PASS] owner deletion repaired    row removed
[PASS] body really deleted
[PASS] body deletion detected     bodies gone: 1
[PASS] body deletion repaired     row removed

8/8 checks passed
```

The full audit (core + module tables, 30 checks) is clean:

```
players marked online while nobody is logged in                    0
multi bindings whose owner character is gone                      0
multi bindings whose body character is gone                       0
one body bound to more than one owner                             0
a body bound by both companion modules                            0
binding rows whose name disagrees with the body                   0
ledger rows for bodies that no longer exist                       0
pool bodies still carrying a Zz name while bound                  0
bodies not on an AddClass account                                 0
clean
```

**Known edge, not fixed:** if the *owner* is deleted while a companion is out, the module
removes the row but cannot dismiss the body (there is no session left to command). The bot
sits in the world until playerbots reclaims it. Nothing corrupts; it is untidy. Fixing it
needs playerbots to expire a masterless bot, which is their code, not ours.

---

## 5. What I could and could not test without a client

New this session: **`.companionadmin gear <player>`** prints what a bound body is actually
wearing, slot by slot, and **`.companionadmin dress <player>`** runs the dress step on demand —
both over SOAP. Create and release also work for an **offline** character now, so the whole
lifecycle can be driven without a game client.

Proven that way:

```
create  → binding row, body renamed, level 1, offline            PASS (4 races/classes)
cap     → the fifth is refused, "maximum of 4 companions"        PASS
names   → a name that belongs to another character is refused    PASS (real collision, not a fake)
release → body back to Zz<guid>, unbound, ledger row written     PASS
release of a stranger is refused                                 PASS
retention → a ledger row dated 2020 is pruned by the sweep       PASS
integrity after all of it                                        clean
```

`admin_cycle.py` runs all of that in one go (9/9) and leaves the owner exactly as it found them.

**Not provable without you:** whether the *dress* path succeeds against a body that is actually
in the world with empty slots. Every bot online on this realm is a fully-geared level 80, so
nothing had a free slot to fill; `dress` on a geared body correctly changed nothing, and the gear
report listed all 19 slots. That last mile is test **T3** in the plan.

## 6. Pool

`scripts/pool-grow.py` clones a template body for each thin race/class, level 1, named
`ZzNNNNN`, until every combination has a floor.

| | Before | After |
|---|---|---|
| Free bodies | 504 | 602 |
| Thinnest combination | 1 | 8 |
| Human Priest | 2 | 9 |

Re-runnable: `python scripts/pool-grow.py --apply --target 12`.

**Design question for you:** pool bodies are generic characters, not class-flavoured — a
priest body was originally a random bot. Names, looks and gear are the module's doing
(fallback kit today). If you want pool bodies that look deliberate, the route is to generate
them from templates with a proper starting outfit per class rather than cloning a random bot.

---

## 7. Automated coverage

| Script | Needs a client | What it proves |
|---|---|---|
`lifecycle_cascade.py` | no | deletions cascade and are repaired (8 checks) |
`integrity_audit.py` | no | 30 SQL invariants across core and module tables |
`pool-grow.py` | no | pool depth, idempotent |
`multi_companion.py` | **yes** | 15 checks: mirrors, pins, comp sync, cap, pruning, cross-owner, membership, summon/dismiss **including "server still answering"** and **"body is dressed"** |

The two in-game assertions added tonight are the ones that would have caught today's
failures before you did. Run `multi_companion.py` after any change to the summon, dismiss or
create paths — it drives the real client.

---

## 8. Open items and recommendations

**Needs your decision**

1. **`RequireGrouped`** — live config is now `0`. With `1`, a companion that drops out of the
   group gets upkeep only, no quest sync. My recommendation: leave it at `0`; grouping is
   playerbots' job and it re-adds the bot on summon anyway.
2. **Original companion module** — restored and unchanged (verified: the shipped 48-line
   `Companion.lua` with no disable marker, registered in the `.toc`). Both panels therefore
   exist: `WoWLegendsPlayer → Companion` for the single companion, `CompanionSync` for the
   group. Say if you would rather the original tab be hidden again.

**Waiting on an in-game check** (all in the test plan)

3. Gear actually visible on the body (the code path is new; T3 proves it).
4. Level alignment and quest mirroring have not been re-tested since r63 changed the payload
   channel to system messages.
5. Panel visuals after the redesign.

**Minor, not done**

6. Pool stock is pushed on demand, so a count can be seconds stale if another player releases
   a body while your panel is open.
7. `.companionsync list` prints to chat only — a `Comp sync`-style payload could drive an
   in-panel roster instead.

---

## 9. Deployment log (tonight)

| Build | Purpose |
|---|---|
r65 | character-cache registration (summon of a new body) |
r66 | dismiss queued to the update tick (crash) |
r67 | `.companionsync refresh` |
r68 | starting gear, first cut |
r69 | upkeep moved above the digest memo |
r70 | grouping no longer drops upkeep |
r71 | integrity engine + `companionadmin integrity` |
r72 | ledger retention; gear fallback kit for a repack with no starting-item list |
r73 | per-slot dressing with read-back; `.companionadmin gear` and `dress` |
r74 | bag recovery: a kit item that was stored instead of worn is put on |
r75 | create/release work for an offline character; loose gear in the bags is worn |
r76 | wear a bagged copy before making another; throttled creation; throttle purged with the companion |

Staged binaries are kept in `D:\AppData\wow-legends\builds\` (e.g. `worldserver-r71-integrity.exe`).
