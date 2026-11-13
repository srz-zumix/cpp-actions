#!/bin/bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: bash scripts/install-hosted-python.sh --version VERSION [OPTIONS]

Prepare stable CPython for actions/setup-python on macOS, outside cpp-actions.
VERSION is 3.MINOR (latest published compatible patch) or 3.MINOR.PATCH.
Only official package-based macOS releases are supported, not source archives,
PyPy, GraalPy, prereleases or free-threaded Python.

Options:
  --version VERSION          Required Python version
  --architecture ARCH        arm64 or x64; must match this terminal's architecture
  --dry-run                  Download, verify and inspect without installing
  --accept-system-changes    Acknowledge host changes and permit installation
  -h, --help                 Show this help

Run as the same OS user as cpp-actions, not as root or inside setup-steps.
The upstream script invokes sudo itself. It installs a system Python.framework,
applications, /usr/local/bin links, pip and certificates, and may change shell
profiles. Existing conflicting installations are never overwritten.
Stop other cpp-actions jobs and Python installers before proceeding.

The cache must be a real directory, readable/writable/searchable by this user:
  /Users/runner/hostedtoolcache
If needed, have an administrator create it and assign appropriate permissions.
This script never automatically changes existing ownership or permissions.

Examples:
  bash scripts/install-hosted-python.sh --version 3.11 --dry-run
  bash scripts/install-hosted-python.sh --version 3.11.9 --accept-system-changes
EOF
}

die() {
  printf 'Error: %s\n' "$*" >&2
  exit 1
}

download() {
  printf 'Downloading %s\n' "$1"
  curl --fail --location --silent --show-error --proto '=https' \
    --proto-redir '=https' --connect-timeout 15 --max-time 300 --retry 3 \
    --output "$2" "$1" || die "Download failed: $1"
}

cleanup() {
  local failed=0
  if [ -n "${LOCK_DIR:-}" ]; then
    rmdir "$LOCK_DIR" || { printf 'Error: could not remove lock %s\n' "$LOCK_DIR" >&2; failed=1; }
  fi
  if [ -n "${WORK_DIR:-}" ]; then
    # WORK_DIR is a private, canonical directory created by mktemp below TEMP_ROOT.
    case "$WORK_DIR" in
      "$TEMP_ROOT"/cpp-actions-python.*)
        rm -rf -- "$WORK_DIR" || { printf 'Error: could not clean %s\n' "$WORK_DIR" >&2; failed=1; }
        ;;
      *) printf 'Error: refusing to clean unexpected path %s\n' "$WORK_DIR" >&2; failed=1 ;;
    esac
  fi
  return "$failed"
}

on_exit() {
  local status=$?
  cleanup || status=1
  exit "$status"
}

resolve_release() {
  jq -er --arg requested "$1" --arg architecture "$2" '
    [.[] |
      select(.stable == true and (.version | test("^3\\.[0-9]+\\.[0-9]+$"))) |
      select(.version == $requested or
        (($requested | split(".") | length) == 2 and
         (.version | startswith($requested + ".")))) |
      . as $release | .files[] |
      select(.platform == "darwin" and .arch == $architecture) |
      {version: $release.version, filename, download_url}] |
    sort_by(.version | split(".") | map(tonumber)) |
    last // error("No matching stable macOS release")' "$3"
}

check_cache() {
  local path="$CACHE_ROOT"
  while [ "$path" != / ]; do
    [ ! -L "$path" ] || die "Cache paths must not contain symlinks: $path"
    path="${path%/*}"
    [ -n "$path" ] || path=/
  done
  [ -d "$CACHE_ROOT" ] || die "Have an administrator provision $CACHE_ROOT for the cpp-actions OS user."
  if [ ! -r "$CACHE_ROOT" ] || [ ! -w "$CACHE_ROOT" ] || [ ! -x "$CACHE_ROOT" ]; then
    die "Have an administrator grant this user read/write/search access to $CACHE_ROOT."
  fi
  [ ! -L "$CACHE_ROOT/Python" ] || die "Python cache directory must not be a symlink."
}

