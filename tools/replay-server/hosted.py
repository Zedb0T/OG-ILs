"""Bounded WSGI deployment behind an HTTPS reverse proxy (Waitress)."""
import json
import os
from pathlib import Path
import shutil
import threading
from http import HTTPStatus
from urllib.parse import urlsplit

from server import APIError, Store, require, route

MIB = 1024 * 1024
BODY_LIMIT = 4 * MIB  # Start small for the 300 MB service; reject larger files explicitly.


class Application:
    def __init__(self, store, origin, disk_budget=128 * MIB):
        parsed = urlsplit(origin)
        if (parsed.scheme != "https" or not parsed.hostname or parsed.username or
                parsed.password or parsed.path or parsed.query or parsed.fragment):
            raise ValueError("GHOST_PUBLIC_ORIGIN must be an HTTPS origin without a path")
        self.store, self.origin, self.host = store, origin, parsed.netloc
        self.disk_budget = disk_budget
        self.slot = threading.BoundedSemaphore(1)

    def __call__(self, env, start_response):
        acquired = False
        try:
            require(env.get("HTTP_HOST") == self.host, "Invalid Host")
            require(env.get("HTTP_ORIGIN", self.origin) == self.origin, "Cross-origin requests are not allowed")
            path = env.get("PATH_INFO", "")
            method = env["REQUEST_METHOD"]
            # Health remains responsive while a replay is being parsed/written.
            if path != "/health":
                acquired = self.slot.acquire(blocking=False)
                if not acquired:
                    raise APIError("Server busy; retry shortly", 503)

            def body():
                size = int(env.get("CONTENT_LENGTH") or "0")
                limit = BODY_LIMIT if path == "/replays" else 4096
                if not 0 < size <= limit:
                    raise APIError("Upload exceeds this test server's size limit", 413)
                used = sum(p.stat().st_size for p in self.store.root.rglob("*") if p.is_file())
                if used + size * 2 + 4 * MIB > self.disk_budget or shutil.disk_usage(self.store.root).free < 128 * MIB:
                    raise APIError("Replay storage is near capacity; contact the admin", 507)
                raw = env["wsgi.input"].read(size)
                require(len(raw) == size, "Incomplete body")
                return raw

            status, data, kind = route(self.store, method, path + "?" + env.get("QUERY_STRING", ""),
                                       env.get("HTTP_AUTHORIZATION", ""), env.get("HTTP_X_PLAYER_ID", ""), body)
        except APIError as error:
            status, data, kind = error.status, {"error": str(error)}, "application/json"
        except (ValueError, TypeError, KeyError, AttributeError, UnicodeError, RecursionError):
            status, data, kind = 400, {"error": "Malformed request"}, "application/json"
        except Exception:
            status, data, kind = 500, {"error": "Storage error"}, "application/json"
        finally:
            if acquired:
                self.slot.release()
        raw = data if isinstance(data, bytes) else json.dumps(data, allow_nan=False).encode()
        headers = [("Content-Type", kind), ("Content-Length", str(len(raw))),
                   ("Cache-Control", "no-store"), ("X-Content-Type-Options", "nosniff"),
                   ("Content-Security-Policy", "default-src 'self'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; frame-ancestors 'none'")]
        if status == 503:
            headers.append(("Retry-After", "1"))
        start_response(f"{status} {HTTPStatus(status).phrase}", headers)
        return [raw]


def main():
    from waitress import serve
    config_path = Path(os.environ.get("GHOST_CONFIG", "/home/container/ghost-config.json"))
    config = json.loads(config_path.read_text())
    data = Path(os.environ.get("GHOST_DATA", "/home/container/data"))
    # Config/data live outside downloaded releases and are never replaced on restart.
    store = Store(data, config["admin_user"], config["admin_password"])
    app = Application(store, config["public_origin"], int(config.get("disk_budget_mb", 128)) * MIB)
    print("Ghost service starting; HTTPS origin:", app.origin, flush=True)
    print("Limits: one active replay request, 4 MiB uploads, persistent data:", store.root, flush=True)
    try:
        serve(app, host=os.environ.get("GHOST_BIND", "0.0.0.0"), port=int(os.environ["SERVER_PORT"]),
              threads=2, connection_limit=8, backlog=16, channel_timeout=15, cleanup_interval=5,
              max_request_body_size=BODY_LIMIT, max_request_header_size=8192,
              inbuf_overflow=64 * 1024, outbuf_overflow=64 * 1024,
              outbuf_high_watermark=256 * 1024, expose_tracebacks=False)
    finally:
        store.close()


if __name__ == "__main__":
    main()
