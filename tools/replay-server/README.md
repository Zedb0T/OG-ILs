# Local ghost server and game client

A Python-standard-library service for Jak 3 replay uploads, mission catalogs,
downloads, and player display-name administration. Python 3.11+; no pip packages.
This first version is deliberately **local-only**, not an internet leaderboard.

## Start

From the repository root:

```powershell
python tools/replay-server/server.py
```

Server: `http://127.0.0.1:8765`. Admin page: `http://127.0.0.1:8765/admin`.
For local testing, sign in with username **`user`** and password **`pass`**.
Override them with `GHOST_ADMIN_USER` and `GHOST_ADMIN_PASSWORD` environment
variables before starting the server. Credentials are never logged. The old
`admin-token.txt` is no longer used (existing copies are left untouched).
Stop the foreground server with Ctrl+C. These default credentials are for
loopback testing only; never expose this service publicly.

Optional: `--data D:/Ghosts --port 8765`. The client port must match; it reads
`server` from the configuration below on game startup.

## Native menu

Restart the rebuilt game, choose an IL, then open **Options → Game Options →
Ghost Replays**. These are native progress-menu controls, not an ImGui overlay.
Change Race Mode and retry the mission to apply it at the canonical IL start.

| Mode | Selected replay |
| --- | --- |
| Default | No completed PB: slowest available **player best**. With a PB: the nearest strictly faster opponent's best. At the top, race your PB. |
| Race vs Your Best Time | Local fastest completed, non-truncated replay. |
| Race vs WR | Fastest completed, non-truncated server replay for this mission. |
| Race vs Your Last Attempt | Local most recent attempt, including failure/unfinished runs. |
| Custom | Up to eight selected server replays for this mission, played simultaneously. |

“Place + 1” means the next **faster** runner here, not the next slower numerical
rank. Each player occupies one leaderboard place, regardless of upload count.
Equal times aren't faster; ties use stable upload-time/replay-ID ordering.
An empty server cannot invent a ghost: Default falls back to your local PB if
one exists. WR/Custom show an unavailable status if the service can't be reached.
Local Best/Last Attempt work without a server. Refresh after starting an offline
server; the client does not continuously hammer it with retries.

For Custom, select that mode, choose **Refresh Mission Replays**, browse with
left/right and confirm the row, then choose **Toggle Selected Replay**. `[X]`
marks selected rows. Use next/previous page for catalogs over 100 entries.
Selections persist per mission. Unfinished uploads are marked `DNF` and are
eligible only for Custom, never Default/WR ranking.

**Submit Your Best Time** uploads an existing PB. **Submit Your Last Attempt**
also permits an unfinished attempt. **Auto Submit Completed Runs** defaults On
and submits future completed, non-truncated attempts to this local server.
Historical files are not bulk-uploaded. If submission fails, the local recording
is still saved; retry with a submit button. There is no durable upload outbox yet.

## Identity, files, and ownership

The game generates a random 128-bit player ID plus a separate 256-bit upload
credential on first client use, and atomically saves them in:

```text
%APPDATA%/OpenGOAL/jak3/features/ghost-client.json
```

The same file stores server URL, mode, auto-submit choice, and per-mission Custom
IDs. Keep it to preserve identity; don't share the upload credential. A corrupt
identity file reports an error rather than silently creating another player.
Run one game instance per profile when modifying these settings.

The server registers this identity on first submission as `Unknown-<id prefix>`.
The admin page lists those IDs and lets you assign a display name. Display names
are labels, not authentication or storage keys; duplicates are allowed. Tokens
are hashed in the server database and never returned by metadata endpoints.

```text
data/
  index.sqlite3                 # players, stable IDs, metadata, dedupe index
  jak3/
    desert-bbush-get-to-19/
      Zed_26.767s_20260904T170000123456Z_a1b2c3d4.ogr.json
```

The filename contains a sanitized player name, final/elapsed seconds to three
decimals, UTC **submission** date/time, and a short collision-resistant replay-ID
suffix. Exact time remains in the JSON/index. Admin renaming updates existing
filenames too, while full replay IDs and download URLs stay unchanged. Interrupted
renames are recovered at startup using the payload digest. Unknown players get
the same readable layout using their Unknown label.

