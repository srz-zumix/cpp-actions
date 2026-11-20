#!/usr/bin/env python3
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys


VERSION = r"(?:0|[1-9][0-9]*)[.](?:0|[1-9][0-9]*)[.](?:0|[1-9][0-9]*)"
TAG = re.compile(rf"v(?P<version>{VERSION})")
SHA = re.compile(r"[0-9a-f]{40}")
PROJECT_VERSION = re.compile(
    rf"(?m)^project\(cpp-actions[ \t]*\r?\n[ \t]*VERSION[ \t]+"
    rf"(?P<version>{VERSION})[ \t]*\r?$"
)
MANIFEST_VERSION = re.compile(
    rf'(?m)^[ \t]*"version"[ \t]*:[ \t]*"(?P<version>{VERSION})"[ \t]*,?[ \t]*\r?$'
)


def read_versions():
    project_path = Path("CMakeLists.txt")
    manifest_path = Path("vcpkg.json")
    project = project_path.read_bytes().decode("utf-8")
    manifest = manifest_path.read_bytes().decode("utf-8")
    project_matches = list(PROJECT_VERSION.finditer(project))
    manifest_matches = list(MANIFEST_VERSION.finditer(manifest))
    data = json.loads(manifest)
    if len(project_matches) != 1:
        raise ValueError("Expected exactly one cpp-actions project VERSION in CMakeLists.txt")
    if (
        not isinstance(data, dict)
        or data.get("name") != "cpp-actions"
        or len(manifest_matches) != 1
        or data.get("version") != manifest_matches[0].group("version")
    ):
        raise ValueError("Expected exactly one top-level cpp-actions version in vcpkg.json")
    return (
        (project_path, project, project_matches[0]),
        (manifest_path, manifest, manifest_matches[0]),
    )


def update_version(tag):
    match = TAG.fullmatch(tag)
    if not match:
        raise ValueError("Release tag must have the form vMAJOR.MINOR.PATCH without leading zeros")
    version = match.group("version")
    target = tuple(int(part) for part in version.split("."))
    files = read_versions()
    for path, _, version_match in files:
        current = version_match.group("version")
        if target < tuple(int(part) for part in current.split(".")):
            raise ValueError(f"Refusing to downgrade {path} from {current} to {version}")
    for path, content, version_match in files:
        start, end = version_match.span("version")
        updated = content[:start] + version + content[end:]
        if updated != content:
            path.write_bytes(updated.encode("utf-8"))
    print(f"Project versions are set to {version}")


def required_env(name):
    value = os.environ.get(name)
    if not value:
        raise ValueError(f"{name} is required")
    return value


def gh_api(*args):
    return subprocess.run(
        ["gh", "api", *args], check=True, text=True, stdout=subprocess.PIPE
    ).stdout.strip()


def main_is_current(expected_sha):
    if not SHA.fullmatch(expected_sha):
        raise ValueError("Expected a full lowercase commit SHA")
    repo = required_env("GH_REPO")
    actual_sha = gh_api(f"repos/{repo}/commits/main", "--jq", ".sha")
    if not SHA.fullmatch(actual_sha):
        raise ValueError("GitHub returned an invalid main branch commit SHA")
    return actual_sha == expected_sha


def print_current(current):
    print(f"current={'true' if current else 'false'}")
    if not current:
        print("::notice::Main has moved; a newer push will update the release draft", file=sys.stderr)


def release_context():
    event = required_env("GITHUB_EVENT_NAME")
    sha = required_env("GITHUB_SHA")
    if event not in ("push", "workflow_dispatch") or not SHA.fullmatch(sha):
        raise ValueError("Expected a push or workflow_dispatch with a full commit SHA")
    if event == "push" and not main_is_current(sha):
        print_current(False)
        return

    files = read_versions()
    project_version = files[0][2].group("version")
    manifest_version = files[1][2].group("version")
    repo = required_env("GH_REPO")
    pages = json.loads(gh_api("--paginate", "--slurp", f"repos/{repo}/releases?per_page=100"))
    if not isinstance(pages, list) or any(not isinstance(page, list) for page in pages):
        raise ValueError("GitHub returned an invalid release list")
    releases = [release for page in pages for release in page]
    if any(
        not isinstance(release, dict)
        or not isinstance(release.get("draft"), bool)
        or not isinstance(release.get("prerelease"), bool)
        for release in releases
    ):
        raise ValueError("GitHub returned an invalid release entry")
    has_stable_release = any(not release["draft"] and not release["prerelease"] for release in releases)
    if not has_stable_release and project_version != manifest_version:
        raise ValueError("CMake and vcpkg versions must match before creating the first release draft")

    print_current(True)
    print(f"cmake_version={project_version}")
    print(f"vcpkg_version={manifest_version}")
    print(f"initial_version={'' if has_stable_release else project_version}")


def main():
    parser = argparse.ArgumentParser(description="Manage cpp-actions release versions")
    subcommands = parser.add_subparsers(dest="command", required=True)
    subcommands.add_parser("context")
    check = subcommands.add_parser("check-main")
    check.add_argument("sha")
    update = subcommands.add_parser("update")
    update.add_argument("tag")
    args = parser.parse_args()
    try:
        if args.command == "context":
            release_context()
        elif args.command == "check-main":
            print_current(main_is_current(args.sha))
        else:
            update_version(args.tag)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"::error::{error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
