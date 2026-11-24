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
| `dps` | `-tank`, `-pull` — contributes damage, cannot taunt or pull off you |
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

- **Pet taunt** (Growl) is not covered by dropping the `tank` strategy.
- **Threat** from a DPS companion still accumulates; a role can stop taunts and pulls, not
  the threat a fight generates.
- A companion whose spec contradicts its role (a protector warrior marked `dps`) loses the
  tank strategy but will not suddenly deal damage like a real DPS.
- **Engagement is not threat.** `mine` keeps a companion off targets you have not chosen; it
  cannot stop the threat that fighting alongside you generates.

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

`mine` is implemented by holding the companion in playerbots' passive strategy and pointing
it at your target when you have one, so the two cannot diverge: if you switch targets, so
does it; if you stop fighting, so does it.

**If the build cannot do it, the card says so.** The lever is playerbots' passive strategy,
and this core's headers are mangled in places, so the module *probes* for the strategy the
first time it is needed. A build without it gets a warning in the log and
`|cffffd200no passive strategy in this build|r` in chat when you set a policy — rather than
a button that quietly does nothing.

## Setting it up

```sql
-- characters database, once
source sql/companion-roles.sql
```

The module probes for the `role` column at first use. Missing, it logs one warning and says
so in chat instead of failing quietly; existing companions default to `auto`.
