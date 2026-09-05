"""SparkedHost entry file: full branch checkout, safe updates on every restart.

Place this bootstrap at /home/container/start_hosted.py. Repository, persistent
replays and credentials are separate siblings. Never reset/clean the checkout.
"""
import os
from pathlib import Path
import subprocess
import sys

REPO = "https://github.com/Zedb0T/OG-ILs.git"
BRANCH = "codex/replay-rebuild"


def git(checkout, *args):
    return subprocess.run(["git", "-c", "pack.threads=1", "-c", "pack.windowMemory=16m",
                           "-C", str(checkout), *args], check=True, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=300).stdout.strip()


def validate(checkout):
    service = checkout / "tools" / "replay-server"
    if (service / "requirements-hosted.txt").read_text().strip() != "waitress==3.0.2":
        raise ValueError("Dependency change needs a deployment update")
    subprocess.run([sys.executable, "-m", "unittest", "discover", "-s", str(service), "-v"],
                   cwd=service, check=True, timeout=120)


def update(root):
    checkout = root / "repo"
    previous = None
    if checkout.exists():
        if git(checkout, "remote", "get-url", "origin") != REPO:
            raise ValueError("Unexpected checkout origin; refusing to update")
        if git(checkout, "status", "--porcelain", "--untracked-files=normal"):
            raise ValueError("Checkout has local changes; refusing to overwrite them")
        previous = git(checkout, "rev-parse", "HEAD")
    try:
        if previous:
            git(checkout, "fetch", "origin", BRANCH)
            git(checkout, "merge", "--ff-only", "FETCH_HEAD")
        else:
            # Full working tree, shallow history: no sparse checkout or subdirectory export.
            git(root, "clone", "--depth", "1", "--single-branch", "--branch", BRANCH, REPO, str(checkout))
        validate(checkout)
    except Exception as error:
        print(f"Ghost update failed ({type(error).__name__})", flush=True)
        if not previous:
            raise RuntimeError("No previous checkout available; startup stopped") from None
        # A normal checkout switch, not reset --hard: never discards local changes.
        git(checkout, "switch", "--detach", previous)
        validate(checkout)
        print("Using previous tested commit", flush=True)
    print(f"Ghost branch {BRANCH}: {git(checkout, 'rev-parse', 'HEAD')}", flush=True)
    return checkout / "tools" / "replay-server"


if __name__ == "__main__":
    root = Path(__file__).resolve().parent
    release = update(root)
    os.execv(sys.executable, [sys.executable, "-u", str(release / "hosted.py")])
