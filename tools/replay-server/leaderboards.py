"""Public, source-separated IL points and a bounded last-good SRC cache."""
from collections import OrderedDict, defaultdict
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import re
import threading
import time
from urllib.error import HTTPError
from urllib.parse import urlencode, urlsplit
from urllib.request import HTTPRedirectHandler, Request, build_opener

SOURCES = ("speedrun", "ghosts")
GROUPS = ("all", "main", "orb", "side")
SRC_GAME = "j1l7q0zd"  # jak3og_missions
SRC_CATEGORY = "rkl7n8qd"  # Any%, not Major % Warp
SRC_ORIGIN = "https://www.speedrun.com/api/v1"
CACHE_VERSION = 1
REFRESH_SECONDS = 3600
MAX_CACHE_BYTES = 8 * 1024 * 1024
MAX_RUNS = 20000
MISSIONS = json.loads(Path(__file__).with_name("missions.json").read_text(encoding="utf-8"))
MISSION_NAMES = {m["mission_id"]: m for m in MISSIONS}
SCORING = {
    "id": "il-points-v1", "podium": [100, 97, 95],
    "remaining_formula": "max(round(94 - (place - 4) * 94 / max(1, total_runners - 3)), 0)",
    "total_runners": "Distinct players with a ranked run across this source, before group filtering",
    "ties": "Exact unrounded times share place and points; subsequent places skip (1, 1, 3)",
    "rounding": "Nearest integer, ties to even (Python round)",
    "counting": "One fastest completed, non-truncated run per player per mission; sum mission points",
    "overall_ties": "Equal point totals share rank; player ID provides stable display order",
    "speedrun_rules": "Jak 3 OpenGOAL Missions, Any%, verified single-player runs, default subcategory values",
}


class LeaderboardError(Exception):
    def __init__(self, message, status=400):
        super().__init__(message)
        self.status = status


def utc_now():
    return datetime.now(timezone.utc).isoformat()


def encoded(data):
    return json.dumps(data, ensure_ascii=True, separators=(",", ":"), allow_nan=False).encode()


def points(place, total_runners):
    if place < 1:
        return 0
    if place <= 3:
        return (100, 97, 95)[place - 1]
    return max(round(94 - (place - 4) * 94 / max(1, total_runners - 3)), 0)


def safe_src_url(value):
    if not isinstance(value, str):
        return None
    parsed = urlsplit(value)
    if (parsed.scheme == "https" and parsed.hostname in ("speedrun.com", "www.speedrun.com")
            and not parsed.username and not parsed.password and parsed.port in (None, 443)):
        return value
    return None


