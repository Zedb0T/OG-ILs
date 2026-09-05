"""Public routes and shared cache headers for both HTTP transports."""
import hashlib
from pathlib import Path
import re
from urllib.parse import parse_qs, urlsplit

from leaderboards import GROUPS, SCORING, SOURCES, LeaderboardError, encoded

STATIC = Path(__file__).with_name("public")


def public_route(store, target):
    parts = urlsplit(target)
    path = parts.path
    if path in ("/", "/api"):
        return 200, page("leaderboard.html" if path == "/" else "api.html"), "text/html; charset=utf-8"
    if path in assets():
        return 200, *assets()[path]
    if not path.startswith("/api/v1/"):
        return None
    query = parse_qs(parts.query, keep_blank_values=True, max_num_fields=12)
    allowed = {"source", "game", "group", "offset", "limit"}
    if set(query) - allowed or any(len(v) != 1 for v in query.values()):
        raise LeaderboardError("Unknown or repeated query parameter")
    get = lambda key, default: query.get(key, [default])[0]
    source, game, group = get("source", "speedrun"), get("game", "jak3"), get("group", "all")
    if source not in SOURCES or group not in GROUPS or game != "jak3":
        raise LeaderboardError("Supported game: jak3; sources: speedrun, ghosts; groups: all, main, orb, side")
    if path == "/api/v1/scoring":
        return 200, {"api_version": 1, "scoring": SCORING, "sources": list(SOURCES)}, "application/json"
    if path == "/api/v1/status":
        status = store.leaderboards.speedrun.status() if source == "speedrun" else {
            "available": True, "refreshing": False, "stale": False, "message": None}
        return 200, {"api_version": 1, "game": game, "source": source, **status}, "application/json"
    try:
        offset, limit = int(get("offset", "0")), int(get("limit", "50"))
    except ValueError:
        raise LeaderboardError("Offset and limit must be integers") from None
    identifier = ""
    if path == "/api/v1/leaderboards/points":
        resource = "points"
    elif path == "/api/v1/missions":
        resource = "missions"
    elif re.fullmatch(r"/api/v1/missions/[A-Za-z0-9_-]{1,96}/leaderboard", path):
        resource, identifier = "mission", path.split("/")[4]
    elif re.fullmatch(r"/api/v1/players/[A-Za-z0-9_-]{1,96}", path):
        resource, identifier = "player", path.split("/")[4]
    else:
        raise LeaderboardError("Not found", 404)
    return 200, store.leaderboards.query(source, resource, identifier, group, offset, limit), "application/json"


_assets = None
_pages = {}


def assets():
    global _assets
    if _assets is None:
        loaded = {}
        for name, kind in (("leaderboard.css", "text/css; charset=utf-8"), ("leaderboard.js", "text/javascript; charset=utf-8")):
            body = (STATIC / name).read_bytes()
            stem, suffix = name.rsplit(".", 1)
            path = f"/assets/{stem}-{hashlib.sha256(body).hexdigest()[:16]}.{suffix}"
            loaded[path] = (body, kind)
        _assets = loaded
    return _assets


def page(name):
    if name not in _pages:
        text = (STATIC / name).read_text(encoding="utf-8")
        for path in assets():
            text = text.replace("{{" + path.rsplit(".", 1)[1] + "}}", path)
        _pages[name] = text.encode()
    return _pages[name]


def response_parts(status, data, kind, target, method="GET", if_none_match=""):
    """Admin, identities, errors, and legacy game APIs are never shared-cached."""
    body = data if isinstance(data, bytes) else encoded(data)
    path = urlsplit(target).path
    public = status == 200 and method in ("GET", "HEAD") and (
        path in ("/", "/api") or path.startswith("/api/v1/") or path in assets())
    headers = {"Content-Type": kind, "X-Content-Type-Options": "nosniff", "Cache-Control": "no-store",
               "Content-Security-Policy": "default-src 'self'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; frame-ancestors 'none'"}
    if public:
        headers["Content-Security-Policy"] = "default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; base-uri 'none'; frame-ancestors 'none'; form-action 'self'"
        headers["Referrer-Policy"] = "strict-origin-when-cross-origin"
        headers["Cache-Control"] = "public, max-age=15, stale-while-revalidate=30"
        if path in assets():
            headers["Cache-Control"] = "public, max-age=31536000, immutable"
        elif path in ("/", "/api"):
            headers["Cache-Control"] = "public, no-cache"
        elif path == "/api/v1/status":
            headers["Cache-Control"] = "no-store"
        etag = '"' + hashlib.sha256(body).hexdigest() + '"'
        headers["ETag"] = etag
        # Weak comparison is correct for GET/HEAD If-None-Match; cap work.
        tags = [tag.strip().removeprefix("W/") for tag in if_none_match[:8192].split(",")]
        if etag in tags or "*" in tags:
            return 304, b"", headers  # no Content-Length: 0 on a 304 representation
    if status == 503:
        headers["Retry-After"] = "60" if path.startswith("/api/v1/") else "1"
    headers["Content-Length"] = str(len(body))
    return status, b"" if method == "HEAD" else body, headers
