#!/bin/bash
set -euo pipefail

installer="$1"
for tool in jq shasum tar; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    printf 'Skipping installer tests: %s is unavailable\n' "$tool"
    exit 77
  fi
done
installer="$(cd "$(dirname "$installer")" && pwd -P)/$(basename "$installer")"
temp_root="$(cd "${TMPDIR:-/tmp}" && pwd -P)"
test_root="$(mktemp -d "$temp_root/cpa-python-script-tests.XXXXXX")"
trap 'rm -rf -- "$test_root"' EXIT
mkdir -p "$test_root/bin" "$test_root/tmp" "$test_root/releases"
export FIXTURE="$test_root" TMPDIR="$test_root/tmp"
export PATH="$test_root/bin:$PATH"
release_root=https://github.com/actions/python-versions/releases/download

cat > "$test_root/bin/uname" <<'EOF'
#!/bin/bash
case "$1" in -s) echo "${TEST_OS:-Darwin}" ;; -m) echo "${TEST_ARCH:-arm64}" ;; *) exit 1 ;; esac
EOF
cat > "$test_root/bin/id" <<'EOF'
#!/bin/bash
[ "$1" = -u ] || exit 1
echo "${TEST_UID:-1000}"
EOF
cat > "$test_root/bin/pkgutil" <<'EOF'
#!/bin/bash
[ "$1" = --check-signature ] && [ -s "$2" ] || exit 1
if [ "${BAD_SIGNATURE:-0}" = 1 ]; then echo 'Invalid package signature' >&2; exit 1; fi
echo 'Developer ID Installer: Python Software Foundation (test fixture)'
EOF
cat > "$test_root/bin/sudo" <<'EOF'
#!/bin/bash
echo 'Unexpected sudo invocation' >> "$FIXTURE/executed"
exit 99
EOF
cat > "$test_root/bin/curl" <<'EOF'
#!/bin/bash
set -eu
output='' url=''
while [ "$#" -gt 0 ]; do
  case "$1" in
    --output) output="$2"; shift 2 ;;
    --proto|--proto-redir|--connect-timeout|--max-time|--retry) shift 2 ;;
    --fail|--location|--silent|--show-error) shift ;;
    *) url="$1"; shift ;;
  esac
done
printf '%s\n' "$url" >> "$FIXTURE/network"
root=https://github.com/actions/python-versions/releases/download
case "$url" in
  https://raw.githubusercontent.com/actions/python-versions/main/versions-manifest.json)
    cp "$FIXTURE/manifest.json" "$output" ;;
  "$root/"*)
    relative="${url#"$root/"}"
    cp "$FIXTURE/releases/$relative" "$output" ;;
  *) echo "Unexpected network URL: $url" >&2; exit 1 ;;
esac
EOF
chmod +x "$test_root/bin/uname" "$test_root/bin/id" "$test_root/bin/pkgutil" \
  "$test_root/bin/sudo" "$test_root/bin/curl"

jq -n --arg root "$release_root" '
  ["3.11.9", "3.11.10", "3.12.0", "3.13.0rc1"] |
  map(. as $version | {version: ., stable: (. != "3.13.0rc1"),
    files: (["arm64", "x64"] | map(. as $arch |
      {arch: ., platform: "darwin",
       filename: ("python-" + $version + "-darwin-" + $arch + ".tar.gz"),
       download_url: ($root + "/" + $version + "-123/python-" +
         $version + "-darwin-" + $arch + ".tar.gz")}))})' > "$test_root/manifest.json"
cp "$test_root/manifest.json" "$test_root/original-manifest.json"

make_release() {
  local version="$1" architecture="$2" stage="$test_root/stage-$1-$2"
  local tag="$version-123" asset="python-$version-darwin-$architecture.tar.gz" checksum
  mkdir -p "$stage" "$test_root/releases/$tag"
  printf 'PYTHON_FULL_VERSION="%s"\nPYTHON_PKG_NAME="python-%s-macos11.pkg"\nARCH="%s"\n' \
    "$version" "$version" "$architecture" > "$stage/setup.sh"
  cat >> "$stage/setup.sh" <<'EOF'
echo 'Unexpected upstream execution' >> "$FIXTURE/executed"
exit 99
EOF
  printf 'signed package fixture\n' > "$stage/python-$version-macos11.pkg"
  tar -czf "$test_root/releases/$tag/$asset" -C "$stage" .
  checksum="$(shasum -a 256 "$test_root/releases/$tag/$asset")"
  printf '%s %s\n' "${checksum%% *}" "$asset" >> "$test_root/releases/$tag/hashes.sha256"
}
for version in 3.11.9 3.11.10 3.12.0; do
  for architecture in arm64 x64; do make_release "$version" "$architecture"; done
