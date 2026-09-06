"""Native client integration test, using only an ephemeral loopback server/profile."""

import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class ClientIdentityTests(unittest.TestCase):
    def test_null_custom_selection_regression(self):
        binary = Path(__file__).resolve().parents[2] / "out/build/Release/bin/replay-client-test.exe"
        if not binary.is_file():
            self.skipTest("Build replay-client-test to run the native loopback integration test")
        requests = []

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_GET(self):
                requests.append(self.path)
                response = json.dumps({"replays": [{
                    "id": "c" * 32, "display_name": "Test Player",
                    "duration_seconds": 1.0, "completed": True,
                }], "next_offset": None}).encode()
                self.send_response(200)
                self.send_header("Content-Length", str(len(response)))
                self.end_headers()
                self.wfile.write(response)

        with ThreadingHTTPServer(("127.0.0.1", 0), Handler) as server:
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            try:
                with tempfile.TemporaryDirectory(prefix="ghost-null-selection-") as profile:
                    features = Path(profile) / "OpenGOAL/jak3/features"
                    features.mkdir(parents=True)
                    (features / "ghost-client.json").write_text(json.dumps({
                        "player_id": "a" * 32, "player_token": "b" * 64,
                        "server": f"http://127.0.0.1:{server.server_port}",
                        "mode": 4, "submit_completed": False,
                        "custom": {"wascity-bbush-get-to-18": None, "bad-entry": 7,
                                   "valid-entry": [None, 42, "bad-id", "d" * 32, "d" * 32]},
                        "custom_by_server": {"http://127.0.0.1:8765": {"legacy-mission": None}},
                    }), encoding="utf-8")
                    result = subprocess.run([
                        str(binary), "--gtest_filter=ReplayClient.NullCustomSelectionsAreSafeAndMenuReadsDoNotMutateSettings",
                    ], env={**os.environ, "OG_REPLAY_TEST_PROFILE": profile},
                        capture_output=True, text=True, timeout=20)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(requests, [
                    "/replays?game=jak3&category=wascity-bbush-get-to-18&offset=0",
                    "/replays?game=jak3&category=previously-unseen-mission&offset=0",
                ])
            finally:
                server.shutdown()
                thread.join()

    def test_boot_lookup_and_manual_refresh(self):
        binary = Path(__file__).resolve().parents[2] / "out/build/Release/bin/replay-client-test.exe"
        if not binary.is_file():
            self.skipTest("Build replay-client-test to run the native loopback integration test")
        requests = []
        player, token = "a" * 32, "b" * 64

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_POST(self):
                body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                requests.append((self.path, body))
                status = 503 if len(requests) == 2 else 200
                response = json.dumps({
                    "player_id": player,
                    "display_name": "Zed" if len(requests) == 1 else "New~Name",
                    "identified": True,
                }).encode()
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(response)))
                self.end_headers()
                self.wfile.write(response)

        with ThreadingHTTPServer(("127.0.0.1", 0), Handler) as server:
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            try:
                with tempfile.TemporaryDirectory(prefix="ghost-client-identity-") as profile:
                    features = Path(profile) / "OpenGOAL/jak3/features"
                    features.mkdir(parents=True)
                    (features / "ghost-client.json").write_text(json.dumps({
                        "player_id": player, "player_token": token,
                        "server": f"http://127.0.0.1:{server.server_port}",
                        "mode": 0, "submit_completed": False, "custom": {},
                    }), encoding="utf-8")
                    result = subprocess.run(
                        [str(binary), "--gtest_filter=ReplayClient.BootIdentityFromSelectedServer"],
                        env={**os.environ, "OG_REPLAY_TEST_PROFILE": profile},
                        capture_output=True, text=True, timeout=30,
                    )
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(requests, [("/players/ping", {
                    "player_id": player, "token": token,
                })] * 3)
            finally:
                server.shutdown()
                thread.join()


class ClientTwoFasterTests(unittest.TestCase):
    def test_two_opponent_default_and_saved_legacy_mode(self):
        from server import Server, Store
        from test_server import replay
        binary = Path(__file__).resolve().parents[2] / "out/build/Release/bin/replay-client-test.exe"
        if not binary.is_file():
            self.skipTest("Build replay-client-test for the native two-opponent integration test")
        cases = [("second", 20, None, [10, 30], [10]),
                 ("first", 5, None, [10, 30], [5]),
                 ("no-pb", None, None, [30, 40], [40]),
                 ("better-local", 20, 8, [10, 30], [8])]
        for label, own, local, two, single in cases:
            with self.subTest(case=label), tempfile.TemporaryDirectory(prefix="ghost-two-faster-") as root:
                store = Store(Path(root) / "server")
                try:
                    for pid, seconds in (("1" * 32, own), ("2" * 32, 10), ("3" * 32, 30), ("4" * 32, 40)):
                        if seconds is not None:
                            store.register(pid, "a" * 64)
                            store.upload(pid, "a" * 64, json.dumps(replay(seconds)).encode())
                    with Server(("127.0.0.1", 0), store) as server:
                        thread = threading.Thread(target=server.serve_forever)
                        thread.start()
                        try:
                            profile = Path(root) / "profile"
                            features = profile / "OpenGOAL/jak3/features"
                            features.mkdir(parents=True)
                            (features / "ghost-client.json").write_text(json.dumps({
                                "player_id": "1" * 32, "player_token": "a" * 64,
                                "server": f"http://127.0.0.1:{server.server_port}", "mode": 5,
                                "submit_completed": False, "custom": {},
                            }), encoding="utf-8")
                            if local is not None:
                                local_path = features / "replays" / replay()["category"]
                                local_path.mkdir(parents=True)
                                (local_path / "best-completed.ogr.json").write_text(json.dumps(replay(local)))
                            result = subprocess.run(
                                [str(binary), "--gtest_filter=ReplayClient.TwoFasterDefaultAndLegacySingleSelection"],
                                env={**os.environ, "OG_REPLAY_TEST_PROFILE": str(profile),
                                     "OG_REPLAY_EXPECTED_TWO": json.dumps(two), "OG_REPLAY_EXPECTED_SINGLE": json.dumps(single)},
                                capture_output=True, text=True, timeout=40)
                            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                            self.assertIn("[  PASSED  ] 1 test.", result.stdout)  # don't silently pass an outdated binary
                        finally:
                            server.shutdown()
                            thread.join()
                finally:
                    store.close()


if __name__ == "__main__":
    unittest.main()
