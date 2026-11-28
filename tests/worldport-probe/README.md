# Worldport probe

A standalone MSVC program that replays the dungeon-exit follow logic in
`src/mod_companion_questsync.cpp` eight ways, without a world server. It exists
because the original stranding bug survived the Python suite: the failure lived
in the interaction between the teleport hooks and the retry tick, which only
shows up when the code is compiled and ticked.

## Run

```sh
tests/worldport-probe/run.bat     # builds probe.exe with the VS Build Tools, then runs it
```

All eight scenarios must print PASS (exit code 0):

- normal window
- same-instance straggler after worldport ACK
- missed 5-minute deadline
- different instance of same dungeon
- not grouped
- unconfirmed exit
- owner re-enters
- ordinary outdoor travel expires at 60s

## Keeping the slices current

`cq-exit-timing.inc`, `cq-exit-pending.inc` and `cq-exit-function.inc` are
verbatim cuts of the module source; `cq-exit-hooks.inc` is hand-trimmed because
`OnPlayerLevelChanged` sits between the two hooks in the source.
`tests/test_worldport_probe.py` fails when the source drifts - re-cut with:

```sh
python scripts/cut-worldport-incs.py   # timing, pending, function
```

and by hand for the hooks. If a change adds a call the probe does not stub,
extend the stubs in `probe.cpp` the same way `SayToOwner` was added.
