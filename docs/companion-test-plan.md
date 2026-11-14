# Multi-companion test plan

A schedule you can work through in one sitting, roughly 40 minutes, covering everything the
module does. Each test states what to do, what you should see, and what to send me if it
fails. Do them in order — later tests assume the earlier ones left a working companion.

**Before you start**

- Worldserver up (`.server info` answers over SOAP) and no error lines in
  `D:\AppData\wow-legends\logs\world-test.log`
- `CompanionSync` addon enabled (`/reload` first if it was just replaced)
- Pilot character logged in, standing in a starting zone
- Note the time when you start, so log lines can be matched to a test

If a test fails, grab the reason from:

```
D:\AppData\wow-legends\logs\world-test.log        (console relay)
D:\Applications\wow-legends-server\Server.log     (module log, authoritative)
```

and send me the last 40 lines plus the test number.

---

## T1 — Panel opens and is readable

1. Click the minimap paw icon.
2. Look at the window.

**Expected**: a dark 660×540 window titled **Companions**, with the companion count under
it, `+ Add companion` top right, and a footer of four buttons (Summon missing / Dismiss all /
Refresh / Quest log).

- No text overlapping any button; the status line stops short of the button column
- Four rows maximum, each 98 px, with a class-coloured stripe and dot
- Every button shows a tooltip on hover

**Fails if**: anything overlaps, rows are clipped, or the window opens off-screen. Drag the
window somewhere else, close and reopen — it should reappear where you left it (T8 checks
the same after a restart).

---

## T2 — Create a companion

1. Press **+ Add companion**.
2. Set **Race** = Human, **Class** = Priest.
3. The button reads `Create Human Priest`; press **Suggest** for a name.
4. Press **Create**.

**Expected** in order: the pool line shows `N free in the pool`; the companion appears in the
roster within a couple of seconds; in the log,
`Pilot created companion <Name> (race 1, class 5) from pool body <guid> (was 'Zz<guid>')`.

**Then try the two refusals** (both must be refused politely, with the reason shown in the
panel, not only in chat):

1. Create the *same name* again → `the name <X> is already taken`
2. Create a race/class whose pool count reads `none` → a refusal naming the missing body

**Fails if**: the button does nothing, the panel goes blank, or a refusal is silent.

---

## T3 — Summon, dismiss, and the two crash cases

This is the pair that killed the server twice, so do it carefully.

1. In the roster, press **Summon** on the new companion.
2. Watch the chat/log for `summon came up: <Name>` (give it up to 10 seconds).

**Expected**: the companion appears next to you, **wearing a weapon and cloth armour** (not
naked), with 4 extra 16-slot bags, in your group.

3. Press **Dismiss** on the card.
4. Watch for `dismiss from the update tick: <Name>` followed by `dismiss took: <Name> left`.

**Expected**: the companion leaves, **and the server stays up** — type `/who` or simply wait:
if the client disconnects, the server died. The card should flip to dimmed/Summon within
~8 seconds without closing the window.

5. Summon it again — it must come back and still be dressed.

**Fails if**: the bot does not appear, appears naked, the panel does not update on its own,
or the worldserver is gone (`:8085` not listening).

---

## T4 — Level alignment

1. Set your own level higher: `.character level Pilot 10`
   (or use the GM command you normally use).
2. Wait up to 30 seconds.

**Expected**: the log shows `<Name> -> level 10 (owner Pilot level 10)`, the card's status
stops saying `level differs`, and the companion fights at your level.

3. Repeat once more to level 20 to prove it keeps tracking.

---

## T5 — Quest mirroring

Use a quest with a simple objective in the starting zone.

1. Accept a quest yourself. **Expected**: the companion accepts it too within ~30 s; the
   card's quest count goes up.
2. Abandon it. **Expected**: the companion drops it too, and whispers a short line about
   laying theirs down as well.
3. Accept and complete another quest. **Expected**: the companion completes its copy; walk to
   the giver with the companion nearby and the hand-in happens for both.

**Fails if**: the companion's log diverges, it never accepts, or hand-in needs doing twice.

---

## T6 — Several companions at once (the point of the module)

1. **+ Add companion** twice more, different race/class each time.
2. Press **Summon missing** in the footer.

**Expected**: three companions out, each in your group; the roster shows three cards, each
with its own Summon/Dismiss/Level/Map buttons; the status lines differ per companion
(`in your party`, `not in your party`, `level differs`, `waiting on you`).
3. Repeat the sequencing on the *middle* card only — dismiss it, summon it back. The other two
   must not move.
4. Try to create a **fifth** companion.

**Expected**: refused, `a fifth companion is more than a group` (cap is four).

---

## T7 — Release back to the pool

1. Pick one companion and open the `...` menu on its card → **Release**.
2. Confirm.
3. Watch the ladder in the log: `releasing <Name>`, `step 5: finalising`, `6: body reset`,
   `7: renamed, unbinding`, `8: ledger written`, `release <Name> (<guid>) complete`.

**Expected**: the card disappears, the body returns to the pool (its name becomes
`Zz<guid>`), the name is free to use again, and **the server stays up**. Now create a
companion with the *same name* — it should succeed.

**Fails if**: the server dies, the name stays taken, or the pool count does not recover.

---

## T8 — Freshness, persistence and restart

1. Leave the window open, summon a companion by chat (`.companionsync summon <Name>`).
   **Expected**: the card reflects it within ~8 seconds with no reopen.
2. Log out and back in.
   **Expected**: the companions are still bound and listed (cards dimmed until summoned).
3. Restart the worldserver yourself (`.server shut` + start), log back in.
   **Expected**: the same companions, same levels, same gear; the window opens where you left
   it; nothing is duplicated in the roster.
4. Run `.companionadmin integrity` from the console (or ask me to). **Expected**: `owners
   gone: 0   bodies gone: 0   bound twice: 0`.

---

## T9 — Coexistence with the original companion module

1. Open `WoWLegendsPlayer` → **Companion** tab.
   **Expected**: the original tab is present and unchanged (My companion / Create / Summon /
   Dismiss / Forget).
2. Try to bind a body that the group module already owns via `.companionsync bind <name>`.
   **Expected**: refused, because one body must not be driven by two modules.

---

## What each failure means for you

| Symptom | Send me |
|---|---|
| Server gone after a button | the test number, and the last 40 lines of `Server.log` |
| Companion naked | T3, and the line `dressed for level 1` (present or absent) |
| Panel stale | T8, and whether Refresh fixes it |
| Quest did not mirror | T5, the quest name, and `comp sync` lines from the log |
| Refusal invisible in game | which button, and the panel's server line |

Every test above is either already covered by the automated harness
(`tests/companion-sim/multi_companion.py`, `lifecycle_cascade.py`) or needs a client and
therefore you — that split is deliberate: the server-side lifecycle is proven without you,
and the in-game behaviour is what only you can see.
