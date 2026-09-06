"""Paced native prefetch, restart reuse, full catalog and foreground preemption."""
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


class LeaderboardWarmTests(unittest.TestCase):
    def test_fill_resume_pacing_preemption_and_failure_backoff(self):
        binary = Path(__file__).resolve().parents[2] / "out/build/Release/bin/replay-client-test.exe"
        if not binary.is_file():
            self.skipTest("Build replay-client-test for native integration")
        state = {"mode": "fill", "requests": []}

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_GET(self):
                parts = urlsplit(self.path)
                query = parse_qs(parts.query)
                offset, limit = int(query["offset"][0]), int(query["limit"][0])
                mode = state["mode"]
                state["requests"].append((self.path, time.monotonic(), self.headers.get("Authorization"), self.headers.get("X-Player-ID")))
                if mode == "interrupt" and limit == 96:
                    time.sleep(5)
                group = query["group"][0]
                reply = {"api_version": 1, "source": "ghosts", "game": "jak3", "group": group,
                         "offset": offset, "ranked_missions": 1, "total": 10}
                if parts.path == "/api/v1/missions":
                    reply["total"] = 110
                    reply["items"] = [{"mission_id": f"warm-mission-{i}", "label": f"Warm Mission {i}", "group": "orb",
                                       "player_count": 10 if i == 0 else 0, "wr_seconds": 7.5 if i == 0 else None}
                                      for i in range(offset, min(offset + limit, 110))]
                elif parts.path == "/api/v1/leaderboards/points":
                    reply["total"] = 10 if group in ("all", "orb") else 0
                    reply["items"] = [{"rank": i + 1, "player_id": f"{i:032x}", "display_name": f"Warm Racer {i + 1}",
                                       "points": 100 - i, "mission_count": 1, "tied_wr_count": 0,
                                       "untied_wr_count": 1 if i == 0 else 0}
                                      for i in range(offset, min(offset + limit, reply["total"]))]
                    if mode == "badbatch" and len(reply["items"]) > 8:
                        reply["items"][-1]["points"] = -1
                elif parts.path == "/api/v1/missions/warm-mission-0/leaderboard":
                    reply["mission"] = {"mission_id": "warm-mission-0", "wr_seconds": 7.5}
                    reply["items"] = [{"place": i + 1, "player_id": f"{i:032x}", "display_name": f"Warm Racer {i + 1}",
                                       "duration_seconds": 7.5 + i, "points": 100 - i}
                                      for i in range(offset, min(offset + limit, 10))]
                else:
                    self.send_error(404)
                    return
                raw = json.dumps(reply).encode()
                self.send_response(503 if mode in ("resume", "failure") or (mode == "badbatch" and limit == 8) else 200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(raw)))
                self.end_headers()
                try:
                    self.wfile.write(raw)
                except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                    pass # expected: foreground work canceled the background request

        with tempfile.TemporaryDirectory(prefix="ghost-board-warm-") as profile:
            with ThreadingHTTPServer(("127.0.0.1", 0), Handler) as server:
                thread = threading.Thread(target=server.serve_forever, daemon=True)
                thread.start()
                try:
                    origin = f"http://127.0.0.1:{server.server_port}"
                    features = Path(profile) / "OpenGOAL/jak3/features"
                    features.mkdir(parents=True)
                    settings = features / "ghost-client.json"
                    settings.write_text(json.dumps({"player_id": "a" * 32, "player_token": "b" * 64,
                        "server": origin, "mode": 0, "submit_completed": False, "custom": {}}))
                    cache = features / "ghost-cache/servers" / origin.encode().hex() / "leaderboards-v1.json"

                    def run(mode):
                        state.update(mode=mode, requests=[])
                        before = settings.read_bytes()
                        result = subprocess.run([str(binary), "--gtest_filter=ReplayClient.LeaderboardWarmsWithoutOpeningMenu"],
                            env={**os.environ, "OG_LEADERBOARD_WARM_PROFILE": profile, "OG_LEADERBOARD_WARM_MODE": mode},
                            capture_output=True, text=True, timeout=25)
                        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                        self.assertIn("[  PASSED  ] 1 test.", result.stdout)
                        self.assertEqual(settings.read_bytes(), before)
                        self.assertTrue(all(auth is None and player is None for _, _, auth, player in state["requests"]))

                    run("fill")
                    self.assertEqual(len(state["requests"]), 7) # 4 groups + 2 catalog batches + 1 populated mission
                    self.assertTrue(all("limit=96" in path for path, *_ in state["requests"]))
                    stamps = [stamp for _, stamp, *_ in state["requests"]]
                    self.assertTrue(all(b - a >= 0.9 for a, b in zip(stamps, stamps[1:])))
                    snapshot = json.loads(cache.read_bytes())
                    self.assertEqual(len(snapshot["pages"]), 131) # more than the old 32-page limit
                    self.assertEqual(sum(p["location"]["screen"] == 3 for p in snapshot["pages"]), 111)
                    before = cache.read_bytes()
                    run("resume")
                    self.assertEqual(state["requests"], []) # complete recent cache: no startup refetch
                    self.assertEqual(cache.read_bytes(), before)

                    cache.unlink()
                    run("interrupt")
                    self.assertEqual(len(state["requests"]), 2)
                    self.assertIn("limit=96", state["requests"][0][0])
                    self.assertIn("/api/v1/missions?", state["requests"][1][0])
                    self.assertIn("limit=8", state["requests"][1][0])
                    self.assertLess(state["requests"][1][1] - state["requests"][0][1], 3)
                    for mode in ("failure", "badbatch"):
                        if cache.exists():
                            cache.unlink()
                        run(mode)
                        background = [path for path, *_ in state["requests"] if "limit=96" in path]
                        self.assertEqual(len(background), 1) # backed off instead of retrying every idle tick
                finally:
                    server.shutdown()
                    thread.join()
