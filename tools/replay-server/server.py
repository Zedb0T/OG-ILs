"""Local, dependency-free OpenGOAL replay service. Run with Python 3.11+."""
from __future__ import annotations

import argparse
import base64
import binascii
import hashlib
import hmac
import json
import math
import os
from pathlib import Path
import re
import secrets
import sqlite3
import threading
import uuid
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

from leaderboards import Leaderboards, LeaderboardError
from leaderboard_http import public_route, response_parts

MAX_BYTES = 32 * 1024 * 1024
IDENTIFIER = re.compile(r"[A-Za-z0-9_-]{1,96}\Z")
PLAYER_ID = re.compile(r"[a-f0-9]{32}\Z")


class APIError(Exception):
    def __init__(self, message, status=400):
        super().__init__(message)
        self.status = status


def require(condition, message):
    if not condition:
        raise APIError(message)


def number(value, low=-1e12, high=1e12):
    return type(value) in (int, float) and math.isfinite(value) and low <= value <= high


def validate_replay(data):
    require(isinstance(data, dict), "Expected replay object")
    require(data.get("schema") == "opengoal-replay", "Unsupported schema")
    version = data.get("version")
    require(type(version) is int and 1 <= version <= 3, "Unsupported version")
    require(data.get("game") == "jak3", "Unsupported game")
    category = data.get("category")
    require(isinstance(category, str) and IDENTIFIER.fullmatch(category), "Invalid category")
    require(data.get("units") == "meters" and data.get("sample_rate_hz") == 60,
            "Unsupported units or sample rate")
    require(type(data.get("completed")) is bool and type(data.get("truncated")) is bool,
            "Invalid completion flags")
    require(number(data.get("duration_seconds"), 0, 601), "Invalid duration")
    samples = data.get("samples")
    require(isinstance(samples, list) and 1 <= len(samples) <= 36000, "Invalid sample count")
    require(type(data.get("sample_count")) is int and data["sample_count"] == len(samples),
            "Sample count mismatch")
    previous = -1
    for sample in samples:
        require(isinstance(sample, list) and len(sample) == {1: 7, 2: 8, 3: 14}[version],
                "Malformed sample")
        require(number(sample[0], 0, 601) and sample[0] >= previous, "Invalid sample time")
        previous = sample[0]
        for index, size in [(1, 3), (2, 4), (3, 3)] + ([(9, 3), (10, 4), (11, 4)] if version == 3 else []):
            vector = sample[index]
            require(isinstance(vector, list) and len(vector) == size and all(number(v) for v in vector),
                    "Invalid sample vector")
        require(type(sample[4]) is int and 0 <= sample[4] <= 0xffffffff, "Invalid status")
        offset = int(version >= 2)
        for index, bound in [(5 + offset, 48), (6 + offset, 64)] + ([(8, 64), (13, 64)] if version == 3 else []):
            value = sample[index]
            require(isinstance(value, str) and len(value.encode("utf-8")) < bound
                    and all(32 <= ord(c) < 127 for c in value), "Invalid animation/state name")
        require(bool(sample[5 + offset]), "Empty state")
        if version >= 2:
            require(number(sample[5], 0, 1e8), "Invalid animation frame")
        if version == 3:
            require(number(sample[12], 0, 1e8), "Invalid extra animation frame")
    require(abs(data["duration_seconds"] - samples[-1][0]) <= 0.1, "Duration does not match samples")
    return data


def atomic_write(path, contents):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + "." + secrets.token_hex(8) + ".tmp")
    try:
        with temporary.open("xb") as stream:
            stream.write(contents)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


