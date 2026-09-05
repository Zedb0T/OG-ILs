# Ghost rebuild deployment

New subserver: **OpenGOAL Ghost Rebuild**, panel ID `531b8d59`, under parent
`10219a80`. Initial allocation: **10% CPU, 300 MiB RAM, 1000 MiB disk**.
Primary address: `136.0.251.17:25749`. Existing services are not modified.

## Startup

Python 3.12, Python Packages `waitress==3.0.2`, Startup File `start_hosted.py`.
Upload this folder's `start_hosted.py` to `/home/container/start_hosted.py`.
Leave the panel's Git install fields empty: the bootstrap owns the checkout.
Every start/restart clones or fetches the **full working tree** from
`https://github.com/Zedb0T/OG-ILs.git`, branch `codex/replay-rebuild`.
Initial history is shallow, but no source directories are omitted.

The bootstrap requires a clean checkout, updates fast-forward only, then runs
the server tests. Failed fetches/tests return to the previous commit using a
normal checkout switch. It never runs `reset --hard` or `clean`. If a checkout
has local edits, startup fails rather than deleting them. Dependencies are pinned;
changes to them require a coordinated panel update. The bootstrap itself is a
stable root-level file, not automatically replaced by the branch.

```
/home/container/
  start_hosted.py       # stable restart bootstrap
  ghost-config.json    # credentials/config, never in git
  data/                # SQLite + replays, never in git
  repo/                # full branch checkout
```

Set `/home/container/ghost-config.json` through the panel (not GitHub):

```json
{
  "public_origin": "https://YOUR-GHOST-HOSTNAME",
  "admin_user": "user",
  "admin_password": "pass",
  "disk_budget_mb": 128
}
```

The weak password is explicitly retained for the user's temporary test. Replace
it before a public release. Only use HTTPS for clients/admin. Configure an HTTPS
reverse proxy for the chosen hostname to the primary allocation, preserving Host;
restrict the origin allocation to that proxy where the host supports it.
The application does not trust forwarded host/protocol headers.

The hosted entry point uses Waitress, not the local development HTTP server.
It limits active replay processing to one request, allows concurrent health checks,
caps uploads at **4 MiB** (413 above this limit), bounds connections to eight,
spools large socket buffers to disk, and rejects writes near storage capacity (507).
Larger recordings remain saved locally. Raise limits only after measurement.

The full tracked tree is initially about 469 MiB before Git objects/dependencies.
The initial replay budget is 128 MiB to leave room for that checkout and updates.
No replays are automatically deleted. Monitor total container disk as well as data.

## Acceptance and monitoring

Verify HTTPS `/health`, authenticated upload/download, admin name mapping, and
restart persistence. Check the startup console for the actual Git commit.
Monitor CPU, memory, restarts, disk growth, health latency and 413/503/507 errors.
Ignore short startup spikes. On sustained pressure, increase only the constrained
resource within existing parent capacity; ask before any paid plan change or taking
resources from another service. Other new services start at 10% CPU / 300 MiB /
1000 MiB; Communicator's requested baseline is 100% / 1.5 GB / about 5 GB.

## Deployment status

Public HTTPS URL: https://opengoal-ghosts.sparked.network/admin.
The first full clone and all 20 startup tests passed on SparkedHost at commit
`2092ca39d`. Initial total disk usage was 597.57 MiB; post-clone memory 179.34 MiB.
Health, correct/incorrect admin authentication and the real 1607-sample PB's
upload/download round trip passed. The local PB's SHA-256 remained unchanged.
The 30-minute `monitor-ghost-rebuild-server` heartbeat monitors this service only.
The game client now supports verified HTTPS and the new profile default is this
host; the user's existing test profile was updated without changing identity.
Restart verification passed: SparkedHost fetched `1b17714ed`, ran all 20 tests,
and retained the exact same replay ID and content after repeat submission (dedupe).
Correct admin auth, wrong-password rejection and HTTPS health passed again using
curl, the game's HTTP transport. Eight sequential curl health checks also passed.
Post-restart usage was about 110 MiB memory / 598 MiB disk, so no allocation
increase was needed. The verification's Python urllib intermediary intermittently
returned `502 Server: solar-system`; curl and the browser passed. Monitoring
should distinguish intermediary connectivity failures from actual server outages.
Full GOAL build (1168 targets), native gk build, and 10 replay-format tests passed.
The rebuilt game is ready for the user's manual hosted-playback test; no new
end-to-end in-game hosted playback run is claimed here.
