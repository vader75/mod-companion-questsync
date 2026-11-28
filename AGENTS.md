# Working on mod-companion-questsync

**Project of record: this repository.** `docs/state-of-play.md` is the handover page — read it
first; `README.md` carries install, command reference, the config table and the test commands.
Roles/gear/talents/strategies are described in `docs/companion-roles.md`.

## Where to edit

- Edit **here**. Never in `D:\Projects\wow-legends\modules\mod-companion-questsync` (a
  git-ignored clone inside the realm tree) and never in the build tree
  (`D:\Build\WOW_Legends_Server_Source\modules\mod-companion-questsync`). Pull those two; do not
  copy files between them.
- Refresh the build tree from this clone:
  `scripts/install-to-server.sh D:/Build/WOW_Legends_Server_Source`

## Tests — all must pass before a commit

```sh
python -B -m unittest discover -s tests      # 386 tests, 13 skipped
python -B addon/CompanionSync/test_parse.py  # payload parsing, 14 cases
python -B tests/lua_sandbox.py               # both Lua files load and answer
```

Most of the suite is **source-contract** tests: they read `src/mod_companion_questsync.cpp` as
text and assert the shape of a function. A behaviour change needs the matching assertion updated
in the same commit, and a new rule needs a new test in the file that already covers that area.

## Build, stage, deploy

Full ladder, including the `rc.exe` `MSB6003` trap and the "prove the change is in the binary"
step: `docs/state-of-play.md` → *Flows, in the order they run*. In short: install-to-server →
`touch` the changed `.cpp` → `cmake --build D:/Build/build` → grep the built exe for a string the
change emits → stage a copy in `D:/AppData/wow-legends/builds/`. Deploying needs the user's word;
the worldserver is started and stopped through the portal, never by signalling it.

## Docs ship with the code

A behaviour change updates the README tables and the matching `docs/*.md` in the same commit —
commands from the `ChatCommandTable`, defaults from the shipped `.conf.dist`, never an argument
shape a handler does not accept.
