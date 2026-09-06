"""Pause leaderboard integration against a private loopback fixture, never production."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit


class ClientLeaderboardTests(unittest.TestCase):
    def test_native_paging_cache_errors_and_identity_highlight(self):
        binary = Path(__file__).resolve().parents[2] / "out/build/Release/bin/replay-client-test.exe"
        if not binary.is_file():
            self.skipTest("Build replay-client-test for native integration")
        requests = []
        mission = "wascity-bbush-get-to-18"
        mission_requests = []

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_GET(self):
                requests.append((self.path, self.headers.get("Authorization"), self.headers.get("X-Player-ID")))
                parts = urlsplit(self.path)
                query = parse_qs(parts.query)
                offset = int(query["offset"][0])
                group = query.get("group", ["all"])[0]
                items = [{"place": i + 1, "points": 100 - i, "duration_seconds": 8.9 + i,
                          "player_id": "a" * 32 if i == 9 else f"{i + 1:032x}",
                          "display_name": "You" if i == 9 else "Record Holder" if i == 0 else
                          "Long~Name\nPlayer_Name_That_Exceeds_The_Column" if i == 6 else f"Racer {i + 1}"}
                         for i in range(offset, min(offset + 8, 10))]
                reply = {"api_version": 1, "source": "ghosts", "game": "jak3", "offset": offset,
                         "group": group, "ranked_missions": 2,
                         "total": 10, "mission": {"mission_id": mission, "wr_seconds": 8.9}, "items": items}
                status = 200
                if parts.path == "/api/v1/missions":
                    reply["items"] = [{"mission_id": mission if i == 0 else f"mission-{i}",
                                       "label": "Orb Search 18 (Spargus E)" if i == 0 else f"Mission {i}",
                                       "group": "orb" if i == 0 else "main" if i < 9 else "side",
                                       "player_count": 10 if i == 0 else 1 if i == 8 else 0,
                                       "wr_seconds": 8.9 if i == 0 else 12.0 if i == 8 else None}
                                      for i in range(offset, min(offset + 8, 10))]
                elif parts.path == "/api/v1/leaderboards/points":
                    reply["items"] = [] if group == "side" else [{
                        "player_id": "a" * 32, "display_name": group.title() + " Champion",
                        "rank": 1, "points": 197, "mission_count": 2, "tied_wr_count": 0, "untied_wr_count": 1}]
                    reply["total"] = len(reply["items"])
                elif parts.path.endswith("mission-1/leaderboard"):
                    reply.update(total=0, items=[], mission={"mission_id": "mission-1", "wr_seconds": None})
                elif parts.path.endswith("mission-8/leaderboard"):
                    reply.update(total=1, items=[{**items[0], "duration_seconds": 12.0}],
                                 mission={"mission_id": "mission-8", "wr_seconds": 12.0})
                elif parts.path == f"/api/v1/missions/{mission}/leaderboard":
                    mission_requests.append(self.path)
                    if len(mission_requests) == 3:
                        status = 503
                    if len(mission_requests) == 4:
                        reply["items"][1]["duration_seconds"] = -1 # reject even after a valid first row
                else:
                    status = 404
                raw = json.dumps(reply).encode()
                time.sleep(0.05)
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(raw)))
                self.end_headers()
                self.wfile.write(raw)

        with tempfile.TemporaryDirectory(prefix="ghost-board-") as profile:
            with ThreadingHTTPServer(("127.0.0.1", 0), Handler) as server:
                thread = threading.Thread(target=server.serve_forever, daemon=True)
                thread.start()
                try:
                    features = Path(profile) / "OpenGOAL/jak3/features"
                    features.mkdir(parents=True)
                    (features / "ghost-client.json").write_text(json.dumps({
                        "player_id": "a" * 32, "player_token": "b" * 64,
                        "server": f"http://127.0.0.1:{server.server_port}",
                        "mode": 0, "submit_completed": False, "custom": {},
                    }), encoding="utf-8")
                    result = subprocess.run([str(binary),
                        "--gtest_filter=ReplayClient.InventoryLeaderboardIsPagedCachedAndReadOnly"],
                        env={**os.environ, "OG_LEADERBOARD_TEST_PROFILE": profile},
                        capture_output=True, text=True, timeout=35)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertIn("[  PASSED  ] 1 test.", result.stdout)
                    path = f"/api/v1/missions/{mission}/leaderboard?source=ghosts&game=jak3&group=all&limit=8&offset="
                    self.assertEqual(mission_requests, [path + str(offset) for offset in (0, 8, 0, 0)])
                    self.assertEqual(len(requests), 12, requests)
                    self.assertTrue(all(auth is None and player is None for _, auth, player in requests))
                    self.assertEqual(sum(urlsplit(path).path == "/api/v1/missions" for path, _, _ in requests), 2)
                    self.assertEqual({parse_qs(urlsplit(path).query)["group"][0] for path, _, _ in requests
                                      if urlsplit(path).path.endswith("/points")}, {"all", "main", "orb", "side"})
                finally:
                    server.shutdown()
                    thread.join()
