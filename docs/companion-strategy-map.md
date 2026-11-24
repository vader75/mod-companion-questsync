# What the module may edit, and what playerbots offers

Written after a real bug: `engage mine` left a warrior standing still. The cause was not the
target logic, it was the stance.

## The lever that bit us: passive

`PassiveStrategy` adds one multiplier, `PassiveMultiplier`, and `PassiveMultiplier::GetValue`
returns **0** for every action except:

| Allowed exactly | Allowed by substring |
|---|---|
| `co`, `nc`, `reset botAI`, `check mount state`, `lfg` | `follow`, `move from group`, `stay`, `chat shortcut`, `wl guide`, `hearthstone` |

So a passive bot will *accept* a whispered `attack` - the chat shortcut runs - and then the
attack itself is multiplied to zero and never happens. `attack my target` is not on the list.

That is the whole bug: `mine` held the companion passive while ordering it to attack.

**Rule the module now follows:** passive means *idle*, never *"fight, but carefully"*. It is
only held while the owner is not fighting (and always for `never`). Once the owner is in
combat it comes off, and `mine` re-points the bot at the owner's target on every tick.

## Every strategy in this build

`adds`, `affli`, `aggressive`, `alterac`, `aoe`, `aq20`, `arathi`, `arcane`, `arena`, `arms`,
`balance`, `Battleground`, `bdps`, `bear`, `behind`, `bg`, `blacktemple`, `blanketing`, `blood`,
`bm`, `bmana`, `boost`, `buff`, `bwl`, `cat`, `cc`, `chat`, `cleansing`, `close`, `collision`,
`cure`, `dead`, `debug`, `default`, `demo`, `destro`, `DK`, `dps`, `duel`, `earthbind`, `ele`,
`emote`, `enh`, `explore`, `eye`, `felguard`, `felhunter`, `fire`, `firestarter`, `firestone`,
`flametongue`, `flee`, `focus`, `follow`, `food`, `formation`, `frost`, `frostfire`, `fury`,
`gather`, `grind`, `grounding`, `group`, `gruulslair`, `guard`, `guide`, `guild`, `gundrak`, `heal`,
`hunter`, `hyjal`, `icc`, `imp`, `isle`, `karazhan`, `kite`, `lfg`, `loot`, `mage`, `magma`,
`magtheridon`, `maintenance`, `map`, `melee`, `mm`, `moltencore`, `mount`, `naxx`, `nc`, `nexus`,
`oculus`, `offheal`, `onyxia`, `paladin`, `passive`, `pet`, `potions`, `pull`, `pvp`, `racials`,
`ranged`, `resto`, `return`, `reveal`, `rpg`, `rshadow`, `RTSC`, `runaway`, `say`, `searing`,
`shadow`, `sit`, `spellstone`, `ssc`, `stay`, `stealth`, `stealthed`, `stoneskin`, `succubus`,
`surv`, `tank`, `TellTarget`, `tempestkeep`, `threat`, `tranquility`, `travel`, `tremor`, `ulduar`,
`unholy`, `voa`, `voidwalker`, `warlock`, `warrior`, `warsong`, `windfury`, `worldbuff`, `wrath`,
`zulaman`

## What the module edits

| Role | Edits | Why |
|---|---|---|
| `tank` | `+tank`, and `+bear` for druids with Bear Form | taunt and hold |
| `healer` | `+heal`, `-tank`, `-pull`, `-bear` for druids | heal, never taunt or pull |
| `dps` | `-tank`, `-pull`, `-bear`, `+cat` for druids that know Cat Form | damage, no taunt, no pull |
| `support` | as `dps` | damage and utility, out of your way |
| `auto` | nothing | playerbots' own behaviour |

Engagement, on top of the role:

| Policy | Edits |
|---|---|
| `auto` | nothing |
| `mine` | `+passive` while you are out of combat; `attack my target` while you fight |
| `joined` | `+passive` while you are out of combat |
| `never` | `+passive` always |

## Levers that exist and we are not using

- **`offheal`** - a real strategy, but only for two classes in this build:
  `OffhealRetPaladinStrategy` (paladin) and the druid cat off-heal. So "give it off-heals" is
  a lever for ret paladins and feral druids, not a general one - a shadow priest still has no
  off-heal strategy to enable. We deliberately do not enable it: `healer` is the lever
  (companion-roles.md).
- **`threat`** - threat reduction, which would make a `dps` role quieter than dropping taunts
  alone achieves.
- **`guard`**, **`kite`**, **`flee`**, **`nc`** / **`co`** - defensive and stance verbs the
  module leaves to playerbots.
- **`focus`** (threat focus) and **`dps assist`** / **`tank assist`** - the assist strategies
  behind ordinary combat; `attack my target` works by setting `prioritized targets`, which is
  why it does not need re-sending every tick (see companion-behaviour-review.md).
- **`formation`** - playerbots has its own formation strategy; the module's presets drive
  `MoveFollow` directly instead, so the two cannot fight over the same bot.
