# Companion roles

A role is your statement of intent for one companion: **what it is for** in the way you
fight together. It is stored per companion, survives logout, and is shown on its card.

```
.companionsync role Vorrin tank
.companionsync role Branros support
.companionsync role              # what every companion is currently set to
```

| Role | Your intent |
|---|---|
| `auto` | default — the module keeps its hands off this companion entirely |
| `tank` | it holds the fight; taunting is allowed |
| `healer` | it heals first and stays out of the way otherwise |
| `dps` | it contributes damage but never competes for aggro |
| `support` | buffs, utility, follows your target; never pulls, never taunts |

## Why this is the module's record, not playerbots'

playerbots has a `BotRoles` enum (tank/healer/dps) but **no setter**: `AiFactory` derives it
from the talent spec when the AI is built, and it is consumed by group invitations and the
`%role%` placeholder — not by threat. So "make this companion a healer" can never be a field
we flip upstream.

What the module can actually do is control the strategies a companion runs and when it is
allowed to engage. The role is the record those decisions will read, which is why it is
stored here and not in the bot.

## What a role does

Each role is translated into strategy edits on that companion's own AI, applied when the
role is set, when the companion arrives, and re-asserted every minute:

| Role | Edits |
|---|---|
| `tank` | `+tank` in combat — it is allowed to taunt and hold the fight |
| `healer` | `+heal` in combat, `-tank` and `-pull` — it heals, never taunts, never pulls |
| `dps` | `-tank`, `-pull` — contributes damage, cannot taunt or pull off you; `+threat` when `quiet` is on |
| `support` | `-tank`, `-pull` — as `dps`, but expected to be buffing and following your target |
| `auto` | nothing at all |

The re-assert matters: playerbots resets a bot's strategies when its spec or level changes,
so a one-shot apply would quietly lapse and the companion would go back to taunting.

### Support does not heal, on purpose

`support` means "stays out of the tank's way": it removes `tank` and `pull` and adds
nothing. It never removes the `heal` strategy either, so a **healing spec** keeps healing
exactly as playerbots would have it - nothing this module does takes that away.

A **hybrid in a damage spec** (shadow priest, enhancement shaman, ret paladin, feral druid)
will not off-heal as `support`, because playerbots never gave it a `heal` strategy and we do
not add one. That is deliberate:

- a shadow priest spending globals on heals it never trained for does poor damage and no
  good healing;
- heals generate threat, so an off-healing support can pull a mob off you simply by healing
  you hard - the opposite of keeping out of your way;
- its heal thresholds are playerbots' own config, not something we could set per companion.

**The lever exists anyway: mark it `healer`.** That is the role that says "heal first", with
`+heal` and no taunt, no pull. So:

| Situation | Role to use |
|---|---|
| Holy priest, resto druid/shaman, holy paladin — let it keep doing what it does | `support` or `healer` (both keep healing; `healer` also blocks taunt/pull explicitly) |
| Shadow priest / enhancement shaman / ret paladin / feral druid that you want off-healing | `healer` |
| The same hybrids that should only add damage | `dps` |

### When a class cannot honour the role

The strategy names are playerbots' own and they are class-specific — a warrior has no
`heal`, a priest has no `tank`. Every add is verified afterwards, and a role the class
cannot fully satisfy is marked on the card (`Role: healer !`) and said plainly in chat:

> this class has no heal strategy, so that part of the role is not in force — it will
> fight, just not that way.

A failure to establish part of a role is reported, never swallowed.

Known gaps, so nobody expects them:

- **Druids shift form by talent aura, not by intent.** playerbots puts any feral druid
  carrying Thick Hide into bear form for good, so `dps` and `support` also drop the `bear`
  strategy and add `cat` (Cat Form known); `healer` drops it too, and `tank` adds it. If a
  build refuses either edit, the card says so - the companion is still in the form it was in,
  and that is worth seeing rather than guessing.
- **Pet taunt** (Growl) is not covered by dropping the `tank` strategy.
- **Threat** from a DPS companion still accumulates; a role can stop taunts and pulls, not
  the threat a fight generates.
- A companion whose spec contradicts its role (a protector warrior marked `dps`) loses the
  tank strategy but will not suddenly deal damage like a real DPS.
- **Engagement is not threat.** `mine` keeps a companion off targets you have not chosen; it
  cannot stop the threat that fighting alongside you generates.

## Quiet: hold the threat down

"Will not taunt" is not "will not pull aggro": a companion's damage builds threat on its own.
`quiet` turns on playerbots' `threat` strategy for a companion whose role is `dps` or
`support`:

```
.companionsync quiet Vorrin on
.companionsync quiet Branros off
```

Off by default, because it costs damage. On the card it is the third control on the role row.

## Inspecting a companion

```
.companionsync inspect            # every companion
.companionsync inspect Vorrin
```