class Store:
    def __init__(self, root, admin_user="user", admin_password="pass"):
        require(bool(admin_user) and ":" not in admin_user and bool(admin_password), "Invalid admin credentials")
        self.admin_user = admin_user
        self.admin_password = admin_password
        self.root = Path(root).resolve()
        self.root.mkdir(parents=True, exist_ok=True)
        self.lock = threading.RLock()
        self.db = sqlite3.connect(self.root / "index.sqlite3", check_same_thread=False)
        self.db.row_factory = sqlite3.Row
        self.db.executescript("""
            PRAGMA journal_mode=WAL;
            PRAGMA foreign_keys=ON;
            CREATE TABLE IF NOT EXISTS players (
              id TEXT PRIMARY KEY, token_hash TEXT NOT NULL, display_name TEXT NOT NULL,
              last_ping_at TEXT);
            CREATE TABLE IF NOT EXISTS replays (
              id TEXT PRIMARY KEY, player_id TEXT NOT NULL REFERENCES players(id),
              digest TEXT NOT NULL, game TEXT NOT NULL, category TEXT NOT NULL,
              duration_seconds REAL NOT NULL, completed INTEGER NOT NULL,
              truncated INTEGER NOT NULL, sample_count INTEGER NOT NULL,
              uploaded_at TEXT NOT NULL, path TEXT NOT NULL,
              UNIQUE(player_id, digest));
            CREATE INDEX IF NOT EXISTS by_mission ON replays(game, category, duration_seconds);
            CREATE INDEX IF NOT EXISTS leaderboard_bests ON replays(game, completed, truncated, category, player_id, duration_seconds);
            CREATE TABLE IF NOT EXISTS leaderboard_revision (
              singleton INTEGER PRIMARY KEY CHECK(singleton=1), revision INTEGER NOT NULL,
              updated_at TEXT NOT NULL);
            INSERT OR IGNORE INTO leaderboard_revision VALUES (1, 0, strftime('%Y-%m-%dT%H:%M:%fZ','now'));
            CREATE TRIGGER IF NOT EXISTS leaderboard_insert AFTER INSERT ON replays BEGIN
              UPDATE leaderboard_revision SET revision=revision+1,updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE singleton=1;
            END;
            CREATE TRIGGER IF NOT EXISTS leaderboard_delete AFTER DELETE ON replays BEGIN
              UPDATE leaderboard_revision SET revision=revision+1,updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE singleton=1;
            END;
            CREATE TRIGGER IF NOT EXISTS leaderboard_update AFTER UPDATE ON replays BEGIN
              UPDATE leaderboard_revision SET revision=revision+1,updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE singleton=1;
            END;
            CREATE TRIGGER IF NOT EXISTS leaderboard_name AFTER UPDATE OF display_name ON players
            WHEN OLD.display_name != NEW.display_name BEGIN
              UPDATE leaderboard_revision SET revision=revision+1,updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE singleton=1;
            END;
        """)
        # Additive migration: retain existing identities, names, and replay files.
        if "last_ping_at" not in {row[1] for row in self.db.execute("PRAGMA table_info(players)")}:
            with self.db:
                self.db.execute("ALTER TABLE players ADD COLUMN last_ping_at TEXT")
        # Recover a process interruption between a filename rename and its DB
        # commit. Match payload digest, never trust just the readable filename.
        with self.db:
            for row in self.db.execute("SELECT id, path, category, digest FROM replays").fetchall():
                if (self.root / row["path"]).exists():
                    continue
                for candidate in (self.root / "jak3" / row["category"]).glob(f"*_{row['id'][:8]}.ogr.json"):
                    if hashlib.sha256(candidate.read_bytes()).hexdigest() == row["digest"]:
                        candidate.rename(self.root / row["path"])
                        break
        self.leaderboards = Leaderboards(self)

    def close(self):
        self.leaderboards.speedrun.stop()
        self.db.close()

    def authenticate_admin(self, authorization):
        if not authorization.startswith("Basic ") or len(authorization) > 2048:
            return False
        try:
            credentials = base64.b64decode(authorization[6:], validate=True).decode("utf-8")
            username, password = credentials.split(":", 1)
        except (ValueError, UnicodeError, binascii.Error):
            return False
        user_matches = hmac.compare_digest(username.encode(), self.admin_user.encode())
        password_matches = hmac.compare_digest(password.encode(), self.admin_password.encode())
        return user_matches and password_matches

    def register(self, player_id, token):
        require(isinstance(player_id, str) and PLAYER_ID.fullmatch(player_id), "Invalid player ID")
        require(isinstance(token, str) and re.fullmatch(r"[a-f0-9]{64}", token), "Invalid player token")
        digest = hashlib.sha256(token.encode()).hexdigest()
        with self.lock, self.db:
            existing = self.db.execute("SELECT token_hash FROM players WHERE id=?", (player_id,)).fetchone()
            if existing and not hmac.compare_digest(existing[0], digest):
                raise APIError("Player authentication failed", 403)
            self.db.execute("INSERT OR IGNORE INTO players (id, token_hash, display_name) VALUES (?,?,?)",
                            (player_id, digest, "Unknown-" + player_id[:8]))
        return {"player_id": player_id}

    def ping(self, player_id, token):
        # A first ping registers the persistent game identity; later pings must
        # prove ownership before changing its timestamp. Never trust client time.
        with self.lock:
            self.register(player_id, token)
            now = datetime.now(timezone.utc).isoformat()
            with self.db:
                self.db.execute("UPDATE players SET last_ping_at=? WHERE id=?", (now, player_id))
            player = self.authenticate(player_id, token)
            return {"player_id": player_id, "display_name": player["display_name"],
                    "identified": player["display_name"] != "Unknown-" + player_id[:8],
                    "last_ping_at": now}

    def authenticate(self, player_id, token):
        with self.lock:
            row = self.db.execute("SELECT * FROM players WHERE id=?", (player_id,)).fetchone()
            if not row or not hmac.compare_digest(row["token_hash"], hashlib.sha256(token.encode()).hexdigest()):
                raise APIError("Player authentication failed", 403)
            return row

    def upload(self, player_id, token, raw):
        require(len(raw) <= MAX_BYTES, "Replay too large")
        data = validate_replay(json.loads(raw))
        canonical = json.dumps(data, separators=(",", ":"), allow_nan=False).encode()
        digest = hashlib.sha256(canonical).hexdigest()
        with self.lock:
            player = self.authenticate(player_id, token)
            old = self.db.execute("SELECT id FROM replays WHERE player_id=? AND digest=?", (player_id, digest)).fetchone()
            if old:
                return self.metadata(old[0])
            replay_id = uuid.uuid4().hex
            now = datetime.now(timezone.utc)
            name = re.sub(r"[^A-Za-z0-9_-]", "_", player["display_name"])[:40] or "Unknown"
            filename = f"{name}_{data['duration_seconds']:.3f}s_{now:%Y%m%dT%H%M%S%fZ}_{replay_id[:8]}.ogr.json"
            relative = Path("jak3") / data["category"] / filename
            atomic_write(self.root / relative, canonical)
            try:
                with self.db:
                    self.db.execute("INSERT INTO replays VALUES (?,?,?,?,?,?,?,?,?,?,?)", (
                        replay_id, player_id, digest, "jak3", data["category"], data["duration_seconds"],
                        data["completed"], data["truncated"], data["sample_count"], now.isoformat(), relative.as_posix()))
            except Exception:
                (self.root / relative).unlink(missing_ok=True)
                raise
            return self.metadata(replay_id)

    def metadata(self, replay_id):
        with self.lock:
            row = self.db.execute("SELECT r.*, p.display_name FROM replays r JOIN players p ON p.id=r.player_id WHERE r.id=?", (replay_id,)).fetchone()
            if not row:
                raise APIError("Replay not found", 404)
            result = {k: row[k] for k in ("id", "player_id", "display_name", "game", "category", "duration_seconds", "sample_count", "uploaded_at")}
            result.update(completed=bool(row["completed"]), truncated=bool(row["truncated"]), download=f"/replays/{replay_id}")
            return result

    def catalog(self, game, category, offset=0, limit=100):
        require(game == "jak3" and isinstance(category, str) and IDENTIFIER.fullmatch(category), "Invalid mission")
        require(0 <= offset <= 1000000 and 1 <= limit <= 100, "Invalid pagination")
        with self.lock:
            rows = self.db.execute("SELECT id FROM replays WHERE game=? AND category=? ORDER BY duration_seconds, uploaded_at, id LIMIT ? OFFSET ?",
                                   (game, category, limit + 1, offset)).fetchall()
            return {"replays": [self.metadata(row[0]) for row in rows[:limit]],
                    "next_offset": offset + limit if len(rows) > limit else None}

    def selection(self, category, mode, player_id, best=None):
        require(mode in ("default", "wr"), "Invalid selection mode")
        require(isinstance(category, str) and IDENTIFIER.fullmatch(category), "Invalid mission")
        require(best is None or number(best, 0, 601), "Invalid best time")
        with self.lock:
            rows = self.db.execute("SELECT * FROM replays WHERE game='jak3' AND category=? AND completed=1 AND truncated=0 ORDER BY duration_seconds, uploaded_at, id", (category,)).fetchall()
            # One best per player. Ties have stable ordering and aren't 'faster'.
            by_player = {}
            for row in rows:
                by_player.setdefault(row["player_id"], row)
            ranked = list(by_player.values())
            selected = None
            if ranked and mode == "wr":
                selected = ranked[0]
            elif ranked:
                own = by_player.get(player_id)
                if own:
                    best = min(best, own["duration_seconds"]) if best is not None else own["duration_seconds"]
                opponents = [r for r in ranked if r["player_id"] != player_id]
                if best is None:
                    selected = opponents[-1] if opponents else None
                else:
                    faster = [r for r in opponents if r["duration_seconds"] < best]
                    selected = faster[-1] if faster else (own if own and own["duration_seconds"] <= best else None)
            return {"replays": [self.metadata(selected["id"])] if selected else []}

    def download(self, replay_id):
        with self.lock:
            row = self.db.execute("SELECT path FROM replays WHERE id=?", (replay_id,)).fetchone()
            if not row:
                raise APIError("Replay not found", 404)
            path = (self.root / row[0]).resolve()
            require(path.is_relative_to(self.root), "Invalid stored path")
            return path.read_bytes()

    def players(self):
        with self.lock:
            return [dict(r) for r in self.db.execute("SELECT id, display_name, last_ping_at FROM players ORDER BY display_name, id")]

    def rename(self, player_id, name):
        require(isinstance(name, str) and 1 <= len(name.strip()) <= 40 and all(ord(c) >= 32 for c in name), "Display name must be 1-40 characters")
        with self.lock:
            moved = []
            try:
                with self.db:
                    changed = self.db.execute("UPDATE players SET display_name=? WHERE id=?", (name.strip(), player_id)).rowcount
                    if not changed:
                        raise APIError("Player not found", 404)
                    slug = re.sub(r"[^A-Za-z0-9_-]", "_", name.strip())[:40] or "Unknown"
                    for row in self.db.execute("SELECT * FROM replays WHERE player_id=?", (player_id,)).fetchall():
                        date = datetime.fromisoformat(row["uploaded_at"])
                        filename = f"{slug}_{row['duration_seconds']:.3f}s_{date:%Y%m%dT%H%M%S%fZ}_{row['id'][:8]}.ogr.json"
                        old = self.root / row["path"]
                        new = old.with_name(filename)
                        if old != new:
                            require(not new.exists(), "Filename collision")
                            old.rename(new)
                            moved.append((old, new))
                            self.db.execute("UPDATE replays SET path=? WHERE id=?", (new.relative_to(self.root).as_posix(), row["id"]))
            except Exception:
                for old, new in reversed(moved):
                    new.rename(old)
                raise
        return {"player_id": player_id, "display_name": name.strip()}