class SourceRedirects(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        destination = urlsplit(newurl)
        if (destination.scheme != "https" or destination.hostname != "www.speedrun.com"
                or destination.port not in (None, 443) or destination.username
                or destination.password or not destination.path.startswith("/api/v1/")):
            raise ValueError("Unexpected SRC redirect")
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def make_snapshot(missions, runs, updated_at):
    """Only normalized public metadata enters this cache; never replay samples/tokens."""
    if len(missions) > 512 or len(runs) > MAX_RUNS:
        raise LeaderboardError("Leaderboard exceeds this server's capacity", 503)
    by_mission = {m["mission_id"]: {**m, "runs": []} for m in missions}
    best = {}
    for row in runs:
        seconds = row["duration_seconds"]
        if (type(seconds) not in (int, float) or not math.isfinite(seconds) or seconds < 0
                or row["mission_id"] not in by_mission):
            raise ValueError("Invalid normalized leaderboard run")
        key = (row["mission_id"], row["player_id"])
        order = (seconds, row.get("submitted_at") or "", row["run_id"])
        previous = best.get(key)
        if previous is None or order < (previous["duration_seconds"], previous.get("submitted_at") or "", previous["run_id"]):
            best[key] = row
    total_players = len({key[1] for key in best})
    for row in best.values():
        by_mission[row["mission_id"]]["runs"].append(dict(row))
    for mission in by_mission.values():
        rows = mission["runs"]
        rows.sort(key=lambda r: (r["duration_seconds"], r["player_id"]))
        wr_count = sum(r["duration_seconds"] == rows[0]["duration_seconds"] for r in rows) if rows else 0
        previous_time = None
        place = 0
        for index, row in enumerate(rows, 1):
            if row["duration_seconds"] != previous_time:
                place = index
                previous_time = row["duration_seconds"]
            row.update(place=place, points=points(place, total_players),
                       tied_wr=place == 1 and wr_count > 1, untied_wr=place == 1 and wr_count == 1)
        mission.update(player_count=len(rows), wr_seconds=rows[0]["duration_seconds"] if rows else None)
    snapshot = {"updated_at": updated_at, "total_players": total_players, "missions": list(by_mission.values())}
    snapshot["revision"] = hashlib.sha256(encoded(snapshot)).hexdigest()[:24]
    return snapshot


def src_subcategories(variables, level_id):
    filters, labels = {}, []
    for variable in variables:
        scope = variable["scope"]
        applies = scope["type"] in ("global", "all-levels") or (
            scope["type"] == "single-level" and scope.get("level") == level_id)
        if not applies or not variable.get("is-subcategory") or variable.get("category") not in (None, SRC_CATEGORY):
            continue
        values = variable["values"]["values"]
        selected = variable["values"].get("default")
        if selected not in values:
            if len(values) != 1:
                raise ValueError("SRC subcategory has no unambiguous default")
            selected = next(iter(values))
        filters["var-" + variable["id"]] = selected
        labels.append(variable["name"] + ": " + values[selected]["label"])
    return filters, labels


def fetch_speedrun(fetch, progress=lambda done, total: None):
    game = fetch(f"/games/{SRC_GAME}?embed=categories,levels,variables")["data"]
    if game["id"] != SRC_GAME or game["abbreviation"] != "jak3og_missions":
        raise ValueError("Unexpected SRC game")
    if not any(c["id"] == SRC_CATEGORY and c["type"] == "per-level" for c in game["categories"]["data"]):
        raise ValueError("SRC Any% IL category unavailable")
    levels, variables = game["levels"]["data"], game["variables"]["data"]
    if not 1 <= len(levels) <= 512:
        raise ValueError("Invalid SRC mission count")
    missions, runs = [], []
    for index, level in enumerate(levels):
        level_id = level["id"]
        if not re.fullmatch(r"[a-z0-9]{8}", level_id):
            raise ValueError("Invalid SRC level ID")
        filters, variants = src_subcategories(variables, level_id)
        params = urlencode({"embed": "players", **filters})
        board = fetch(f"/leaderboards/{SRC_GAME}/level/{level_id}/{SRC_CATEGORY}?{params}")["data"]
        if board["game"] != SRC_GAME or board["level"] != level_id or board["category"] != SRC_CATEGORY:
            raise ValueError("SRC returned a different leaderboard")
        if any(board["values"].get(key.removeprefix("var-")) != value for key, value in filters.items()):
            raise ValueError("SRC did not apply the requested subcategory")
        # Match the reference's moderator-ordered Jak 3 mission groups. Prefer
        # matching native names so new/reordered known missions remain correct.
        known = next((m for m in MISSIONS if m["label"].casefold() == level["name"].casefold()), None)
        fallback_group = "main" if index <= 63 else "orb" if (64 <= index <= 71 or 77 <= index <= 90 or 106 <= index <= 124) else "side"
        missions.append({"mission_id": level_id, "label": level["name"], "group": known["group"] if known else fallback_group,
                         "source_url": safe_src_url(board.get("weblink")), "variants": variants})
        names = {p["id"]: p["names"]["international"] for p in board["players"]["data"] if p.get("rel") == "user"}
        for placed in board["runs"]:
            run = placed["run"]
            if run["status"]["status"] != "verified" or len(run["players"]) != 1:
                continue
            player = run["players"][0]
            if player["rel"] == "user":
                player_id = player["id"]
                name = names.get(player_id, "Runner " + player_id)
            elif player["rel"] == "guest":
                name = player["name"]
                player_id = "guest-" + hashlib.sha256(name.encode()).hexdigest()[:24]
            else:
                raise ValueError("Unsupported SRC player")
            runs.append({"mission_id": level_id, "player_id": player_id, "display_name": name,
                         "duration_seconds": run["times"]["primary_t"], "run_id": run["id"],
                         "submitted_at": run.get("submitted") or run.get("date"),
                         "run_url": safe_src_url(run.get("weblink")), "replay_id": None})
        if len(runs) > MAX_RUNS:
            raise ValueError("SRC run limit exceeded")
        progress(index + 1, len(levels))
    return {"missions": missions, "runs": runs, "updated_at": utc_now()}


class SpeedrunCache:
    """One paced background refresh; failures never replace the last good data."""
    def __init__(self, root):
        self.path = Path(root) / "speedrun-leaderboard-v1.json"
        self.lock = threading.RLock()
        self.stop_event = threading.Event()
        self.thread = None
        self.snapshot = None
        self.refreshing = False
        self.progress = (0, 0)
        self.error = None
        self.next_refresh = 0.0
        self.last_request = 0.0
        self.failures = 0
        self.opener = build_opener(SourceRedirects())
        self.load()

    def load(self):
        try:
            if self.path.stat().st_size > MAX_CACHE_BYTES:
                return
            envelope = json.loads(self.path.read_bytes())
            if envelope["version"] != CACHE_VERSION or envelope["game"] != SRC_GAME or envelope["category"] != SRC_CATEGORY:
                return
            data = envelope["data"]
            self.snapshot = make_snapshot(data["missions"], data["runs"], data["updated_at"])
            stamp = datetime.fromisoformat(data["updated_at"]).timestamp()
            if stamp > time.time() + 60:
                self.snapshot = None
                return
            self.next_refresh = stamp + REFRESH_SECONDS
        except (OSError, ValueError, TypeError, KeyError, AttributeError, LeaderboardError):
            self.snapshot = None

    def fetch(self, path):
        # 48 requests/minute, sequential, below the public API's request budget.
        delay = max(0, 1.25 - (time.monotonic() - self.last_request))
        if self.stop_event.wait(delay):
            raise RuntimeError("Refresh stopped")
        self.last_request = time.monotonic()
        req = Request(SRC_ORIGIN + path, headers={"User-Agent": "OpenGOAL-IL-Leaderboards/1", "Accept": "application/json"})
        with self.opener.open(req, timeout=15) as response:
            destination = urlsplit(response.url)
            if destination.scheme != "https" or destination.hostname != "www.speedrun.com":
                raise ValueError("Unexpected SRC redirect")
            raw = response.read(2 * 1024 * 1024 + 1)
            if len(raw) > 2 * 1024 * 1024:
                raise ValueError("SRC response too large")
            return json.loads(raw)

    def refresh(self, fetch=None):
        with self.lock:
            if self.refreshing:
                return
            self.refreshing = True
            self.progress = (0, 0)
        try:
            def progress(done, total):
                with self.lock:
                    self.progress = (done, total)
            data = fetch_speedrun(fetch or self.fetch, progress)
            snapshot = make_snapshot(data["missions"], data["runs"], data["updated_at"])
            raw = encoded({"version": CACHE_VERSION, "game": SRC_GAME, "category": SRC_CATEGORY, "data": data})
            if len(raw) > MAX_CACHE_BYTES:
                raise ValueError("SRC cache size limit exceeded")
            # Reuse the service's fsync + atomic rename writer; no pickle.
            from server import atomic_write
            atomic_write(self.path, raw)
            with self.lock:
                self.snapshot = snapshot
                self.error = None
                self.failures = 0
                self.next_refresh = time.time() + REFRESH_SECONDS
        except Exception as error:
            with self.lock:
                self.failures += 1
                delay = min(3600, 300 * 2 ** min(self.failures - 1, 4))
                if isinstance(error, HTTPError) and error.code == 429:
                    try:
                        delay = max(delay, min(7200, int(error.headers.get("Retry-After", "0"))))
                    except ValueError:
                        pass
                if isinstance(error, HTTPError):
                    error.close()
                self.error = "Speedrun.com refresh failed; keeping the last successful snapshot."
                self.next_refresh = time.time() + delay
                print("SRC leaderboard refresh failed:", type(error).__name__, flush=True)
        finally:
            with self.lock:
                self.refreshing = False

    def status(self):
        with self.lock:
            updated = self.snapshot["updated_at"] if self.snapshot else None
            age = max(0, time.time() - datetime.fromisoformat(updated).timestamp()) if updated else None
            return {"available": self.snapshot is not None, "refreshing": self.refreshing,
                    "updated_at": updated, "stale": age is not None and age > REFRESH_SECONDS,
                    "next_refresh_at": datetime.fromtimestamp(self.next_refresh, timezone.utc).isoformat() if self.next_refresh else None,
                    "completed_missions": self.progress[0], "total_missions": self.progress[1], "message": self.error}

    def start(self):
        with self.lock:
            if self.thread is not None:
                return
            def loop():
                while not self.stop_event.is_set():
                    if self.stop_event.wait(max(0, self.next_refresh - time.time())):
                        break
                    self.refresh()
            self.thread = threading.Thread(target=loop, daemon=True, name="src-leaderboards")
            self.thread.start()

    def stop(self):
        self.stop_event.set()


class Leaderboards:
    def __init__(self, store):
        self.store = store
        self.speedrun = SpeedrunCache(store.root)
        self.ghost_revision = None
        self.ghost_snapshot = None
        self.responses = OrderedDict()
        self.response_bytes = 0

    def ghost_data(self):
        # This is a single indexed-row read on cache hits. DB triggers also catch
        # administrative SQL deletions; routine pings do not churn the cache.
        row = self.store.db.execute("SELECT revision, updated_at FROM leaderboard_revision WHERE singleton=1").fetchone()
        if self.ghost_revision == row[0]:
            return self.ghost_snapshot
        raw = self.store.db.execute("""SELECT * FROM (
            SELECT r.id, r.player_id, p.display_name, r.category, r.duration_seconds, r.uploaded_at,
              ROW_NUMBER() OVER (PARTITION BY r.category,r.player_id ORDER BY r.duration_seconds,r.uploaded_at,r.id) AS best
            FROM replays r JOIN players p ON p.id=r.player_id
            WHERE r.game='jak3' AND r.completed=1 AND r.truncated=0
            ) WHERE best=1 LIMIT ?""", (MAX_RUNS + 1,)).fetchall()
        if len(raw) > MAX_RUNS:
            raise LeaderboardError("Ghost leaderboard exceeds this server's capacity", 503)
        missions = {m["mission_id"]: {**m, "source_url": None, "variants": []} for m in MISSIONS}
        runs = []
        for r in raw:
            cat = r["category"]
            missions.setdefault(cat, {"mission_id": cat, "label": cat.replace("-", " ").title(), "group": "side", "source_url": None, "variants": []})
            runs.append({"mission_id": cat, "player_id": r["player_id"], "display_name": r["display_name"],
                         "duration_seconds": r["duration_seconds"], "run_id": r["id"], "replay_id": r["id"],
                         "run_url": "/replays/" + r["id"], "submitted_at": r["uploaded_at"]})
        self.ghost_snapshot = make_snapshot(list(missions.values()), runs, row[1])
        self.ghost_revision = row[0]
        return self.ghost_snapshot

    def query(self, source, resource, identifier="", group="all", offset=0, limit=50):
        if source not in SOURCES or group not in GROUPS or not 0 <= offset <= 1000000 or not 1 <= limit <= 100:
            raise LeaderboardError("Invalid source, group, or pagination")
        with self.store.lock:
            if source == "ghosts":
                snapshot = self.ghost_data()
            else:
                with self.speedrun.lock:
                    snapshot = self.speedrun.snapshot
                if snapshot is None:
                    raise LeaderboardError("Speedrun.com data is warming up. Please try again shortly.", 503)
            key = (source, snapshot["revision"], resource, identifier, group, offset, limit)
            if key in self.responses:
                self.responses.move_to_end(key)
                return self.responses[key]
            missions = [m for m in snapshot["missions"] if group == "all" or m["group"] == group]
            players = {}
            for mission in missions:
                for run in mission["runs"]:
                    player = players.setdefault(run["player_id"], {"player_id": run["player_id"], "display_name": run["display_name"],
                        "points": 0, "mission_count": 0, "tied_wr_count": 0, "untied_wr_count": 0})
                    player["points"] += run["points"]
                    player["mission_count"] += 1
                    player["tied_wr_count"] += int(run["tied_wr"])
                    player["untied_wr_count"] += int(run["untied_wr"])
            ordered = sorted(players.values(), key=lambda p: (-p["points"], p["player_id"]))
            previous = None
            for index, player in enumerate(ordered, 1):
                if player["points"] != previous:
                    rank, previous = index, player["points"]
                player["rank"] = rank
            data = {"api_version": 1, "game": "jak3", "source": source, "group": group, "scoring_id": SCORING["id"],
                    "revision": snapshot["revision"], "updated_at": snapshot["updated_at"],
                    "total_players": snapshot["total_players"], "ranked_missions": sum(bool(m["runs"]) for m in missions)}
            if resource == "points":
                items = ordered
            elif resource == "missions":
                items = [{k: v for k, v in m.items() if k != "runs"} for m in missions]
            elif resource == "mission":
                mission = next((m for m in missions if m["mission_id"] == identifier), None)
                if mission is None:
                    raise LeaderboardError("Mission not found", 404)
                data["mission"] = {k: v for k, v in mission.items() if k != "runs"}
                items = mission["runs"]
            elif resource == "player":
                if identifier not in players:
                    raise LeaderboardError("Player has no ranked runs in this source/group", 404)
                data["player"] = players[identifier]
                items = [{**r, "mission_label": m["label"]} for m in missions for r in m["runs"] if r["player_id"] == identifier]
            else:
                raise LeaderboardError("Unknown leaderboard", 404)
            data.update(items=items[offset:offset + limit], total=len(items), offset=offset, limit=limit,
                        next_offset=offset + limit if offset + limit < len(items) else None)
            body = encoded(data)
            if len(body) <= MAX_CACHE_BYTES:
                while self.responses and (len(self.responses) >= 64 or self.response_bytes + len(body) > MAX_CACHE_BYTES):
                    _, old = self.responses.popitem(last=False)
                    self.response_bytes -= len(old)
                self.responses[key] = body
                self.response_bytes += len(body)
            return body