Prints what the module actually believes: role, engagement, quiet, the strategies that are
*active* on that bot (`tank`, `pull`, `heal`, `threat`, `passive`, `bear`, `cat`), the exact
edits it has made (its ledger), and any reason something is missing - a strategy the class does
not have, an engagement lever this build lacks, pet taunts turned off.

## Engagement: whether it gets into a fight at all

A role says what a companion may do *in* a fight. Engagement says whether it enters one,
and it is set per companion too:

```
.companionsync engage Vorrin mine
.companionsync engage Branros never
.companionsync engage                 # what every companion is set to
```

| Policy | Behaviour |
|---|---|
| `auto` | default — playerbots decides, exactly as before |
| `mine` | **it fights only what you are fighting** — you pick the target, it follows; it never starts one |
| `joined` | it joins fights you are already in, and starts none |
| `never` | stays out of combat; nothing here stops it defending itself |

`mine` holds the companion passive while *you* are out of combat, and drops passive the
moment you are fighting, re-pointing it at your target every tick - so if you switch targets,
so does it; if you stop fighting, so does it.

Why it works that way: `PassiveMultiplier` returns a value of 0 for every action except
`co`/`nc`/`reset botAI`/`check mount state`/`lfg` and the `follow`/`stay`/`chat shortcut`
family. A passive bot therefore *accepts* an `attack` order (the chat shortcut runs) and then
never attacks - `attack my target` is not on the allowed list. Passive is an idle stance,
never "fight, but carefully". See `docs/companion-strategy-map.md`.

**If the build cannot do it, the card says so.** The lever is playerbots' `passive` strategy;
the module checks that the strategy actually took the first time a policy is applied and
reports `|cffffd200no passive strategy in this build|r` if it did not, rather than drawing a
control that quietly does nothing.

### Engagement needs a way in, and a dps role used to take it away

Two things walk a companion into a fight, and a role must leave at least one of them:

- **`pull`** — how a companion *starts* a fight of its own. It is only taken away on the
  player-directed policies (`mine`, `joined`, `never`), because there the player is the one who
  chooses the target. On `auto` it stays: an assist-only companion reads as one that does not
  engage, which is exactly how a dps companion was reported.
- **the walk into melee** — `enemy out of melee -> reach melee` on the class strategy, the
  generic path every melee class has under its own gap closer.

That second one is why Charge is **muted with a cooldown, not removed from the spellbook**.
Removing the spell does not remove the action: the charge trigger stays registered, the action
still counts as possible, and every "enemy out of melee" tick is spent on a charge that cannot
land. `reach melee` sits at a lower priority, so it never gets its turn and the warrior simply
stands there — reported as "the warrior does not engage when its role is dps". A cooldown is one
of the two things `PlayerbotAI::CanCastSpell` refuses over (the other is an unknown spell), so
the charge goes impossible, the walk takes over, and the spellbook is untouched.

The same rule applies to role edits in general: **a role never disarms the companion.** `tank`
is both a role word and the name of a protection warrior's (or paladin's) entire combat
strategy, so `-tank` on a protection companion would leave it with no combat actions at all.
The module refuses that removal and reports `kept tank` in the role card instead.

## A role change is deliberate, out of combat, and rate-limited

The panel no longer sends a role the moment you click it. Clicking stages it (`Role: dps > tank`),
and an **Apply** button beside it sends it. That is the whole reason the button exists: a role is
not a checkbox. It drives strategies today, and it will drive a respec and a gear swap, and five
clicks in ten seconds is how a companion ends up respeccing in a doorway with a shield it cannot
use.

The server is the authority, and it refuses the change on two counts:

- **Out of combat** - either the companion or the player fighting. A role that changes mid-swing
  has to unwind strategies, talents and gear while the bot is being hit; nothing good comes of it.
  The panel greys Apply with `In fight` and says the same thing if a macro slips past it.
- **One change per `RoleChangeCooldown`** (60 s by default). Asking for the role it already has is
  a no-op and does not spend the window, so a mistaken click costs nothing. The remaining seconds
  travel to the addon in the `Roles` line (`:l<seconds>`), which is why Apply can count down
  instead of looking broken.

Two smaller things worth knowing. A realm that imported `sql/companion-roles.sql` but not
`sql/companion-role-lock.sql` still changes roles, it just cannot rate-limit them - the module logs
one warning at startup rather than failing the command. And `0` in the config turns the window off
without turning off the combat rule, which is the one that protects the companion.

### Talents follow the role, as far as the level allows

A role is only half a decision while the talents still say something else: a protection companion
with fury talents is a companion with a shield and no mitigation. So the module can retrain a
companion to the tree its role asks for, per class:

