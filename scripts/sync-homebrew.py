#!/usr/bin/env python3
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


VERSION = r"(?:0|[1-9][0-9]*)[.](?:0|[1-9][0-9]*)[.](?:0|[1-9][0-9]*)"
TAG = re.compile(rf"v(?P<version>{VERSION})")
SHA = re.compile(r"[0-9a-f]{40}")
FORMULA_VERSION = re.compile(rf'(?m)^[ \t]*version[ \t]+"({VERSION})"[ \t]*$')
REPOSITORY = "srz-zumix/cpp-actions"
TAP_REPOSITORY = "srz-zumix/homebrew-tap"


def required_env(name):
    value = os.environ.get(name)
    if not value:
        raise ValueError(f"{name} is required")
    return value


def run(*args, environment=None):
    return subprocess.run(args, check=True, text=True, stdout=subprocess.PIPE, env=environment).stdout.strip()


def output(name, value):
    with Path(required_env("GITHUB_OUTPUT")).open("a", encoding="utf-8") as file:
        print(f"{name}={value}", file=file)


def latest_tag(token=None):
    environment = dict(os.environ, GH_TOKEN=token) if token is not None else None
    latest = run("gh", "api", f"repos/{REPOSITORY}/releases/latest", "--jq", ".tag_name", environment=environment)
    if not TAG.fullmatch(latest):
        raise ValueError("GitHub returned an invalid latest release tag")
    return latest


def is_latest(tag, token=None):
    latest = latest_tag(token)
    if latest != tag:
        requested_version = tuple(int(part) for part in tag[1:].split("."))
        latest_version = tuple(int(part) for part in latest[1:].split("."))
        if requested_version > latest_version:
            raise ValueError(f"Release {tag} is newer than GitHub's latest release {latest}; retry the Homebrew update")
        print(f"::notice::Release {tag} is not the latest release ({latest}); skipping Homebrew update")
        return False
    return True


def formula_version(content, description):
    matches = FORMULA_VERSION.findall(content)
    if len(matches) != 1:
        raise ValueError(f"Cannot determine the version of {description}")
    return tuple(int(part) for part in matches[0].split("."))


def refuse_downgrade(content, version, description):
    current = formula_version(content, description)
    if current > version:
        raise ValueError(f"Refusing to overwrite a newer {description}")


def read_formula(path):
    if path.is_symlink():
        raise ValueError(f"Homebrew formula must not be a symlink: {path}")
    if not path.exists():
        return None
    if not path.is_file():
        raise ValueError(f"Homebrew formula is not a regular file: {path}")
    return path.read_text(encoding="utf-8")


def validate_formula_template(template):
    for placeholder in ("{{VERSION}}", "{{SHA256}}", "{{URL}}"):
        if template.count(placeholder) != 1:
            raise ValueError(f"Homebrew formula template must contain one {placeholder} placeholder")


def render_formula(template, version, checksum, url):
    validate_formula_template(template)
    replacements = {"{{VERSION}}": version, "{{SHA256}}": checksum, "{{URL}}": url}
    for placeholder, value in replacements.items():
        template = template.replace(placeholder, value)
    return template


def prepare(tag, version, tap):
    if not is_latest(tag):
        output("eligible", "false")
        return

    formula_path = tap / "cpp-actions.rb"
    existing = read_formula(formula_path)
    if existing is not None:
        refuse_downgrade(existing, version, "Homebrew formula")

    template = Path(required_env("HOMEBREW_TEMPLATE")).read_text(encoding="utf-8")
    validate_formula_template(template)
    archive = f"cpp-actions-{tag}-macos-arm64.tar.gz"
    with tempfile.TemporaryDirectory(prefix="cpp-actions-homebrew-", dir=required_env("RUNNER_TEMP")) as directory:
        run(
            "gh", "release", "download", tag, "--repo", REPOSITORY,
            "--pattern", archive, "--pattern", f"{archive}.sha256", "--dir", directory,
        )
        archive_path = Path(directory) / archive
        checksum_path = Path(directory) / f"{archive}.sha256"
        if not archive_path.is_file() or not checksum_path.is_file():
            raise ValueError("The published release is missing the archive or checksum")
        digest = hashlib.sha256()
        with archive_path.open("rb") as file:
            for block in iter(lambda: file.read(1024 * 1024), b""):
                digest.update(block)
        checksum = digest.hexdigest()
        if checksum_path.read_text(encoding="utf-8") != f"{checksum}  {archive}\n":
            raise ValueError("The published archive does not match its checksum")

    url = f"https://github.com/{REPOSITORY}/releases/download/{tag}/{archive}"
    rendered = render_formula(template, tag[1:], checksum, url)
    if rendered == existing:
        print("::notice::The checked-out Homebrew formula is already up to date")
    else:
        formula_path.write_text(rendered, encoding="utf-8")
    output("eligible", "true")