lock_cache() {
  mkdir "$CACHE_ROOT/.cpp-actions-python-install.lock" ||
    die "Another provisioning operation or stale lock exists at $CACHE_ROOT/.cpp-actions-python-install.lock. Inspect it; no lock was removed."
  LOCK_DIR="$CACHE_ROOT/.cpp-actions-python-install.lock"
}

check_conflicts() {
  local version="$1" minor="${1%.*}" existing='' path name
  if [ -d "$CACHE_ROOT/Python" ]; then
    existing="$(find "$CACHE_ROOT/Python" -name "$minor.*" -print -quit)" ||
      die "Could not inspect existing Python caches."
  elif [ -e "$CACHE_ROOT/Python" ]; then
    die "Python cache path is not a directory: $CACHE_ROOT/Python"
  fi
  [ -z "$existing" ] || die "Refusing to replace an existing Python $minor cache: $existing"
  for path in "$FRAMEWORK_ROOT" "$APPLICATION_ROOT/Python $minor"; do
    if [ -e "$path" ] || [ -L "$path" ]; then
      die "Refusing to change an existing system installation: $path"
    fi
  done
  for name in 2to3 "2to3-$minor" idle3 "idle$minor" pydoc3 "pydoc$minor" \
    python3 python3-config python3-intel64 "python$minor" "python$minor-config" \
    "python$minor-intel64" pip pip3 "pip$minor" easy_install "easy_install-$minor"; do
    path="$COMMAND_ROOT/$name"
    if [ -e "$path" ] || [ -L "$path" ]; then
      die "Refusing to replace an existing command: $path"
    fi
  done
  printf 'No conflicting installation found for Python %s.\n' "$version"
}

verify_installation() {
  local version="$1" architecture="$2" minor="${1%.*}"
  local installation="$CACHE_ROOT/Python/$version/$architecture" path component
  for path in "$CACHE_ROOT/Python/$version" "$installation" "$installation.complete"; do
    if [ -L "$path" ] || [ ! -e "$path" ]; then
      printf 'Error: missing or symlinked installation entry: %s\n' "$path" >&2
      return 1
    fi
  done
  [ -f "$installation.complete" ] || {
    printf 'Error: completion marker is not a regular file.\n' >&2
    return 1
  }
  for component in bin include share lib; do
    path="$installation/$component"
    if [ ! -L "$path" ] || [ "$(readlink "$path")" != "$FRAMEWORK_ROOT/Versions/$minor/$component" ]; then
      printf 'Error: unexpected framework link: %s\n' "$path" >&2
      return 1
    fi
  done
  if [ ! -x "$installation/bin/python" ]; then
    printf 'Error: Python executable is unavailable: %s/bin/python\n' "$installation" >&2
    return 1
  fi
  "$installation/bin/python" -I -B -c '
import platform, sqlite3, ssl, sys, venv
actual_arch = {"x86_64": "x64", "arm64": "arm64"}.get(platform.machine())
if platform.python_version() != sys.argv[1] or actual_arch != sys.argv[2]:
    raise SystemExit("Installed Python version/architecture does not match the request")
print("Verified Python " + platform.python_version() + " (" + actual_arch + ")")
' "$version" "$architecture" || {
    printf 'Error: installed Python failed runtime verification.\n' >&2
    return 1
  }
}

