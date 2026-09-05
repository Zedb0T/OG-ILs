import copy
import io
import json
from pathlib import Path
import sqlite3
import tempfile
import threading
import time
import unittest
from urllib.error import HTTPError
from urllib.request import Request, urlopen
from unittest.mock import patch

from hosted import Application
from leaderboards import (CACHE_VERSION, MAX_CACHE_BYTES, MISSIONS, REFRESH_SECONDS,
    SRC_CATEGORY, SRC_GAME, LeaderboardError, SourceRedirects, SpeedrunCache,
    encoded, fetch_speedrun, make_snapshot, points, src_subcategories, utc_now)
from leaderboard_http import assets, public_route, response_parts
from server import APIError, Server, Store, route
from test_server import replay


def fake_src():
    levels = [{"id": "level001", "name": "Complete Arena Training Course"},
              {"id": "level002", "name": "Orb Search 18 (Spargus E)"}]
    variable = {"id": "var00001", "name": "Test variant", "is-subcategory": True,
                "category": None, "scope": {"type": "single-level", "level": "level001"},
                "values": {"default": "value002", "values": {"value001": {"label": "First"}, "value002": {"label": "Default"}}}}
    game = {"data": {"id": SRC_GAME, "abbreviation": "jak3og_missions",
            "categories": {"data": [{"id": SRC_CATEGORY, "type": "per-level"}]},
            "levels": {"data": levels}, "variables": {"data": [variable]}}}
    calls = []
    def fetch(path):
        calls.append(path)
        if path.startswith("/games/"):
            return copy.deepcopy(game)
        level = "level001" if "/level001/" in path else "level002"
        def run(player, seconds, rid, status="verified"):
            return {"place": 1, "run": {"id": rid, "status": {"status": status},
                "players": [{"rel": "user", "id": player}], "times": {"primary_t": seconds},
                "submitted": "2026-01-01T00:00:00Z", "weblink": "https://www.speedrun.com/runs/" + rid}}
        return {"data": {"game": SRC_GAME, "category": SRC_CATEGORY, "level": level,
            "values": {"var00001": "value002"} if level == "level001" else {},
            "players": {"data": [{"rel": "user", "id": p, "names": {"international": "Same Name"}} for p in ("player01", "player02")]},
            "runs": [run("player01", 10, "run00001"), run("player01", 12, "run00002"), run("player02", 10, "run00003"), run("player02", 1, "pending0", "new")],
            "weblink": "https://www.speedrun.com/jak3og_missions"}}
    return fetch, calls, variable


class ScoringTests(unittest.TestCase):
    def test_reference_formula(self):
        self.assertEqual([points(n, 10) for n in range(1, 11)], [100, 97, 95, 94, 81, 67, 54, 40, 27, 13])
        self.assertEqual(points(4, 4), 94)
        self.assertEqual(points(0, 10), 0)
        self.assertEqual(points(100, 10), 0)

    def test_src_defaults_verified_dedupe_and_identity(self):
        fetch, calls, variable = fake_src()
        data = fetch_speedrun(fetch)
        self.assertIn("var-var00001=value002", calls[1])
        self.assertNotIn("var-", calls[2])
        snapshot = make_snapshot(**data)
        self.assertEqual(snapshot["total_players"], 2)
        rows = snapshot["missions"][0]["runs"]
        self.assertEqual(len(rows), 2)
        self.assertEqual([r["points"] for r in rows], [100, 100])
        self.assertTrue(all(r["tied_wr"] for r in rows))
        self.assertEqual(snapshot["missions"][0]["variants"], ["Test variant: Default"])
        self.assertEqual(snapshot["missions"][1]["group"], "orb")
        variable["values"]["default"] = None
        with self.assertRaises(ValueError):
            src_subcategories([variable], "level001")

    def test_native_mission_registry(self):
        self.assertEqual(len({m["mission_id"] for m in MISSIONS}), len(MISSIONS))
        self.assertEqual(len(MISSIONS), 131)
        self.assertEqual(next(m for m in MISSIONS if m["mission_id"] == "wascity-bbush-get-to-18")["group"], "orb")

    def test_src_rejects_wrong_subcategory(self):
        fetch, _, _ = fake_src()
        def wrong(path):
            result = fetch(path)
            if path.startswith("/leaderboards/"):
                result["data"]["values"] = {}
            return result
        with self.assertRaisesRegex(ValueError, "requested subcategory"):
            fetch_speedrun(wrong)

    def test_redirects_cannot_reach_another_origin(self):
        for target in ("http://www.speedrun.com/api/v1/games", "https://evil.example/api/v1/games", "https://www.speedrun.com/not-api"):
            with self.assertRaises(ValueError):
                SourceRedirects().redirect_request(Request("https://www.speedrun.com/api/v1/games"), None, 302, "Found", {}, target)


class PublicLeaderboardTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.store = Store(self.temp.name)
        self.player, self.token = "1" * 32, "a" * 64
        self.store.register(self.player, self.token)

    def tearDown(self):
        self.store.close()
        self.temp.cleanup()

    def upload(self, seconds, player=None, mission="wascity-bbush-get-to-18", **kwargs):
        player = player or self.player
        self.store.register(player, self.token)
        data = replay(seconds, **kwargs)
        data["category"] = mission
        return self.store.upload(player, self.token, encoded(data))

    def query(self, resource="points", **kwargs):
        return json.loads(self.store.leaderboards.query("ghosts", resource, **kwargs))

    def test_ghost_dedupe_ties_groups_pagination_and_no_credentials(self):
        for pid, seconds in ((self.player, 10), ("2" * 32, 10), ("3" * 32, 12)):
            self.upload(seconds, pid)
        self.upload(20)
        self.upload(1, completed=False)
        self.upload(2, truncated=True)
        self.upload(5, mission="arena-training-1")
        result = self.query(limit=1)
        self.assertEqual(result["total"], 3)
        self.assertEqual(result["next_offset"], 1)
        self.assertEqual(result["items"][0]["points"], 200)
        self.assertEqual(result["items"][0]["mission_count"], 2)
        rows = self.query("mission", identifier="wascity-bbush-get-to-18")["items"]
        self.assertEqual([r["place"] for r in rows], [1, 1, 3])
        self.assertEqual([r["points"] for r in rows], [100, 100, 95])
        self.assertEqual([p["rank"] for p in self.query(group="orb")["items"]], [1, 1, 3])
        self.assertEqual(self.query("player", identifier=self.player)["player"]["untied_wr_count"], 1)
        self.assertEqual(self.query(group="main")["total_players"], 3)
        raw = json.dumps(result)
        for secret in (self.token, "token_hash", "last_ping_at", "digest", self.temp.name):
            self.assertNotIn(secret, raw)

    def test_cache_invalidation_upload_rename_delete_not_ping(self):
        record = self.upload(10)
        manager = self.store.leaderboards
        first = manager.query("ghosts", "points")
        snapshot = manager.ghost_snapshot
        self.assertIs(first, manager.query("ghosts", "points"))
        self.store.ping(self.player, self.token)
        self.assertIs(first, manager.query("ghosts", "points"))
        self.assertIs(snapshot, manager.ghost_snapshot)
        self.store.rename(self.player, "New Name")
        second = manager.query("ghosts", "points")
        self.assertNotEqual(first, second)
        self.assertIn(b"New Name", second)
        self.upload(9)
        self.assertNotEqual(second, manager.query("ghosts", "points"))
        # External SQL uses the same revision triggers, even from another connection.
        with sqlite3.connect(self.store.root / "index.sqlite3") as db:
            db.execute("DELETE FROM replays WHERE id=?", (record["id"],))
        db.close()
        before = manager.ghost_snapshot
        manager.query("ghosts", "points")
        self.assertIsNot(before, manager.ghost_snapshot)

    def test_bounded_response_cache_and_source_separation(self):
        self.upload(10)
        manager = self.store.leaderboards
        fetch, _, _ = fake_src()
        manager.speedrun.refresh(fetch)
        src = json.loads(manager.query("speedrun", "points"))
        self.assertEqual(src["total"], 2)
        self.assertNotIn(self.player, {p["player_id"] for p in src["items"]})
        for offset in range(100):
            manager.query("ghosts", "points", offset=offset)
        self.assertLessEqual(len(manager.responses), 64)
        self.assertLessEqual(manager.response_bytes, MAX_CACHE_BYTES)

    def test_src_disk_cache_partial_failure_and_backoff(self):
        cache = self.store.leaderboards.speedrun
        fetch, calls, _ = fake_src()
        cache.refresh(fetch)
        before, raw = cache.snapshot, cache.path.read_bytes()
        reloaded = SpeedrunCache(self.store.root)
        self.assertEqual(before, reloaded.snapshot)
        def partial(path):
            if "/level002/" in path:
                raise HTTPError(path, 429, "Too many", {"Retry-After": "600"}, None)
            return fetch(path)
        cache.refresh(partial)
        self.assertIs(before, cache.snapshot)
        self.assertEqual(raw, cache.path.read_bytes())
        self.assertGreater(cache.next_refresh, time.time() + 590)
        self.assertIsNotNone(cache.status()["message"])
        self.assertFalse(cache.refreshing)
        cache.path.write_text('{"version":1,"game":"'+SRC_GAME+'","category":"'+SRC_CATEGORY+'","data":{"missions":null}}')
        self.assertIsNone(SpeedrunCache(self.store.root).snapshot)

    def test_src_single_flight_refresh(self):
        cache = self.store.leaderboards.speedrun
        fetch, calls, _ = fake_src()
        entered, release = threading.Event(), threading.Event()
        def slow(path):
            entered.set()
            if not release.wait(5):
                raise TimeoutError("Test refresh was not released")
            return fetch(path)
        worker = threading.Thread(target=cache.refresh, args=(slow,))
        worker.start()
        try:
            self.assertTrue(entered.wait(5))
            self.assertTrue(cache.status()["refreshing"])
            cache.refresh(fetch)
            self.assertEqual(calls, [])
        finally:
            release.set()
            worker.join(5)
        self.assertFalse(worker.is_alive())
        self.assertIsNotNone(cache.snapshot)
        self.assertEqual(len(calls), 3)

    def test_cold_cache_errors_invalid_queries_and_static_home(self):
        with self.assertRaises(LeaderboardError) as caught:
            self.store.leaderboards.query("speedrun", "points")
        self.assertEqual(caught.exception.status, 503)
        for query in ("source=no", "limit=0", "limit=101", "offset=-1", "game=jak2", "limit=x", "source=ghosts&source=speedrun", "unknown=x"):
            with self.subTest(query=query), self.assertRaises(LeaderboardError):
                public_route(self.store, "/api/v1/leaderboards/points?source=ghosts&" + query)
        home = public_route(self.store, "/")[1]
        for fragment in (b'href="/admin"', b'id="source"', b'value="speedrun"', b'value="ghosts"', b'/assets/leaderboard-'):
            self.assertIn(fragment, home)
        self.assertNotIn(b"{{", home)
        self.assertEqual(public_route(self.store, "/api")[0], 200)
        with self.assertRaises(LeaderboardError) as error:
            public_route(self.store, "/api/v1/players/no-player?source=ghosts")
        self.assertEqual(error.exception.status, 404)

    def test_etag_cache_security_and_head_both_transports(self):
        self.upload(10)
        target = "/api/v1/leaderboards/points?source=ghosts"
        status, data, kind = public_route(self.store, target)
        status, body, headers = response_parts(status, data, kind, target)
        self.assertEqual(status, 200)
        self.assertIn("max-age=15", headers["Cache-Control"])
        status, body, cached = response_parts(200, data, kind, target, if_none_match='"other", W/'+headers["ETag"])
        self.assertEqual((status, body), (304, b""))
        self.assertNotIn("Content-Length", cached)
        for path in ("/admin/players", "/players/ping", "/selection"):
            self.assertEqual(response_parts(200, {}, kind, path)[2]["Cache-Control"], "no-store")
        self.assertEqual(response_parts(503, {}, kind, target)[2]["Cache-Control"], "no-store")
        self.assertIn("immutable", response_parts(200, b"css", "text/css", next(iter(assets())))[2]["Cache-Control"])
        app = Application(self.store, "https://ghost.example.com")
        def request(method, etag=""):
            result = []
            payload = b"".join(app({"REQUEST_METHOD": method, "PATH_INFO": "/api/v1/leaderboards/points",
                "QUERY_STRING": "source=ghosts", "HTTP_HOST": "ghost.example.com", "HTTP_IF_NONE_MATCH": etag,
                "CONTENT_LENGTH": "0", "wsgi.input": io.BytesIO()}, lambda status, headers: result.append((status, dict(headers)))))
            return result[0], payload
        (status, headers), body = request("GET")
        self.assertEqual(json.loads(body)["total"], 1)
        (status, _), body = request("GET", headers["ETag"])
        self.assertTrue(status.startswith("304"))
        self.assertEqual(body, b"")
        (status, head), body = request("HEAD")
        self.assertTrue(status.startswith("200"))
        self.assertEqual(body, b"")
        self.assertEqual(head["Content-Length"], headers["Content-Length"])
        app.slot.acquire()
        try:
            (status, _), body = request("GET")
            self.assertTrue(status.startswith("200"))
        finally:
            app.slot.release()

    def test_local_http_conditional_head_and_private_auth(self):
        self.upload(10)
        server = Server(("127.0.0.1", 0), self.store)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        base = f"http://127.0.0.1:{server.server_port}"
        target = base + "/api/v1/leaderboards/points?source=ghosts"
        try:
            with urlopen(target, timeout=5) as response:
                raw, etag = response.read(), response.headers["ETag"]
                self.assertEqual(json.loads(raw)["total"], 1)
            with self.assertRaises(HTTPError) as caught:
                urlopen(Request(target, headers={"If-None-Match": etag}), timeout=5)
            try:
                self.assertEqual(caught.exception.code, 304)
            finally:
                caught.exception.close()
            with urlopen(Request(target, method="HEAD"), timeout=5) as response:
                self.assertEqual(response.read(), b"")
                self.assertEqual(int(response.headers["Content-Length"]), len(raw))
            with self.assertRaises(HTTPError) as caught:
                urlopen(base + "/admin/players", timeout=5)
            try:
                self.assertEqual(caught.exception.code, 401)
                self.assertEqual(caught.exception.headers["Cache-Control"], "no-store")
            finally:
                caught.exception.close()
        finally:
            server.shutdown()
            server.server_close()
            worker.join(5)


if __name__ == "__main__":
    unittest.main()
