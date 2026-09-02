# Replay System Rebuild TODO

Branch: `codex/replay-rebuild`

Base: `main` at `fdf6921fd641907e11eb644149eb004619bb88e1`

Reference implementation: `codex/sparkedhost-release`. Treat that branch as a
source of requirements and small reference changes, not as something to merge or
cherry-pick wholesale.

## Working rules

- Implement one phase at a time. Do not begin the next phase until the current
  phase's acceptance gate passes.
- Keep each phase in its own small commit or short commit series with a clear
  rollback point.
- Reproduce a bug before changing code whenever possible, and keep useful unit
  and regression tests. Remove only temporary game-driving/debug instrumentation.
- Test the packed retail game, not only hot-loaded GOAL or debug mode.
- Never commit generated replay files, caches, credentials, API keys, or local
  server state.
- Avoid committing locally built executables. Release workflows should build and
  package binaries from source.
- Record the exact mission, replay mode, replay count, action sequence, log file,
  and result for every manual crash reproduction.
- Stop on the first new runtime warning involving ghosts, art, heaps, invalid
  processes, or stale downloads. A test is not a pass merely because the window
  stayed open.

## Acceptance gate used by every game-facing phase

- [x] Relevant automated tests pass.
- [x] Full Jak 3 GOAL `iso` group compiles successfully.
- [x] C++ Release build passes when C++ files changed. (Not applicable in Phase
      0; no C++ source changed.)
- [x] Fresh retail boot uses the newly packed build.
- [x] Complete the target mission once, fail once, and reset/retry at least five
      times.
- [x] Skip the intro cutscene and repeat the mission from a fresh boot.
- [x] No ghost/art/process errors or access violations appear in the newest log.
- [x] Replay and global heap usage return to their baseline after every reset.
- [x] Windows working-set growth is bounded across repeated attempts.
- [x] Temporary prints, forced inputs, and automated game-driving hooks are
      disabled from the normal build after the result is observed.
- [x] Commit the phase only after the packed retail verification passes.

## Phase 0 — Establish a trustworthy baseline

- [x] Confirm `main` boots Jak 3 in retail mode.
- [x] Verify an unmodified IL can start, finish, fail, skip its cutscene, and
      restart repeatedly.
- [x] Capture baseline global heap, process-pool, and Windows memory values.
- [x] Add `ghost/` replay output and local cache/state paths to `.gitignore` if
      they are not already ignored.
- [x] Document the single fast test mission and at least one second mission that
      streams different art.

Gate: the unmodified game must pass the complete acceptance gate before replay
code is introduced.

## Phase 1 — Bounded local recording only

- [x] Add a minimal replay sample format for time, position, rotation, velocity,
      state, and optional animation metadata.
- [x] Allocate recording storage once with an explicit maximum duration.
- [x] Start recording at the real IL start signal.
- [x] Stop safely on completion, mission failure, reset, and exit.
- [x] Save completed and unfinished attempts locally using an atomic write.
- [x] Reject malformed, oversized, and incompatible replay files.
- [x] Do not add playback, networking, menus, or submission yet.
- [x] Add parser/serializer round-trip and bounds tests.

Gate: 25 start/reset cycles and 10 completed runs with no heap or handle growth.

## Phase 2 — One local ghost

- [ ] Play one local replay with a clearly owned, immutable sample buffer.
- [ ] Keep recording and playback buffers separate.
- [ ] Use a fixed/reusable process allocation rather than allocating a new ghost
      every attempt.
- [ ] Ensure the ghost never owns pointers to task, transformation, or other
      streamed animation art.
- [ ] Stop and destroy playback before mission/task art unload begins.
- [ ] Draw the runner name below the ghost using the native GOAL text path.
- [ ] Initially support only “Race vs your best time.”
- [ ] Add a reset/finish memory regression test.

Gate: test completion, failure, cutscene skip, death/reset, and immediate retry
for at least 25 cycles in two missions. Logs must contain no `art-error`,
`anim-decomp ... still in use`, invalid process, or access-violation evidence.

## Phase 3 — Standalone replay server and dashboard

- [ ] Build the replay service under `tools/replay-server/` with no game-client
      dependency.
- [ ] Add health, upload, list, metadata, and download endpoints.
- [ ] Assign every accepted replay a permanent cryptographically random unique
      replay ID.