inspect_archive() {
  local version="$1" architecture="$2" member name
  local setup_member='' package_member='' package_name='' setups=0 packages=0
  tar -tzf "$WORK_DIR/python.tar.gz" > "$WORK_DIR/members.txt" ||
    die "Could not read release archive."
  while IFS= read -r member; do
    name="${member#./}"
    case "$name" in
      ''|.|build_output.txt) ;;
      setup.sh) setup_member="$member"; setups=$((setups + 1)) ;;
      "python-$version-"*.pkg)
        [[ "$name" =~ ^[A-Za-z0-9._-]+$ ]] || die "Unsafe package filename: $name"
        package_member="$member"; package_name="$name"; packages=$((packages + 1))
        ;;
      *) die "Unsupported archive layout: $member (only package-based macOS releases are supported)." ;;
    esac
  done < "$WORK_DIR/members.txt"
  [ "$setups" -eq 1 ] && [ "$packages" -eq 1 ] ||
    die "Archive must contain exactly one setup.sh and one macOS package."

  # Extract selected members to regular files, never archive-controlled paths.
  tar -xzOf "$WORK_DIR/python.tar.gz" "$setup_member" > "$WORK_DIR/setup.sh" ||
    die "Could not extract setup.sh."
  tar -xzOf "$WORK_DIR/python.tar.gz" "$package_member" > "$WORK_DIR/$package_name" ||
    die "Could not extract the macOS package."
  if ! grep -Fx "PYTHON_FULL_VERSION=\"$version\"" "$WORK_DIR/setup.sh" >/dev/null ||
    ! grep -Fx "ARCH=\"$architecture\"" "$WORK_DIR/setup.sh" >/dev/null ||
    ! grep -Fx "PYTHON_PKG_NAME=\"$package_name\"" "$WORK_DIR/setup.sh" >/dev/null; then
    die "Unsupported installer template or mismatched version/architecture."
  fi
  pkgutil --check-signature "$WORK_DIR/$package_name" > "$WORK_DIR/signature.txt" 2>&1 || {
    cat "$WORK_DIR/signature.txt" >&2
    die "macOS package signature verification failed."
  }
  cat "$WORK_DIR/signature.txt"
  grep -F 'Developer ID Installer: Python Software Foundation' "$WORK_DIR/signature.txt" >/dev/null ||
    die "The package is not signed by the Python Software Foundation."
}

invoke_installer() {
  if ! (cd "$WORK_DIR" &&
    env -u AGENT_TOOLSDIRECTORY RUNNER_TOOL_CACHE="$CACHE_ROOT" bash ./setup.sh); then
    die "Upstream installation failed; host changes may remain. Inspect $FRAMEWORK_ROOT, $APPLICATION_ROOT and $CACHE_ROOT. Do not rerun blindly or create a .complete marker manually; see the README provisioning section."
  fi
}

provision_release() {
  local version="$1" architecture="$2" asset="$3" url="$4" tag="$5" dry_run="$6"
  local checksum actual installation
  if ! "$dry_run"; then
    check_cache
    lock_cache
    installation="$CACHE_ROOT/Python/$version/$architecture"
    if [ -e "$installation" ] || [ -L "$installation" ] ||
      [ -e "$installation.complete" ] || [ -L "$installation.complete" ]; then
      verify_installation "$version" "$architecture" ||
        die "Existing installation is incomplete or incompatible; it was not changed."
      printf 'Already installed; no changes made.\n'
      return
    fi
    check_conflicts "$version"
  fi

  download "$RELEASE_ROOT/$tag/hashes.sha256" "$WORK_DIR/hashes.sha256"
  checksum="$(awk -v asset="$asset" '$2 == asset || $2 == "*" asset {print tolower($1)}' "$WORK_DIR/hashes.sha256")"
  [[ "$checksum" =~ ^[0-9a-f]{64}$ ]] || die "Missing, duplicate or invalid published SHA-256 for $asset."
  download "$url" "$WORK_DIR/python.tar.gz"
  actual="$(shasum -a 256 "$WORK_DIR/python.tar.gz")" || die "Could not calculate SHA-256."
  [ "${actual%% *}" = "$checksum" ] || die "Release archive SHA-256 does not match the published SHA-256."
  inspect_archive "$version" "$architecture"
  printf 'Verified official archive and package signature.\n'
  printf 'Host changes: system Python.framework, applications, command links, shell profiles, pip and certificates.\n'
  if "$dry_run"; then
    printf 'Dry run: no installer, sudo, cache writes or host installation checks were performed.\n'
    printf 'To install, review --help and rerun with --accept-system-changes instead of --dry-run.\n'
    return
  fi

  check_conflicts "$version"
  printf 'Running the upstream installer as this user; sudo may request your password.\n'
  invoke_installer
  verify_installation "$version" "$architecture" ||
    die "Installer finished but verification failed; host changes may remain. Do not create a marker or rerun blindly."
  printf 'Ready for actions/setup-python. No setup-steps change is needed.\n'
}

