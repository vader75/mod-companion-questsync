# Companion formation

Companions follow at a spacing the player chooses. Four presets, one per owner, changed
from the addon header or with a slash command:

| Preset | Spacing | What it is for |
|---|---|---|
| `tight` | 4 yd | melee company — close enough to see your back |
| `loose` | 12 yd | default caster spacing, the same distance playerbots uses |
| `spread` | 20 yd | several companions, out of each other's way |
| `off` | — | the module does not touch movement at all |

```
.companionsync formation            # what is it now
.companionsync formation loose      # off | tight | loose | spread
```

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
- **`WowLegends.BotPitchIn.Radius` is untouched.** That knob is how far *any* bot ranging
  the zone will join a fight near you — it governs quest-mob assist, not follow spacing,
  and it is realm-wide (it affects random bots too). Changing it belongs in
  `configs/modules/mod_wowlegends.conf` followed by `.reload config`.