| Role | What it asks for |
|---|---|
| `tank` | warrior protection, paladin protection, druid feral (bear), death knight blood |
| `healer` | priest holy, paladin holy, druid restoration, shaman restoration |
| `dps` / `support` | the class's damage tree - fury, retribution, feral (cat), shadow, elemental, marksmanship, fire, affliction, combat |

`support` deliberately rides the damage tree: in this module support means "fights, never
off-heals", so a healer tree would be a tree the module then forbids the companion to use. A druid
tank and a druid cat share feral - the tree survives a role flip, the form does not, which is what
the druid form rules are for.

Three things gate it, and they are the same three as a role change: the role must not be `auto`
(the module leaves those alone), the companion must be out of combat, and it must be at or above
`SpecMinLevel` (40). Below that level the trees are six points wide and a "spec" is theatre, so the
module records the decision it *would* take in `Server.log` once per session and changes nothing.

From level 40 a companion holds **two** specs, and that is what makes a role flip cheap: the
tree the role wants is spent into the slot that is not in use, so the tree it was in survives in
the other slot, and flipping back is an `ActivateSpec` rather than a talent wipe. The second spec is
granted to the companion (nothing is bought); it is identified by reading the talents' `specMask`
and summing each tree's known ranks, because the core does not store a "which spec is this" flag.

A better piece picked up in the world is wanted, and playerbots fits it the moment it scores
higher - outside this module's gear pass. The shape is therefore also watched on the slow pass,
once a minute: a two-hander looted by a tank puts the shield back within the minute, a refit takes
a shield the companion already carries before it is given a new one, and the piece a world upgrade
beat is marked for the vendor so it does not rot in the bags. Profession tools, curated deliveries,
bound items and anything in trade are never marked.

The kit is then checked for *shape*, not score. playerbots picks by score for the spec tab, which
is right for damage and support and wrong for the two roles that imply a shape: a tank on a class
that can carry a shield should be holding one, and a healer should not be holding a melee
two-hander with both hands. If the score-driven kit contradicts that, the module refits the one
piece itself - a shield and, if needed, a one-hander to hold beside it; a caster one-hander in
place of a melee two-hander. A staff is left alone, because a staff is what a healer wants. Druid
tanks and death knights are asked for nothing: for them the two-hander *is* the shape. The piece
that comes off goes to the bags and is then marked for the vendor, so the hands change without the
bags filling.

When a retrain does not take, it is not repeated every pass. The module reads the trees back after
each attempt, retries on a widening interval (30 seconds, two minutes, ten), and after three
attempts stops and says so once - `is still fury after 3 attempts at protection: leaving its talents
alone until its level, its role or the trees change`. `inspect` shows the attempt in flight
(`attempt 2, next in 40s`, or `given up after 3`). A different tree, a level up, or a fresh body
starts over.

A spec change also re-picks the kit. playerbots dresses a bot from its spec tab, so the gear a
companion is wearing was chosen for the tree it has just left - a protection warrior in fury's gear
is dressed for the wrong fight. The two "the kit still fits the level" gates in `LevelGear` are
bypassed for that one pass (bags, combat and an open trade window are still respected), which is
why `LevelGear` has to be on for a retrain to re-kit anything.

The pieces such a pass takes off are sold at the next vendor rather than left in the bags.
playerbots destroys whatever it replaces in a slot, but an offhand put down for a two-hander comes
back into the bags, and a companion flipping between tank and damage gear would otherwise shed one
shield per flip. With `SellJunk` off they simply stay; nothing is ever destroyed to make room.

The retrain uses playerbots' own template with a forced tab (`InitTalentsTree`), so the layout is
one players recognise, at the companion's level rather than level 80. Afterwards the AI is rebuilt,
because playerbots chooses its strategies from the trees - a retrain that skipped that step would
leave a protection warrior playing with fury's strategies, which is exactly the bug the role
strategy edits exist to prevent. `SpecFollowRole = 1` does the whole calculation and stops short of
the write, so the decision can be watched in the log before it is trusted.

### What the panel says about it

The addon gets the role and the talents on the same refresh (`Roles` and `Specs` lines), and the
role button's tooltip carries the second half:

| Tooltip | Meaning |
|---|---|
| `Talents fury > protection - needs level 40` | the decision is made, the level gate has not opened |
| `Talents fury > protection - server is only logging it` | `SpecFollowRole = 1` |
| `Talents fury > protection - next out of combat pass` | it will happen on the next settling pass |
| `Talents protection for the tank role` | the trees already agree with the role |
| `Talents fury - this role asks nothing of them` | `auto`, or a class that cannot hold the role |

Sending them together is deliberate: a card that shows the role and not the talents is how a
companion keeps last week's tree with nobody noticing.

## Setting it up

```sql
-- characters database, once
source sql/companion-roles.sql
```

The module probes for the `role` column at first use. Missing, it logs one warning and says
so in chat instead of failing quietly; existing companions default to `auto`.
