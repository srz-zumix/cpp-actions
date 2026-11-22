#!/usr/bin/env python3
import base64
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


TAG = "v0.1.0"
REPO = "srz-zumix/cpp-actions"


def fake_gh():
    state_path = Path(os.environ["FAKE_GH_STATE"])
    state = json.loads(state_path.read_text())
    args = sys.argv[2:]
    state["calls"].append(args)
    if args == ["api", f"repos/{REPO}/releases/latest", "--jq", ".tag_name"]:
        state.setdefault("latest_tokens", []).append(os.environ.get("GH_TOKEN"))
        if state.get("fail_latest"):
            result = 1
        else:
            sequence = state.get("latest_sequence", [])
            print(sequence.pop(0) if sequence else state["latest"])
            result = 0
    elif len(args) >= 3 and args[:2] == ["release", "download"]:
        tag = args[2]
        options = args[3:]
        patterns = []
        directory = None
        repo = None
        while options:
            option = options.pop(0)
            value = options.pop(0)
            if option == "--pattern":
                patterns.append(value)
            elif option == "--dir":
                directory = Path(value)
            elif option == "--repo":
                repo = value
            else:
                raise RuntimeError(f"Unexpected gh option: {option}")
        archive = f"cpp-actions-{tag}-macos-arm64.tar.gz"
        if repo != REPO or patterns != [archive, f"{archive}.sha256"] or directory is None:
            raise RuntimeError(f"Unexpected release download: {args}")
        if state.get("fail_download"):
            result = 1
        else:
            for name in patterns:
                if name not in state.get("omit", []):
                    shutil.copyfile(Path(os.environ["FAKE_ASSETS"]) / name, directory / name)
            result = 0
    elif args == ["api", "graphql", "--input", "-"]:
        payload = json.load(sys.stdin)
        state["graphql"] = payload
        state["graphql_token"] = os.environ.get("GH_TOKEN")
        expected = payload["variables"]["input"]["expectedHeadOid"]
        remote = os.environ["FAKE_REMOTE"]
        actual = subprocess.check_output(
            ["git", "--git-dir", remote, "rev-parse", "refs/heads/main"],
            text=True,
        ).strip()
        if expected != actual:
            print("Simulated branch head conflict", file=sys.stderr)
            result = 1
        elif state.get("fail_graphql"):
            print("Simulated GraphQL failure", file=sys.stderr)
            result = 1
        elif "graphql_response" in state:
            print(state["graphql_response"])
            result = 0
        else:
            changes = payload["variables"]["input"]["fileChanges"]
            if set(changes) != {"additions"} or len(changes["additions"]) != 1:
                raise RuntimeError("Unexpected GraphQL file changes")
            addition = changes["additions"][0]
            if addition["path"] != "cpp-actions.rb":
                raise RuntimeError("Unexpected GraphQL file path")
            with tempfile.TemporaryDirectory(prefix="fake-gh-server-", dir=os.environ["RUNNER_TEMP"]) as directory:
                subprocess.run(["git", "clone", "-q", remote, directory], check=True)
                (Path(directory) / addition["path"]).write_bytes(base64.b64decode(addition["contents"], validate=True))
                subprocess.run(["git", "-C", directory, "add", "--", addition["path"]], check=True)
                headline = payload["variables"]["input"]["message"]["headline"]
                subprocess.run(
                    [
                        "git", "-C", directory, "-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid",
                        "-c", "commit.gpgsign=false", "commit", "-q", "-m", headline, "-m",
                        "Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>",
                    ],
                    check=True,
                )
                oid = subprocess.check_output(["git", "-C", directory, "rev-parse", "HEAD"], text=True).strip()
                subprocess.run(["git", "-C", directory, "push", "-q", "origin", "HEAD:main"], check=True)
            state["created"] = True
            print(json.dumps({"data": {"createCommitOnBranch": {"commit": {"oid": oid}}}}))
            result = 0
    else:
        raise RuntimeError(f"Unexpected gh command: {args}")
    state_path.write_text(json.dumps(state))
    return result


class SyncHomebrewTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="cpa-homebrew-test-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.tap = self.root / "tap"
        self.tap.mkdir()
        self.assets = self.root / "assets"
        self.assets.mkdir()
        self.state_path = self.root / "state.json"
        self.output = self.root / "github-output"
        self.output.write_text("")
        fake_bin = self.root / "bin"
        fake_bin.mkdir()
        wrapper = fake_bin / "gh"
        wrapper.write_text(
            "#!/bin/sh\n"
            f'exec "{sys.executable}" "{Path(__file__).resolve()}" --fake-gh "$@"\n'
        )
        wrapper.chmod(0o755)
        self.git_env = dict(
            os.environ, GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull, GIT_TERMINAL_PROMPT="0"
        )
        self.env = dict(
            self.git_env,
            PATH=f"{fake_bin}{os.pathsep}{os.environ['PATH']}",
            GH_REPO=REPO,
            GH_TOKEN="fixture-read-token",
            RELEASE_TOKEN="fixture-read-token",
            TAG=TAG,
            HOMEBREW_TAP_DIR=str(self.tap),
            HOMEBREW_TEMPLATE=TEMPLATE,
            RUNNER_TEMP=str(self.root),
            GITHUB_OUTPUT=str(self.output),
            FAKE_GH_STATE=str(self.state_path),
            FAKE_ASSETS=str(self.assets),
        )
        self.save({"latest": TAG, "calls": []})
        self.make_assets(TAG)

    def save(self, state):
        self.state_path.write_text(json.dumps(state))

    def state(self):
        return json.loads(self.state_path.read_text())

    def make_assets(self, tag):
        archive = f"cpp-actions-{tag}-macos-arm64.tar.gz"
        payload = f"archive for {tag}\n".encode()
        (self.assets / archive).write_bytes(payload)
        (self.assets / f"{archive}.sha256").write_text(
            f"{hashlib.sha256(payload).hexdigest()}  {archive}\n"
        )

    def run_script(self, command, success=True):
        self.output.write_text("")
        result = subprocess.run(
            [sys.executable, SCRIPT, command],
            env=self.env,
            capture_output=True,
            text=True,
            timeout=15,
        )
        if success:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("::error::", result.stderr)
            self.assertEqual(self.output.read_text(), "")
        return result

    def git(self, directory, *args):
        return subprocess.run(
            ["git", "-C", str(directory), *args],
            env=self.git_env,
            check=True,
            capture_output=True,
            text=True,
            timeout=15,
        ).stdout.strip()

    def commit_seed(self, message):
        self.git(self.seed, "add", ".")
        self.git(
            self.seed, "-c", "commit.gpgsign=false", "commit", "-q",
            "-m", message, "-m", "Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>",
        )

    def make_remote(self):
        self.seed = self.root / "seed"
        self.seed.mkdir()
        self.git(self.seed, "init", "-q", "-b", "main")
        self.git(self.seed, "config", "user.name", "Fixture")
        self.git(self.seed, "config", "user.email", "fixture@example.invalid")
        (self.seed / "README.md").write_text("Homebrew tap fixture\n")
        self.commit_seed("Create fixture")
        remote = self.root / "remote.git"
        subprocess.run(["git", "clone", "-q", "--bare", str(self.seed), str(remote)], env=self.git_env, check=True)
        self.git(self.seed, "remote", "add", "origin", str(remote))
        self.tap.rmdir()
        subprocess.run(["git", "clone", "-q", str(remote), str(self.tap)], env=self.git_env, check=True)
        self.env["FAKE_REMOTE"] = str(remote)

    def publish_seed_formula(self, content):
        (self.seed / "cpp-actions.rb").write_text(content)
        self.commit_seed("Update fixture formula")
        self.git(self.seed, "push", "-q", "origin", "main")

    def test_prepare_renders_verified_archive_and_preserves_other_tap_files(self):
        (self.tap / "gali.rb").write_text("existing formula\n")
        (self.tap / "Casks").mkdir()
        (self.tap / "Casks" / "jirafs.rb").write_text("existing cask\n")
        self.run_script("prepare")
        self.assertEqual(self.output.read_text(), "eligible=true\n")
        content = (self.tap / "cpp-actions.rb").read_text()
        payload = (self.assets / f"cpp-actions-{TAG}-macos-arm64.tar.gz").read_bytes()
        self.assertIn('version "0.1.0"', content)
        self.assertIn(f'sha256 "{hashlib.sha256(payload).hexdigest()}"', content)
        self.assertIn(
            f'url "https://github.com/{REPO}/releases/download/{TAG}/cpp-actions-{TAG}-macos-arm64.tar.gz"',
            content,
        )
        self.assertNotIn("{{", content)
        self.assertEqual((self.tap / "gali.rb").read_text(), "existing formula\n")
        self.assertEqual((self.tap / "Casks" / "jirafs.rb").read_text(), "existing cask\n")
        self.assertFalse((self.tap / "Formula").exists())
        ruby = shutil.which("ruby")
        if ruby:
            subprocess.run([ruby, "-c", str(self.tap / "cpp-actions.rb")], check=True, capture_output=True)

    def test_unchanged_checkout_still_requires_remote_recheck(self):
        self.run_script("prepare")
        before = (self.tap / "cpp-actions.rb").read_bytes()
        result = self.run_script("prepare")
        self.assertEqual(self.output.read_text(), "eligible=true\n")
        self.assertIn("already up to date", result.stdout)
        self.assertEqual((self.tap / "cpp-actions.rb").read_bytes(), before)

    def test_older_release_is_skipped_without_downloading(self):
        before = 'class CppActions < Formula\n  version "0.2.0"\nend\n'
        (self.tap / "cpp-actions.rb").write_text(before)
        self.save({"latest": "v0.2.0", "calls": []})
        result = self.run_script("prepare")
        self.assertEqual(self.output.read_text(), "eligible=false\n")
        self.assertIn("not the latest", result.stdout)
        self.assertEqual((self.tap / "cpp-actions.rb").read_text(), before)
        self.assertEqual(len(self.state()["calls"]), 1)

    def test_newer_release_not_yet_latest_fails_explicitly(self):
        self.env["TAG"] = "v0.2.0"
        result = self.run_script("prepare", success=False)
        self.assertIn("retry the Homebrew update", result.stderr)
        self.assertFalse((self.tap / "cpp-actions.rb").exists())
        self.assertEqual(len(self.state()["calls"]), 1)

    def test_newer_existing_formula_is_not_downgraded(self):
        content = 'class CppActions < Formula\n  version "0.1.10"\nend\n'
        (self.tap / "cpp-actions.rb").write_text(content)
        self.run_script("prepare", success=False)
        self.assertEqual((self.tap / "cpp-actions.rb").read_text(), content)
        self.assertEqual(len(self.state()["calls"]), 1)

    def test_invalid_inputs_and_latest_api_errors_do_not_modify_the_tap(self):
        original_env = dict(self.env)
        for changes in ({"TAG": "v01.1.0"}, {"TAG": "v0.1.0;echo injected"}, {"GH_REPO": "other/repo"}):
            with self.subTest(changes=changes):
                self.env = dict(original_env, **changes)
                self.run_script("prepare", success=False)
                self.assertEqual(self.state()["calls"], [])
        self.env = original_env
        for changes in ({"latest": "invalid"}, {"fail_latest": True}):
            with self.subTest(changes=changes):
                self.save(dict({"latest": TAG, "calls": []}, **changes))
                self.run_script("prepare", success=False)
                self.assertFalse((self.tap / "cpp-actions.rb").exists())

    def test_missing_or_corrupted_assets_do_not_modify_the_tap(self):
        archive = f"cpp-actions-{TAG}-macos-arm64.tar.gz"
        original_checksum = (self.assets / f"{archive}.sha256").read_text()
        cases = (
            {"omit": [archive]},
            {"omit": [f"{archive}.sha256"]},
            {"fail_download": True},
            {"checksum": original_checksum.replace(archive, "other.tar.gz")},
            {"checksum": "0" * 64 + f"  {archive}\n"},
        )
        for changes in cases:
            with self.subTest(changes=changes):
                self.save(dict({"latest": TAG, "calls": []}, **{k: v for k, v in changes.items() if k != "checksum"}))
                (self.assets / f"{archive}.sha256").write_text(changes.get("checksum", original_checksum))
                self.run_script("prepare", success=False)
                self.assertFalse((self.tap / "cpp-actions.rb").exists())

    def test_invalid_template_and_symlink_are_rejected(self):
        invalid_template = self.root / "invalid.rb"
        invalid_template.write_text("{{VERSION}} {{VERSION}} {{SHA256}}\n")
        self.env["HOMEBREW_TEMPLATE"] = str(invalid_template)
        self.run_script("prepare", success=False)
        self.assertEqual(len(self.state()["calls"]), 1)
        self.env["HOMEBREW_TEMPLATE"] = TEMPLATE
        sentinel = self.root / "sentinel"
        sentinel.write_text("unchanged\n")
        (self.tap / "cpp-actions.rb").symlink_to(sentinel)
        self.run_script("prepare", success=False)
        self.assertEqual(sentinel.read_text(), "unchanged\n")

    def test_recheck_allows_new_formula_and_detects_concurrent_same_update(self):
        self.make_remote()
        self.run_script("prepare")
        self.run_script("check")
        outputs = dict(line.split("=", 1) for line in self.output.read_text().splitlines())
        self.assertEqual(outputs["current"], "true")
        self.assertEqual(outputs["head_sha"], self.git(self.tap, "rev-parse", "FETCH_HEAD"))
        self.publish_seed_formula((self.tap / "cpp-actions.rb").read_text())
        result = self.run_script("check")
        self.assertEqual(self.output.read_text(), "current=false\n")
        self.assertIn("already updated", result.stdout)

    def test_recheck_detects_remote_change_after_unchanged_checkout(self):
        self.make_remote()
        self.run_script("prepare")
        formula = (self.tap / "cpp-actions.rb").read_text()
        self.publish_seed_formula(formula)
        self.run_script("prepare")
        self.assertEqual(self.output.read_text(), "eligible=true\n")

        self.publish_seed_formula(formula + "# Concurrent change\n")
        concurrent_head = self.git(self.seed, "rev-parse", "HEAD")
        self.run_script("check")
        outputs = dict(line.split("=", 1) for line in self.output.read_text().splitlines())
        self.assertEqual(outputs["current"], "true")
        self.assertEqual(outputs["head_sha"], concurrent_head)
        self.env.update(EXPECTED_HEAD=outputs["head_sha"], GH_TOKEN="fixture-app-token")
        self.run_script("commit")

        self.git(self.seed, "fetch", "-q", "origin", "main")
        self.assertEqual(self.git(self.seed, "rev-parse", "FETCH_HEAD^"), concurrent_head)
        self.assertEqual(self.git(self.seed, "show", "FETCH_HEAD:cpp-actions.rb"), formula.rstrip("\n"))

    def test_recheck_skips_when_a_newer_release_appears(self):
        self.make_remote()
        self.run_script("prepare")
        self.save({"latest": TAG, "latest_sequence": [TAG, "v0.2.0"], "calls": []})
        self.run_script("check")
        self.assertEqual(self.output.read_text(), "current=false\n")

    def test_recheck_refuses_newer_or_invalid_remote_formula(self):
        self.make_remote()
        self.run_script("prepare")
        self.publish_seed_formula('class CppActions < Formula\n  version "0.2.0"\nend\n')
        result = self.run_script("check", success=False)
        self.assertIn("newer Homebrew formula on tap main", result.stderr)
        self.publish_seed_formula("not a formula\n")
        result = self.run_script("check", success=False)
        self.assertIn("Cannot determine the version", result.stderr)

    def test_commit_uses_validated_head_and_only_the_formula(self):
        self.make_remote()
        self.run_script("prepare")
        self.run_script("check")
        outputs = dict(line.split("=", 1) for line in self.output.read_text().splitlines())
        self.env.update(EXPECTED_HEAD=outputs["head_sha"], GH_TOKEN="fixture-app-token")
        result = self.run_script("commit")
        self.assertIn("Updated the Homebrew tap", result.stdout)
        state = self.state()
        self.assertTrue(state["created"])
        self.assertEqual(state["graphql_token"], "fixture-app-token")
        self.assertTrue(state["latest_tokens"])
        self.assertTrue(all(token == "fixture-read-token" for token in state["latest_tokens"]))
        payload = state["graphql"]
        self.assertIn("createCommitOnBranch", payload["query"])
        change = payload["variables"]["input"]
        self.assertEqual(change["expectedHeadOid"], outputs["head_sha"])
        self.assertEqual(
            change["branch"], {"repositoryNameWithOwner": "srz-zumix/homebrew-tap", "branchName": "main"}
        )
        self.assertEqual(change["message"]["headline"], "Update cpp-actions Homebrew formula for v0.1.0")
        self.assertEqual(set(change["fileChanges"]), {"additions"})
        self.assertEqual(len(change["fileChanges"]["additions"]), 1)
        addition = change["fileChanges"]["additions"][0]
        self.assertEqual(addition["path"], "cpp-actions.rb")
        self.assertEqual(base64.b64decode(addition["contents"], validate=True), (self.tap / "cpp-actions.rb").read_bytes())

    def test_commit_preserves_changes_before_recheck(self):
        self.make_remote()
        self.run_script("prepare")
        (self.seed / "README.md").write_text("Concurrent tap update\n")
        (self.seed / "other.rb").write_text("Another formula\n")
        self.commit_seed("Concurrent fixture change")
        self.git(self.seed, "push", "-q", "origin", "main")
        concurrent_head = self.git(self.seed, "rev-parse", "HEAD")

        self.run_script("check")
        outputs = dict(line.split("=", 1) for line in self.output.read_text().splitlines())
        self.assertEqual(outputs["head_sha"], concurrent_head)
        self.env.update(EXPECTED_HEAD=outputs["head_sha"], GH_TOKEN="fixture-app-token")
        self.run_script("commit")

        self.git(self.seed, "fetch", "-q", "origin", "main")
        self.assertEqual(self.git(self.seed, "rev-parse", "FETCH_HEAD^"), concurrent_head)
        self.assertEqual(self.git(self.seed, "diff", "--name-only", concurrent_head, "FETCH_HEAD"), "cpp-actions.rb")
        self.assertEqual(self.git(self.seed, "show", "FETCH_HEAD:README.md"), "Concurrent tap update")
        self.assertEqual(self.git(self.seed, "show", "FETCH_HEAD:other.rb"), "Another formula")
        self.assertEqual(
            self.git(self.seed, "show", "FETCH_HEAD:cpp-actions.rb"),
            (self.tap / "cpp-actions.rb").read_text().rstrip("\n"),
        )

    def test_commit_rejects_branch_changes_after_recheck(self):
        self.make_remote()
        self.run_script("prepare")
        self.run_script("check")
        outputs = dict(line.split("=", 1) for line in self.output.read_text().splitlines())
        (self.seed / "README.md").write_text("Concurrent tap update\n")
        self.commit_seed("Concurrent fixture change")
        self.git(self.seed, "push", "-q", "origin", "main")
        concurrent_head = self.git(self.seed, "rev-parse", "HEAD")
        self.env.update(EXPECTED_HEAD=outputs["head_sha"], GH_TOKEN="fixture-app-token")
        result = self.run_script("commit", success=False)
        self.assertIn("branch head conflict", result.stderr)
        self.assertNotIn("created", self.state())
        self.git(self.seed, "fetch", "-q", "origin", "main")
        self.assertEqual(self.git(self.seed, "rev-parse", "FETCH_HEAD"), concurrent_head)
        self.assertEqual(self.git(self.seed, "show", "FETCH_HEAD:README.md"), "Concurrent tap update")
        self.assertEqual(self.git(self.seed, "ls-tree", "--name-only", "FETCH_HEAD", "--", "cpp-actions.rb"), "")

    def test_commit_skips_when_release_changes_after_recheck(self):
        self.make_remote()
        self.run_script("prepare")
        self.run_script("check")
        outputs = dict(line.split("=", 1) for line in self.output.read_text().splitlines())
        self.env.update(EXPECTED_HEAD=outputs["head_sha"], GH_TOKEN="fixture-app-token")
        self.save({"latest": "v0.2.0", "calls": []})
        result = self.run_script("commit")
        self.assertIn("not the latest", result.stdout)
        self.assertNotIn("graphql", self.state())

    def test_commit_rejects_invalid_head_and_missing_tokens(self):
        self.env.update(EXPECTED_HEAD="not-a-sha", GH_TOKEN="fixture-app-token")
        self.run_script("commit", success=False)
        self.assertEqual(self.state()["calls"], [])
        self.env["EXPECTED_HEAD"] = "a" * 40
        self.env.pop("RELEASE_TOKEN")
        result = self.run_script("commit", success=False)
        self.assertIn("RELEASE_TOKEN is required", result.stderr)
        self.assertEqual(self.state()["calls"], [])
        self.env.pop("GH_TOKEN")
        self.run_script("commit", success=False)
        self.assertEqual(self.state()["calls"], [])

    def test_commit_reports_graphql_errors_and_invalid_responses(self):
        self.make_remote()
        self.run_script("prepare")
        self.run_script("check")
        outputs = dict(line.split("=", 1) for line in self.output.read_text().splitlines())
        self.env.update(EXPECTED_HEAD=outputs["head_sha"], GH_TOKEN="fixture-app-token")
        cases = (
            ({"fail_graphql": True}, "Simulated GraphQL failure"),
            ({"graphql_response": json.dumps({"errors": [{"message": "access denied"}]})}, "access denied"),
            ({"graphql_response": "[]"}, "GitHub rejected the Homebrew tap update"),
            ({"graphql_response": '{"data":'}, "Expecting value"),
            ({"graphql_response": json.dumps({"data": {"createCommitOnBranch": None}})}, "invalid Homebrew tap commit"),
            (
                {"graphql_response": json.dumps({"data": {"createCommitOnBranch": {"commit": {"oid": "invalid"}}}})},
                "invalid Homebrew tap commit",
            ),
        )
        for response, message in cases:
            with self.subTest(response=response):
                self.save(dict({"latest": TAG, "calls": []}, **response))
                result = self.run_script("commit", success=False)
                self.assertIn(message, result.stderr)
                self.assertNotIn("created", self.state())
        self.git(self.seed, "fetch", "-q", "origin", "main")
        self.assertEqual(self.git(self.seed, "rev-parse", "FETCH_HEAD"), outputs["head_sha"])


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--fake-gh":
        sys.exit(fake_gh())
    if len(sys.argv) < 3:
        sys.exit("Usage: sync_homebrew_test.py PATH_TO_SYNC_SCRIPT PATH_TO_FORMULA_TEMPLATE")
    SCRIPT = str(Path(sys.argv.pop(1)).resolve())
    TEMPLATE = str(Path(sys.argv.pop(1)).resolve())
    if sys.version_info < (3, 7) or any(shutil.which(tool) is None for tool in ("bash", "git")):
        print("Skipping Homebrew sync tests: Python 3.7+, bash, and git are required")
        sys.exit(77)
    unittest.main()