def check(tag, version, tap):
    if not is_latest(tag):
        output("current", "false")
        return

    local = read_formula(tap / "cpp-actions.rb")
    if local is None or formula_version(local, "generated Homebrew formula") != version:
        raise ValueError("The generated Homebrew formula no longer matches the release")

    run("git", "-C", str(tap), "fetch", "--no-tags", "origin", "main")
    head = run("git", "-C", str(tap), "rev-parse", "FETCH_HEAD^{commit}")
    if not SHA.fullmatch(head):
        raise ValueError("Git returned an invalid Homebrew tap commit SHA")
    remote_path = run("git", "-C", str(tap), "ls-tree", "--name-only", "FETCH_HEAD", "--", "cpp-actions.rb")
    if remote_path:
        if remote_path != "cpp-actions.rb":
            raise ValueError("Unexpected Homebrew formula path in the tap")
        remote = run("git", "-C", str(tap), "show", "FETCH_HEAD:cpp-actions.rb")
        refuse_downgrade(remote, version, "Homebrew formula on tap main")
        if remote == local.rstrip("\n"):
            print("::notice::The Homebrew formula was already updated")
            output("current", "false")
            return
    if not is_latest(tag):
        output("current", "false")
        return
    output("current", "true")
    output("head_sha", head)


def commit(tag, version, tap):
    required_env("GH_TOKEN")
    head = required_env("EXPECTED_HEAD")
    if not SHA.fullmatch(head):
        raise ValueError("Expected a full lowercase Homebrew tap commit SHA")
    if not is_latest(tag, required_env("RELEASE_TOKEN")):
        return
    content = read_formula(tap / "cpp-actions.rb")
    if content is None or formula_version(content, "generated Homebrew formula") != version:
        raise ValueError("The generated Homebrew formula no longer matches the release")
    mutation = """
        mutation($input: CreateCommitOnBranchInput!) {
          createCommitOnBranch(input: $input) { commit { oid } }
        }
    """
    payload = {
        "query": mutation,
        "variables": {
            "input": {
                "branch": {"repositoryNameWithOwner": TAP_REPOSITORY, "branchName": "main"},
                "expectedHeadOid": head,
                "message": {"headline": f"Update cpp-actions Homebrew formula for {tag}"},
                "fileChanges": {
                    "additions": [{
                        "path": "cpp-actions.rb",
                        "contents": base64.b64encode(content.encode("utf-8")).decode("ascii"),
                    }],
                },
            },
        },
    }
    result = subprocess.run(
        ["gh", "api", "graphql", "--input", "-"],
        input=json.dumps(payload),
        check=True,
        text=True,
        stdout=subprocess.PIPE,
    )
    response = json.loads(result.stdout)
    if not isinstance(response, dict) or response.get("errors"):
        raise ValueError(f"GitHub rejected the Homebrew tap update: {response}")
    data = response.get("data")
    result = data.get("createCommitOnBranch") if isinstance(data, dict) else None
    created = result.get("commit") if isinstance(result, dict) else None
    oid = created.get("oid") if isinstance(created, dict) else None
    if not isinstance(oid, str) or not SHA.fullmatch(oid):
        raise ValueError("GitHub returned an invalid Homebrew tap commit")
    print(f"::notice::Updated the Homebrew tap at {oid}")


def main():
    parser = argparse.ArgumentParser(description="Prepare and validate the Homebrew tap update")
    parser.add_argument("command", choices=("prepare", "check", "commit"))
    args = parser.parse_args()
    try:
        if required_env("GH_REPO") != REPOSITORY:
            raise ValueError(f"Homebrew updates are only supported for {REPOSITORY}")
        tag = required_env("TAG")
        match = TAG.fullmatch(tag)
        if not match:
            raise ValueError("Release tag must have the form vMAJOR.MINOR.PATCH without leading zeros")
        version = tuple(int(part) for part in match.group("version").split("."))
        tap = Path(required_env("HOMEBREW_TAP_DIR"))
        if not tap.is_dir():
            raise ValueError(f"Homebrew tap directory does not exist: {tap}")
        required_env("GITHUB_OUTPUT")
        if args.command == "prepare":
            prepare(tag, version, tap)
        elif args.command == "check":
            check(tag, version, tap)
        else:
            commit(tag, version, tap)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"::error::{error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
