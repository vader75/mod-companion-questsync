# WLPins — companion map pins (client addon)

Companion addon for `mod-companion-questsync`. Draws the companion's quest turn-ins and unfinished quests on your world map, at the coordinates the companion already whispers.

**Client-side only. No server change, no core edit, nothing to configure on the realm.**

## Why an addon at all

The 3.3.5a client decides how quest areas look and only draws them for quests in *your own* log. The server's quest-POI packet carries no colour or icon field, so "a differently coloured pin for a quest I finished and the companion did not" cannot be expressed server-side. An addon can draw whatever it likes, so it does.

It reads the whispers the module already sends:

```
[Companion] I still owe William Pestle my own copy of 'Kobold Candles' - I will hand it in next time we are near (map 82, 34).
```

`map X, Y` → a pin on the world map at that spot.

## Requirements

| | |
|---|---|
| Client | 3.3.5a (Interface `30300`) |
| Dependencies | none — no Ace, no libraries |
| Saved variables | `WLPinsDB` (pins survive logout, expire after 30 min) |
| Server side | none. Works with the whispers `mod-companion-questsync` sends today |

## Install

Copy the `WLPins` folder into `Interface/AddOns/`, so the path is:

```
Interface/AddOns/WLPins/WLPins.toc
Interface/AddOns/WLPins/WLPins.lua
```

Restart the client (a `/reload` is enough once it is listed) and enable **WL Companion Pins** on the addon screen.

## Colours

| Pin | Meaning |
|---|---|
| **Gold** | the companion still owes its own copy of a quest you already finished |
| **Green** | a quest you are both working on ("could we help each other finish…") |
| **Blue** | a turn-in the companion is ready for |
| **Red** | the pin is older than 30 minutes — stale, right-click to remove |

## Commands

| Command | Effect |
|---|---|
| `/wlpins` | help |
| `/wlpins list` | the pins it is holding |
| `/wlpins clear` | remove them all |
| `/wlpins flip` | pins mirrored top/bottom? flips them |

Right-click any pin to remove it. Hover for its coordinates and source.

## Limits, honestly

- It **parses whispers**, so it only pins what the companion says out loud. That is deliberate: a real addon-message channel (`SendAddonMessage`) does not exist in this core, and adding one is a core edit.
- Pins are placed as a percentage of the zone map, so they land in the right place while you are on the same continent map the coordinates came from. `/wlpins flip` covers the case where a zone's map is mirrored.
- No minimap arrow (that needs world→minimap projection); the world map and the tooltip carry the position.

## If you want it as a proper addon-message consumer later

That needs two small things the satellite rule forbids:

1. `WorldSession::SendAddonMessage(prefix, msg, type, receiver)` in the core (roughly 15 lines, mirroring `SMSG_MESSAGECHAT` with `CHAT_MSG_ADDON`), and
2. the module calling it instead of embedding coordinates in a whisper.

Until then, whisper parsing keeps the feature entirely optional and shippable today.