done

assert_contains() {
  grep -F -- "$2" "$1" >/dev/null || {
    printf 'Missing expected output: %s\n' "$2" >&2
    cat "$1" >&2
    exit 1
  }
}

run_ok() {
  local name="$1"; shift
  if ! /bin/bash "$installer" "$@" > "$test_root/$name.out" 2>&1; then
    cat "$test_root/$name.out" >&2
    exit 1
  fi
}

run_fail() {
  local name="$1" expected="$2"; shift 2
  if /bin/bash "$installer" "$@" > "$test_root/$name.out" 2>&1; then
    printf 'Unexpected success: %s\n' "$name" >&2
    exit 1
  fi
  assert_contains "$test_root/$name.out" "$expected"
}

run_ok help --help
[ ! -e "$test_root/network" ]
run_fail missing 'Select a stable CPython version'
run_fail invalid 'Select a stable CPython version' --version '3.11; unsafe' --dry-run
run_fail option 'Unknown option' --version 3.11 --replace
run_fail consent 'explicitly specify --accept-system-changes' --version 3.11
TEST_OS=Linux run_fail platform 'macOS only' --version 3.11 --dry-run
TEST_ARCH=unknown run_fail architecture 'Unsupported process architecture' --version 3.11 --dry-run
run_fail mismatch 'Use a terminal/process matching x64' --version 3.11 --architecture x64 --dry-run
TEST_UID=0 run_fail root 'not with sudo bash' --version 3.11 --accept-system-changes
RUNNER_NAME=cpp-actions run_fail sandbox 'not from cpp-actions or setup-steps' --version 3.11 --dry-run
[ ! -e "$test_root/network" ]

run_ok minor --version 3.11 --dry-run
assert_contains "$test_root/minor.out" 'Selected Python 3.11.10 (arm64)'
assert_contains "$test_root/minor.out" 'no installer, sudo, cache writes'
run_ok exact --version 3.11.9 --dry-run
assert_contains "$test_root/exact.out" 'Selected Python 3.11.9 (arm64)'
TEST_ARCH=x86_64 run_ok x64 --version 3.12 --architecture x64 --dry-run
assert_contains "$test_root/x64.out" 'Selected Python 3.12.0 (x64)'
run_fail unavailable 'No published stable macOS release matches' --version 3.99 --dry-run

cp "$test_root/releases/3.11.9-123/hashes.sha256" "$test_root/original-hashes.sha256"
printf '%064d %s\n' 0 python-3.11.9-darwin-arm64.tar.gz > "$test_root/releases/3.11.9-123/hashes.sha256"
run_fail checksum 'SHA-256 does not match' --version 3.11.9 --dry-run
cat "$test_root/original-hashes.sha256" "$test_root/original-hashes.sha256" \
  > "$test_root/releases/3.11.9-123/hashes.sha256"
run_fail duplicate 'Missing, duplicate or invalid published SHA-256' --version 3.11.9 --dry-run
cp "$test_root/original-hashes.sha256" "$test_root/releases/3.11.9-123/hashes.sha256"

jq 'map(if .version == "3.11.9" then
  .files |= map(.download_url = "https://example.invalid/python.tar.gz") else . end)' \
  "$test_root/original-manifest.json" > "$test_root/manifest.json"
run_fail url 'not an official Python asset' --version 3.11.9 --dry-run
cp "$test_root/original-manifest.json" "$test_root/manifest.json"
BAD_SIGNATURE=1 run_fail signature 'signature verification failed' --version 3.11.9 --dry-run

mkdir -p "$test_root/stage-3.11.9-arm64/nested"
printf unsafe > "$test_root/stage-3.11.9-arm64/nested/file"
tar -czf "$test_root/releases/3.11.9-123/python-3.11.9-darwin-arm64.tar.gz" \
  -C "$test_root/stage-3.11.9-arm64" .
checksum="$(shasum -a 256 "$test_root/releases/3.11.9-123/python-3.11.9-darwin-arm64.tar.gz")"
printf '%s %s\n' "${checksum%% *}" python-3.11.9-darwin-arm64.tar.gz \
  > "$test_root/releases/3.11.9-123/hashes.sha256"
run_fail layout 'Unsupported archive layout' --version 3.11.9 --dry-run
[ ! -e "$test_root/executed" ]
[ -z "$(find "$test_root/tmp" -mindepth 1 -print -quit)" ]

