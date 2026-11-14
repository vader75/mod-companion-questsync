# State of play

What changed in the layout, and the rules that follow from it. Any session working on this
module should read this before touching files — the copies used to drift and this is how
that was fixed.

*Last updated: 14 November 2026, at `6965d98`.*

## Where the code lives

| Location | Role | Rule |
|---|---|---|
| `github.com/vader75/mod-companion-questsync` (`master`) | **Project of record** | Everything lands here |
| `D:\Projects\wow-legends-companion-questsync` | Working clone | Edit, commit, push |
| `D:\Projects\wow-legends\modules\mod-companion-questsync` | Clone inside the realm tree (git-ignored there) | **Pull only** — never edit |
| `D:\Build\WOW_Legends_Server_Source\modules\mod-companion-questsync` | Build tree | Refreshed from a clone, never edited |
| `<client>\Interface\AddOns\` | Installed addons | Installed by `scripts/install-to-server.sh` |

**Both local clones are clones now.** The realm copy used to be a hand-copied folder; it was
found four commits behind (`b3ab88f` + one comment line) while the build tree was current.
That is the drift this layout exists to prevent: never edit in two places, and never copy
files between them by hand.

```bash
# bring any clone up to date
git -C D:/Projects/wow-legends/modules/mod-companion-questsync pull --ff-only
git -C D:/Projects/wow-legends-companion-questsync pull --ff-only

# they must report the same commit
git -C D:/Projects/wow-legends/modules/mod-companion-questsync rev-parse --short HEAD
git -C D:/Projects/wow-legends-companion-questsync rev-parse --short HEAD
```

Refresh the build tree and the client from a clone with
`scripts/install-to-server.sh <server-src> [<client>/Interface/AddOns]` — never file by file.

## The addon rename (v0.9)

The pins addon is **`CompanionPins`**; it was `WLPins` before v0.9.

- Folder `addon/CompanionPins/`, files `CompanionPins.toc` / `CompanionPins.lua`
- Globals and saved variables: `CompanionPinsDB` (was `WLPinsDB`)
- Unchanged: the chat command `/cpins`, the title **Companion Pins**, and the wire protocol
  (`CSYNC|` system lines) — no server rebuild is needed for the rename
- `install-to-server.sh` installs by folder name, so a rename must update it in the same
  commit; otherwise the next install recreates the folder you just retired

Pins already on a map survive the rename: the saved-variable files were migrated in place
(`WTF/Account/<account>/SavedVariables/WLPins.lua` → `CompanionPins.lua`, with the global
renamed inside). The old `WLPinsDB` global is **not** read at runtime — the TOC declares
`SavedVariables: CompanionPinsDB`, so the client never loads the old name.

## Documentation moved

`docs/companion-test-plan.md` and `docs/companion-lifecycle-report.md` used to sit in the
realm repo, linked from here as `../../docs/...`. Those links resolve in the realm tree but
point at nothing in this repository — the one place the README ships. They describe this
module, so they now live in `docs/` here.

- The test plan is a living checklist (T1–T9, about 40 minutes) — use it after any change
  that touches companion behaviour.
- The lifecycle report is a dated snapshot (r76, 12–13 November). History, not design.

## Verified at this revision

- `python tests/lua_sandbox.py` — PASS (Core.lua and UI.lua load, launcher built, `/csync` answers)
- Lua syntax compiles for `CompanionPins.lua`, `CompanionSync/Core.lua`, `CompanionSync/UI.lua`
- `src/mod_companion_questsync.cpp` byte-identical across the working clone, the realm clone
  and the build tree (check with `md5sum` on the three paths; CRLF differences alone are not
  drift)
- Both addons installed in the client as `CompanionPins` + `CompanionSync`

## Still open

- `docs/verification-open.md`: one whisper per pass under contention, and the 300 s nudge
  throttle that may be suppressing the second candidate before the selection rule sees it.
- The pins addon has not been exercised in the client since the rename — first login after
  this change is the real proof that map pins still draw.
