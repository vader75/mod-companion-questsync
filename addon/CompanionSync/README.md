# CompanionSync

A companions window for **several** companions, plus a launcher in the minimap's
corner. Everything in it is a button; nothing needs typing except a name.

It talks to `mod-companion-questsync` over the server's own protocol lines: the server
answers with system messages prefixed `CSYNC|`, which this addon filters out of the chat
frame and renders, and the window asks for a refresh with the module's dot-commands.
The only interface between the two is that closed-form grammar, so a server change
cannot break the client beyond the payload it already understands.

Requires `mod-companion-questsync` on the server. If nothing arrives 20 seconds after
login, the addon says so in one line rather than showing an empty window.

## What it shows

One card per companion, up to four (a party of five with you):

| Part | Meaning |
|---|---|
| **name** | class colour, greyed when the body is not in the world |
| **subtitle** | race · class · level, in muted text |
| **status** | quest count, free bag slots, and what is wrong: not in your party, level differs, waiting on you, releasing… |
| **buttons** | `Summon` / `Dismiss` (one, depending on state), `Level`, `Map`, `Untrack`, `Release` |

The button colours say which is which: `Dismiss` is gold text, `Summon` is green, and
`Release` is red. One sends the companion away, one calls it back, and the last returns
the body to the pool for good. `waiting on you` is the flag the quest log cannot give
you — you carry a quest this companion has not been given yet.

Footer: **Summon missing**, **Dismiss all**, **Refresh**, **Quest log**, **Ground quests**.
While the window
is open it asks the server for its state every 8 seconds, so a companion dismissed a
moment ago stops showing as present without closing and reopening the panel.

**Ground quests** is the fallback for a collection quest whose items lie on the ground — oats
in a field, cloth in a crate. The companion hops to those objects and takes them by itself
(on a two-second clock, so it is not a wait); the button finishes the ones it cannot reach,
and only ever collects items that actually lie on the ground. It answers in chat with what it
gathered or that nothing is owed.

`+ Add companion` opens the create panel: race and class dropdowns (with pool stock
counted per combination), a suggested name you can edit, and the server's answer shown
in the panel when it accepts or refuses. The addon never refuses a create locally — the
server is the only authority on what the pool holds.

## The minimap launcher

A square dark plate with the class icon, not a disc: the round tracking border belongs
to the minimap's own button cluster and read as a stray lens against the clock.

- **Drag** it anywhere; a drag is not a click, and the position is saved relative to the
  minimap so it comes back where you left it.
- A small count in the corner shows how many companions are bound.
- The border warms to gold on hover, and the tooltip says whether anything is bound.

## Slash commands

Everything else is a button, so there are only a few:

```
/csync            open / close            (/wlcg and /companions also work)
/csync test       parser self-test: two known payloads, one chunked in two parts
/csync quests     ask the server for the quest log view
/csync catchup    finish ground-object quests (the Ground quests button sends this)
/csreset          put the minimap launcher back in its corner
/csstate          print where the launcher is and what it is anchored to
```

Version 1.3.0.

`/csync test` is the in-game sanity check. If the window looks empty or stale, run it
first: it separates "the addon cannot parse" from "the server is not sending".

## How the data arrives

```
Comp sync 2: Healy,5,32,4,ogs,110|Soak,1,32,2,o,105
             name,class,level,quests,flags,free slots
```

Flags: `o` in world, `g` in your party, `s` same level, `b` behind (you carry a quest it
has not got), `-` offline. Long payloads arrive as `Comp sync i/n:` and are joined when
every part is in. The payload is authoritative — anything not mentioned is not bound, so
the window empties itself when companions are released.

Two more payloads share the same channel: `Stock N: race-class=count|…` for the create
panel, and `Pin sync N: x,y,h|g=Name|…` for the quest pins that go on your world map.

## Why a separate addon

`WoWLegendsPlayer` ships with the repack (v0.5.0, eight tabs, ~2,700 lines). Edits there
are overwritten by the next repack update, and its Companion tab is built around one
permanent companion. This addon survives repack updates untouched, and the original tab
still works if you prefer it — the two do not share state and can run at once.

## Tests

```
python test_parse.py            # the Comp sync / Stock grammar, without a client
python ../../tests/lua_sandbox.py  # loads Core.lua and UI.lua against a stub WoW API
```

`test_parse.py` mirrors `Core.lua`'s parser in Python, so a payload change cannot silently
break the window. `lua_sandbox.py` catches the failure the client hides — a file that
does not compile at all, which looks exactly like an addon that simply shows nothing.

## Licence

GNU General Public License, version 2 — the same terms as the module it talks to; see
[`LICENSE`](../../LICENSE) in the repository root. Note that the server side links against
`mod-playerbots` (AGPL-3.0), which is why AGPL-3.0 suits a distributed install.
