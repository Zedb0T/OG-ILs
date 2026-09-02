# Replay Rebuild Baseline

This file is the repeatable Phase 0 test record for the replay-system rebuild.
It deliberately tests the unmodified IL implementation before any replay code is
introduced.

## Fixed test missions

- Fast retry mission: `wascity-bbush-get-to-18`. This is the primary completion,
  failure, cutscene-skip, and five-retry test because it has a short attempt and
  exercises the Wasteland City Burning Bush task flow.
- Different-art mission: `desert-bbush-get-to-17`. This validates a transition to
  the Desert Burning Bush task flow and prevents a same-area-only baseline from
  hiding streamed-art lifetime bugs.

Do not silently substitute missions. If a mission becomes unavailable, record the
replacement and why before running the gate.

## Phase 0 procedure

1. Check out `codex/replay-rebuild` and confirm its base is
   `fdf6921fd641907e11eb644149eb004619bb88e1` (`main` when the rebuild began).
2. Build the complete Jak 3 GOAL `iso` group.
3. Boot the newly packed game in retail mode (`-boot -fakeiso`, without `-debug`)
   and skip the fresh-boot cutscene through the normal `scene-player` abort path.
4. In `wascity-bbush-get-to-18`:
   - complete one attempt;
   - fail one attempt;
   - start and reset/retry at least five additional attempts.
5. Start `desert-bbush-get-to-17` so its different task/art set is streamed, then
   complete or fail one attempt and retry once.
6. Record the process/global-heap snapshot and Windows `gk.exe` working set before
   the retry loop and after every retry. The final settled readings must return to
   the observed baseline; a monotonic increase is a failure.
7. Inspect only the newly created retail log for `art-error`, animation art still
   in use, invalid processes, heap exhaustion, assertions, access violations, and
   unexpected task failures.
8. Remove temporary instrumentation. Commit only when every Phase 0 gate item has
   been observed to pass.

## Verification record

Date: 2026-09-02

Source branch: `codex/replay-rebuild`

Source base: `fdf6921fd641907e11eb644149eb004619bb88e1`

| Check | Result | Evidence |
| --- | --- | --- |
| Full Jak 3 GOAL `iso` build | Pass | All 1,167 targets built with the harness; after removing it, all 1,166 normal targets built successfully in 23.544 seconds. |
| Fresh retail boot | Pass | Final harness-free retail process stayed alive and responsive through the opening stream; `log/jak3.2026-09-02T14-15-03.log` contains no `PHASE0:` hook output. |
| Intro cutscene skipped | Pass | `PHASE0: PASS fresh-boot-cutscene-abort` from the normal `scene-player` `abort` event. |
| Completed attempt | Pass | `PHASE0: PASS completed-attempt`. |
| Failed attempt | Pass | `PHASE0: PASS failed-attempt-transition`; manager selected its normal `fail` state. |
| Five reset/retry cycles | Pass | Five distinct `start-run!` cycles reached released player control and a live task manager. |
| Different-art mission streamed | Pass | `desert-bbush-get-to-17` reached its Desert manager and completed. |
| Global heap returned to baseline | Pass | Exactly 50,650,852 bytes at baseline, all five retries, and the settled different-art sample; maximum retry growth was 0 bytes. |
| Process pool returned to baseline | Pass | `*pc-pool*` process count stayed exactly 5; active processes stabilized at 283 for every retry after the first post-failure transition. |
| Windows working set stayed bounded | Pass | Retry samples stayed between 909,537,280 and 909,717,504 bytes; 180,224-byte spread. Different-art settled at 1,110,810,624 bytes; whole-run streaming peak was 1,201,246,208 bytes. |
| New retail log stayed clean | Pass | `log/jak3.2026-09-02T14-12-18.log`: zero matches for art errors, art still in use, invalid process, heap exhaustion, assertions, access violations, or `PHASE0: ERROR`; final result was PASS. |

### Measurements

| Sample | Global heap | Process pool | `gk.exe` working set | Notes |
| --- | ---: | ---: | ---: | --- |
| Before attempt loop | 50,650,852 | 5 | 917,684,224 | Active process count 280 after failure transition. |
| Retry 1 | 50,650,852 | 5 | 909,537,280 | Active process count reached steady-state 283. |
| Retry 2 | 50,650,852 | 5 | 909,717,504 | Active process count 283. |
| Retry 3 | 50,650,852 | 5 | 909,717,504 | Active process count 283. |
| Retry 4 | 50,650,852 | 5 | 909,717,504 | Active process count 283. |
| Retry 5 | 50,650,852 | 5 | 909,697,024 | Active process count 283. |
| Settled after loop | 50,650,852 | 5 | 1,110,810,624 | Different-art Desert mission active; active process count 65. |

## Automated harness notes

The temporary GOAL harness was packed into `GAME.CGO` after `speedruns.o`. It
used production `start-run!`, task-manager `complete`/`fail` events, and the
scene-player `abort` event. Its process lived in `*pc-pool*`, set a 2,048-unit
thread stack before formatting, and printed structured `PHASE0:` records.

The harness source is retained at
`goal_src/jak3/pc/features/phase0-smoke-test.gc` for future regression work. Its
`GAME.CGO` entry remains commented out in `goal_src/jak3/dgos/game.gd`, so the
harness is not compiled or linked into normal builds.

The final harness-free retail boot log was scanned independently and contained
zero matches for access violations, failed assertions, art errors, art still in
use, invalid processes, or heap exhaustion.
