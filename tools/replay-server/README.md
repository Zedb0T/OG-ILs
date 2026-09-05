# Local ghost server and game client

A Python-standard-library service for Jak 3 replay uploads, mission catalogs,
downloads, and player display-name administration. Python 3.11+; no pip packages.
`server.py` is the loopback-only development entry point. SparkedHost uses the
bounded Waitress entry point described in [SPARKEDHOST.md](SPARKEDHOST.md).
Test admin: https://opengoal-ghosts.sparked.network/admin (`user` / `pass`).

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

The ImGui toolbar has **Settings → Ghost Server → SparkedHost (default) / Localhost**.
Press Left Alt to show the toolbar if hidden. SparkedHost is the default for new
profiles; selecting localhost uses `http://127.0.0.1:8765`. The choice is saved
automatically without changing player identity or race mode. Retry the mission
to use the newly selected server's ghosts. Custom selections and downloaded
replay caches are separate per server; existing uploads finish on their original
server. The menu does not start a local server—run the command above first.

## Player ping

The bottom-left gameplay HUD says **Undetected player - Press L3 + D-pad Down
to ping server** until a successful ping returns an admin-assigned name. Hold
L3 and tap D-pad Down (either press order works). A ping uses the selected
server, registers the saved player ID on first contact, and returns its current
display name. Requests run off the game thread, with a pending-request guard
and a three-second cooldown. Switching servers resets the displayed identity
until the next ping; it does not change the saved ID or token.

The admin player table includes **Last ping (your local time)** and refreshes
every five seconds while visible, without discarding unsaved name edits.
`Never` means no explicit ping has been received. The authenticated
`POST /players/ping` body has `player_id` and `token`; timestamps are generated
by the server in UTC and survive restarts. Existing databases gain a nullable
`last_ping_at` column without replacing players or replay files. Uploads alone
do not update this timestamp. After assigning a name, ping again in game to
refresh the label.

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

New profiles default to `https://opengoal-ghosts.sparked.network`; existing
profiles keep their saved URL. Remote URLs must use HTTPS (certificate checks
stay enabled), while local testing can still use `http://127.0.0.1:8765`.
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
the standard-library development server to the internet or forward its port.
The hosted entry point uses separate bounds: 4 MiB per upload, one active replay
request, eight connections, and a 128 MiB replay-storage budget on the initial
1,000 MiB allocation. See SPARKEDHOST.md for the restart and monitoring setup.

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
.\out\build\Release\bin\replay-client-test.exe
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
