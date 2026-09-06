"""Real native client restarts with isolated profiles; never production HTTP."""
import copy
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


class LeaderboardDiskTests(unittest.TestCase):
    def test_restart_offline_recovery_validation_and_bounded_cache(self):
        binary = Path(__file__).resolve().parents[2] / "out/build/Release/bin/replay-client-test.exe"
        if not binary.is_file():
            self.skipTest("Build replay-client-test for native integration")
        state = {"mode": "populate", "requests": []}

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_GET(self):
                state["requests"].append((self.path, self.headers.get("Authorization"), self.headers.get("X-Player-ID")))
                mode = state["mode"]
                if mode == "offline" and len(state["requests"]) == 1:
                    time.sleep(2) # cache must appear before this request completes
                parts = urlsplit(self.path)
                offset = int(parse_qs(parts.query)["offset"][0])
                live = mode == "updated"
                name, seconds = ("Live Runner", 6.5) if live else ("Disk Runner", 7.5)
                reply = {"api_version": 1, "game": "jak3", "source": "ghosts", "group": "all",
                         "offset": offset, "ranked_missions": 2, "total": 1, "unexpected": "must-not-persist"}
                if parts.path == "/api/v1/leaderboards/points":
                    reply["total"] = 4800 if mode == "evict" else 1
                    reply["items"] = [{"rank": i + 1, "player_id": "a" * 32 if i == 0 else f"{i:032x}",
                                       "display_name": name, "points": 199 if live else 197, "mission_count": 2,
                                       "tied_wr_count": 0, "untied_wr_count": 1, "Authorization": "must-not-persist"}
                                      for i in range(offset, min(offset + 8, reply["total"]))]
                elif parts.path == "/api/v1/missions":
                    reply["items"] = [{"mission_id": "disk-mission", "label": "Disk Mission", "group": "orb",
                                       "player_count": 1, "wr_seconds": seconds}]
                elif parts.path == "/api/v1/missions/disk-mission/leaderboard":
                    reply["mission"] = {"mission_id": "disk-mission", "wr_seconds": seconds}
                    reply["items"] = [{"place": 1, "player_id": "a" * 32, "display_name": name,
                                       "points": 100, "duration_seconds": seconds}]
                else:
                    self.send_error(404)
                    return
                raw = json.dumps(reply).encode()
                self.send_response(503 if mode in ("offline", "rejected") else 200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(raw)))
                self.end_headers()
                self.wfile.write(raw)

        with tempfile.TemporaryDirectory(prefix="ghost-board-disk-") as profile:
            with ThreadingHTTPServer(("127.0.0.1", 0), Handler) as server:
                thread = threading.Thread(target=server.serve_forever, daemon=True)
                thread.start()
                try:
                    features = Path(profile) / "OpenGOAL/jak3/features"
                    features.mkdir(parents=True)
                    origin = f"http://127.0.0.1:{server.server_port}"
                    settings = features / "ghost-client.json"
                    config = {"player_id": "a" * 32, "player_token": "b" * 64, "server": origin,
                              "mode": 0, "submit_completed": False, "custom": {}}
                    settings.write_text(json.dumps(config), encoding="utf-8")
                    cache = features / "ghost-cache/servers" / origin.encode().hex() / "leaderboards-v1.json"

                    def run(mode):
                        state.update(mode=mode, requests=[])
                        before = settings.read_bytes()
                        result = subprocess.run([str(binary), "--gtest_filter=ReplayClient.LeaderboardDiskCacheAcrossSessions"],
                            env={**os.environ, "OG_LEADERBOARD_DISK_PROFILE": profile, "OG_LEADERBOARD_DISK_MODE": mode},
                            capture_output=True, text=True, timeout=20)
                        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                        self.assertIn("[  PASSED  ] 1 test.", result.stdout)
                        self.assertEqual(settings.read_bytes(), before)
                        self.assertTrue(all(auth is None and player is None for _, auth, player in state["requests"]))

                    run("populate")
                    saved = json.loads(cache.read_bytes())
                    self.assertEqual(len(saved["pages"]), 3) # points, catalog, mission
                    self.assertLess(cache.stat().st_size, 4 * 1024 * 1024)
                    for forbidden in ("player_token", "Authorization", "must-not-persist", "b" * 64, '"own"'):
                        self.assertNotIn(forbidden, cache.read_text())
                    self.assertFalse(cache.with_suffix(".json.tmp").exists())
                    for page in saved["pages"]:
                        page["fetched_at"] = int(time.time()) - 2 * 3600
                    original = json.dumps(saved).encode()
                    cache.write_bytes(original)
                    config["player_id"] = "c" * 32 # YOU must be recomputed, not loaded from disk
                    settings.write_text(json.dumps(config), encoding="utf-8")
                    cache.with_suffix(".json.tmp").write_text('{"interrupted":')
                    run("offline")
                    self.assertEqual(len(state["requests"]), 3)
                    self.assertEqual(cache.read_bytes(), original) # failed GETs never overwrite last-good

                    # Invalid envelopes, rows and unbounded snapshots are ignored safely.
                    invalid = [b'{"version":', b"x" * (4 * 1024 * 1024 + 1)]
                    for key, value in (("version", 99), ("server", "https://other.example"),
                                       ("source", "speedrun"), ("game", "jak1"), ("page_size", 100)):
                        damaged = copy.deepcopy(saved)
                        damaged[key] = value
                        invalid.append(json.dumps(damaged).encode())
                    for field, value in (("fetched_at", int(time.time()) + 86400), ("fetched_at", -1)):
                        damaged = copy.deepcopy(saved)
                        damaged["pages"][0][field] = value
                        invalid.append(json.dumps(damaged).encode())
                    damaged = copy.deepcopy(saved)
                    damaged["pages"][0]["location"]["group"] = 999
                    invalid.append(json.dumps(damaged).encode())
                    damaged = copy.deepcopy(saved)
                    damaged["pages"] = [damaged["pages"][0]] * 513
                    invalid.append(json.dumps(damaged).encode())
                    damaged = copy.deepcopy(saved)
                    damaged["pages"].append(damaged["pages"][0])
                    invalid.append(json.dumps(damaged).encode())
                    damaged = copy.deepcopy(saved)
                    damaged["pages"][-1]["data"]["items"][0]["duration_seconds"] = -1
                    invalid.append(json.dumps(damaged).encode())
                    for index, raw in enumerate(invalid):
                        with self.subTest(corrupt_snapshot=index):
                            cache.write_bytes(raw)
                            run("rejected")
                            self.assertEqual(cache.read_bytes(), raw)

                    cache.write_bytes(original)
                    # Failed atomic save must keep old file, while showing fresh HTTP data.
                    temporary = cache.with_suffix(".json.tmp")
                    temporary.unlink()
                    temporary.mkdir()
                    run("updated")
                    self.assertEqual(cache.read_bytes(), original)
                    temporary.rmdir()
                    run("updated")
                    updated = json.loads(cache.read_bytes())
                    self.assertIn("Live Runner", cache.read_text())
                    self.assertGreater(updated["pages"][0]["fetched_at"], saved["pages"][0]["fetched_at"])

                    # Start at capacity without hundreds of HTTP calls or disk rewrites.
                    capacity = copy.deepcopy(saved)
                    template = copy.deepcopy(saved["pages"][0])
                    capacity["pages"] = []
                    for page in range(512):
                        entry = copy.deepcopy(template)
                        entry["location"]["page"] = page
                        entry["data"].update(offset=page * 8, total=4800, items=[{
                            "rank": i + 1, "player_id": f"{i:032x}", "display_name": "Disk Runner",
                            "points": 197, "mission_count": 2, "tied_wr_count": 0, "untied_wr_count": 1}
                            for i in range(page * 8, page * 8 + 8)])
                        capacity["pages"].append(entry)
                    cache.write_text(json.dumps(capacity))
                    run("evict")
                    bounded = json.loads(cache.read_bytes())
                    pages = {page["location"]["page"] for page in bounded["pages"]}
                    self.assertEqual(len(pages), 512)
                    self.assertIn(0, pages) # recently revisited page survives LRU eviction
                    self.assertTrue(all(page not in pages for page in range(1, 5)))
                    self.assertLess(cache.stat().st_size, 4 * 1024 * 1024)
                finally:
                    server.shutdown()
                    thread.join()
