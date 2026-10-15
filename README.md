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

`--parallel 0` (the default) uses the CPU count. `--cache-dir` sets shared caches;
the default is the platform user-cache directory's `cpp-actions` subdirectory.
`--run-dir` selects an output directory instead of a unique directory beneath
`<cache>/runs`. Use a fresh directory: matching job directories can be replaced.
Logs and `report.json` remain after execution; `summary.md` is written when steps
produce summaries. Workspaces are removed unless `--keep-workdir` is given.
Exit codes are 0 for success/skipped, 1 for workflow failure, 2 for setup errors,
and 130 for cancelled runs; argument-parser errors also return a nonzero code.
SIGINT/SIGTERM request graceful cancellation; a second signal requests forced
cancellation.

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
Homebrew system-software read grant is limited to `/opt/homebrew`.

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