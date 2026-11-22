#!/usr/bin/env python3
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


SHA = "a" * 40
OTHER_SHA = "b" * 40
REPO = "fixture-owner/fixture-repo"


def fake_gh():
    args = sys.argv[2:]
    if not args or args.pop(0) != "api":
        raise RuntimeError("Unexpected gh command")
    if f"repos/{REPO}/commits/main" in args:
        if os.environ.get("FAIL_HEAD"):
            return 1
        print(os.environ["FAKE_HEAD"])
    elif f"repos/{REPO}/releases?per_page=100" in args:
        if os.environ.get("FAIL_RELEASES"):
            return 1
        print(os.environ["FAKE_RELEASES"])
    else:
        raise RuntimeError(f"Unexpected gh API request: {args}")
    return 0


class ReleaseVersionTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="cpa-version-test-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.project = self.root / "CMakeLists.txt"
        self.manifest = self.root / "vcpkg.json"
        self.project.write_text(
            'cmake_minimum_required(VERSION 3.24)\n'
            'project(cpp-actions\n'
            '  VERSION 0.1.0\n'
            '  LANGUAGES CXX)\n'
            'set(UNRELATED_VERSION 4.5.6)\n'
        )
        self.manifest.write_text(
            '{\n  "name": "cpp-actions",\n  "version": "0.1.0",\n'
            '  "dependencies": ["fixture-dependency"]\n}\n'
        )
        fake_bin = self.root / "bin"
        fake_bin.mkdir()
        wrapper = fake_bin / "gh"
        wrapper.write_text(
            "#!/bin/sh\n"
            f'exec "{sys.executable}" "{Path(__file__).resolve()}" --fake-gh "$@"\n'
        )
        wrapper.chmod(0o755)
        self.env = dict(
            os.environ,
            PATH=f"{fake_bin}{os.pathsep}{os.environ['PATH']}",
            GH_REPO=REPO,
            GITHUB_SHA=SHA,
            GITHUB_EVENT_NAME="push",
            FAKE_HEAD=SHA,
            FAKE_RELEASES="[]",
        )

    def run_script(self, *args, success=True):
        result = subprocess.run(
            [sys.executable, SCRIPT, *args],
            cwd=self.root,
            env=self.env,
            capture_output=True,
            text=True,
            timeout=10,
        )
        if success:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def files(self):
        return self.project.read_bytes(), self.manifest.read_bytes()

    def github_config(self, name):
        ruby = shutil.which("ruby")
        if ruby is None:
            self.skipTest("Ruby is required to parse workflow YAML")
        path = Path(SCRIPT).parent.parent / ".github" / name
        result = subprocess.run(
            [ruby, "-rjson", "-ryaml", "-e", "print JSON.generate(YAML.load_file(ARGV[0]))", str(path)],
            capture_output=True,
            text=True,
            timeout=10,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(result.stdout)

    def test_update_changes_both_versions_only(self):
        project, manifest = self.files()
        self.run_script("update", "v0.2.1")
        self.assertEqual(self.project.read_bytes(), project.replace(b"VERSION 0.1.0", b"VERSION 0.2.1"))
        self.assertEqual(self.manifest.read_bytes(), manifest.replace(b'"version": "0.1.0"', b'"version": "0.2.1"'))

    def test_equal_version_is_a_no_op(self):
        before = self.files()
        self.run_script("update", "v0.1.0")
        self.assertEqual(self.files(), before)

    def test_forward_update_repairs_inconsistent_versions(self):
        self.manifest.write_text(self.manifest.read_text().replace("0.1.0", "0.1.1"))
        self.run_script("update", "v0.2.0")
        self.assertIn("VERSION 0.2.0", self.project.read_text())
        self.assertEqual(json.loads(self.manifest.read_text())["version"], "0.2.0")

    def test_numeric_version_order_prevents_downgrades(self):
        self.project.write_text(self.project.read_text().replace("0.1.0", "0.1.10"))
        self.manifest.write_text(self.manifest.read_text().replace("0.1.0", "0.1.10"))
        before = self.files()
        self.run_script("update", "v0.1.9", success=False)
        self.assertEqual(self.files(), before)
        self.run_script("update", "v0.1.11")
        self.assertIn("VERSION 0.1.11", self.project.read_text())

    def test_invalid_tags_never_modify_files(self):
        before = self.files()
        for tag in ("v1.2", "v01.2.3", "v1.2.3-rc1", "1.2.3", "v1.2.3;echo injected"):
            with self.subTest(tag=tag):
                self.run_script("update", tag, success=False)
                self.assertEqual(self.files(), before)

    def test_invalid_project_or_manifest_never_modify_either_file(self):
        originals = self.files()
        cases = (
            (originals[0] + b"project(cpp-actions\n  VERSION 0.1.0\n)\n", originals[1]),
            (originals[0], b'{"name":"cpp-actions","version":'),
            (originals[0], originals[1].replace(b'"version": "0.1.0"', b'"other": "0.1.0"')),
        )
        for project, manifest in cases:
            with self.subTest(project=project, manifest=manifest):
                self.project.write_bytes(project)
                self.manifest.write_bytes(manifest)
                self.run_script("update", "v0.2.0", success=False)
                self.assertEqual(self.files(), (project, manifest))

    def test_initial_version_ignores_drafts_and_prereleases_on_all_pages(self):
        self.env["FAKE_RELEASES"] = json.dumps([
            [{"draft": True, "prerelease": False}],
            [{"draft": False, "prerelease": True}],
        ])
        result = self.run_script("context")
        self.assertEqual(
            result.stdout.splitlines(),
            ["current=true", "cmake_version=0.1.0", "vcpkg_version=0.1.0", "initial_version=0.1.0"],
        )

    def test_stable_release_uses_drafter_resolver(self):
        self.env["FAKE_RELEASES"] = json.dumps([
            [{"draft": True, "prerelease": False}],
            [{"draft": False, "prerelease": False}],
        ])
        result = self.run_script("context")
        self.assertEqual(result.stdout.splitlines()[-1], "initial_version=")

    def test_manual_context_uses_selected_commit_not_main(self):
        self.env.update(GITHUB_EVENT_NAME="workflow_dispatch", FAKE_HEAD=OTHER_SHA, FAIL_HEAD="1")
        result = self.run_script("context")
        self.assertIn("current=true", result.stdout)
        self.assertIn("initial_version=0.1.0", result.stdout)

    def test_stale_push_does_not_read_versions_or_releases(self):
        self.env.update(FAKE_HEAD=OTHER_SHA, FAIL_RELEASES="1")
        self.project.unlink()
        result = self.run_script("context")
        self.assertEqual(result.stdout.strip(), "current=false")
        self.assertIn("Main has moved", result.stderr)

    def test_first_release_rejects_mismatched_versions(self):
        self.manifest.write_text(self.manifest.read_text().replace("0.1.0", "0.1.1"))
        result = self.run_script("context", success=False)
        self.assertEqual(result.stdout, "")
        self.assertIn("must match", result.stderr)

    def test_existing_release_allows_version_repair(self):
        self.manifest.write_text(self.manifest.read_text().replace("0.1.0", "0.1.1"))
        self.env["FAKE_RELEASES"] = json.dumps([[{"draft": False, "prerelease": False}]])
        result = self.run_script("context")
        self.assertIn("cmake_version=0.1.0", result.stdout)
        self.assertIn("vcpkg_version=0.1.1", result.stdout)
        self.assertIn("initial_version=", result.stdout)

    def test_invalid_release_response_and_api_failures_are_errors(self):
        cases = (
            {"FAKE_RELEASES": '{"draft":false}'},
            {"FAKE_RELEASES": '[[{"draft":false}]]'},
            {"FAIL_RELEASES": "1"},
            {"FAIL_HEAD": "1"},
        )
        original = dict(self.env)
        for changes in cases:
            with self.subTest(changes=changes):
                self.env = dict(original, **changes)
                result = self.run_script("context", success=False)
                self.assertEqual(result.stdout, "")

    def test_main_freshness_check(self):
        self.assertEqual(self.run_script("check-main", SHA).stdout.strip(), "current=true")
        self.env["FAKE_HEAD"] = OTHER_SHA
        self.assertEqual(self.run_script("check-main", SHA).stdout.strip(), "current=false")
        self.run_script("check-main", "bad-sha", success=False)

    def test_draft_updates_call_version_pr_for_push_and_manual_main(self):
        draft = self.github_config("workflows/release-drafter.yml")
        jobs = draft["jobs"]
        push = jobs["bump_version_pr"]
        manual = jobs["bump_version_dispatch"]
        self.assertEqual(push["if"], "github.event_name == 'push'")
        self.assertEqual(push["needs"], "draft_release")
        self.assertEqual(push["with"]["tag_name"], "${{ needs.draft_release.outputs.tag_name }}")
        self.assertEqual(
            manual["if"],
            "github.event_name == 'workflow_dispatch' && github.ref == 'refs/heads/main'",
        )
        self.assertEqual(manual["needs"], "prepare_release")
        self.assertEqual(manual["with"]["tag_name"], "${{ needs.prepare_release.outputs.tag_name }}")
        for job in (push, manual):
            self.assertEqual(job["uses"], "./.github/workflows/reusable-bump-version-pr.yml")
            self.assertEqual(job["with"]["base_sha"], "${{ github.sha }}")
        self.assertEqual(jobs["check_release"]["needs"], ["release_context", "prepare_release"])
        self.assertEqual(jobs["publish_release"]["needs"], ["prepare_release", "check_release"])
        bump = self.github_config("workflows/reusable-bump-version-pr.yml")
        self.assertNotEqual(
            draft["concurrency"]["group"], bump["jobs"]["bump_version_pr"]["concurrency"]["group"]
        )

    def test_version_pr_is_excluded_from_release_resolution(self):
        config = self.github_config("release-drafter.yml")
        category = next(category for category in config["categories"] if category.get("type") == "pre-exclude")
        version_condition = next(condition for condition in category["when"] if "conventional" in condition)
        self.assertEqual(version_condition["conventional"], {"type": "chore", "scope": "release"})
        self.assertEqual(version_condition["paths"], ["CMakeLists.txt", "vcpkg.json"])
        self.assertEqual(version_condition["paths-mode"], "only")
        label_condition = next(condition for condition in category["when"] if "labels" in condition)
        self.assertEqual(label_condition, {"labels": ["invalid", "internal-fix", "misc", "ci"]})

    def test_release_drafter_uses_modern_categories_without_changing_label_rules(self):
        config = self.github_config("release-drafter.yml")
        for field in ("exclude-labels", "exclude-paths", "include-labels", "include-paths", "version-resolver"):
            self.assertNotIn(field, config)
        for category in config["categories"]:
            self.assertNotIn("label", category)
            self.assertNotIn("labels", category)
        changelog = {
            category["title"]: category["when"]["labels"]
            for category in config["categories"]
            if "title" in category
        }
        self.assertEqual(changelog, {
            "BREAKING CHANGES": ["BREAKING CHANGE"],
            "Features": ["feature", "enhancement"],
            "Fixes": ["fix", "bug", "security"],
            "Dependencies": ["dependencies", "renovate"],
            "Documentation": ["document", "documentation"],
        })
        resolvers = [
            category for category in config["categories"] if category.get("type") == "version-resolver"
        ]
        self.assertEqual(
            {category["semver-increment"]: category["when"]["labels"] for category in resolvers if "when" in category},
            {
                "major": ["BREAKING CHANGE", "major"],
                "minor": ["feature", "enhancement", "minor"],
                "patch": ["improvement", "fix", "bug", "security", "patch"],
            },
        )
        self.assertEqual(
            [category for category in resolvers if "when" not in category],
            [{"type": "version-resolver", "semver-increment": "patch"}],
        )

    def test_manual_publication_checks_both_project_versions(self):
        jobs = self.github_config("workflows/release-drafter.yml")["jobs"]
        step = next(step for step in jobs["check_release"]["steps"] if step["name"] == "Check release version")
        cases = (
            ("v0.1.0", "0.1.0", "0.1.0", True),
            ("v0.2.0", "0.1.0", "0.1.0", False),
            ("v0.1.0", "0.1.0", "0.1.1", False),
            ("", "0.1.0", "0.1.0", False),
            ("v", "", "", False),
        )
        for tag, cmake, vcpkg, expected in cases:
            with self.subTest(tag=tag, cmake=cmake, vcpkg=vcpkg):
                result = subprocess.run(
                    ["bash", "-e", "-o", "pipefail", "-c", step["run"]],
                    env=dict(self.env, TAG=tag, CMAKE_VERSION=cmake, VCPKG_VERSION=vcpkg),
                    capture_output=True,
                    text=True,
                    timeout=10,
                )
                self.assertEqual(result.returncode == 0, expected, result.stdout + result.stderr)
                if not expected:
                    self.assertIn("::error::", result.stdout + result.stderr)

    def test_ci_covers_release_runner_and_configuration(self):
        ci = self.github_config("workflows/ci.yml")
        release = self.github_config("workflows/release.yml")
        job = ci["jobs"]["build"]
        self.assertIn({"os": "macos-15", "preset": "release"}, job["strategy"]["matrix"]["include"])
        configure = next(step for step in job["steps"] if step.get("name") == "Configure macOS release")
        release_configure = next(step for step in release["jobs"]["build"]["steps"] if step.get("name") == "Configure")
        self.assertEqual(configure["if"], "matrix.os == 'macos-15' && matrix.preset == 'release'")
        self.assertEqual(configure["run"], release_configure["run"])

    def test_homebrew_sync_follows_publication_and_forwards_secrets(self):
        draft = self.github_config("workflows/release-drafter.yml")
        release = self.github_config("workflows/release.yml")
        homebrew = self.github_config("workflows/homebrew.yml")
        job = release["jobs"]["homebrew"]
        self.assertEqual(job["needs"], "publish")
        self.assertEqual(job["uses"], "./.github/workflows/homebrew.yml")
        self.assertEqual(homebrew["jobs"]["update"]["if"], "github.repository == 'srz-zumix/cpp-actions'")
        self.assertEqual(homebrew["concurrency"]["group"], "cpp-actions-homebrew")
        self.assertEqual(homebrew["concurrency"]["queue"], "max")
        events = homebrew.get("on", homebrew.get("true"))
        for event in ("workflow_dispatch", "workflow_call"):
            tag = events[event]["inputs"]["tag"]
            self.assertTrue(tag["required"])
            self.assertEqual(tag["type"], "string")
        secrets = ("MY_ACTIONS_APP_CLIENT_ID", "MY_ACTIONS_APP_PRIVATE_KEY")
        for secret in secrets:
            self.assertEqual(draft["jobs"]["publish_release"]["secrets"][secret], f"${{{{ secrets.{secret} }}}}")
            self.assertEqual(job["secrets"][secret], f"${{{{ secrets.{secret} }}}}")
            self.assertFalse(events["workflow_call"]["secrets"][secret]["required"])
        steps = homebrew["jobs"]["update"]["steps"]
        prepare = next(step for step in steps if step.get("id") == "prepare")
        token = next(step for step in steps if step.get("id") == "app-token")
        check = next(step for step in steps if step.get("id") == "recheck")
        commit = next(step for step in steps if step.get("name") == "Update Homebrew tap")
        self.assertLess(steps.index(prepare), steps.index(check))
        self.assertLess(steps.index(check), steps.index(token))
        self.assertLess(steps.index(token), steps.index(commit))
        self.assertEqual(check["if"], "steps.prepare.outputs.eligible == 'true'")
        self.assertEqual(token["if"], "steps.recheck.outputs.current == 'true'")
        self.assertEqual(commit["if"], "steps.recheck.outputs.current == 'true'")
        self.assertEqual(commit["env"]["EXPECTED_HEAD"], "${{ steps.recheck.outputs.head_sha }}")
        self.assertEqual(commit["env"]["GH_TOKEN"], "${{ steps.app-token.outputs.token }}")
        self.assertEqual(commit["env"]["RELEASE_TOKEN"], "${{ github.token }}")
        self.assertEqual(token["with"]["owner"], "srz-zumix")
        self.assertEqual(token["with"]["repositories"], "homebrew-tap")
        self.assertEqual(token["with"]["permission-contents"], "write")
        build_steps = release["jobs"]["build"]["steps"]
        homebrew_test = next(step for step in build_steps if step.get("name") == "Test Homebrew package")
        upload = next(step for step in build_steps if step.get("name") == "Upload release files")
        self.assertLess(build_steps.index(homebrew_test), build_steps.index(upload))
        self.assertIn("brew install --formula", homebrew_test["run"])
        self.assertIn("brew test ", homebrew_test["run"])

    def test_stale_bases_cannot_create_version_pr(self):
        steps = self.github_config("workflows/reusable-bump-version-pr.yml")["jobs"]["bump_version_pr"]["steps"]
        base = next(step for step in steps if step.get("name") == "Check release base")
        update = next(step for step in steps if step.get("name") == "Update project versions")
        recheck = next(step for step in steps if step.get("name") == "Recheck release base")
        create = next(step for step in steps if step.get("name") == "Create or update version PR")
        self.assertLess(steps.index(base), steps.index(update))
        self.assertLess(steps.index(update), steps.index(recheck))
        self.assertLess(steps.index(recheck), steps.index(create))
        self.assertEqual(update["if"], "steps.base.outputs.current == 'true'")
        self.assertEqual(recheck["if"], "steps.base.outputs.current == 'true'")
        self.assertEqual(create["if"], "steps.recheck.outputs.current == 'true'")
        self.assertEqual(create["with"]["base"], "main")
        self.assertEqual(create["with"]["branch"], "bump/version")
        self.assertTrue(create["with"]["sign-commits"])
        self.assertEqual(create["with"]["add-paths"].splitlines(), ["CMakeLists.txt", "vcpkg.json"])

        git = self.root / "bin" / "git"
        git.write_text(
            '#!/bin/sh\n'
            '[ "$#" -eq 2 ] && [ "$1" = rev-parse ] && [ "$2" = HEAD ] || exit 1\n'
            'printf "%s\\n" "$FAKE_CHECKOUT"\n'
        )
        git.chmod(0o755)
        scripts = self.root / "scripts"
        scripts.mkdir()
        shutil.copyfile(SCRIPT, scripts / "release-version.py")
        output = self.root / "github-output"
        env = dict(self.env, EXPECTED_SHA=SHA, GITHUB_OUTPUT=str(output), FAKE_CHECKOUT=SHA)

        def run_step(step):
            output.write_text("")
            result = subprocess.run(
                ["bash", "-e", "-o", "pipefail", "-c", step["run"]],
                cwd=self.root,
                env=env,
                capture_output=True,
                text=True,
                timeout=10,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            return output.read_text().strip()

        env.update(FAKE_CHECKOUT=OTHER_SHA, FAIL_HEAD="1")
        self.assertEqual(run_step(base), "current=false")
        env.update(FAKE_CHECKOUT=SHA, FAKE_HEAD=SHA)
        env.pop("FAIL_HEAD")
        self.assertEqual(run_step(base), "current=true")
        env["FAKE_HEAD"] = OTHER_SHA
        self.assertEqual(run_step(recheck), "current=false")


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--fake-gh":
        sys.exit(fake_gh())
    if len(sys.argv) < 2:
        sys.exit("Usage: release_version_test.py PATH_TO_RELEASE_VERSION_SCRIPT")
    SCRIPT = str(Path(sys.argv.pop(1)).resolve())
    if sys.version_info < (3, 7) or shutil.which("bash") is None:
        print("Skipping release version tests: Python 3.7+ and bash are required")
        sys.exit(77)
    unittest.main()
