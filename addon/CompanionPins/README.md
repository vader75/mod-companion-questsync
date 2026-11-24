# Companion Pins

World-map quest markers for WoW 3.3.5a and `mod-companion-questsync`. Enable the addon on the character screen. The server's `PinSync` option defaults to on. No core patch or third-party library is needed.

## Reading the map

- Gold `?`: hand in at this NPC or object. Green `!`: work remains in this area.
- Muted `?`: hover to see which companions still have work left and which have completed their objectives but cannot hand in yet. For completed companions, the tooltip suggests bringing them to the hand-in spot if they aren't already there.
- Nearby markers can share a glyph and counter without combining distinct hand-in locations. Hover for quests, companion names, and progress. Smart labels are off by default. When enabled, one quest title can appear per glyph; labels try either side, shorten to fit, or hide without hiding the pin. Gold `?` labels get space first, then green `!`, then muted `?`.
- The server uses fixed objective areas instead of switching to the nearest spawns as companions walk.

## State and recovery

Only prefixed system messages can change the map. A complete snapshot replaces the pins; an empty snapshot clears them. Whispers are narrative only.

The new addon starts with `.companionsync pins v2`. New servers advertise `CSYNC|Pin set session <epoch>` and still answer with ordinary v2 parts. Only after seeing that advertisement does the addon ask for `v2 epoch`, whose parts are `CSYNC|Pin set <epoch>:<revision> <part>/<total>:<bytes>`. A newer epoch can start at revision 1; old-epoch and incomplete sets never replace the map. Untagged v2 and legacy `Pin sync` still work without the upgrade. No change to `Hello` is required.

`CompanionPinsDB` keeps display preferences, not pins. Open the map after `/reload` to recover the snapshot. On an open, visible map, a missing or interrupted snapshot is retried at most three times, 15 seconds apart. An unchanged snapshot is treated as stale after two minutes if no complete set arrives; stale markers are dimmed until another complete set arrives. The map distinguishes waiting, disabled, stale, an empty snapshot, and no matches on this map. Use `/cpins refresh` with the map open to retry manually. Older servers may not explicitly report unsupported or disabled pins, in which case a manual request reports that no snapshot was received.

Only exact zone names or map texture keys match (ignoring ASCII case, spaces and punctuation). World/continent views do not use zone coordinates. Type accented characters in companion filters as they appear in the name.

| Command | Effect |
|---|---|
| `/cpins only <name>` / `/cpins all` | Filter every contributor on a shared marker, or show everyone |
| `/cpins show` / `/cpins hide` | Show or hide the overlay and its automatic requests |
| `/cpins refresh` | Request a fresh snapshot with the map open |
| `/cpins labels` / `/cpins legend` | Toggle smart labels or the map key |
| `/cpins list` / `/cpins clear` | Inspect or clear the local view |
| `/cpins test` / `/cpins debug` | Draw a sample marker or toggle diagnostics |
| `/cpins flip` / `/cpins offset <x> <y>` | Calibrate the overlay |

`/wlpins` is still an alias. From the module root, run `python -m unittest discover -s tests -p test_companion_pins.py` for the protocol and clustering checks.

## DragonUI

DragonUI's windowed zoom moves `WorldMapDetailFrame` under a clipped scroll child. Pins stay on the map canvas, while its scroll bounds restrict CompanionPins hit rectangles. The legend and status stay anchored to the viewport and do not capture the mouse. Native and DragonUI map frames, scripts, and settings are not modified. This was checked against the installed DragonUI source and a stubbed zoom/pan geometry test, not visually in game.
