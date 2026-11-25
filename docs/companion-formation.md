# Companion formation

> **This document was rewritten when the module adopted playerbots' formations.** It used to
> describe a ring the module walked companions onto with `MoveFollow`, out of combat only. That
> was a second spacing system, and the weaker one: it could not position anybody during a fight.
> The presets below are now names for playerbots' own formations, and movement is the bot's.
> The reasoning and the alternatives are in `docs/companion-distance-review.md`.

The module adopts playerbots' formation system rather than re-implementing it: it tells each
companion which formation to stand in, and playerbots' formation strategy does the walking -
including in combat, with combat movement priority. Because a formation is persisted by the bot
itself, the module re-sends it on change, after every summon, and on the slow reconcile.

| Preset | playerbots formation | Reads as |
|---|---|---|
| `auto` (default) | by role | a tank leads, a healer keeps its distance, melee closes |
| `close` | `near` | arm's length, for handing items over |
| `lead` | `shield` | a wall between you and the mob |
| `tight` | `queue` | single file behind you |
| `loose` | `circle` | a ring around you, out of cleave |
| `spread` | `far` | out of each other's way |
| `off` | *(untouched)* | distance entirely playerbots' business |

`auto` resolves per companion:

| Role | Formation |
|---|---|
| `tank` | `shield` |
| `healer` | `far` |
| `support` | `circle` |
| `dps` melee | `melee` |
| `dps` ranged | `far` |
| `auto` | `chaos` |

Two overrides sit on top, and both are temporary by construction: an open trade window puts the
companion at `near` so a hand-over is not a walk, and a companion engaged `never` takes `far`
once you are fighting - a companion that refuses the fight also refuses to stand in it.

## Out of a fight, distance serves the interaction

A vendor, a quest giver, a trade window and a body to loot all happen out of combat, and all of
them are spoiled by a companion standing twelve yards away. Two clamps sit on top of the
formation while you are not fighting:

- **Interaction** - a trade window on *either* side, or an open gossip menu, puts the party at
  `near`. This is the case the old trade clamp missed: it only looked at the bot's own window,
  so the trade *you* opened was not covered.
- **Idle gather** - once you have stood on the same spot for three seconds, the shape goes one
  step tighter (`far` → `circle` → `queue` → `near`). Start moving and the role formation comes
  straight back.

Both are deliberately out of combat only, because in a fight distance belongs to the role. Neither
of them moves anybody by hand: they change which formation you are in, and playerbots walks it.

## Distance is also the leash

The same number answers "how far will it follow me into a fight". The module will not pin a
target (`engage mine`) for a companion further behind than its formation works - 5 yd for
`near`, 9 for `melee`/`shield`, 15 for `circle`, 25 for `far`, plus 5 yd of slack - because
ordering a bot in from across the field is how companions sprint at a mob that is already dead.

`WowLegends.BotPitchIn.Radius` stays untouched and realm-wide: the leash is per companion, in
the module, and it does not change what the server considers "close enough to help".


Companions follow at a spacing the player chooses. Four presets, one per owner, changed
from the addon header or with a slash command:

| Preset | Spacing | What it is for |
|---|---|---|
| `close` | 2 yd | arm's length — hand an item over, or stand in a doorway together |
| `tight` | 4 yd | melee company — close enough to see your back |
| `loose` | 12 yd | default caster spacing, the same distance playerbots uses |
| `spread` | 20 yd | several companions, out of each other's way |
| `off` | — | the module does not touch movement at all |

```
.companionsync formation            # what is it now
.companionsync formation loose      # close | tight | loose | spread | off
```

**An open trade window overrides the preset**: the companion you are trading with closes
to 2 yd whatever the ring says, and drifts back out when the window shuts. That is the
only case where the module answers to something other than your setting — and it is the
case where a formation would otherwise make you walk to your own companion to hand it
something.

`.companionsync formation` is per **player**, not per companion: the spacing is measured
behind *you*, so it makes no sense to set it for one body. It starts at
`CompanionQuestSync.Formation` and resets to that default at the next login.

## What the module actually does

Every two seconds, for each of the owner's summoned companions:

1. Skip unless the owner and the companion are both alive, in the same map, and the
   companion is neither fighting, flying nor riding a vehicle. **Combat is the bot's
   business** — the module stops asking and the AI moves itself, so there is no
   tug-of-war and no "companion ignores the fight to hold a formation".
2. Compute the angle with playerbots' own follow formula
   (`Formation::GetFollowAngle`): `orientation + (0.125 + 1.75·index/total + …) · π`.
   Mirroring it means the ring reads the way the AI's own formation would.
3. Re-issue `MoveFollow` **only when the companion is more than 2.5 yd off its mark**.
   Restarting the motion generator every tick is what makes followers jitter instead of
   walk; the hysteresis is why this is calm.

Indexes are sorted by guid, so a companion dismissed and resummoned keeps its place in
the ring rather than shuffling everyone.

## What this deliberately does not do

- **No attack/stay/guard buttons.** Those are playerbots verbs; combat already belongs to
  the AI, and a `stay` that strands a companion is a support problem, not a formation one.
- **No per-companion spacing.** It is one ring; two presets at once would mean two owners.
  The trade-window override is the single exception, and it is temporary by nature.
- **`WowLegends.BotPitchIn.Radius` is untouched.** That knob is how far *any* bot ranging
  the zone will join a fight near you — it governs quest-mob assist, not follow spacing,
  and it is realm-wide (it affects random bots too). Changing it belongs in
  `configs/modules/mod_wowlegends.conf` followed by `.reload config`.