ADMIN_HTML = b'''<!doctype html><meta charset="utf-8"><title>OpenGOAL ghost admin</title>
<style>body{font:16px system-ui;max-width:1050px;margin:3em auto;padding:0 1em;background:#17202b;color:#eee}input,button{padding:.6em;margin:.3em}code{font-size:12px}.table-wrap{overflow-x:auto}table{width:100%;border-collapse:collapse}th,td{text-align:left;padding:.7em;border-bottom:1px solid #435063}time{white-space:nowrap;font-size:14px}</style>
<h1>Ghost players</h1><p>Sign in to manage display names. Names and readable filenames update together; replay IDs remain unchanged.</p>
<p><a href="/" style="color:#86dceb">&larr; Public leaderboards</a></p>
<form id="login"><label>Username <input id="username" name="username" value="user" autocomplete="username" maxlength="128" required></label><label>Password <input id="password" name="password" type="password" autocomplete="current-password" maxlength="256" required></label><button type="submit">Sign in / Load players</button></form><p id="status" role="status"></p>
<p>Players can press L3 + D-pad Down in game to ping. Last ping refreshes every 5 seconds while this page is visible.</p>
<div class="table-wrap"><table><thead><tr><th>Player ID</th><th>Display name</th><th>Last ping (your local time)</th><th>Action</th></tr></thead><tbody id="players"></tbody></table></div>
<script>
async function api(path, body){const credentials=document.querySelector('#username').value+':'+document.querySelector('#password').value;const encoded=btoa(String.fromCharCode(...new TextEncoder().encode(credentials)));const r=await fetch(path,{method:body?'POST':'GET',headers:{Authorization:'Basic '+encoded,'Content-Type':'application/json'},body:body?JSON.stringify(body):undefined});const j=await r.json();if(!r.ok)throw Error(j.error);return j;}
let signedIn=false,loading=false;
const rows=new Map(),status=document.querySelector('#status');
async function loadPlayers(){
  if(loading)return;loading=true;
  try{
    const data=await api('/admin/players');signedIn=true;
    for(const p of data.players){
      let row=rows.get(p.id);
      if(!row){
        const tr=document.createElement('tr'),name=document.createElement('input'),ping=document.createElement('time'),save=document.createElement('button'),id=document.createElement('code');
        id.textContent=p.id;name.value=p.display_name;name.maxLength=40;name.setAttribute('aria-label','Display name for '+p.id);save.textContent='Save';
        row={tr,name,ping,saved:p.display_name};rows.set(p.id,row);
        save.onclick=async()=>{save.disabled=true;try{const result=await api('/admin/players/'+p.id,{display_name:name.value});row.saved=result.display_name;name.value=result.display_name;status.textContent='Saved';}catch(e){status.textContent=e.message}finally{save.disabled=false}};
        for(const element of [id,name,ping,save]){const td=document.createElement('td');td.append(element);tr.append(td)}
        document.querySelector('#players').append(tr);
      }
      // Refresh timestamps without replacing inputs or erasing an unsaved edit.
      if(document.activeElement!==row.name&&row.name.value===row.saved){row.name.value=p.display_name;row.saved=p.display_name}
      row.ping.dateTime=p.last_ping_at||'';
      row.ping.textContent=p.last_ping_at?new Date(p.last_ping_at).toLocaleString():'Never';
      row.ping.title=p.last_ping_at||'No ping received yet';
    }
    status.textContent=data.players.length?'Signed in - player pings are live':'Signed in - no players yet';
  }catch(e){status.textContent=e.message}finally{loading=false}
}
document.querySelector('#login').onsubmit=async(event)=>{event.preventDefault();signedIn=false;await loadPlayers()};
setInterval(()=>{if(signedIn&&!document.hidden)loadPlayers()},5000);
</script>'''


