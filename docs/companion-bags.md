# Companion bag policy

A companion's pack is managed the way a careful player manages theirs: keep what the
work needs, sell what it does not, and never carry a warehouse. The guiding numbers live
in `src/mod_companion_questsync.cpp` next to the policies that use them.

| Number | Value | Meaning |
|---|---|---|
| `CompanionBagReserve` | 8 | Hard floor. Nothing may leave a companion with fewer free slots |
| `CompanionBagComfort` | 16 | Pressure line. Below this, cleanup runs and restocking stops |
| `CompanionReagentKeep` | 5 | One recipe's worth of a bought reagent (vial, thread, flux) |
| `CompanionMatKeep` | 10 | Two stacks of a gathered material |
| `CompanionConsumableStacks` | 2 | Food, drink and potions: what a player carries |
| `CompanionGearLag` | 4 | Item level the kit may trail the companion's level before re-gearing |
| `CompanionGearStep` | 5 | Levels between re-gears, so dinging is not a shopping trip |

## What happens, by scenario

| Situation | What the companion does | Why |
|---|---|---|
| Grey junk drops | Sold at a merchant when Playerbots rates it vendor trash | Already the case before this policy; unchanged |
| Replaced gear (any upgrade) | Sold once its required level is 2+ below the companion's | Playerbots auto-stores the piece it swaps out, so old sets used to live in the bag forever |
| Gear the owner handed over, or a blue | Kept | Outgrown gear is uncommon or lower; a lucky blue is not bag filler |
| Reagents over budget | Whole stacks sold, fullest stacks kept to cover the budget first | A recipe consumes 5, so spares are bag filler |
| Consumables over two stacks | Sold | Restocking and loot both add food and potions faster than they are used |
| Loose bags (containers) | Sold | Four bags are already equipped; a loose one occupies a slot and holds nothing |
| Profession tools (pick, knife) | Never sold | Matched by item id from the same table that grants them |
| Quest items, keys, containers in use | Never sold | Guarded by `CanReleaseItem` and the quest-need lookups |
| Items in a trade window, refundable items | Never touched | Selling mid-trade corrupted stacks before; refundables are the owner's purchase |
| Bags tight, no merchant nearby | Spare stacks posted to the owner by mail, one letter at a time | A player would bank or mail, not stand there holding it |
| Bags tight near a merchant | Restocking stops until the pack is under control | Never buy into a wall |
| Kit lags the level raised for it | Equipment slots re-geared incrementally at claim and every 5 levels | A pool body is a level-1 starter; Playerbots dresses it for its real level |

## Order of operations

`SellBagSurplus` runs immediately before `RestockNearVendor` on the periodic pass, so a
tight pack is freed before anything is bought. Cleanup is throttled to one vendor visit
per five minutes per companion and only ever runs under the comfort line.

## Turning it down

`CompanionQuestSync.TrimSurplus = 0` disables the whole cleanup, including the mail
fallback. `CompanionQuestSync.LevelGear = 0` keeps whatever kit the body arrived in.
Restocking keeps its own `CompanionQuestSync.SellJunk` and `BagMargin` settings.