Every upload receives a random stable 128-bit replay ID. Repeating the same
player/payload upload returns the original ID. Payloads are atomically written
before transactional metadata is committed. Back up the whole data directory,
including the database; don't manually rename the files while the service runs.

Downloads are validated again by the game and cached under
`features/ghost-cache/<replay-id>.ogr.json`. Cache files are disposable; the server
remains authoritative. Disk cache/server archives currently have no automatic
retention policy. Monitor disk space for long-running use.

## HTTP API (v1)

All JSON; no CORS access. The raw replay JSON is the upload/download body, using
the existing `opengoal-replay` v1–v3 schema. Category names are strict identifiers.

| Method / endpoint | Purpose |
| --- | --- |
| `GET /health` | Health and API version. |
| `POST /players` | Register `{player_id, token}`; repeat registration must authenticate with the original token. |
| `POST /replays` | Raw replay upload; `X-Player-ID` and `Authorization: Bearer <player-token>` required. |
| `GET /replays?game=jak3&category=...&offset=0&limit=100` | Paginated mission metadata; response contains `replays` and `next_offset`. |
| `GET /replays/<id>` | Download immutable replay JSON. |
| `GET /replays/<id>/metadata` | Current name/metadata independent of catalog page. |
| `GET /selection?category=...&mode=default&player_id=...&best_seconds=...` | Server-side next-faster selection; omit best time if none. Also supports `mode=wr`. |
| `GET /admin` | Tiny name-mapping page. |
| `GET /admin/players` | List player IDs/names; admin HTTP Basic username/password required. |
| `POST /admin/players/<id>` | `{display_name: "Zed"}`; admin HTTP Basic username/password required. |

Bounds: 32 MiB/request, 36,000 samples, ten-minute recordings, finite vectors,
bounded names, matching category/schema/count/timestamps, eight concurrent HTTP
requests. The server only binds loopback and rejects foreign Host/Origin values.
Admin writes require username/password authentication; player upload Bearer
credentials are unchanged. Payload validation is **not anti-cheat**:
completion claims and times are not cryptographically verified. Do not expose
this standard-library server to the internet or forward its port.

## Client lifetime and resource limits

HTTP, replay parsing, and download/cache I/O run on a bounded background worker.
No GOAL pointers cross that worker boundary. Category/mode generations reject
stale responses; active ghosts retain immutable snapshots until teardown.
Different Custom files have independent lengths, cursors, poses and companions.
Per-sample native calls pack the file slot into the sample-index argument to
stay within the Windows trampoline's four-register argument limit.

Custom is capped at eight replays and 64 MiB of decoded sample storage. Ghost
process pools retain the previous allocation guard; the 51-copy stress result
isn't a universal mission/vehicle budget. The separate `*replay-ghost-count*`
stress-copy setting should stay at its normal value of one for these modes.

## Verification

```powershell
python -m unittest discover -s tools/replay-server -v
.\out\build\Release\bin\goalc-test.exe --gtest_filter=ReplayFormat.*
```

16 server tests cover authenticated HTTP upload/download/admin, player and ID
persistence, concurrent retry deduplication, rename recovery, pagination,
ranking/ties, unfinished/truncated filtering, and malformed/path-traversal input.
All ten existing replay-format tests pass. Native Release and full GOAL ISO
builds pass.

The isolated packed-retail integration run uses `seed_test.py` and the opt-in
`replay-service-test.gc` harness (disabled in normal builds). It submitted the
real PB's **test copy**, then verified local PB (1,607 samples), unfinished Last
Attempt (400), Default next-faster (1,200), WR (600), and two distinct Custom
files (600/1,200), including animation/board playback. Log:
`out/ghost-service/game.stdout.log`, ending `GHOST-SERVICE: RESULT PASS`.
The repeat after both server and game restart also passed (`game-2.stdout.log`),
and the catalog remained four players/four replays after the duplicate PB upload.
`out/ghost-service/menu-2.png` was inspected to verify the native page layout.
The final harness-free retail boot stayed responsive and had no test-hook output.
It retained the existing intro-animation master-slot warnings also present in
the pre-service `out/ghost-stress/normal-smoke.stdout.log`; these are not a new
ghost-service failure. The isolated smoke process was closed afterward.
Fixtures, credentials, server data, caches, and test profile are excluded from
version control. No broader cross-mission/pause/soak acceptance gate is claimed.
