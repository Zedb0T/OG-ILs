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


if __name__ == "__main__":
    unittest.main()
