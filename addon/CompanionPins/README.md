# Companion Pins

World-map quest markers for WoW 3.3.5a and `mod-companion-questsync`. Enable the addon on the character screen. The server's `PinSync` option defaults to on. No core patch or third-party library is needed.

## Reading the map

- Gold `?`: hand in at this NPC. Green `!`: work remains in this area.
- Nearby markers share a glyph and counter, without moving hand-ins away from their NPCs. Hover for quests, companion names, and progress. Labels are off by default.
- The server uses fixed objective areas instead of switching to the nearest spawns as companions walk.

## State and recovery

Only prefixed system messages can change the map. A complete snapshot replaces the pins; an empty snapshot clears them. Whispers are narrative only.

The updated server replies to map refreshes with `CSYNC|Pin set <revision> <part>/<total>:<bytes>`. The addon renders only a complete revision and discards older parts. The older `Pin sync` format still works. Opening the roster does not request pins, and the server heartbeat no longer forces a map resend.

`CompanionPinsDB` keeps display preferences, not pins. With this addon, open the map after `/reload` to recover the snapshot. If automatic sync is disabled or the map is stale, use `/cpins refresh` while the map is open. If you keep an older CompanionPins client on the updated server, use `.companionsync pins` after `/reload` to recover an unchanged map immediately; the new server no longer forces legacy maps through on every heartbeat.

| Command | Effect |
|---|---|
| `/cpins only <name>` / `/cpins all` | Filter every contributor on a shared marker, or show everyone |
| `/cpins refresh` | Request a fresh snapshot |
| `/cpins labels` / `/cpins legend` | Toggle labels or the map key |
| `/cpins list` / `/cpins clear` | Inspect or clear the local view |
| `/cpins test` / `/cpins debug` | Draw a sample marker or toggle diagnostics |
| `/cpins flip` / `/cpins offset <x> <y>` | Calibrate the overlay |

`/wlpins` is still an alias. From the module root, run `python -m unittest discover -s tests -p test_companion_pins.py` for the protocol and clustering checks.
