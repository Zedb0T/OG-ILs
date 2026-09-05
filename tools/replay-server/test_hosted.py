import base64
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from hosted import Application, BODY_LIMIT
from server import Store
from test_server import replay
import start_hosted


class HostedTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.store = Store(self.temp.name)
        self.app = Application(self.store, "https://ghost.example.com")

    def tearDown(self):
        self.store.close()
        self.temp.cleanup()

    def request(self, path, body=None, **headers):
        raw = json.dumps(body).encode() if body is not None else b""
        env = {"REQUEST_METHOD": "POST" if body is not None else "GET", "PATH_INFO": path,
               "HTTP_HOST": "ghost.example.com", "CONTENT_LENGTH": str(len(raw)),
               "wsgi.input": io.BytesIO(raw), **headers}
        result = []
        payload = b"".join(self.app(env, lambda status, headers: result.append((status, headers))))
        return int(result[0][0][:3]), payload

    def test_health_host_origin(self):
        self.assertEqual(self.request("/health")[0], 200)
        self.assertEqual(self.request("/health", HTTP_HOST="evil.example")[0], 400)
        self.assertEqual(self.request("/health", HTTP_ORIGIN="https://evil.example")[0], 400)
        with self.assertRaises(ValueError):
            Application(self.store, "http://ghost.example.com")

    def test_upload_download_and_admin(self):
        player, token = "a" * 32, "b" * 64
        self.assertEqual(self.request("/players", {"player_id": player, "token": token})[0], 200)
        status, result = self.request("/replays", replay(), HTTP_X_PLAYER_ID=player, HTTP_AUTHORIZATION="Bearer " + token)
        self.assertEqual(status, 201)
        replay_id = json.loads(result)["id"]
        self.assertEqual(json.loads(self.request("/replays/" + replay_id)[1]), replay())
        self.assertEqual(self.request("/admin/players")[0], 401)
        auth = "Basic " + base64.b64encode(b"user:pass").decode()
        self.assertEqual(self.request("/admin/players", HTTP_AUTHORIZATION=auth)[0], 200)
        self.assertEqual(self.request("/admin/players/" + player, {"display_name": "Test"}, HTTP_AUTHORIZATION=auth)[0], 200)

    def test_bounds_and_busy_health(self):
        self.store.register("a" * 32, "b" * 64)
        self.assertEqual(self.request("/replays", {}, CONTENT_LENGTH=str(BODY_LIMIT + 1),
                                     HTTP_X_PLAYER_ID="a" * 32, HTTP_AUTHORIZATION="Bearer " + "b" * 64)[0], 413)
        self.app.disk_budget = 1
        self.assertEqual(self.request("/players", {})[0], 507)
        self.app.slot.acquire()
        try:
            self.assertEqual(self.request("/replays")[0], 503)
            self.assertEqual(self.request("/health")[0], 200)
        finally:
            self.app.slot.release()

    def test_hosted_ping_and_admin_timestamp(self):
        status, raw = self.request("/players/ping", {"player_id": "a" * 32, "token": "b" * 64})
        self.assertEqual(status, 200)
        ping = json.loads(raw)
        auth = "Basic " + base64.b64encode(b"user:pass").decode()
        status, raw = self.request("/admin/players", HTTP_AUTHORIZATION=auth)
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(raw)["players"][0]["last_ping_at"], ping["last_ping_at"])
        self.assertEqual(self.request("/players/ping", {"player_id": "a" * 32, "token": "c" * 64})[0], 403)

    def test_full_checkout_update_and_safe_fallback(self):
        with tempfile.TemporaryDirectory() as root:
            (Path(root) / "repo").mkdir()
            commands = []
            def git(_, *args):
                commands.append(args)
                if args[:2] == ("remote", "get-url"):
                    return start_hosted.REPO
                if args[0] == "rev-parse":
                    return "a" * 40
                return ""
            with patch.object(start_hosted, "git", side_effect=git), patch.object(start_hosted, "validate") as validate:
                start_hosted.update(Path(root))
                self.assertIn(("fetch", "origin", start_hosted.BRANCH), commands)
                self.assertIn(("merge", "--ff-only", "FETCH_HEAD"), commands)
                validate.side_effect = [ValueError("bad update"), None]
                start_hosted.update(Path(root))
                self.assertIn(("switch", "--detach", "a" * 40), commands)
                self.assertFalse(any("--hard" in c or "clean" in c for c in commands))


if __name__ == "__main__":
    unittest.main()
