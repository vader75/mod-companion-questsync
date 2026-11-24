# Behaviour vs gameplay: a review, and a strategy

Written after three bugs in a row that all had the same shape: the module asked playerbots
for something it does not do that way.

- a warrior with `engage mine` stood still (passive zeroes `attack my target`);
- a companion set back to `auto` stopped fighting altogether (we abandoned the edits we had
  made instead of undoing them);
- a druid stayed a bear under `dps` (the form is chosen from a talent aura, not from intent).

Each was fixed in place. This document is the wider look: what the player experiences, what
this module actually controls, and where the design should go so the next feature is data
rather than another patch.

## What we are allowed to touch

playerbots exposes three kinds of lever, in ascending order of how much they fight the AI:

1. **Inputs to the AI** - values such as `prioritized targets`, which the AI then reasons
   about normally. `AttackMyTargetAction` does exactly this: it sets `prioritized targets` to
   `{master's target}` and `pull target`, then attacks. One call, not a stream of orders.
2. **Strategies** - named bundles of actions/triggers (`dps assist`, `tank`, `heal`, `cat`,
   `bear`, `threat`, `guard`, `pull`, `offheal`, `passive`, ...). Enabling or disabling one
   changes what the AI is *able* to do, still leaving it free to choose.
3. **Multipliers** - a strategy that scales actions to zero. `passive` is one: it returns 0
   for everything except `co`, `nc`, `reset botAI`, `check mount state`, `lfg` and the
   `follow` / `move from group` / `stay` / `chat shortcut` / `wl guide` / `hearthstone`
   families. This is the sledgehammer, and using it as "careful mode" was the first bug.

**Principle for anything new: prefer inputs, then strategies, and use a multiplier only to
mean "stop", never "be careful".**

## What the player wants, and what each lever can deliver

| Situation | What you want | Lever that actually does it | What it cannot do |
|---|---|---|---|
| Pulling in the open world | it waits, then joins your fight | `passive` while you are idle; clears the moment you are in combat | stop it re-targeting once the fight spreads |
| "Only my target" | it hits exactly what you hit | set `prioritized targets` to your target (`attack my target`) | prevent splash/AoE hitting a second pack |
| Dungeon, human tank | never taunt | `-tank` (and `-bear` for druids) | stop proximity threat from its own damage |
| "Don't compete with me" | quieter threat | `-tank`, `-pull`, optionally `+threat` | remove threat its rotation generates |
| Off-heals | top you up between pulls | `+heal`; `+offheal` exists for **paladin and druid only** | proper healing on a shadow priest |
| Flee / low health | run away | playerbots' own `flee`; the module stays out | - |
| Vendor / trade | come close | formation presets (`close` = 2 yd), trade clamp | - |
| Travel / mount | follow without a fight | `passive` while you are not fighting | - |
| Pets | pet does not taunt | **not covered** - Growl/Torment are pet abilities, not strategies | everything today |

## The five structural problems

1. **We poll where playerbots is event-driven.** `mine` whispers `attack my target` every two
   seconds. The action *persists* its intent (`prioritized targets` is a value, not one swing),
   so re-sending is noise - and noise that fights the AI's own targeting on every tick.
   Target pinning should happen on change: owner's target changed, combat started, combat
   ended.
2. **We reach for the multiplier too early.** `passive` is correct for `never` and correct
   while you are idle. It is never correct as a state in which the bot is expected to fight.
3. **The role table is class-blind.** One table for eleven classes; the druid form bug is what
   that costs. Real per-class levers we are ignoring: druid `bear`/`cat`, paladin and druid
   `offheal`, paladin auras, shaman totems, death knight presence, warrior stance, rogue
   stealth, hunter/warlock pets.
4. **`auto` had no inverse.** We recorded nothing, so "keep your hands off" meant "leave my
   edits in place". There is now a ledger (name, state, added) and a revert, but the general
   rule needs to be enforced, not remembered: every edit the module makes must have an inverse,
   and `auto`/`off`/release must replay them.
5. **No observability.** Every one of these bugs was found by reading playerbots' source.
   A single command that prints what the module believes - role, policy, ledger, capability
   notes, probe results - would have made each one a one-line answer.

## Where this should go

Keep the layers separate so each feature is a small addition:

| Layer | Responsibility | Today |
|---|---|---|
| Intent | role + engagement, per companion | done |
| Translation | intent → edits, per class, with capability checks | one flat table |
| Runtime | when to reconcile: summon, change, level, spec, plus a slow 60 s re-assert | 2 s poll for everything |
| Ledger | every edit recorded with its inverse; reverted on auto/off/release | partly, and only for roles/passive |
| Observability | `inspect` dumps state and capabilities | missing |

### Sequenced work, smallest first

1. **Done** - `auto` reverts the ledger and releases the stance.
2. **Done** - `mine` pins the target on change only, through the AI's own input
   (`DoSpecificAction("attack my target")`), never a whisper per tick.
3. **Done** - `.companionsync inspect [name]`: role, engagement, quiet, active strategies,
   ledger contents, and every reason something is missing.
4. **Done** - the role table is data (`kClassRules`), so a class is a row: druid forms are two
   rows with spell requirements, and the result is checked rather than assumed.
5. **Done** - `quiet` on `dps`/`support` adds playerbots' `threat` strategy, per companion and
   off by default.
6. **Done** - pet taunts (Growl, Torment, Suffering, Anguish) are turned off for anything that
   is not tanking, and only the abilities this module turned off are turned back on.

What is left is not plumbing but judgement: threat values are playerbots' own, and the model
still cannot express "help only when I am below half health" without an event. Everything the
module does now goes through the three levers in order - input, then strategy, then stance -
and every edit is written down and reversible.
