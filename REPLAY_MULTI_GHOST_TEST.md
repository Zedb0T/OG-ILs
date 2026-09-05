# Multi-ghost playback stress test

Date: 2026-09-04. Packed retail Jak 3, local Windows Release build.

**Measured ceiling for this workload: 51 simultaneous PB copies.** 52 cannot fit
the guarded pool allocation. This is an allocation-limited, three-cycle tested
ceiling, not a universal stability guarantee or recommended gameplay setting.

## Implementation and use

Set `(set! *replay-ghost-count* N)` before starting/retrying an IL. Normal builds
default to one ghost. `(replay-ghost-play!)` restarts the selected category's PB
immediately for a visual check. Counts are bounded to 1–150.

Instances currently share **one immutable local PB**, but each owns its sample
cursor, transform offset, animation state, and companion handle. This is not yet
selection/loading of multiple different players' replay files. Multiple copies
use a 15-column grid with 0.35 m spacing; the saved replay is never edited.
`ghost-render-extras?` remains `#t` by default.

Each slot lazily allocates reusable 128 KiB player and 128 KiB companion pools.
Pools persist until game exit. Insufficient global heap rejects the entire batch
and destroys partial spawns. Restart the game after a capacity test to reclaim
these pools; returning the count to one alone does not reclaim them.

## Workload and isolation

- Latest PB category: `desert-bbush-get-to-19` (the category maps to the
  `wascity-bbush-get-to-19-resolution` mission manager).
- Schema v3; 1,607 samples / 26.7666664 seconds, including 1,275 board samples.
- Source filename: `best-completed.ogr.json`.
- Source SHA-256:
  `34FDDB9EE3AB184ACBB1162FFC9D15520ECCA9D41CC335FC96F44DCC91ECA006`.
- Test profile: `out/ghost-stress/profile/OpenGOAL`; real saves and PBs are not
  used as the test write destination. Recording is disabled during the harness.
- Each search trial boots a fresh process with
  `gk.exe -g jak3 --config-path out/ghost-stress/profile -boot -fakeiso`.
- The opt-in harness aborts the intro normally and starts the selected IL using
  `start-run!`. It extends the mission timeout so task art remains loaded for
  the full replay, follows the leading ghost with the camera, and checks all
  players and boards every sample. It destroys all ghosts at each cycle end.
- A pass requires full sample traversal, valid idle-state players/companions,
  all boards active together, the full group on screen during playback, and no
  remaining ghost processes after cleanup. Some route frames can cull edge
  instances; logs report the actual on-screen count every 300 frames.

The harness is dedicated-runtime-only: recording stays disabled after it ends.
It is not a replacement for mission-failure, pause/debug, or cross-level soak
testing. The measured limit applies to this PB, mission, pool sizing and machine,
not to all vehicle art or missions.

## Regression fixes uncovered during setup

Initial exploratory trials were invalidated by two bugs and are not capacity
measurements: empty raw-zero handles reached `handle->process`, and cold retail
boots looked for board art in the player's world level instead of its owning
GAME level. Empty handles are now guarded and companion art resolves its owner.
One control run also exposed benign first-playback global allocations; repeated
cycle comparisons now use the post-warm-up baseline. The engine screenshot API
errored in this test setup and was removed from the harness.

## Results

Final trial logs are under `out/ghost-stress/*-final.stdout.log` and matching
stderr logs. The one-ghost camera control is `1-camera.stdout.log`.

| Requested copies | Outcome |
| ---: | --- |
| 1 | Full camera-follow playback passed. |
| 150 | Memory guard rejected batch after allocating 51; cleaned up without crashing. |
| 75 | Same guarded rejection at 51. |
| 37 | Full playback passed, including 37 boards; cleanup returned PC pool to 5 processes. |
| 56 | Same guarded rejection at 51. |
| 46 | Full playback passed, including 46 boards; cleanup returned PC pool to 5 processes. |
| 51 | Three full playback/reset cycles passed; every instance reached the final sample with all 51 companions valid. |
| 53 | Guarded rejection after 51 allocations; no crash, cleanup returned PC pool to 5. |
| 52 | Adjacent boundary confirmed: same guarded rejection after 51 allocations. |

At 51 copies, player process usage was 67,584 bytes per instance. Global usage
was 64,064,576 bytes before the first playback, then 64,066,212 bytes on both
subsequent cycles (1,636 bytes of first-playback lazy allocation, zero further
growth). PC pool returned to 5 processes after every cycle. This leaves only
389,916 bytes of global heap free: do not treat this stress ceiling as a safe
default for other missions or larger companion assets.

The game-window capture `out/ghost-stress/51-visible.png` confirms the rendered
offset crowd; no computer-control skill or mouse/keyboard automation was used.
The passing 51-copy log and stderr contain no ghost/art/process/heap errors.

Replay format regression suite: all 10 `ReplayFormat.*` tests passed.

## Final state and reproduction

- Normal build defaults to one ghost, with extras enabled; the stress DGO entry
  is commented out and the harness has no executable top-level test call.
- Final harness-free ISO build passed all 1,168 targets. A fresh isolated retail
  boot remained responsive, produced no stress-hook output, and was then closed.
  Its logs are `out/ghost-stress/normal-smoke.stdout.log` and matching stderr.
- Both original PB and isolated copy were re-hashed after testing and still
  match the SHA-256 above.
- To repeat the automated retail trial, enable `replay-ghost-stress.o` in
  `goal_src/jak3/dgos/game.gd` and temporarily enable the two example calls at
  the bottom of `replay-ghost-stress.gc`, choosing the cycle/count values.
  Rebuild the ISO group and launch only with the isolated `--config-path` above.
  Remove both hooks and rebuild when finished. Never use this harness in a
  gameplay session: it takes over the mission/camera and disables recording.
- For an ordinary manual multi-copy check, set `*replay-ghost-count*` in the
  listener, then retry the mission or call `(replay-ghost-play!)`. Return it to
  1 afterward. Restart the game to reclaim high-count test pool allocations.

This change does not claim completion of the broader phase-2 acceptance gate.
