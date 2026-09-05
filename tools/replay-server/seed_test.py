"""Create isolated integration fixtures, never read/write the real user profile."""
import copy
import json
from pathlib import Path
import secrets
import shutil

from server import Store, atomic_write


def main():
    root = Path(__file__).resolve().parents[2]
    source = root / "out/ghost-stress/profile/OpenGOAL"
    destination = root / "out/ghost-service/profile/OpenGOAL"
    if not destination.exists():
        shutil.copytree(source, destination)
    category = "desert-bbush-get-to-19"
    features = destination / "jak3/features"
    original = json.loads((features / "replays" / category / "best-completed.ogr.json").read_text())
    config = {"player_id": secrets.token_hex(16), "player_token": secrets.token_hex(32),
              "server": "http://127.0.0.1:8876", "mode": 1,
              "submit_completed": False, "custom": {}}
    config_path = features / "ghost-client.json"
    if not config_path.exists():
        atomic_write(config_path, json.dumps(config).encode())
    last = copy.deepcopy(original)
    last["samples"] = last["samples"][:400]
    last["sample_count"] = 400
    last["duration_seconds"] = last["samples"][-1][0]
    last["completed"] = False
    atomic_write(features / "replays" / category / "last-attempt.ogr.json", json.dumps(last).encode())
    store = Store(root / "out/ghost-service/server")
    for index, (name, frames) in enumerate([("Fast Tester", 600), ("Middle Tester", 1200), ("Slow Tester", 1607)]):
        player = str(index + 1) * 32
        token = "a" * 64
        store.register(player, token)
        store.rename(player, name)
        data = copy.deepcopy(original)
        data["samples"] = data["samples"][:frames]
        data["sample_count"] = len(data["samples"])
        data["duration_seconds"] = data["samples"][-1][0]
        for sample in data["samples"]:
            sample[1][0] += index * 0.5
            sample[9][0] += index * 0.5
        store.upload(player, token, json.dumps(data).encode())
    store.close()
    print("Isolated profile and three server fixtures ready; no real profile modified.")


if __name__ == "__main__":
    main()
