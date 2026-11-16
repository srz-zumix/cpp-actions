# cpp-actions

[English](README.md) | [日本語](README.ja.md)

Run GitHub Actions workflows locally with a C++20 command-line tool. Validate
workflow YAML, inspect job dependencies, preview which workflows an event selects,
and run jobs against your local working tree, including uncommitted changes.

**Sandboxed execution requires macOS.** Steps use macOS Seatbelt by default.
This is not a VM or a replacement for a GitHub-hosted runner: jobs use tools
installed on your host. Linux execution requires explicitly disabling isolation
and selecting host labels; use it only for trusted local debugging.

## Contents

- [Build](#build)
- [Quick start](#quick-start)
- [Select workflows and jobs](#select-workflows-and-jobs)
- [Configure access, environment, and secrets](#configure-access-environment-and-secrets)
- [Find results and clean up](#find-results-and-clean-up)
- [Workflow compatibility](#workflow-compatibility)
- [Security and troubleshooting](#security-and-troubleshooting)
- [Detailed reference](#detailed-reference)

## Build

Requires CMake 3.24+, Ninja, a C++20 compiler, and Git. CMake uses installed
dependencies when available and otherwise downloads the declared dependencies.
Run these commands from the cpp-actions source directory:

```sh
cmake --preset release
cmake --build --preset release
build/release/src/cpp-actions --help
```

Use `build/release/src/cpp-actions` directly, or install it to a prefix on your
`PATH`. For example:

```sh
cmake --install build/release --prefix "$HOME/.local"
export PATH="$HOME/.local/bin:$PATH"
```

For development and tests:

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

The sandboxed Xcode integration tests are opt-in. They require Xcode, an iOS SDK,
and an available iOS Simulator runtime; they can start or interact with the
host's Simulator infrastructure and may take a while:

```sh
CPA_TEST_XCODE_STORYBOARD=1 build/debug/tests/cpa_unit_tests \
  --iutest_filter=ActionRunnerTest.XcodeStoryboardCompilesInSandbox
CPA_TEST_XCODE_ASSET_SYMBOLS=1 build/debug/tests/cpa_unit_tests \
  --iutest_filter=ActionRunnerTest.XcodeAssetSymbolsGeneratedInSandbox
CPA_TEST_XCODE_ASSET_CATALOG=1 build/debug/tests/cpa_unit_tests \
  --iutest_filter=ActionRunnerTest.XcodeAssetCatalogCompilesInSandbox
```

## Quick start

Run from the repository whose workflows you want to inspect. Workflows are
discovered in `.github/workflows` by default.

```sh
# Check workflow YAML and list workflows.
cpp-actions validate
cpp-actions list

# Inspect dependencies and preview a push event without executing anything.
cpp-actions graph --workflow ci.yml --format dot
cpp-actions run --workflow ci.yml --event push --dry-run

# Execute the selected workflow on macOS.
cpp-actions run --workflow ci.yml --event push
```

Replace `ci.yml` with your workflow's filename, path, or name, and choose an
event supported by its `on:` triggers. A workflow that does not match the event
is skipped. `--dry-run` does not execute steps, load execution configuration or
secrets, or create run output.

Jobs start with an empty workspace. Use `actions/checkout` in the workflow to
populate it from a snapshot of your local repository. The snapshot includes
uncommitted modifications, deletions, and non-ignored untracked files; the
original repository is not modified.

On macOS, macOS runner labels match automatically. Other labels require
`--host-label`, which maps a label to this host without emulating an OS or
installing tools. On Linux, both explicit options below are required:

```sh
# Trusted local debugging only: no sandbox or filesystem/network isolation.
cpp-actions run --workflow ci.yml --event push \
  --no-sandbox --host-label ubuntu-latest
```

**Do not use `--no-sandbox` for untrusted workflows or pull requests.** Workflow
code gains your user's filesystem and network access, including credentials
outside its minimal environment. A missing or unsupported sandbox never causes
an automatic fallback to this mode.

## Select workflows and jobs

| Option | Purpose |
| --- | --- |
| `-C, --directory PATH` | Select the repository instead of the current directory. |
| `-W, --workflows PATH` | Select workflow files or directories instead of `.github/workflows`. |
| `-w, --workflow SELECTOR` | Select a workflow by name, filename, or path (`run` / `graph`). |
| `-e, --event NAME` | Select the event to simulate. |
| `--event-path FILE` | Read an event payload from JSON. |
| `--input NAME=VALUE` | Supply a `workflow_dispatch` input; overrides the payload value. |
| `-j, --job ID` | Run selected jobs, including their dependencies. |
| `--no-needs` | Do not automatically include dependencies of selected jobs. |
| `--matrix NAME=VALUE` | Filter matrix combinations. |
| `--parallel N` | Limit parallel jobs; `0` (the default) uses the CPU count. |
| `-n, --dry-run` | Preview event selection and jobs without executing them. |

```sh
cpp-actions validate -C /path/to/repository
cpp-actions run --workflow ci.yml --job build --matrix configuration=debug
cpp-actions run --event workflow_dispatch --input target=debug
```

Use `cpp-actions --help` and `cpp-actions run --help` for the complete CLI.

## Configure access, environment, and secrets

Steps receive a minimal environment, isolated per-job `HOME` and `TMPDIR`, and
runner/workflow variables. Your host `PATH`, tokens, SSH agent, and other
variables are not implicitly inherited.

Grant only the access a trusted workflow needs:

```sh
cpp-actions run --workflow ci.yml \
  --allow-host registry.example.test \
  --allow-read "$HOME/sdk" \
  --pass-env SDKROOT --pass-env BUILD_MODE=debug

cpp-actions run --workflow ci.yml \
  --secret API_TOKEN \
  --secret-file "$HOME/.config/cpp-actions/secrets.env" \
  --var BUILD_MODE=debug
```

`registry.example.test` is a fictional hostname; replace it with your trusted
service. These grant/environment/secret/variable options are repeatable.
`--pass-env NAME` copies a host variable; `NAME=VALUE` sets it explicitly.
Passing an environment variable does **not** grant read access to paths it
references: use `--allow-read` separately. Avoid granting your entire home.

`--secret NAME` imports a host variable and errors if unset. Secrets and vars
are available in expression contexts, not automatically as step environment
variables. Prefer environment or file imports over putting secrets in command
arguments or shell history. An explicitly supplied `GITHUB_TOKEN` secret
authenticates action resolution; `--github-token` (alias `--token`) takes
precedence. The host token is never automatically imported.

### Trusted user configuration

For repeated runs, place configuration at
`$XDG_CONFIG_HOME/cpp-actions/config.yml`, or
`~/.config/cpp-actions/config.yml` when XDG is unset:

```yaml
allow-hosts:
  - registry.example.test
allow-read:
  - ~/sdk
env-pass:
  - SDKROOT
parallel: 2
secrets-file: secrets.env
vars:
  BUILD_MODE: debug
```

Configuration paths are relative to the configuration file; CLI paths are
relative to the current directory. Leading `~` is expanded. CLI grants add to
configuration grants; CLI scalar settings and supplied values take precedence.
Repository `.env`, secrets, and configuration files are never auto-discovered.
Automatic configuration resolving inside the repository is refused.
`--config /trusted/path/config.yml` selects a file you deliberately trust.
Dry runs do not load execution configuration.

See the [configuration reference](docs/reference.md#user-configuration) for
environment-specific secrets/vars, setup steps, and additional settings.

## Find results and clean up

By default, each run gets a unique directory under `<cache>/runs`, where
`<cache>` is the platform user-cache directory's `cpp-actions` subdirectory.
`--cache-dir` overrides the cache root. `--run-dir PATH` selects a run directory
that must not already exist.

Logs and `report.json` remain after execution; `summary.md` is written when steps
produce summaries. Artifacts are ordinary files under `<run>/artifacts`, or
under a separate root selected with `--artifact-dir PATH`.

Workspaces are normally removed after execution. `--keep-workdir` retains them
for inspection; they live in separate short temporary directories, not inside
the run directory. Their locations are logged. Retained files may contain
secrets.

```sh
cpp-actions clean --dry-run /path/to/run-dir
cpp-actions clean /path/to/run-dir
cpp-actions clean --all
```

`clean` removes retained workspaces, Unity temporary directories, and run
`jobs`, `control`, and `snapshot` directories. It keeps logs, reports, summaries,
and artifacts, and never touches a locked running run. Interrupted runs without
`report.json` require `--force`. See the
[output and cleanup reference](docs/reference.md#explicit-grants-and-inputs)
for path validation and cleanup failure behavior.

Exit codes: `0` success/skipped, `1` workflow failure, `2` setup error, and `130`
cancellation. Argument errors also return a nonzero code. SIGINT/SIGTERM request
graceful cancellation; a second signal requests forced cancellation. Background
services cannot carry over to later steps.

## Workflow compatibility

| Feature | Local behavior |
| --- | --- |
| Jobs and steps | `needs`, `if`, outputs, matrix expansion, fail-fast, max-parallel, continue-on-error, and timeouts. |
| Shells | bash, sh, python, pwsh, and custom commands with `{0}`; tools must exist on the host. |
| Actions | Local/remote JavaScript (`node20` / `node24`) and composite actions, including pre/main/post. |
| `actions/checkout` | Local working-tree snapshot or remote checkout; submodules and Git LFS are supported. |
| Cache and artifacts | Built-in local storage; not shared with GitHub's cache/artifact services. |
| Reusable workflows | Local/remote calls, typed inputs, explicit/inherited secrets, and outputs; up to 10 workflow levels. |
| Environments | Configured secrets/vars; no approvals, protection rules, or deployment APIs. |
| `concurrency` | Evaluated and displayed only; does not serialize or cancel runs. |
| `permissions` | Warns only; does not reduce supplied token permissions. |
| Containers and OIDC | Container/service jobs, Docker container actions, and OIDC services are unsupported. |

See [action compatibility](docs/reference.md#actions) and
[workflow execution](docs/reference.md#workflow-execution) for exact behavior.
Results are not guaranteed to match GitHub-hosted runners.

## Security and troubleshooting

Seatbelt denies access by default and limits steps to required system software
and permitted job/cache paths. Network access goes through a per-job
host-allowlisting HTTP/HTTPS proxy. GitHub and selected package/tool download
hosts are allowed by default; redirects to other hosts require separate grants.
Clients that ignore proxy settings are blocked, not automatically redirected.

| Symptom | What to check |
| --- | --- |
| No workflow/job runs | Event triggers, job conditions, and host-label matching; use `--dry-run`. |
| A tool cannot be found | Install it on the host and explicitly configure its environment/read access. |
| A download is denied | Grant the required host and any redirect hosts; ensure the client uses the proxy. |
| A host file is denied | Grant a narrow `--allow-read` path, not the entire home directory. |
| `actions/setup-python` fails on macOS | Read the [hosted Python provisioning guidance](docs/reference.md#actions); do not disable the sandbox to fix it. |
| Interactive shell waits | Review the opt-in [non-interactive shell mode](docs/reference.md#opt-in-non-interactive-shells). |
| Docker, MSBuild, Unity, or Xcode fails | Review the tool-specific grants below; they can expose shared host state. |

**Explicit grants weaken isolation.** Docker daemon access bypasses the sandbox;
Apple security services can expose Keychain operations; Unity and Xcode grants
can expose shared host IPC, caches, and temporary files. Enable them only for
trusted code, inputs, and earlier steps that can modify the selected code.

This is not complete host isolation. Shared caches, built-in operations outside
step isolation, and permitted services remain relevant trust boundaries.
Secrets can be exfiltrated to allowed services, and log masking is best-effort,
not a security boundary. Read the
[full security model](docs/reference.md#security-model-and-limitations) before
running workflows with sensitive data or extra grants.

## Detailed reference

The [reference guide](docs/reference.md) is part of this README's user-facing
specification. It preserves the detailed settings, compatibility rules, and
security caveats:

- [Environment, secrets, Java/Maven defaults, output, and cleanup](docs/reference.md#explicit-grants-and-inputs)
- [User configuration](docs/reference.md#user-configuration) and [setup steps](docs/reference.md#cpp-actions-only-setup-steps)
- [Apple security](docs/reference.md#opt-in-apple-security-services), [Xcode / Simulator](docs/reference.md#opt-in-xcode--simulator-compatibility), and [Xcode temporary files](docs/reference.md#opt-in-xcode-temporary-files)
- [Docker CLI](docs/reference.md#opt-in-docker-cli-access) and [MSBuild IPC](docs/reference.md#opt-in-msbuild-worker-ipc)
- [Unity EditorPrefs](docs/reference.md#opt-in-unity-editorprefs-inheritance-macos), [Unity files / IPC](docs/reference.md#opt-in-unity-startup-files-and-ipc), and [Unity network hosts](docs/reference.md#opt-in-unity-network-hosts)

## License

[MIT](LICENSE)
