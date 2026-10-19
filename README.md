# cpp-actions

A local GitHub Actions workflow runner in C++20. Validate workflows, inspect
their dependency graphs, preview event selection, and execute host jobs.

## Build

Requires CMake 3.24+, Ninja, a C++20 compiler, and Git. CMake uses installed
dependencies when available and otherwise downloads the declared dependencies.

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
build/debug/src/cpp-actions --help
```

## Usage

```sh
cpp-actions validate -C /path/to/repository
cpp-actions list
cpp-actions graph --workflow ci.yml --format dot
cpp-actions run --dry-run --event push
cpp-actions run --workflow ci.yml
cpp-actions run --event workflow_dispatch --input target=debug
```

`-C/--directory` selects a repository; `-W/--workflows` selects workflow files
or directories. By default, workflows come from `.github/workflows`.
`--workflow` selects a workflow by name, filename, or path. `--dry-run` preserves
the event/graph preview without executing steps, loading execution configuration
or secrets, or creating run output.
`--job` (`-j`) selects jobs and includes their dependencies unless `--no-needs`
is supplied. `--matrix NAME=VALUE` filters matrix combinations.
Use `run --help` for the complete option list.

Execution is **sandboxed by default and supported only on macOS**, using
`/usr/bin/sandbox-exec` (Seatbelt). A missing or unsupported sandbox fails closed;
it never silently falls back to unsandboxed execution. macOS runner labels are
matched on macOS. Other runner labels are skipped unless explicitly granted
with `--host-label`; this does not emulate another operating system.

For **trusted local debugging only**, Linux can run explicitly selected host
jobs without isolation:

```sh
cpp-actions run --no-sandbox --host-label ubuntu-latest --workflow ci.yml
```

`--no-sandbox` emits a prominent warning. It grants workflow code your user's
filesystem and network access, including access to credentials outside its
minimal environment. Do not use it for untrusted workflows or pull requests.

### Explicit grants and inputs

```sh
cpp-actions run --allow-host registry.npmjs.org \
  --allow-read "$HOME/sdk" --pass-env SDKROOT \
  --pass-env BUILD_MODE=debug --parallel 2 --keep-workdir
cpp-actions run --secret API_TOKEN --secret-file "$HOME/.config/cpp-actions/secrets.env" \
  --var BUILD_MODE=debug --var-file "$HOME/.config/cpp-actions/vars.env"
```

All grant, environment, secret, and variable options above are repeatable.
`--pass-env NAME` copies only that host variable; `NAME=VALUE` sets it explicitly.
Steps start with a minimal environment, a per-job `HOME` and `TMPDIR`, a default
system `PATH`, locale, fixed `runner` values for `USER`/`LOGNAME`, and
runner/workflow variables. In sandboxed steps, `HOME` and `TMPDIR` are forced
back to job-owned directories after environment overrides are merged. Your host
`PATH`, tokens, SSH agent, and other variables are not implicitly inherited.
Passing environment variables does **not** grant permission to read referenced
files: use `--allow-read` separately. Avoid granting your entire home directory.

`--secret NAME` explicitly imports that host variable and errors if it is unset;
`--secret NAME=VALUE` sets it directly. Prefer environment/file imports to avoid
putting secrets in shell history or process arguments. Secrets and variables are
available through workflow expression contexts, not automatically as step
environment variables. Dotenv files are read only when explicitly configured;
repository `.env`, secrets, and configuration files are never auto-discovered.
Later files override earlier files, and command-line values override files.
An explicitly supplied `GITHUB_TOKEN` secret also authenticates GitHub action
resolution; the host's token is never imported automatically.
`--github-token` (alias `--token`) explicitly supplies an authentication token and
takes precedence over the `GITHUB_TOKEN` secret.

`--parallel 0` (the default) uses the CPU count. `--cache-dir` sets shared caches;
the default is the platform user-cache directory's `cpp-actions` subdirectory.
`--run-dir` selects an output directory instead of a unique directory beneath
`<cache>/runs`. It must not already exist; run directories are created privately.
Logs and `report.json` remain after execution; `summary.md` is written when steps
produce summaries. Workspaces are removed unless `--keep-workdir` is given.
Exit codes are 0 for success/skipped, 1 for workflow failure, 2 for setup errors,
and 130 for cancelled runs; argument-parser errors also return a nonzero code.
SIGINT/SIGTERM request graceful cancellation; a second signal requests forced
cancellation.
Remaining processes in a step's process group are killed when that step ends;
background services cannot be carried over to later steps.

### User configuration

Only `$XDG_CONFIG_HOME/cpp-actions/config.yml`, or
`~/.config/cpp-actions/config.yml` when XDG is unset, is loaded automatically.
An automatic configuration resolving inside the selected repository is refused.
`--config /trusted/path/config.yml` explicitly selects a different file, including
a repository file only when you deliberately trust it. Dry runs do not load it.

```yaml
allow-hosts:
  - registry.npmjs.org
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
relative to your current directory. Leading `~` is expanded. CLI grants add to
configuration grants; CLI scalar settings and supplied values take precedence.
The configuration also supports `host-labels`, `cache-dir`, `node`, `vars-file`,
and environment-specific `environments` mappings with `secrets-file`,
`vars-file`, and `vars`. Advanced `allow-write` and `allow-localhost` settings
relax isolation and should be used sparingly.
`--node /absolute/path/to/node` overrides the configured `node` executable.
`--allow-write` and `--allow-localhost` provide explicit CLI grants for the
corresponding advanced settings.

