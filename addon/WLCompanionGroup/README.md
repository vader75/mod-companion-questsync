# WLCompanionGroup

A companions window for **several** companions. Fed entirely by the server's `Comp sync`
line from `mod-companion-questsync`; it never asks the server anything, because a 3.3.5
client cannot.

It replaces the single-companion tab in `WoWLegendsPlayer` (that tab is disabled with a
one-line early return — see *Re-enabling the original* below).

## What it shows

One row per companion, up to four (a group of five with you). Each row:

| Column | Meaning |
|---|---|
| **name** | class colour, greyed when the body is not in the world |
| **level** | shown only when it differs from yours, next to the **Level** button that fixes it |
| **status** | quest count, free bag slots, and any of: `not in your party`, `level differs`, `waiting on you` |
| **buttons** | `Level` `Summon` `Dismiss` `Map` `Unbind` — only the ones that apply to that row are shown |

`waiting on you` is the flag the quest log cannot give you: the player carries a quest that
companion has not been given yet.

Footer: **Summon missing** (every companion that is not out), **Dismiss all**, **Refresh**,
**Quest view**. A small button by the minimap opens and closes the window.

## Commands

```
/wlcg                     open / close
/wlcg bind <name>         make an in-world bot a companion (max 4)
/wlcg unbind <name>       stop tracking it - asks first, the bot stays yours
/wlcg summon [name]       bring it out, or every one that is missing
/wlcg dismiss [name]      send it away, or all of them
/wlcg level [name]        align its level with yours
/wlcg pins                re-send the map markers
/wlcg quests              print what each companion is carrying
/wlcg test                parse a known payload and report pass/fail
```

`/wlcg test` is the in-game sanity check: it parses a three-row sample and a two-chunk
sample and prints whether both came out right. If the window ever looks empty or stale, run
that first — it separates "the addon cannot parse" from "the server is not sending".

## How the data arrives

```
Comp sync 2: Healy,5,32,4,ogs,110|Soak,1,32,2,o,105
             name,class,level,quests,flags,free slots
```

Flags: `o` in world, `g` in your party, `s` same level, `b` behind (you carry a quest it has
not got). Long payloads arrive as `Comp sync i/n:` and are joined when every part is in.
Anything not mentioned is not bound — the payload is authoritative, so the window empties
itself when companions are released.

## Why a new addon rather than a patch

`WoWLegendsPlayer` ships with the repack (v0.5.0, eight tabs, ~2,700 lines). Edits there are
overwritten by the next repack update, and its Companion tab is hardcoded to "your ONE
permanent companion". This one only reads a whisper and sends dot-commands, so it survives
server and repack changes, and the closed-form grammar above is the only interface between
it and the module.

## Re-enabling the original tab

```
Interface/AddOns/WoWLegendsPlayer/UI/Tabs/Companion.lua
```

Delete the `if true then return end` near the top, or restore the backup saved next to it
(`Companion.lua.disabled-by-wlcompaniongroup`). Both can run at once if you want to compare;
they do not share state.

## Note on `test_parse.py`

`test_parse.py` mirrors `Core.lua`'s parser in Python so the grammar can be tested without a
client. It is a check on the *rules* (splitting, fields, flags), not on the Lua itself —
run `/wlcg test` in game for that.