helper_paths() {
  # Source only helpers; these paths are deliberately not CLI/environment options.
  # shellcheck disable=SC1090,SC1091
  source "$installer"
  CACHE_ROOT="$helper_root/cache"
  FRAMEWORK_ROOT="$helper_root/Python.framework"
  APPLICATION_ROOT="$helper_root/Applications"
  COMMAND_ROOT="$helper_root/commands"
  # shellcheck disable=SC2034
  RELEASE_ROOT="$release_root"
  TEMP_ROOT="$helper_root"
  WORK_DIR="$(mktemp -d "$TEMP_ROOT/cpp-actions-python.XXXXXX")"
  LOCK_DIR=''
  mkdir -p "$CACHE_ROOT" "$APPLICATION_ROOT" "$COMMAND_ROOT"
}

empty_cache() { check_cache; check_conflicts 3.11.9; }
framework_conflict() { mkdir "$FRAMEWORK_ROOT"; check_conflicts 3.11.9; }
command_conflict() { ln -s "$helper_root/missing" "$COMMAND_ROOT/python3"; check_conflicts 3.11.9; }
version_conflict() { mkdir -p "$CACHE_ROOT/Python/3.11.8"; check_conflicts 3.11.9; }
cache_symlink() {
  mkdir "$helper_root/target"
  ln -s "$helper_root/target" "$CACHE_ROOT/Python"
  check_cache
}
lock_cleanup() {
  lock_cache
  printf retained > "$CACHE_ROOT/sentinel"
  cleanup
  [ ! -e "$LOCK_DIR" ] && [ ! -e "$WORK_DIR" ]
  [ -f "$CACHE_ROOT/sentinel" ]
  LOCK_DIR='' WORK_DIR=''
}
lock_conflict() { lock_cache; lock_cache; }
unowned_lock() {
  mkdir "$CACHE_ROOT/.cpp-actions-python-install.lock"
  touch "$CACHE_ROOT/.cpp-actions-python-install.lock/owner"
  lock_cache
}
missing_marker() {
  mkdir -p "$CACHE_ROOT/Python/3.11.9/arm64"
  verify_installation 3.11.9 arm64
}
valid_installation() {
  local version="${1:-3.11.9}" minor installation component
  minor="${version%.*}"
  installation="$CACHE_ROOT/Python/$version/arm64"
  mkdir -p "$installation" "$FRAMEWORK_ROOT/Versions/$minor/bin" \
    "$FRAMEWORK_ROOT/Versions/$minor/include" "$FRAMEWORK_ROOT/Versions/$minor/share" \
    "$FRAMEWORK_ROOT/Versions/$minor/lib"
  for component in bin include share lib; do
    ln -s "$FRAMEWORK_ROOT/Versions/$minor/$component" "$installation/$component"
  done
  touch "$installation.complete"
  # shellcheck disable=SC2016
  printf '#!/bin/bash\n[ "$1" = -I ] && [ "$2" = -B ] && [ "$3" = -c ] && [ "$5" = "%s" ] && [ "$6" = arm64 ]\n' \
    "$version" > "$FRAMEWORK_ROOT/Versions/$minor/bin/python"
  chmod +x "$FRAMEWORK_ROOT/Versions/$minor/bin/python"
  verify_installation "$version" arm64
}
installer_environment() {
  export EXPECTED_CACHE="$CACHE_ROOT" AGENT_TOOLSDIRECTORY="$helper_root/wrong"
  cat > "$WORK_DIR/setup.sh" <<'EOF'
set -eu
[ -z "${AGENT_TOOLSDIRECTORY+x}" ]
[ "$RUNNER_TOOL_CACHE" = "$EXPECTED_CACHE" ]
echo installed > "$RUNNER_TOOL_CACHE/sentinel"
EOF
  invoke_installer
  [ -f "$CACHE_ROOT/sentinel" ] && [ ! -e "$helper_root/wrong" ]
}
installer_failure() {
  printf 'echo upstream-error >&2\nexit 7\n' > "$WORK_DIR/setup.sh"
  invoke_installer
}