main() {
  local requested='' architecture='' detected dry_run=false accept=false selection version asset url tag
  while [ "$#" -gt 0 ]; do
    case "$1" in
      --version|--architecture)
        [ "$#" -ge 2 ] || die "$1 requires a value."
        case "$1" in --version) requested="$2" ;; --architecture) architecture="$2" ;; esac
        shift 2 ;;
      --dry-run) dry_run=true; shift ;;
      --accept-system-changes) accept=true; shift ;;
      -h|--help) usage; return ;;
      *) die "Unknown option: $1 (see --help)." ;;
    esac
  done
  [[ "$requested" =~ ^3\.[0-9]+(\.[0-9]+)?$ ]] ||
    die "Select a stable CPython version with --version 3.MINOR or 3.MINOR.PATCH."
  [ "$(uname -s)" = Darwin ] || die "This script supports macOS only."
  if [ "${RUNNER_NAME:-}" = cpp-actions ]; then
    die "Run this script from the host terminal, not from cpp-actions or setup-steps."
  fi
  case "$(uname -m)" in arm64) detected=arm64 ;; x86_64) detected=x64 ;; *) die "Unsupported process architecture." ;; esac
  architecture="${architecture:-$detected}"
  [ "$architecture" = arm64 ] || [ "$architecture" = x64 ] || die "Architecture must be arm64 or x64."
  [ "$architecture" = "$detected" ] ||
    die "Use a terminal/process matching $architecture (including Rosetta when needed), and matching the workflow's architecture."
  if ! "$dry_run"; then
    [ "$(id -u)" -ne 0 ] || die "Run as the cpp-actions OS user, not with sudo bash."
    "$accept" || die "Review --help, then choose --dry-run or explicitly specify --accept-system-changes."
  fi
  for command in curl jq shasum tar pkgutil mktemp; do
    command -v "$command" >/dev/null 2>&1 || die "Required command is missing: $command (install jq with Homebrew if needed)."
  done

  readonly CACHE_ROOT=/Users/runner/hostedtoolcache
  readonly FRAMEWORK_ROOT=/Library/Frameworks/Python.framework
  readonly APPLICATION_ROOT=/Applications
  readonly COMMAND_ROOT=/usr/local/bin
  readonly RELEASE_ROOT=https://github.com/actions/python-versions/releases/download
  TEMP_ROOT="$(cd "${TMPDIR:-/tmp}" && pwd -P)" || die "Temporary directory is inaccessible."
  WORK_DIR="$(mktemp -d "$TEMP_ROOT/cpp-actions-python.XXXXXX")" || die "Could not create a private temporary directory."
  LOCK_DIR=''
  trap on_exit EXIT
  trap 'exit 129' HUP
  trap 'exit 130' INT
  trap 'exit 143' TERM

  download https://raw.githubusercontent.com/actions/python-versions/main/versions-manifest.json "$WORK_DIR/manifest.json"
  selection="$(resolve_release "$requested" "$architecture" "$WORK_DIR/manifest.json")" ||
    die "No published stable macOS release matches $requested ($architecture)."
  version="$(printf '%s' "$selection" | jq -er .version)"
  asset="$(printf '%s' "$selection" | jq -er .filename)"
  url="$(printf '%s' "$selection" | jq -er .download_url)"
  [[ "$version" =~ ^3\.[0-9]+\.[0-9]+$ ]] || die "Invalid release version."
  [ "$asset" = "python-$version-darwin-$architecture.tar.gz" ] || die "Unexpected release asset: $asset"
  case "$url" in "$RELEASE_ROOT/"*"/$asset") ;; *) die "Release URL is not an official Python asset: $url" ;; esac
  tag="${url#"$RELEASE_ROOT/"}"; tag="${tag%"/$asset"}"
  case "$tag" in "$version"-*) ;; *) die "Release tag does not match the selected version." ;; esac
  [[ "${tag#"$version"-}" =~ ^[0-9]+$ ]] || die "Invalid official release tag: $tag"
  printf 'Selected Python %s (%s), release %s\n' "$version" "$architecture" "$tag"

  provision_release "$version" "$architecture" "$asset" "$url" "$tag" "$dry_run"
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  main "$@"
fi
