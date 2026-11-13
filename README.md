# mod-companion-questsync

Server module **and** client addon for playerbots companions that stay on their
owner's quest log, at a level that can take those quests.

| Part | Where | What it is |
|---|---|---|
| **Module** | `src/`, `conf/`, `sql/` | the worldserver module: binds bodies, dresses them, mirrors quests, levels and releases |
| **Addon** | `addon/WLCompanionGroup/` | the in-game window and minimap launcher, fed by the module's protocol lines |
| **Tests** | `tests/`, `addon/WLCompanionGroup/test_parse.py` | what can run without a client |

The module is self-contained: it claims bodies from the `mod-playerbots` AddClass pool
itself, owns its own tables (`cq_bindings`, `cq_pool`), keeps its own ledger, and
repairs its own rows. It does **not** need `mod-wowlegends` — the only thing it can
borrow from that module is a read of its binding table, so two modules never level the
same body, and `RespectLegacyBindings = 0` removes even that. Nothing in the core,
`mod-playerbots` or `mod-wowlegends` is patched.

## Install

```
# 1. schema (idempotent, migrates the old wowlegends_companion_* tables)
mysql -u <user> -p wl_characters < sql/companion-questsync.sql

# 2. build: the module must sit in the server source tree
cp -r <this repo> <server-src>/modules/mod-companion-questsync
cmake --build build --config RelWithDebInfo --target worldserver

# 3. config
cp conf/mod_companion_questsync.conf.dist <server>/configs/modules/mod_companion_questsync.conf

# 4. addon (optional - the module works with commands alone)
cp -r addon/WLCompanionGroup <client>/Interface/AddOns/
```

Adding a module needs a CMake **re-run**, because the module list is baked in at
configure time.

## Commands

Everything a player needs is a button in the addon. The commands exist for admins,
macros and the console:

```
.companionsync status | level | quests | pins | refresh | list | pool
.companionsync create <race> <class> <name>
.companionsync summon <name> | dismiss <name> | release <name>
.companionadmin integrity [fix]        audit and repair both tables
.companionadmin gear <player>          what each bound body is wearing
.companionadmin create <player> <race> <class> <name>
```

Full detail: [`src`-side readme](https://github.com/vader75/wow-legends/blob/master/modules/mod-companion-questsync/README.md)
— that copy is the canonical description, including every configuration option.

## Where this code lives

**This repository is the single source of truth** for the module, the addon and the
tests. The realm source tree holds a clone at `modules/mod-companion-questsync`, which
is where the worldserver build picks it up; that clone is ignored by the realm
repository, so there is exactly one tracked copy of this code.

```
git clone https://github.com/vader75/mod-companion-questsync <server-src>/modules/mod-companion-questsync
```

Edit here, then copy into the build tree before building. Nothing under `modules/` in
the realm repository is tracked any more — that duplication is what let the two copies
drift a thousand lines apart.