- [ ] Accept a stable player ID separately from the replay ID.
- [ ] Persist replay metadata and payloads atomically across restarts.
- [ ] Validate game, category, completion status, time, size, and schema version.
- [ ] Make duplicate upload retries idempotent.
- [ ] Add an authenticated admin dashboard that can rename replays.
- [ ] Group dashboard replays by game and category.
- [ ] Add server tests for corrupt input, duplicate IDs, restarts, and concurrent
      uploads.

Gate: all server tests pass, a restart preserves IDs/data, and no secrets appear
in HTML, API responses, logs, or repository files.

## Phase 4 — Game upload, download, and disk cache

- [ ] Add the C++ replay client behind one narrow GOAL-facing interface.
- [ ] Make server requests asynchronous and bounded by timeouts and size limits.
- [ ] Upload completed and unfinished attempts without blocking mission cleanup.
- [ ] Cache downloads by permanent replay ID and validate cached contents.
- [ ] Reuse cached replays across resets; do not download every attempt.
- [ ] Use atomic cache replacement and tolerate interrupted writes.
- [ ] Treat unavailable networking as a normal offline state.
- [ ] Add mocked client tests for timeouts, stale responses, malformed JSON,
      duplicate requests, and server errors.

Gate: first attempt, retry, fresh boot, slow server, and fully offline play all
behave deterministically without a crash or unbounded memory growth.

## Phase 5 — Native replay browser

- [ ] Add one native progress-menu page for the currently selected mission.
- [ ] Refresh the server list explicitly and show a clear loading/error state.
- [ ] Show runner/display name, time, completion status, and replay ID.
- [ ] Make selection changes visually unambiguous (red to green).
- [ ] Start with single selection, then enable multiple selection only after the
      single-replay gate passes.
- [ ] Prevent a stale asynchronous response from replacing a newer mission or
      selection using generation/revision IDs.
- [ ] Keep replay-mode controls in game, not on the admin website.

Gate: rapidly change missions, refresh repeatedly, and enter/leave the menu while
downloads are active. The selected pack must always match the visible mission.

## Phase 6 — Multiple ghosts and memory safety

- [ ] Define one explicit maximum replay/ghost count based on measured heap,
      process-pool, renderer, shadow, and cloth limits.
- [ ] Allocate replay data from a resettable dedicated heap.
- [ ] Reuse a fixed ghost process pool on every retry.
- [ ] Disable or budget per-ghost cloth, shadows, and other limited renderer
      resources.
- [ ] Reject or truncate excess selections with a visible explanation.
- [ ] Cancel old pack work when selection revision changes.
- [ ] Add automated rapid-selection and max-count memory stress tests.

Gate: select every available replay quickly, run the maximum supported count,
then complete/fail/reset for at least 25 cycles. Heap/process counts must return
to the same baseline and the log must remain clean.

## Phase 7 — Extensible ghost modes

- [ ] Represent modes as data/strategy identifiers so future modes do not require
      rewriting menu and download code.
- [ ] Implement and test each mode independently:
  - [ ] Default: slowest replay with no PB; otherwise the closest faster place.
  - [ ] Next 3 racers ahead.
  - [ ] Race vs your best time.
  - [ ] Race vs world record.
  - [ ] Race vs your last attempt, including unfinished attempts.
  - [ ] Custom selected replays for the mission.
- [ ] Define deterministic fallbacks for no PB, no WR, missing runner mapping,
      incomplete replay, ties, fewer than three faster times, and offline mode.
- [ ] Add server selection tests and game-client mode tests for every fallback.

Gate: the server’s resolved IDs, the menu’s displayed mode, the downloaded pack,
and the names under spawned ghosts must agree for every mode after a fresh boot
and after a retry.

## Phase 8 — Stable player identity and admin mapping

- [ ] Generate one random persistent player ID on first run.
- [ ] Store it outside save slots so it does not change between game sessions.
- [ ] Include it with every replay upload and unknown-player ping.
- [ ] Let an admin map a player ID to an existing speedrun.com runner.
- [ ] Display `welcome back %PLAYERNAME%` for mapped players.
- [ ] Display `you are an unknown player contact barg or zed to be known` for
      unmapped players using `IL-menu-display.gc` and native GOAL formatting.
- [ ] Add an unlikely documented controller combination for an unknown player to
      ping the server.
- [ ] Show the most recent unknown ping, player ID, and age on the admin dashboard.
- [ ] Rate-limit pings and never treat a ping alone as proof of identity.

Gate: identity remains stable across fresh boots, save changes, offline boots,
and upgrades; mapping updates appear without changing replay IDs.

