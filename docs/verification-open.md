# Verification still owed

Behaviour that is implemented and reviewed but not yet *observed* in a live session. Each
entry says what to look for, because "I tested it" without a log line is how a regression
survives three builds.

## 1. One whisper per pass, under contention

**Claim.** When two conditions want a whisper in the same pass, only the higher-priority one
is sent (3 = hand-in ready, 2 = work owed, 1 = chatter), and the loser is dropped rather
than queued.

**Why it is unproven.** It has been seen sending the right single line many times, but never
in a pass where a *second, lower-priority* candidate also existed, so the drop path has not
fired in front of a log.

**How to prove it.**

1. Set `CompanionQuestSync.Debug = 3` and `reload config` (live, no restart).
2. Create the state: one companion quest **complete and awaiting hand-in** (that produces a
   priority-3 note) while the same companion still **owes items** on another quest (the nudge
   half of `NudgeForHelp`, priority 2, throttled to 300 s — so allow five minutes).
3. Read `Server.log` for:

   ```
   note for Pally: priority 2 dropped, 3 outranks it ('Could we help each other finish …')
   note for Pally: sending priority 3
   Ilyrin whispered: When we get a chance, could we stop by …
   ```

   Two candidates, one whisper. That trio is the proof.
4. Put `Debug` back to `2`; at 3 the log is chatty by design.

**If the drop line never appears** while both conditions are known to hold, the fault is in
`NudgeForHelp`'s 300-second throttle (it may be suppressing the second candidate before
`Note()` ever sees it) rather than in the selection rule — check the nudge throttle first.

## Note on other whispers

`Finished '<quest>' - that pin can go` and the gather/catch-up lines bypass the note queue on
purpose: they are one-off events tied to something the player just caused, not standing
conditions that would repeat every pass. If chat ever feels noisy again, the place to look is
this list of direct `SayToOwner` call sites, not the queue.
