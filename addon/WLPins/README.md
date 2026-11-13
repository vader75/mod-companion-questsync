# Companion Pins — quest markers on the world map

Client addon for `mod-companion-questsync`. Draws the companion's hand-ins and its unfinished
quests on your world map, and explains each marker in a small key in the corner.

**Client-side only. No server change, no core edit, nothing to configure on the realm.**

The folder is still called `WLPins` because that is where the client keeps its saved
variables; everything a player sees says **Companion Pins**, and the command is `/cpins`.

## Why an addon at all

The 3.3.5a client decides how quest areas look and only draws them for quests in *your own*
log. The server's quest-POI packet carries no colour or icon field, so "a differently
coloured marker for a quest the companion still owes" cannot be expressed server-side. An
addon can draw whatever it likes, so it does.

## What it reads

The module sends one authoritative line per owner, as a **`CSYNC|`-prefixed system message**,
not a whisper — a whisper needs a live speaker, and the map has to work with no companion in
the world:

```
CSYNC|Pin sync 3: 57,48,g=Brotherhood of Thieves|42,65,h=Report to Goldshire|55,49,g=Milly's Harvest
```

- `h=` hand in (gold `?`), `g=` work left (green `!`), `e=` an errand only the companion can
  run (blue `?`).
- The line is the whole truth: anything it does not mention is removed, so a pin that no
  longer exists cannot linger.
- Long sets arrive in parts (`Pin sync 1/3:`) and are reassembled before anything is drawn;
  parts older than ten seconds are dropped so half a set cannot draw.

## Requirements

| | |
|---|---|
| Client | 3.3.5a (Interface `30300`) |
| Dependencies | none — no Ace, no libraries |
| Saved variables | `WLPinsDB` (pins survive logout, expire after 30 min) |
| Server side | `mod-companion-questsync` with `PinSync = 1` (the default) |

## Install

```
Interface/AddOns/WLPins/WLPins.toc
Interface/AddOns/WLPins/WLPins.lua
```

Enable **Companion Pins** on the addon screen.

## The markers

A marker is a glyph, not a badge: the client's own `?`/`!` language needs no legend, and a
plated pin collapses into a black square at map scale. The quest name sits to its right so
the terrain stays visible.

| Marker | Meaning |
|---|---|
| gold `?` | the companion can hand a quest in here |
| green `!` | an area where it still has work to do |
| blue `?` | a turn-in only it can do |

A small key in the bottom-right corner of the map repeats this in three lines while there
are pins to explain. `/cpins legend` hides or shows it, and the choice is remembered.

## Commands

`/cpins` is the command; `/wlpins` still works.

| Command | Effect |
|---|---|
| `/cpins` | help |
| `/cpins legend` | show or hide the key on the map |
| `/cpins list` | the pins it is holding |
| `/cpins clear` | remove them all |
| `/cpins only <name>` | show one companion's pins (`/cpins all` to show every companion again) |
| `/cpins test` | draw a sample pin, to prove drawing works |
| `/cpins debug` | echo every line it receives |
| `/cpins flip` | marks mirrored top/bottom? flip them |
| `/cpins offset <x> <y>` | nudge every marker, if a zone map is a few percent off |
| `/cpins life <minutes>` | how long a pin survives without being re-sent |

Right-click any marker to remove it. Hover for its quest name and coordinates.

## Limits, honestly

- **It listens on the system channel and ignores anything without the `CSYNC|` tag.** That is
  the whole contract with the module. If the server's tag or channel ever changes, this addon
  must change in the same pass — one consumer left on the old channel receives nothing, keeps
  drawing whatever it last wrote to saved variables, and the report that arrives is "the pin
  for quest A is labelled quest B", which reads like a mislabelled pin rather than a deaf
  addon.
- Markers are placed as a percentage of the zone map, so they land correctly on the continent
  the coordinates came from. `/cpins offset` covers a zone map that is slightly off and
  `/cpins flip` a mirrored one.
- No minimap arrow (that needs world→minimap projection); the world map and the tooltip carry
  the position.
- Whisper parsing is gone from the live path, but the whisper branch stays registered for an
  older server.