## Phase 9 — Points leaderboards

- [ ] Fetch the Jak 2/Jak 3 point leaderboards used by `im.jakmods.dev` through a
      testable server/client boundary.
- [ ] Store organized leaderboard arrays in the GOAL object `*Points-Info*`.
- [ ] Save a versioned on-disk cache atomically.
- [ ] Load the disk cache first during startup, then refresh in the background.
- [ ] Keep cached data visible when offline or when refresh data is malformed.
- [ ] Add the point leaderboard pages to the native progress menu.
- [ ] Add cache migration, offline, partial-response, and malformed-data tests.

Gate: valid cached values display immediately on offline startup; online refresh
updates them without blocking or crashing a cutscene skip.

## Phase 10 — speedrun.com submission

- [ ] Keep the speedrun.com credential only in server environment configuration.
- [ ] Confirm the minimum safe account role and permissions before production use.
- [ ] Never send the credential or privileged API response details to the game or
      dashboard browser.
- [ ] Submit only completed new personal bests for a mapped player.
- [ ] Map every in-game mission to the correct
      `jak3og_missions` category/variables with automated mapping tests.
- [ ] Use an idempotency record so upload retries cannot submit duplicate runs.
- [ ] Hardcode the requested YouTube proof URL in the server submission payload.
- [ ] Expose pending/succeeded/failed status and a safe admin retry action.
- [ ] Use a mocked speedrun.com API for all automated tests.
- [ ] Verify production submission only with an explicitly approved test run.

Gate: tests prove that unfinished attempts, non-PBs, unmapped players, duplicates,
bad category mappings, and server retries cannot create submissions.

## Phase 11 — Hosted deployment and releases

- [ ] Package the replay server so SparkedHost deploys from the repository without
      cloning/building the full game repository at startup.
- [ ] Add a deterministic deploy/update workflow with rollback instructions.
- [ ] Keep dashboard credentials, speedrun.com credentials, and state in hosted
      secrets/volumes rather than Git.
- [ ] Configure health checks and graceful restart behavior.
- [ ] Start within 10% CPU, 300 MB RAM, and 1 GB disk; measure sustained use before
      increasing only the constrained resource.
- [ ] Make release builds default to
      `https://opengoal-replays.sparked.network/` while retaining an explicit
      local/development override.
- [ ] Add the game-aware ImGui link to `ils.jakmods.dev` for Jak 2/Jak 3.
- [ ] Generate the testing mod list without overwriting manually maintained name
      or image fields.
- [ ] Keep unsupported macOS release jobs disabled until deliberately restored.
- [ ] Build a Windows release artifact from a clean checkout and test it beside a
      freshly extracted legal Jak 3 ISO.

Gate: two external testers can install a clean release, connect automatically,
upload/retry replays, use every mode, and play offline after caching without a
crash or manual server-panel update.

## Phase 12 — Final hardening

- [ ] Run a multi-hour soak test covering mission completion, failure, death,
      cutscene skips, custom max-count selection, mode changes, offline/online
      transitions, and server restart.
- [ ] Review every allocation and process lifetime across reset and streamed-art
      transitions.
- [ ] Review authentication, authorization, rate limits, secret handling, input
      limits, path handling, and submission idempotency.
- [ ] Confirm generated replay/cache/server files remain ignored.
- [ ] Compare behavior against `codex/sparkedhost-release` and list any intentionally
      deferred differences.
- [ ] Cut a release candidate and repeat the clean-install two-tester gate.

## Progress log

Add one line after each accepted phase:

```text
YYYY-MM-DD | Phase N | PASS/FAIL | commit | retail log | memory before/after | notes
2026-09-02 | Phase 0 | PASS | this commit | jak3.2026-09-02T14-12-18.log | global 50,650,852 -> 50,650,852; PC processes 5 -> 5; retry WS 909,537,280-909,717,504 | source-driven retail lifecycle harness removed; final harness-free boot jak3.2026-09-02T14-15-03.log
2026-09-02 | Phase 1 | PASS | this commit | jak3.2026-09-02T15-33-25.log | global 50,652,644 -> 50,652,644; active/PC processes 283/5 -> 283/5; steady WS 916,873,216-917,188,608; handles 648 transient -> 644 | 25 reset cycles, 10 completed saves, and one real mission completion; split wide native sample bridge and sanitize GOAL byte metadata; harness retained but disabled; final harness-free boot jak3.2026-09-02T15-41-59.log
```