def route(store, method, target, authorization, player_id, read_body):
    """Transport-independent API shared by the local and hosted servers."""
    parts = urlsplit(target)
    query = parse_qs(parts.query)
    get = lambda key, default="": query.get(key, [default])[0]
    path = parts.path
    if path.startswith("/admin/") and not store.authenticate_admin(authorization):
        raise APIError("Invalid admin username or password", 401)
    if method in ("GET", "HEAD"):
        try:
            public = public_route(store, target)
        except LeaderboardError as error:
            raise APIError(str(error), error.status) from None
        if public is not None:
            return public
        if path == "/health":
            return 200, {"ok": True, "api_version": 1}, "application/json"
        if path == "/admin":
            return 200, ADMIN_HTML, "text/html; charset=utf-8"
        if path == "/admin/players":
            data = {"players": store.players()}
        elif path == "/replays":
            data = store.catalog(get("game", "jak3"), get("category"), int(get("offset", "0")), int(get("limit", "100")))
        elif path == "/selection":
            best = float(get("best_seconds")) if get("best_seconds") else None
            data = store.selection(get("category"), get("mode"), get("player_id"), best)
        elif re.fullmatch(r"/replays/[a-f0-9]{32}", path):
            data = store.download(path.split("/")[-1])
        elif re.fullmatch(r"/replays/[a-f0-9]{32}/metadata", path):
            data = store.metadata(path.split("/")[2])
        else:
            raise APIError("Not found", 404)
        return 200, data, "application/json"
    if method == "POST":
        if path == "/players/ping":
            data = json.loads(read_body())
            require(isinstance(data, dict), "Expected player identity object")
            return 200, store.ping(data.get("player_id"), data.get("token")), "application/json"
        if path == "/players":
            data = json.loads(read_body())
            return 200, store.register(data.get("player_id"), data.get("token")), "application/json"
        if path == "/replays":
            store.authenticate(player_id, authorization.removeprefix("Bearer "))
            return 201, store.upload(player_id, authorization.removeprefix("Bearer "), read_body()), "application/json"
        if re.fullmatch(r"/admin/players/[a-f0-9]{32}", path):
            return 200, store.rename(path.split("/")[-1], json.loads(read_body()).get("display_name")), "application/json"
    raise APIError("Not found", 404)


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def setup(self):
        self.request.settimeout(10)  # also bound waiting for request headers
        super().setup()

    def log_message(self, fmt, *args):
        pass  # Never log request credentials or bodies.

    def reply(self, status, data, kind="application/json"):
        status, raw, headers = response_parts(status, data, kind, self.path, self.command,
                                             self.headers.get("If-None-Match", ""))
        self.send_response(status)
        for key, value in headers.items():
            self.send_header(key, value)
        self.end_headers()
        self.wfile.write(raw)

    def body(self):
        require(not self.headers.get("Transfer-Encoding"), "Chunked uploads are unsupported")
        size = int(self.headers.get("Content-Length", "0"))
        if not 0 < size <= MAX_BYTES:
            raise APIError("Invalid request size", 413)
        raw = self.rfile.read(size)
        require(len(raw) == size, "Incomplete body")
        return raw

    def dispatch(self):
        try:
            self.connection.settimeout(10)
            host = self.headers.get("Host", "")
            require(host in (f"127.0.0.1:{self.server.server_port}", f"localhost:{self.server.server_port}"), "Invalid Host")
            origin = self.headers.get("Origin")
            require(not origin or origin == "http://" + host, "Cross-origin requests are not allowed")
            return self.reply(*route(self.server.store, self.command, self.path,
                                     self.headers.get("Authorization", ""),
                                     self.headers.get("X-Player-ID", ""), self.body))
        except APIError as error:
            self.reply(error.status, {"error": str(error)})
        except (ValueError, TypeError, KeyError, AttributeError, UnicodeError):
            self.reply(400, {"error": "Malformed request"})
        except (BrokenPipeError, ConnectionResetError, TimeoutError):
            pass
        except Exception:
            self.reply(500, {"error": "Storage error; inspect server data directory"})

    do_GET = dispatch
    do_HEAD = dispatch
    do_POST = dispatch


class Server(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, address, store):
        self.store = store
        self.slots = threading.BoundedSemaphore(8)
        super().__init__(address, Handler)

    def process_request(self, request, client_address):
        if not self.slots.acquire(blocking=False):
            self.shutdown_request(request)
            return
        try:
            super().process_request(request, client_address)
        except Exception:
            self.slots.release()
            raise

    def process_request_thread(self, request, client_address):
        try:
            super().process_request_thread(request, client_address)
        finally:
            self.slots.release()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data", type=Path, default=Path(__file__).parent / "data")
    parser.add_argument("--port", type=int, default=8765)
    args = parser.parse_args()
    store = Store(args.data, admin_user=os.environ.get("GHOST_ADMIN_USER", "user"),
                  admin_password=os.environ.get("GHOST_ADMIN_PASSWORD", "pass"))
    store.leaderboards.speedrun.start()
    with Server(("127.0.0.1", args.port), store) as server:
        print(f"Ghost server: http://127.0.0.1:{server.server_port}   Admin: /admin", flush=True)
        print(f"Data: {store.root}\nAdmin login: username/password (local testing only)", flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass
    store.close()


if __name__ == "__main__":
    main()
