# Companion audit release acceptance

This release changes worldserver, the companion schema and both addons. On realms with legacy `mod-wowlegends`, its custody guard must ship in the same worldserver binary. Follow [the custody migration](companion-custody.md); a passing build is not permission to backfill ambiguous ownership.

## Disposable realm gate

Use an isolated character database and test accounts. Never reuse a live character as a fixture. Keep the live world and authentication endpoints out of the test configuration. Use the same core patches and module configuration as the candidate. Capture the initial and final database state for each case, and the relevant world log lines.

| Case | Required result |
| --- | --- |
| Ordinary alt, historical or unreviewed binding, or conflicting legacy binding | No claim or retirement; character name, equipment, quests, gold and reservation remain unchanged across restart. |
| Reviewed owned companion | Summon and dismiss work; Untrack removes tracking only, not custody or the occupied owner slot. Re-track works for the same owner. |
| Reviewed stock from each module | Each allocator sees only its own stock; claim changes reservation, binding and name together. Failed claim leaves all three unchanged. |
| Release during a summon; refused dismissal; restart during retirement | An active summon is rejected, waits end with a useful status, and retiring records remain reserved. No reset or reuse follows a restart. |
| NPC and object hand-ins, missing ender, full bags, changed bot controller | Valid hand-ins run native scripts once. No invalid turn-in changes reward state or consumes items. No movement toward a static point with no valid ender. |
| Gather and quest chains | A ground object yields its actual loot once; no fabricated chain credit. Check repeatable, timed and remotely completed owner copies against companion state. |
| Take, Craft and Collect | Owner uses native Accept. Confirm transferred counts and the retained reserve on both characters. Combat, full bags, trade replacement and disable must not report success. Test declined, cancelled and unconfirmed outcomes. |
| Repairs and fallback mail | Core vendor pricing applies; a non-viable offer does not gate a valid parcel. Check recoverable expiry attachments and outstanding mail across multiple companions. |
| Roster, supplies and map with and without DragonUI | Check stale vs empty, two rapid revisions, reconnection, disabled pins, card changes during delivery, and a missed snapshot on an already-open map. Pan and zoom, then check clipped hitboxes and fixed legend. |

## Live rollout gate

1. Record the installed executable hash, configuration and both addon versions, and establish a restorable characters backup. Do not plan to roll back to enabled old lifecycle code against published custody records.
2. Review a small initial cohort and record the evidence for each reservation, including existing owners. No old binding or release timestamp is proof by itself. Confirm which active users will remain unverified.
3. Announce maintenance, stop the world cleanly, and confirm all old world processes are gone and database writers drained. Apply only the additive schema steps and reviewed rows. Install the paired binary and matching addons, then start the world once through the realm portal.
4. Verify one fresh world PID owns both game and SOAP listeners and the expected binary hash is installed. Check fresh ready, errors and crash output. Have a reviewed test owner exercise summon, dismissal, one real NPC and one object hand-in, and a manually accepted delivery. Report startup checks separately from gameplay checks.
