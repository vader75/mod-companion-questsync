# Distance, roles and engagement: a second review

Written after the role, engagement and quiet controls landed. The question this time is what
*distance* means now that a companion has a role - and whether the module is still the right
thing to be moving it.

## What the module does today

- One spacing per **owner** (not per companion): `off`, `close` 2, `tight` 4, `loose` 12,
  `spread` 20 yards.
- Applied by `ApplyFormation` on a 2 s tick, using its own `MoveFollow` on the ring angle
  playerbots itself uses, with 2.5 yd hysteresis.
- **Skipped entirely in combat**, plus flight, vehicles, dead bots and cross-map bots.
- Clamped to 2 yd while a trade window is open.

That is a *staging* system. It says where a companion stands when nothing is happening.

## What playerbots already has

This is the finding that should change the design: playerbots ships a full formation system,
`Ai/Base/Value/Formations.cpp`, with nine named formations, per-bot persistence
(`FormationValue::Save/Load`) and a combat-aware strategy (`CombatStrategy::getName()` returns
`"formation"`, moving with `MOVEMENT_COMBAT` priority):

| Formation | Geometry | Fits |
|---|---|---|
| `chaos` (default) | loose random spread behind you | what bots do now |
| `near` | tight around you at `followDistance` | vendor, trade, hand-in |
| `queue` | single file behind you, ~2 yd | narrow corridors, doors |
| `circle` | ring around you | keep everyone out of cleave |
| `line` | line, ahead of you (MoveAhead) | open-field march |
| `shield` | wall between you and the threat | a protector that leads |
| `arrow` | wedge ahead | pushing into a pack |
| `melee` | clustered at the target | melee damage |
| `far` | keeps its distance | casters, healers |

They are set with the bot's own `formation` verb and persist by themselves. So the module is
currently a **second spacing system**, and the weaker one: ours cannot move a bot in combat,
is owner-wide instead of per-companion, and fights nothing but is fought by nothing either.

## Four gaps this leaves with the new role controls

1. **Distance is role-blind.** One preset for all companions means a tank at `spread 20` is not
   a tank, and a healer at `close 2` is eating cleave. Role now says what a companion *does*;
   distance should default from it.
2. **Distance stops at the pull.** We skip combat, so during the fight nobody is positioned:
   the protector does not lead, the healer does not hold range, melee does not close. Every
   interesting distance decision happens exactly when we are not applying one.
3. **Distance does not gate engagement.** `WowLegends.BotPitchIn.Radius` is 20 yd realm-wide
   (parsed once into an atomic; we deliberately do not touch it). A companion following at
   `spread 20` will therefore join a fight at maximum following distance, and `mine` will pin a
   target it is standing 20 yd from. Spacing and joining are the same number in gameplay, but
   not in the module.
4. **`never` stands in the fire.** A non-combat companion held out of fights stays on the ring
   you chose - `close` if you were trading - which is inside the AoE of the fight it is
   refusing to join.

## Suggestions

**S1 - Use playerbots' formations as the mechanism (recommended).**
Keep the presets as the friendly face, but stop calling `MoveFollow`: set the bot's formation
instead, with the preset mapping to a name:

| Preset today | Formation | Why |
|---|---|---|
| `close` | `near` | same intent, and playerbots owns the geometry |
| `tight` | `queue` | single file, no jostling at doors |
| `loose` | `circle` | ring, out of cleave |
| `spread` | `far` | keeps clear |
| (new) `lead` | `shield` | the protector stands between you and the mob |

Buys: combat-aware movement, per-bot persistence, no tug-of-war with `FollowMasterStrategy`,
and a formation vocabulary that already suits roles. Costs: our hysteresis and trade clamp have
to be re-expressed as formation choices (the trade clamp is still ours to apply).

**S2 - Role-derived default, owner override.**
Per companion, distance = f(role) unless the owner overrides it:

| Role | Default formation | Distance |
|---|---|---|
| `tank` | `shield` (or `arrow` pushing in) | leads, 3-8 yd ahead of you |
| `dps`, melee | `melee` | at the target |
| `dps`, ranged | `far` | 20-25 yd |
| `healer` | `far` | 15-20 yd, inside heal range |
| `support` | `circle` | 12-18 yd |
| `auto` | `chaos` | as playerbots has it |

The header row then reads **Auto (by role)** as its default, with the presets as an override.
This is the change that makes "tank" mean something you can see.

**S3 - Distance is the leash for engagement.**
Make the number do double duty: a companion will not be pinned or sent in beyond
`spacing + slack` (slack 5 yd). That leaves `BotPitchIn.Radius` alone and realm-wide, and turns
our per-companion distance into the answer to "how far will it follow me into a fight". It also
makes `spread` mean something in combat without touching the global.

**S4 - `never` retreats, `quiet` does not.**
A companion on `never` should move to `far` while you fight, so staying out of it also means
staying out of the AoE. `quiet` keeps its formation - it is fighting, just softly.

**S5 - Inspect the distance too.**
`inspect` should print the effective formation, the spacing, the distance to you, and which
limit is binding: `leash 17 yd (spacing 12 + 5) vs pitch-in 20` - so the next "why did it run
in" is one line rather than a source dive.

## What landed

All of it, in one pass:

- **S1** - presets map to playerbots formations (`close`→`near`, `tight`→`queue`, `loose`→
  `circle`, `spread`→`far`, `lead`→`shield`); the module sends the bot's own `formation` verb
  on change and no longer calls `MoveFollow` at all.
- **S2** - `auto` is the default and resolves per role: tank `shield`, healer `far`, support
  `circle`, melee dps `melee`, ranged dps `far`, `auto` `chaos`.
- **S3** - the leash gates the pin: no target is pinned for a companion further behind than
  its formation works, plus 5 yd.
- **S4** - `never` takes `far` while you fight.
- **S5** - `inspect` reports the formation next to the role.

Two clamps were added after the first pass, for the interaction case a flat formation cannot
serve: an open trade window on **either** side or a gossip menu puts the party at `near`, and
standing still for three seconds gathers the shape one step tighter (`far` → `circle` → `queue`
→ `near`). Both apply out of combat only - in a fight, distance belongs to the role.

The trade-window clamp survives as an override rather than a distance calculation: an open
trade window means `near`, whatever the preset says.

## Recommendation

Do **S1 + S2** together, then **S3**:

1. S1 alone removes a whole redundant system and inherits combat awareness; S2 is a table on top
   of it and is what makes roles visible in play.
2. S3 next, because it is the one that changes *behaviour* rather than staging, and it is small
   once S1 exists.
3. S4 and S5 after - they are refinements of a system that by then has one owner.

Nothing here touches the pitch-in radius, which stays as you asked.
