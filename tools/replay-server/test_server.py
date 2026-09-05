import copy
import base64
import json
from pathlib import Path
import tempfile
import threading
import unittest
from concurrent.futures import ThreadPoolExecutor
from urllib.error import HTTPError
from urllib.request import Request, urlopen

from server import APIError, MAX_BYTES, Server, Store, validate_replay


def replay(seconds=10, completed=True, truncated=False, version=3):
    def sample(t):
        base = [t, [1, 2, 3], [0, 0, 0, 1], [0, 0, 0], 0]
        if version >= 2:
            base += [1]
        base += ["board", "board-ride"]
        if version >= 3:
            base += ["board", [1, 1, 3], [0, 0, 0, 1], [0.5, 0.5, 0.5, 1], 1, "board-open"]
        return base
    return {"schema": "opengoal-replay", "version": version, "game": "jak3",
            "category": "desert-bbush-get-to-19", "completed": completed, "truncated": truncated,
            "duration_seconds": seconds, "sample_rate_hz": 60, "sample_count": 2,
            "units": "meters", "samples": [sample(0), sample(seconds)]}


class StoreTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.store = Store(self.root)
        self.player = "1" * 32
        self.token = "a" * 64
        self.store.register(self.player, self.token)

    def tearDown(self):
        self.store.close()
        self.temp.cleanup()

    def upload(self, seconds=10, player=None, **kwargs):
        player = player or self.player
        self.store.register(player, self.token)
        return self.store.upload(player, self.token, json.dumps(replay(seconds, **kwargs)).encode())

    def test_versions_roundtrip_board_scale(self):
        for version in (1, 2, 3):
            original = replay(version=version)
            info = self.upload(version=version)
            self.assertEqual(json.loads(self.store.download(info["id"])), original)

    def test_identity_auth_and_admin_rename(self):
        first = self.upload()
        self.assertEqual(first["display_name"], "Unknown-11111111")
        with self.assertRaises(APIError):
            self.store.register(self.player, "b" * 64)
        with self.assertRaises(APIError):
            self.store.upload(self.player, "b" * 64, json.dumps(replay()).encode())
        self.store.rename(self.player, "Zed")
        self.assertEqual(self.store.metadata(first["id"])["display_name"], "Zed")
        self.assertEqual(len(list(self.root.rglob("Unknown*.ogr.json"))), 0)
        self.assertEqual(json.loads(self.store.download(first["id"])), replay())
        second = self.upload(9)
        paths = list((self.root / "jak3" / replay()["category"]).glob("Zed_9.000s_*.ogr.json"))
        self.assertEqual(len(paths), 1)
        self.assertNotEqual(first["id"], second["id"])

    def test_restart_preserves_identity_ids_and_payloads(self):
        first = self.upload()
        self.store.rename(self.player, "Zed")
        self.store.close()
        self.store = Store(self.root)
        self.assertTrue(self.store.authenticate_admin("Basic " + base64.b64encode(b"user:pass").decode()))
        self.assertEqual(self.store.metadata(first["id"])["display_name"], "Zed")
        self.assertEqual(self.upload()["id"], first["id"])

    def test_concurrent_duplicate_upload_is_idempotent(self):
        with ThreadPoolExecutor(max_workers=8) as pool:
            results = list(pool.map(lambda _: self.upload(), range(16)))
        self.assertEqual(len({r["id"] for r in results}), 1)
        self.assertEqual(len(list(self.root.rglob("*.ogr.json"))), 1)

    def test_interrupted_rename_recovery(self):
        info = self.upload()
        path = next(self.root.rglob("*.ogr.json"))
        path.rename(path.with_name("Renamed_" + path.name))
        self.store.close()
        self.store = Store(self.root)
        self.assertEqual(json.loads(self.store.download(info["id"])), replay())

    def test_default_slowest_then_next_faster_player_best(self):
        slow = self.upload(30, "2" * 32)
        self.upload(35, "2" * 32)  # older slower run must not occupy a rank
        medium = self.upload(20, "3" * 32)
        fast = self.upload(10, "4" * 32)
        cat = replay()["category"]
        pick = lambda best: self.store.selection(cat, "default", self.player, best)["replays"]
        self.assertEqual(pick(None)[0]["id"], slow["id"])
        self.assertEqual(pick(25)[0]["id"], medium["id"])
        self.assertEqual(pick(20)[0]["id"], fast["id"])
        self.assertEqual(pick(9), [])  # game falls back to its better local PB
        self.assertEqual(self.store.selection(cat, "wr", self.player)["replays"][0]["id"], fast["id"])

    def test_unfinished_and_truncated_are_custom_only(self):
        self.upload(1, completed=False)
        self.upload(2, truncated=True)
        cat = replay()["category"]
        self.assertEqual(self.store.selection(cat, "wr", self.player)["replays"], [])
        self.assertEqual(len(self.store.catalog("jak3", cat)["replays"]), 2)

    def test_own_best_and_empty_leaderboard(self):
        cat = replay()["category"]
        self.assertEqual(self.store.selection(cat, "default", self.player)["replays"], [])
        own = self.upload(10)
        self.upload(20, "2" * 32)
        self.assertEqual(self.store.selection(cat, "default", self.player)["replays"][0]["id"], own["id"])

    def test_catalog_pagination(self):
        for time in (3, 1, 2):
            self.upload(time)
        cat = replay()["category"]
        one = self.store.catalog("jak3", cat, limit=2)
        two = self.store.catalog("jak3", cat, offset=one["next_offset"], limit=2)
        self.assertEqual([r["duration_seconds"] for r in one["replays"] + two["replays"]], [1, 2, 3])
        self.assertIsNone(two["next_offset"])

    def test_validation_rejects_bad_schema_paths_counts_numbers(self):
        bad = []
        for key, value in [("category", "../escape"), ("category", "x/y"), ("game", "jak1"),
                           ("version", True), ("version", 4), ("sample_count", 3),
                           ("duration_seconds", float("nan")), ("duration_seconds", 8),
                           ("completed", 1)]:
            item = replay()
            item[key] = value
            bad.append(item)
        item = replay(); item["samples"][0][1][0] = float("inf"); bad.append(item)
        item = replay(); item["samples"][0][6] = "x" * 48; bad.append(item)
        item = replay(); item["samples"][0][0] = 20; bad.append(item)
        item = replay(); item["samples"][0][8] = "board\u0000evil"; bad.append(item)
        for item in bad:
            with self.subTest(item=item), self.assertRaises(APIError):
                validate_replay(item)

    def test_no_credentials_in_public_metadata(self):
        info = self.upload()
        public = json.dumps(info) + json.dumps(self.store.players())
        self.assertNotIn(self.token, public)
        self.assertNotIn("token_hash", public)
        self.assertNotIn("path", info)


class HTTPTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.store = Store(self.temp.name)
        self.server = Server(("127.0.0.1", 0), self.store)
        self.thread = threading.Thread(target=self.server.serve_forever)
        self.thread.start()
        self.url = f"http://127.0.0.1:{self.server.server_port}"

    def tearDown(self):
        self.server.shutdown()
        self.thread.join()
        self.server.server_close()
        self.store.close()
        self.temp.cleanup()

    def call(self, path, data=None, headers=None):
        req = Request(self.url + path, data=json.dumps(data).encode() if data is not None else None, headers=headers or {})
        try:
            with urlopen(req, timeout=5) as response:
                return response.status, json.load(response)
        except HTTPError as error:
            with error:
                return error.code, json.load(error)

    def test_end_to_end_register_submit_list_retrieve_and_rename(self):
        player, token = "2" * 32, "f" * 64
        self.assertEqual(self.call("/players", {"player_id": player, "token": token})[0], 200)
        status, upload = self.call("/replays", replay(), {"X-Player-ID": player, "Authorization": "Bearer " + token})
        self.assertEqual(status, 201)
        self.assertEqual(self.call(upload["download"])[1], replay())
        self.assertEqual(self.call("/admin/players")[0], 401)
        auth = {"Authorization": "Basic " + base64.b64encode(b"user:pass").decode()}
        self.assertEqual(self.call("/admin/players/" + player, {"display_name": "Tester"}, auth)[0], 200)
        listing = self.call("/replays?category=" + replay()["category"])[1]
        self.assertEqual(listing["replays"][0]["display_name"], "Tester")

    def test_admin_password_and_legacy_token_rejection(self):
        for credential in ("wrong:pass", "user:wrong", "user:", "user", "usér:pass"):
            auth = {"Authorization": "Basic " + base64.b64encode(credential.encode()).decode()}
            self.assertEqual(self.call("/admin/players", headers=auth)[0], 401)
            self.assertEqual(self.call("/admin/players/" + "2" * 32, {"display_name": "Wrong"}, auth)[0], 401)
        for header in ("Bearer old-admin-token", "Basic !!!", "Basic /w=="):
            self.assertEqual(self.call("/admin/players", headers={"Authorization": header})[0], 401)

    def test_admin_page_uses_login_form(self):
        with urlopen(self.url + "/admin", timeout=5) as response:
            page = response.read().decode()
        self.assertIn('id="username"', page)
        self.assertIn('id="password"', page)
        self.assertIn('event.preventDefault()', page)
        self.assertNotIn("admin-token.txt", page)

    def test_admin_credentials_can_be_overridden(self):
        self.store.admin_user = "tester"
        self.store.admin_password = "different:password"
        good = {"Authorization": "Basic " + base64.b64encode(b"tester:different:password").decode()}
        old = {"Authorization": "Basic " + base64.b64encode(b"user:pass").decode()}
        self.assertEqual(self.call("/admin/players", headers=good)[0], 200)
        self.assertEqual(self.call("/admin/players", headers=old)[0], 401)

    def test_host_origin_auth_and_malformed_requests(self):
        self.assertEqual(self.call("/health")[0], 200)
        self.assertEqual(self.call("/health", headers={"Host": "evil.invalid"})[0], 400)
        self.assertEqual(self.call("/players", {}, {"Origin": "https://evil.invalid"})[0], 400)
        self.assertEqual(self.call("/replays", replay())[0], 403)
        self.assertEqual(self.call("/selection?category=x&mode=wr&best_seconds=nan")[0], 400)
        self.assertEqual(self.call("/players", [1])[0], 400)
        self.assertEqual(self.call("/players", {}, {"Content-Length": str(MAX_BYTES + 1)})[0], 413)


if __name__ == "__main__":
    unittest.main()