## Actions (Phase 5)

- `uses: ./path` loads `action.yml` or `action.yaml` from the checked-out workspace.
- `owner/repo[/path]@ref` resolves GitHub refs to commit SHAs and caches actions
  under `<cache>/actions`.
- JavaScript actions support `node20`/`node24`, `INPUT_*`, pre/main/post,
  `pre-if`/`post-if`, and `GITHUB_STATE`. A matching host Node is preferred;
  otherwise the runner downloads one from `actions/node-versions` GitHub Releases.
  If downloading fails, it falls back to host Node with a warning.
  `--node` explicitly selects the executable.
- Composite actions support inputs/outputs, `github.action_path`, and nested
  steps up to 10 levels deep.
- `actions/checkout` places the local repository working tree, including
  uncommitted changes, in the job workspace.
- `actions/cache`, including `/restore` and `/save`, uses `<cache>/cache`;
  these caches are not shared with GitHub.
- `actions/upload-artifact` and `actions/download-artifact` exchange files
  between jobs within the same workflow run under `<run>/artifacts`. Cross-run
  downloads and GitHub retention, compression, and storage APIs are not emulated.
- Other JavaScript/composite actions, including setup actions, execute normally.
  `RUNNER_TOOL_CACHE` points to `<cache>/toolcache`, shared across runs.
  Use `--allow-host` when setup actions require non-GitHub download hosts.
- Docker container actions fail with an explicit unsupported-action error.

## Security model and limitations

The Seatbelt profile denies access by default, permits required system software
and job/cache directories, and routes network access through a host-allowlisting
HTTP/HTTPS proxy. Default hosts are GitHub's `github.com` and subdomains,
`githubusercontent.com` and subdomains, and `ghcr.io` and subdomains.
Use `--allow-host` for other services. Tools must support the supplied proxy
environment variables; direct outbound connections are blocked rather than
automatically proxied. Wildcard/default host grants do not authorize
private/loopback/link-local destinations. Explicitly granting an exact,
non-default host can open a private service through the proxy, so review grants
carefully.

The generated profile is kept outside the step-writable workspace, home,
temporary, and tool-cache directories. There are no blanket grants to the
operating system's user temporary/cache directories or all of `/opt`; the
read grant under `/opt` is limited to `/opt/homebrew`.
Generated step scripts are also outside step-writable directories. File-command
reads reject symlinks, non-regular files, and multiply linked files; each step's
process group is killed before file commands are consumed.
Tools that ignore `TMPDIR` and use macOS's per-user system cache/temp paths
may require tool-specific configuration to use the job directories instead.

This is not a VM or a complete GitHub-hosted runner. System software and some
system metadata remain visible; shared writable caches are not isolated between
trusted runs. Hostname grants authorize a destination, not individual URL paths
or operations: code can send granted data to an allowed service. Secrets given
to workflows can be exfiltrated, and log masking is best-effort, not a security
boundary. Treat workflow code, third-party actions, explicit grants, and
configuration as trusted inputs. Container/service jobs are unsupported.
The runner itself performs action resolution and built-in operations outside
step process isolation; the sandbox is not a blanket containment boundary for
every operation of the CLI.

