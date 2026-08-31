"""Add a GitHub release to the repository's launcher testing mod list."""

from __future__ import annotations

import json
import os
from datetime import datetime, timezone
from pathlib import Path


ASSET_NAMES = {
    "windows": "windows-{version}.zip",
    "linux": "linux-{version}.tar.gz",
    "macos": "macos-intel-{version}.tar.gz",
}


def comma_separated_environment(name: str, default: str) -> list[str]:
    return [value.strip() for value in os.environ.get(name, default).split(",") if value.strip()]


def select_mod(mod_list: dict) -> tuple[str, dict]:
    mods = mod_list.get("mods", {})
    requested_mod_id = os.environ.get("MOD_ID", "").strip()
    if requested_mod_id:
        if requested_mod_id not in mods:
            raise KeyError(
                f"MOD_ID {requested_mod_id!r} was not found; available mods: {', '.join(mods)}"
            )
        return requested_mod_id, mods[requested_mod_id]

    if len(mods) != 1:
        raise ValueError(
            "mod_list.json must contain exactly one mod when MOD_ID is not set; "
            f"found: {', '.join(mods) or 'none'}"
        )

    mod_id = next(iter(mods))
    return mod_id, mods[mod_id]


def main() -> None:
    version_tag = os.environ["VERSION"].strip()
    if not version_tag:
        raise ValueError("VERSION cannot be empty")

    version_number = version_tag.removeprefix("v")
    supported_games = comma_separated_environment("SUPPORTED_GAMES", "jak2,jak3")
    platforms = comma_separated_environment("RELEASE_PLATFORMS", "windows,linux")
    unknown_platforms = sorted(set(platforms) - ASSET_NAMES.keys())
    if unknown_platforms:
        raise ValueError(f"Unsupported release platforms: {', '.join(unknown_platforms)}")

    repository = os.environ.get("GITHUB_REPOSITORY", "Zedb0T/OG-ILs")
    workspace = Path(os.environ.get("GITHUB_WORKSPACE", "."))
    mod_list_path = workspace / "mod_list.json"
    published_at = os.environ.get("PUBLISHED_AT") or datetime.now(timezone.utc).strftime(
        "%Y-%m-%dT%H:%M:%SZ"
    )

    with mod_list_path.open("r", encoding="utf-8") as mod_list_file:
        mod_list = json.load(mod_list_file)

    mod_id, mod = select_mod(mod_list)
    base_url = f"https://github.com/{repository}/releases/download/{version_tag}"
    assets = {
        platform: f"{base_url}/{ASSET_NAMES[platform].format(version=version_tag)}"
        for platform in platforms
    }

    new_version = {
        "version": version_number,
        "publishedDate": published_at,
        "supportedGames": supported_games,
        "assets": assets,
        "assetDownloadCounts": {platform: 0 for platform in platforms},
    }

    mod["versions"] = [
        existing for existing in mod["versions"] if existing["version"] != version_number
    ]
    mod["versions"].insert(0, new_version)

    all_games = {
        game
        for existing in mod["versions"]
        for game in existing.get("supportedGames", [])
    }
    mod["supportedGames"] = sorted(all_games)
    mod_list["lastUpdated"] = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")

    with mod_list_path.open("w", encoding="utf-8", newline="\n") as mod_list_file:
        json.dump(mod_list, mod_list_file, indent=2, ensure_ascii=False)
        mod_list_file.write("\n")

    print(f"Updated mod {mod_id!r} in {mod_list_path} with release {version_tag}")


if __name__ == "__main__":
    main()
