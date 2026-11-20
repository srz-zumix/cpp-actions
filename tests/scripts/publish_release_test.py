#!/usr/bin/env python3
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
SHA = "a" * 40
OTHER_SHA = "b" * 40
REPO = "fixture-owner/fixture-repo"
RELEASE_ID = "123"
ARCHIVE = f"cpp-actions-{TAG}-macos-arm64.tar.gz"
UPLOAD_URL = f"https://uploads.github.com/repos/{REPO}/releases/{RELEASE_ID}/assets"


def fake_gh():
    state_path = Path(os.environ["FAKE_GH_STATE"])
    state = json.loads(state_path.read_text())
    arguments = sys.argv[2:]
    if not arguments or arguments.pop(0) != "api":
        raise RuntimeError("Unexpected gh command")
    method, endpoint, query, input_file = "GET", "", "", ""
    fields = {}
    while arguments:
        argument = arguments.pop(0)
        if argument in ("--method", "-X"):
            method = arguments.pop(0)
        elif argument == "--input":
            input_file = arguments.pop(0)
        elif argument == "--jq":
            query = arguments.pop(0)
        elif argument in ("-f", "-F"):
            key, value = arguments.pop(0).split("=", 1)
            fields[key] = value
        elif argument == "-H":
            arguments.pop(0)
        elif argument not in ("--paginate", "--slurp", "--silent"):
            endpoint = argument

    state["calls"].append(f"{method} {endpoint}")
    response = None
    status = 0
    repo_path = f"repos/{REPO}"
    if state.get("fail_endpoint") == endpoint:
        status = 1
    elif endpoint == f"{repo_path}/git/matching-refs/tags/{TAG}":
        response = []
        if state["tag_sha"]:
            response.append({"ref": f"refs/tags/{TAG}", "object": {"type": state["tag_type"]}})
    elif endpoint == f"{repo_path}/commits/{TAG}":
        response = {"sha": state["tag_sha"]}
    elif endpoint == f"{repo_path}/git/refs" and method == "POST":
        if state.get("fail_tag_create") or state["tag_sha"]:
            status = 1
        else:
            assert fields == {"ref": f"refs/tags/{TAG}", "sha": SHA}
            state["tag_sha"] = SHA
            response = {}
    elif endpoint == f"{repo_path}/releases?per_page=100":
        releases = [state["release"]] if state["release_exists"] else []
        response = [releases + state.get("extra_releases", [])]
    elif endpoint == f"{repo_path}/releases" and method == "POST":
        assert fields == {
            "tag_name": TAG,
            "target_commitish": SHA,
            "draft": "true",
            "generate_release_notes": "true",
        }
        state["release_exists"] = True
        response = state["release"]
    elif endpoint == f"{repo_path}/releases/{RELEASE_ID}/assets?per_page=100":
        response = [state["assets"]]
    elif endpoint == f"{repo_path}/releases/{RELEASE_ID}" and method == "GET":
        if not state["release_exists"]:
            status = 1
        else:
            response = state["release"]
    elif endpoint == f"{repo_path}/releases/{RELEASE_ID}" and method == "PATCH":
        assert fields == {"draft": "false"}
        state["release"]["draft"] = False
        response = state["release"]
    elif endpoint.startswith(f"{UPLOAD_URL}?name=") and method == "POST":
        name = endpoint.split("?name=", 1)[1]
        assert input_file == name and Path(input_file).is_file()
        if name == state.get("fail_upload"):
            status = 1
        else:
            state["assets"].append({"name": name, "state": "uploaded"})
            if state.get("move_tag_on_upload") and name.endswith(".sha256"):
                state["tag_sha"] = OTHER_SHA
            response = {}
    else:
        raise RuntimeError(f"Unexpected gh API request: {method} {endpoint}")

    state_path.write_text(json.dumps(state))
    if status:
        print(f"Simulated API error: {endpoint}", file=sys.stderr)
        return status
    if query:
        response = response[query[1:]]
    if response is not None:
        print(response if isinstance(response, str) else json.dumps(response))
    return 0


class PublishReleaseTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="cpa-release-test-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.state_path = self.root / "state.json"
        self.files = self.root / "release"
        self.files.mkdir()
        payload = b"release archive fixture\n"
        (self.files / ARCHIVE).write_bytes(payload)
        (self.files / f"{ARCHIVE}.sha256").write_text(
            f"{hashlib.sha256(payload).hexdigest()}  {ARCHIVE}\n"
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
            FAKE_GH_STATE=str(self.state_path),
            GH_REPO=REPO,
            TAG=TAG,
            EXPECTED_SHA=SHA,
            RELEASE_ID=RELEASE_ID,
            ARTIFACT_DIR=str(self.files),
        )
        self.save(
            {
                "calls": [],
                "tag_sha": None,
                "tag_type": "commit",
                "release_exists": True,
                "release": {
                    "id": int(RELEASE_ID),
                    "tag_name": TAG,
                    "target_commitish": SHA,
                    "draft": True,
                    "body": "Release notes fixture",
                    "upload_url": f"{UPLOAD_URL}{{?name,label}}",
                },
                "assets": [],
            }
        )

    def save(self, state):
        self.state_path.write_text(json.dumps(state))

    def state(self):
        return json.loads(self.state_path.read_text())

    def run_script(self, success):
        result = subprocess.run(
            ["bash", SCRIPT],
            env=self.env,
            capture_output=True,
            text=True,
            timeout=30,
        )
        if success:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def assert_unpublished(self):
        state = self.state()
        self.assertTrue(state["release"]["draft"])
        self.assertFalse(any(call.startswith("PATCH ") for call in state["calls"]))

    def test_manual_release_creates_tag_and_preserves_notes(self):
        self.run_script(True)
        state = self.state()
        self.assertEqual(state["tag_sha"], SHA)
        self.assertFalse(state["release"]["draft"])
        self.assertEqual(state["release"]["body"], "Release notes fixture")
        self.assertEqual([asset["name"] for asset in state["assets"]], [ARCHIVE, ARCHIVE + ".sha256"])

    def test_matching_annotated_tag_is_accepted(self):
        state = self.state()
        state.update(tag_sha=SHA, tag_type="tag")
        self.save(state)
        self.run_script(True)
        self.assertFalse(any(call.endswith("/git/refs") for call in self.state()["calls"]))

    def test_moved_tag_draft_and_api_errors_fail_closed(self):
        cases = (
            {"tag_sha": OTHER_SHA},
            {"fail_endpoint": f"repos/{REPO}/git/matching-refs/tags/{TAG}"},
            {"release": {"tag_name": "v0.2.0"}},
            {"release": {"target_commitish": OTHER_SHA}},
            {"release": {"draft": False}},
        )
        original = self.state()
        for changes in cases:
            with self.subTest(changes=changes):
                state = json.loads(json.dumps(original))
                for key, value in changes.items():
                    if key == "release":
                        state["release"].update(value)
                    else:
                        state[key] = value
                self.save(state)
                self.run_script(False)
                state = self.state()
                self.assertFalse(any(call.startswith("PATCH ") for call in state["calls"]))
                self.assertEqual(state["assets"], [])

    def test_checksum_error_makes_no_api_requests(self):
        (self.files / ARCHIVE).write_bytes(b"corrupted")
        self.run_script(False)
        self.assertEqual(self.state()["calls"], [])

    def test_invalid_inputs_make_no_api_requests(self):
        original = dict(self.env)
        for key, value in (("TAG", "v01.1.0"), ("EXPECTED_SHA", "not-a-sha"), ("RELEASE_ID", "bad")):
            with self.subTest(key=key):
                self.env = dict(original, **{key: value})
                self.run_script(False)
                self.assertEqual(self.state()["calls"], [])

    def test_partial_upload_requires_cleanup_before_retry(self):
        state = self.state()
        state["fail_upload"] = ARCHIVE + ".sha256"
        self.save(state)
        self.run_script(False)
        self.assert_unpublished()
        state = self.state()
        self.assertIsNone(state["tag_sha"])
        self.assertEqual([asset["name"] for asset in state["assets"]], [ARCHIVE])
        state.pop("fail_upload")
        self.save(state)
        self.run_script(False)
        self.assert_unpublished()
        state = self.state()
        state["assets"] = []
        self.save(state)
        self.run_script(True)
        self.assertFalse(self.state()["release"]["draft"])

    def test_tag_changes_and_creation_failure_do_not_publish(self):
        original = self.state()
        for changes in ({"move_tag_on_upload": True}, {"fail_tag_create": True}):
            with self.subTest(changes=changes):
                state = json.loads(json.dumps(original))
                state.update(changes)
                self.save(state)
                self.run_script(False)
                self.assert_unpublished()

    def test_tag_push_creates_or_reuses_draft(self):
        self.env["RELEASE_ID"] = ""
        original = self.state()
        for exists in (False, True):
            with self.subTest(existing_draft=exists):
                state = json.loads(json.dumps(original))
                state.update(tag_sha=SHA, release_exists=exists)
                state["release"]["target_commitish"] = "main" if exists else SHA
                self.save(state)
                self.run_script(True)
                state = self.state()
                self.assertFalse(state["release"]["draft"])
                self.assertEqual(state["release"]["body"], "Release notes fixture")
                creates = [call for call in state["calls"] if call == f"POST repos/{REPO}/releases"]
                self.assertEqual(len(creates), 0 if exists else 1)

    def test_tag_push_requires_existing_tag_and_rejects_published_release(self):
        self.env["RELEASE_ID"] = ""
        original = self.state()
        for changes in ({"tag_sha": None}, {"tag_sha": SHA, "release": {"draft": False}}):
            with self.subTest(changes=changes):
                state = json.loads(json.dumps(original))
                if "release" in changes:
                    state["release"].update(changes["release"])
                state.update({key: value for key, value in changes.items() if key != "release"})
                self.save(state)
                self.run_script(False)
                state = self.state()
                self.assertFalse(any(call.startswith("PATCH ") for call in state["calls"]))
                self.assertEqual(state["assets"], [])

    def test_tag_push_rejects_multiple_matching_drafts(self):
        self.env["RELEASE_ID"] = ""
        state = self.state()
        state["tag_sha"] = SHA
        state["extra_releases"] = [dict(state["release"], id=456)]
        self.save(state)
        self.run_script(False)
        self.assert_unpublished()
        self.assertEqual(self.state()["assets"], [])


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--fake-gh":
        sys.exit(fake_gh())
    if len(sys.argv) < 2:
        sys.exit("Usage: publish_release_test.py PATH_TO_PUBLISH_SCRIPT")
    SCRIPT = str(Path(sys.argv.pop(1)).resolve())
    if sys.version_info < (3, 7) or any(shutil.which(tool) is None for tool in ("bash", "jq", "shasum")):
        print("Skipping release publisher tests: Python 3.7+, bash, jq, and shasum are required")
        sys.exit(77)
    unittest.main()