provision_fixture() {
  local version="${1:-3.11.10}" asset
  asset="python-$version-darwin-arm64.tar.gz"
  provision_release "$version" arm64 "$asset" "$release_root/$version-123/$asset" "$version-123" false
}
existing_orchestration() { valid_installation; provision_fixture 3.11.9; }
incomplete_orchestration() {
  mkdir -p "$CACHE_ROOT/Python/3.11.10/arm64"
  touch "$CACHE_ROOT/Python/3.11.10/arm64/sentinel"
  provision_fixture
}
late_conflict_orchestration() {
  # Simulate a conflicting command appearing during release verification.
  # shellcheck disable=SC2329
  inspect_archive() { touch "$COMMAND_ROOT/python3"; }
  provision_fixture
}
failure_orchestration() {
  # shellcheck disable=SC2329
  inspect_archive() {
    # shellcheck disable=SC2016
    printf 'echo upstream-error >&2\ntouch "$RUNNER_TOOL_CACHE/partial"\nexit 7\n' > "$WORK_DIR/setup.sh"
  }
  provision_fixture
}
redirected_link() {
  valid_installation
  rm "$CACHE_ROOT/Python/3.11.9/arm64/bin"
  ln -s "$helper_root/wrong" "$CACHE_ROOT/Python/3.11.9/arm64/bin"
  verify_installation 3.11.9 arm64
}
wrong_runtime_version() {
  valid_installation 3.0.0
  printf '#!/bin/bash\nexec "%s" "$@"\n' "$TEST_PYTHON" > "$FRAMEWORK_ROOT/Versions/3.0/bin/python"
  verify_installation 3.0.0 arm64
}
wrong_runtime_architecture() {
  local version architecture installation minor
  version="$("$TEST_PYTHON" -I -B -c 'import platform; print(platform.python_version())')"
  architecture="$("$TEST_PYTHON" -I -B -c 'import platform; print(platform.machine())')"
  case "$architecture" in arm64) architecture=x64 ;; *) architecture=arm64 ;; esac
  valid_installation "$version"
  minor="${version%.*}"
  printf '#!/bin/bash\nexec "%s" "$@"\n' "$TEST_PYTHON" > "$FRAMEWORK_ROOT/Versions/$minor/bin/python"
  installation="$CACHE_ROOT/Python/$version/$architecture"
  if [ "$architecture" != arm64 ]; then
    mv "$CACHE_ROOT/Python/$version/arm64" "$installation"
    mv "$CACHE_ROOT/Python/$version/arm64.complete" "$installation.complete"
  fi
  verify_installation "$version" "$architecture"
}

run_helper() {
  local name="$1" expected="$2"; shift 2
  local helper_root status
  helper_root="$(mktemp -d "$test_root/helper.XXXXXX")"
  last_helper_root="$helper_root"
  set +e
  (set -e; trap - EXIT; helper_paths; trap on_exit EXIT; "$@") > "$test_root/$name.out" 2>&1
  status=$?
  set -e
  if [ "$status" -eq 0 ]; then
    [ -z "$expected" ] || { echo "Unexpected helper success: $name" >&2; exit 1; }
  else
    if [ -z "$expected" ]; then cat "$test_root/$name.out" >&2; exit 1; fi
    assert_contains "$test_root/$name.out" "$expected"
  fi
  [ -z "$(find "$helper_root" -name 'cpp-actions-python.*' -print -quit)" ]
}
run_helper empty '' empty_cache
run_helper framework 'existing system installation' framework_conflict
run_helper command 'existing command' command_conflict
run_helper version 'existing Python 3.11 cache' version_conflict
run_helper cache-link 'must not be a symlink' cache_symlink
run_helper cleanup '' lock_cleanup
run_helper lock 'Another provisioning operation' lock_conflict
run_helper marker 'missing or symlinked installation entry' missing_marker
run_helper installed '' valid_installation
run_helper environment '' installer_environment
run_helper partial 'host changes may remain' installer_failure
assert_contains "$test_root/partial.out" 'upstream-error'
run_helper unowned 'Another provisioning operation' unowned_lock
[ -f "$last_helper_root/cache/.cpp-actions-python-install.lock/owner" ]
run_helper already '' existing_orchestration
assert_contains "$test_root/already.out" 'Already installed; no changes made'
run_helper incomplete 'Existing installation is incomplete' incomplete_orchestration
[ -f "$last_helper_root/cache/Python/3.11.10/arm64/sentinel" ]
[ ! -e "$last_helper_root/cache/.cpp-actions-python-install.lock" ]
run_helper late 'Refusing to replace an existing command' late_conflict_orchestration
[ -f "$last_helper_root/commands/python3" ]
[ ! -e "$last_helper_root/cache/.cpp-actions-python-install.lock" ]
[ ! -e "$test_root/executed" ]
run_helper orchestration-failure 'host changes may remain' failure_orchestration
[ -f "$last_helper_root/cache/partial" ]
[ ! -e "$last_helper_root/cache/.cpp-actions-python-install.lock" ]
run_helper redirected 'unexpected framework link' redirected_link
if TEST_PYTHON="$(command -v python3)"; then
  run_helper runtime-version 'Installed Python version/architecture does not match' wrong_runtime_version
  run_helper runtime-architecture 'Installed Python version/architecture does not match' wrong_runtime_architecture
else
  printf 'Skipping real-interpreter validation-expression tests: python3 is unavailable.\n'
fi

printf 'Installer tests passed (offline fixtures; no host installer or sudo executed).\n'