## Workflow execution

Execution supports `needs`, job/step `if`, matrix expansion, `fail-fast`,
`max-parallel`, `continue-on-error`, and timeouts. Shells include bash, sh,
python, pwsh, and custom commands containing `{0}`, with `defaults.run`,
`working-directory`, and scoped environments. Tools must be installed on the host.
Workflow contexts, step/job outputs, and workspace-relative `hashFiles` are supported.

### Reusable workflows and job settings (Phase 6)

Jobs can call `./.github/workflows/build.yml` or
`owner/repo/.github/workflows/build.yml@ref`. Remote refs are resolved to a
commit and cached like actions. Nested local calls resolve relative to the
repository containing the called workflow. Checkouts still use the caller's
working-tree snapshot. Calls support typed `workflow_call` inputs and defaults,
explicit secrets or `secrets: inherit`, and workflow outputs forwarded through
`needs.<job>.outputs`. Secrets are not forwarded unless requested; inheritance
applies only to the immediate call. Nesting is limited to ten workflow levels.

For example, a caller can pass an input and consume the returned output:

```yaml
jobs:
  build:
    uses: ./.github/workflows/build.yml
    with:
      configuration: release
    secrets: inherit
  check:
    needs: build
    runs-on: macos-latest
    steps:
      - run: echo "${{ needs.build.outputs.result }}"
```

The called `.github/workflows/build.yml` declares its interface:

```yaml
on:
  workflow_call:
    inputs:
      configuration: {type: string, default: debug}
    outputs:
      result:
        value: ${{ jobs.compile.outputs.result }}
jobs:
  compile:
    runs-on: macos-latest
    outputs:
      result: ${{ steps.build.outputs.result }}
    steps:
      - id: build
        run: echo 'result=${{ inputs.configuration }}' >> "$GITHUB_OUTPUT"
```

Input values must match the declared string, boolean, or number type; quoted
`"true"` is not a boolean. Explicitly passed secrets must be declared by the
callee, whereas `inherit` can forward undeclared secrets.

`strategy.matrix` and `runs-on` are evaluated after `needs` completes, so
`fromJSON(needs.prepare.outputs.matrix)` and JSON runner-label arrays work.
Use repeated `--host-label LABEL` options or configuration `host-labels` to
map custom/self-hosted labels to the local host. These are explicit overrides,
not operating-system or toolchain emulation.

Jobs selecting an `environment` load that environment's configured secrets and
variables; environment values override repository values for that job only.
Environment names are matched case-insensitively. Protection rules, approvals,
and deployment APIs are not emulated. `concurrency` groups and
`cancel-in-progress` are evaluated and displayed only: they do not serialize or
cancel runs. Workflow/job `permissions` emit a warning; they do not reduce the
permissions of an explicitly supplied token.

Self-repository `actions/checkout` uses a snapshot of the local working tree,
including uncommitted modifications, deletions, and non-ignored untracked files.
Git history is copied, including from linked worktrees; any snapshot commit is
made only in the copy. The original repository is not modified. Job workspaces
are empty until checkout. Snapshots and job workspaces are removed after the run
unless `--keep-workdir` is specified; retained files may contain secrets.

`workflow_dispatch` inputs are also read from `--event-path` JSON; explicit
`--input NAME=VALUE` takes precedence. When no workflow is triggered, no run
directory is created and execution configuration/secrets are not loaded.
`--env-pass` and `--env` are aliases for `--pass-env`; `--secrets-file` and
`--vars-file` remain aliases for `--secret-file` and `--var-file`.
`-j` selects jobs; use `--parallel` to set concurrency.

The runner processes environment/output/path/state/summary file commands and
group, annotation, debug, add-mask, and stop-commands workflow commands.
`GITHUB_STATE` is action state, not an environment variable for later `run:`
steps. Logs and reports mask known secret values on a best-effort basis while
preserving report schema. Summaries are displayed at completion; `--debug`
enables debug commands, and `--no-color` or `NO_COLOR` disables colored logs.
Local execution does not reproduce GitHub-hosted images, GitHub artifact/cache APIs,
or OIDC services and does not guarantee identical results to GitHub.
