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

## What a role does today

Nothing yet, and that is deliberate: every role behaves exactly like `auto` until the next
two layers land. Recording intent first means the behaviour can be introduced against a
value you have already set, rather than guessing.

Coming, in order:

1. **Strategies** — `tank` gains `+tank`; `healer` gains `+heal` and drops `tank`/`pull`;
   `dps` and `support` drop `tank`/`pull` so they cannot taunt or pull off you. Applied on
   summon, login, role change and re-asserted periodically, because playerbots resets
   strategies on spec and level changes. Strategy names are class-specific: anything the
   class does not have is reported on the card instead of silently ignored.
2. **Etiquette** — per-companion engagement policy (auto / your target only / when I am hit
   / never), so "don't compete with me" holds even for classes without a clean strategy split.

Known gaps, so nobody expects them:

- **Pet taunt** (Growl) is not covered by dropping the `tank` strategy.
- **Threat** from a DPS companion still accumulates; etiquette can delay engagement, not
  erase threat.
- A companion whose spec contradicts its role (a protector warrior marked `dps`) will lose
  the tank strategy but will not suddenly deal damage like a real DPS.

## Setting it up

```sql
-- characters database, once
source sql/companion-roles.sql
```

The module probes for the `role` column at first use. Missing, it logs one warning and says
so in chat instead of failing quietly; existing companions default to `auto`.
