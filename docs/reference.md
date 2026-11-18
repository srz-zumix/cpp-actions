# cpp-actions reference

[Getting started](../README.md) | [日本語 README](../README.ja.md)

This guide is the detailed companion to the README. Start with the README for
building and running your first workflow; use this guide for configuration,
compatibility details, and the security implications of additional grants.

## Contents

- [Usage](#usage)
- [Environment, secrets, and output](#explicit-grants-and-inputs)
- [Non-interactive shells](#opt-in-non-interactive-shells)
- [User configuration](#user-configuration)
- [Setup steps](#cpp-actions-only-setup-steps)
- [Unity EditorPrefs](#opt-in-unity-editorprefs-inheritance-macos)
- [Apple security services](#opt-in-apple-security-services)
- [Xcode / Simulator](#opt-in-xcode--simulator-compatibility)
- [Xcode temporary files](#opt-in-xcode-temporary-files)
- [Docker CLI](#opt-in-docker-cli-access)
- [MSBuild IPC](#opt-in-msbuild-worker-ipc)
- [Unity files and IPC](#opt-in-unity-startup-files-and-ipc)
- [Unity network hosts](#opt-in-unity-network-hosts)
- [Actions and Git configuration](#actions)
- [Security model and limitations](#security-model-and-limitations)
- [Workflow execution and reusable workflows](#workflow-execution)

## Usage

Project-specific action selectors, private host examples, and sample run IDs
below are fictional placeholders; replace them with your own trusted values.

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
runner/workflow variables. Every step, setup step, and action process also
receives `CPP_ACTIONS=true` and `CPP_ACTIONS_VERSION=<version>` so scripts can
detect cpp-actions (for example, `if [ "${CPP_ACTIONS:-}" = true ]`). Like
`GITHUB_ACTIONS`, they cannot be overridden by `env` or `GITHUB_ENV`; they are
process environment variables, not entries in the `env` expression context.
`GRADLE_USER_HOME` defaults to `<job HOME>/.gradle`,
keeping Gradle caches and native libraries separate from the host and other
jobs. Explicit values from `--pass-env`, workflow/job/step `env`, or
`GITHUB_ENV` override this default; external paths still require separate
sandbox permissions. In sandboxed steps, `HOME` and `TMPDIR` are forced
back to job-owned directories after environment overrides are merged. On
macOS, `CFFIXED_USER_HOME` also defaults to the job `HOME` so Foundation-based
tools such as Xcode use the isolated directory; sandboxed steps force it back
to the job `HOME` after environment overrides are merged.
All steps, setup steps, and action pre/main/post processes receive
`-Duser.home=<job HOME>`, `-Dmaven.repo.local=<job HOME>/.m2/repository`,
and `-Djava.io.tmpdir=<job temporary directory>`
appended to `JDK_JAVA_OPTIONS` after environment overrides are merged.
This keeps Java's home, Maven's local repository, and Java's temporary files
job-owned, including when Unity launches Gradle. Java's temporary directory uses
the job's default `TMPDIR`, or the short private `TMPDIR` for sandboxed Unity
opt-in processes, regardless of earlier `java.io.tmpdir` options.
Paths containing spaces or quotes are quoted for the Java
launcher. Other options are preserved; conflicting earlier properties in
`JDK_JAVA_OPTIONS` are overridden. This requires Java 9 or later and also applies
with `--no-sandbox`; it does not grant access to the host's `.m2` directory.
Your host
`PATH`, tokens, SSH agent, and other variables are not implicitly inherited.
Passing environment variables does **not** grant permission to read referenced
files: use `--allow-read` separately. Avoid granting your entire home directory.

`--secret NAME` explicitly imports that host variable and errors if it is unset;
`--secret NAME=VALUE` sets it directly. Prefer environment/file imports to avoid
putting secrets in shell history or process arguments. Secrets and variables are
available through workflow expression contexts, not automatically as step
environment variables. Dotenv files are read only when explicitly configured;
repository `.env` and secrets files are never auto-discovered; `.cpp-actions.yml`
is loaded as described under user configuration below.
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
Each job's workspace and `RUNNER_TEMP` live in a private, short real
directory, `/private/tmp/cpa-w-XXXXXX/work` on macOS or `/tmp/cpa-w-XXXXXX/work`
on Linux, independently of `--run-dir`. This avoids long run/job identifiers in
build output paths. The repository-name nesting is unchanged. The actual work
directory is logged and recorded in `<run>/jobs/<run-id>/<job-key>/work-path`.
It is removed after success, failure, or cancellation; `--keep-workdir` retains
it alongside the job metadata and logs its location. Retained work directories
are outside the run directory; remove them with `cpp-actions clean`:

```sh
cpp-actions clean --dry-run 20000101-000000-abcdef   # fictional run name under <cache>/runs
cpp-actions clean /path/to/run-dir                     # or an explicit run directory
cpp-actions clean --all                                # every completed run under <cache>/runs
```

`clean` removes the recorded `cpa-w-XXXXXX` work directories and `cpa-u-XXXXXX`
Unity temporary directories, then the run's `jobs`, `control`, and `snapshot`
directories. Logs, `report.json`, `summary.md`, and artifacts are kept.
`--cache-dir` (or `cache-dir` from the user or `--config` configuration) selects
`<cache>/runs`. Recorded paths are removed only if they are real directories
owned by you directly beneath the temporary root with the expected name;
otherwise the entry is reported, the job's metadata is preserved for a retry,
and the command exits with status 1. Each run holds `<run>/run.lock` while
executing, and `clean` never touches a locked run. A run without `report.json`
(interrupted) is skipped unless `--force` is given; `--all` reports skipped runs
without failing.
Job `HOME`, default `TMPDIR`, and protected control files keep their existing
locations beneath the run directory.
`--artifact-dir PATH` selects a separate artifact output root (relative paths
are relative to the current working directory; `~` is expanded). Uploads are
saved as ordinary files under `PATH/<workflow-run-id>/<artifact-name>/`, not ZIP
archives. The directory may already exist; each workflow gets a new private
run-ID subdirectory. Without this option the root is `<run>/artifacts`.
Downloads within the same workflow use the selected store too. Artifacts remain
after success or failure, independently of `--keep-workdir`; each successful
upload logs its destination. Do not place the store inside a job workspace or
the run's control/snapshot directories.
Exit codes are 0 for success/skipped, 1 for workflow failure, 2 for setup errors,
and 130 for cancelled runs; argument-parser errors also return a nonzero code.
SIGINT/SIGTERM request graceful cancellation; a second signal requests forced
cancellation.
Remaining processes in a step's process group are killed when that step ends;
background services cannot be carried over to later steps.

### Opt-in non-interactive shells

`cpp-actions run --ignore-interactive-shell` removes `-i` (including grouped
flags such as `-ileo`) and `--interactive` from `bash`/`sh` shell options in
`run:` steps, including workflow/job defaults and nested composite actions.
For example, `bash -ileo pipefail {0}` becomes `bash -leo pipefail {0}`.
Other options, including login mode, error handling, and `pipefail`, are kept;
option operands, script arguments, and other shell executables are not changed.
The runner warns whenever it removes an interactive option and displays the
effective shell command.

This is disabled by default. It can also be enabled in trusted user configuration:

```yaml
ignore-interactive-shell: true
```

Use this to avoid interactive-shell terminal/job-control waits in automated
steps. Interactive-only initialization (such as `.bashrc` loading or checks of
`$-`) changes, so tools configured only there may need explicit environment
setup. It does not rewrite scripts or interactive shells launched inside them,
and it does not resume an already stopped process.

### User configuration

By default, `run` loads `$XDG_CONFIG_HOME/cpp-actions/config.yml`, or
`~/.config/cpp-actions/config.yml` when XDG is unset, followed by
`.cpp-actions.yml` at the selected repository root. The root comes from `-C` or
the current directory, including when started from a subdirectory. Either file
may be absent; an invalid existing file fails rather than falling back.

Lists, including permission grants, `host-labels`, and `env-pass`, are appended
in user-then-repository order. Later `env-pass` assignments override earlier
assignments of the same variable. Repository scalar settings override user
settings when supplied; omitted settings retain user values. Variable maps
merge by key. Environments merge by case-insensitive name, including their
variable maps and file settings. `secrets-file` and `vars-file` are scalar
settings: only the final selected files are loaded, and merged inline `vars`
override the selected variables file. An explicit empty file, `node`, or
`cache-dir` value clears the earlier value. Setup steps from both files run in
order; explicit setup step IDs must be unique across files.

`--config /trusted/path/config.yml` loads only that file, skipping both
automatic configurations. CLI grants add to it, and CLI scalar settings and
values take precedence. Dry runs and runs with no matching workflow do not load
configuration or secrets.

**Trust the repository before running it.** Automatic `.cpp-actions.yml`
loading emits a warning: it can execute setup steps, import host environment
variables or dotenv files, and relax sandbox permissions. Review it as carefully
as workflow changes. User configuration resolving inside the repository is
refused; `.cpp-actions.yml` is the designated repository-local configuration.
`clean` uses the same merge rules when looking up a cache by run name or
`--all`; an explicit `--cache-dir` takes precedence without loading
configuration.

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
relative to your current directory. Leading `~` is expanded. CLI grants add to
configuration grants; CLI scalar settings and supplied values take precedence.
The configuration also supports `host-labels`, `cache-dir`, `node`, `vars-file`,
`allow-xcode`, and environment-specific `environments` mappings with
`secrets-file`, `vars-file`, and `vars`. Advanced `allow-write` and
`allow-localhost` settings relax isolation and should be used sparingly.
`--node /absolute/path/to/node` overrides the configured `node` executable.
`--allow-write` and `--allow-localhost` provide explicit CLI grants for the
corresponding advanced settings.
`allow-localhost` permits outbound connections to loopback on any port and
listening on any port. Seatbelt cannot restrict listening to loopback
interfaces, so processes can listen on all interfaces and accept connections
from the network; outbound connections to other addresses remain denied.

### cpp-actions-only setup steps

Trusted user configuration can define `setup-steps`. These run once per executed
job (including each matrix job and jobs inside reusable workflows), before its
workflow steps. They are not added to the GitHub workflow itself. Skipped jobs
do not execute setup, and dry runs do not load configuration or execute setup.

Setup steps use the job's workspace, isolated `HOME`, environment, sandbox,
secrets masking, and job timeout. They do not automatically check out the
repository. The default shell is the explicit `bash` preset
(`--noprofile --norc -eo pipefail {0}`); workflow/job `defaults.run` do not apply
to setup. Logs and reports label these steps `cpp-actions setup / ...`.
Supported fields are `id`, `name`, `if`, `run`, `shell`, `env`,
`working-directory`, and `timeout-minutes`, with normal step expression syntax.
`uses`, `with`, and `continue-on-error` are not supported.

Each setup script runs in its own process. Use `GITHUB_ENV` and `GITHUB_PATH`
to carry environment and PATH changes to subsequent setup and workflow steps;
a plain `export` only affects the current script. Files under the job's `HOME`
also persist for that job. Setup step outputs are available to later setup
steps but do not populate the workflow's `steps` context.
If setup fails, the job fails and all workflow steps are skipped, including
steps with `if: always()`. Job-level `continue-on-error` retains its normal
behavior.

For example, to use an existing Ruby and Bundler installation, and restore
its PATH priority after macOS login-shell initialization:

```yaml
allow-read:
  - /opt/fixture-ruby
ignore-interactive-shell: true
setup-steps:
  - name: Configure Ruby for this job
    run: |
      ruby_bin=/opt/fixture-ruby/bin
      echo "$ruby_bin" >> "$GITHUB_PATH"
      printf 'export PATH="%s:$PATH"\n' "$ruby_bin" >> "$HOME/.bash_profile"
      "$ruby_bin/ruby" --version
      "$ruby_bin/bundle" _2.5.3_ --version
```

Replace `/opt/fixture-ruby` with your trusted installation path. Script text is not subject to the
configuration path's `~` expansion: the script's `$HOME` is the isolated job
home, not your host home. This example only creates a job-local login profile;
it does not install Ruby/gems or modify your host's shell profiles.
Host filesystem/network access still requires the corresponding explicit
grants. Setup applies to every executed job, so keep it idempotent and avoid
assuming workflow steps have already run.

### Opt-in Unity EditorPrefs inheritance (macOS)

`cpp-actions run --inherit-unity-editor-prefs`, or
`inherit-unity-editor-prefs: true` in trusted user configuration, copies
`~/Library/Preferences/com.unity3d.UnityEditor5.x.plist` from the host HOME
to the same relative path in each executed job's isolated HOME. This happens
before `setup-steps` and workflow steps, including matrix and reusable-workflow
jobs. The default is disabled; skipped jobs and dry runs do not copy preferences.
CLI opt-in takes effect even if the config value is `false`.

The whole EditorPrefs file is copied, not the host's other Unity settings,
licenses, Keychain, or cache. The source must be an existing regular file
(not a symlink); missing/unreadable files fail the job explicitly. This option
is macOS-only and fails explicitly on other platforms.
The job receives an independent, owner-readable/writable copy: changes are never
written back to the host. No sandbox access to the host preference directory
is granted. Existing cleanup rules apply; `--keep-workdir` retains the copy.

**Only enable this for trusted workflows.** EditorPrefs can contain sensitive
values and host-specific paths, and the complete copy is accessible to job code.
The runner warns when it copies preferences but does not print their contents.
Configured Git paths may then refer to host tools; those tools
still need to exist and be accessible under the normal sandbox rules.

### Opt-in Apple security services

Apple's `com.apple.SecurityServer` and `com.apple.trustd.agent` services are
blocked by default. Some runtimes, including .NET on macOS, need them for HTTPS
certificate processing. Grant access only to trusted actions with the repeatable
`--allow-apple-security-for` option:

```sh
cpp-actions run -w client-build.yml \
  --allow-apple-security-for 'fixture-owner/version-action@main'
```

The trusted user configuration supports the same additive list:

```yaml
allow-apple-security-for:
  - fixture-owner/version-action@main
  - ./trusted-local-action
```

Selectors are case-sensitive. Remote selectors use `owner/repo[/path]@ref`;
the owner, repository, and action path must match exactly. In the ref only,
`*` matches zero or more characters, including `/`. For example,
`owner/repo@*` permits any ref, including branches and commit hashes, while
`owner/repo@v*` permits refs beginning with `v`. A selector without `*` must
match the complete ref exactly. Local selectors match the literal path:
`./action` and `./action/` are distinct, and `..` components are rejected.
Other wildcards, expressions, and Docker selectors are not supported.

Ref wildcards grant the permission to every matching version, including
unreviewed commits and future refs. Prefer a reviewed full commit SHA when
possible. The option does not change workflow `run:` steps or emulated built-in
actions.

For a composite action, the grant covers its own `run:` steps; nested `uses:`
actions must be permitted separately. JavaScript actions receive the grant for
pre, main, and post. Child processes launched directly by a permitted process
inherit its sandbox permissions. Grants are logged as warnings, and subsequent
unpermitted steps use the default profile.

**This is not a TLS-only permission.** It permits contact with Keychain-related
services as well. The ban on direct Keychain file access remains, but does not
guarantee denial of access through those services. Action selection is not
tamper-proof code isolation: earlier steps can modify shared writable workspaces
or action copies. Trust the selected action, its inputs, and any steps that can
modify its code. Neither a workflow environment variable nor `GITHUB_ENV` can
enable this grant.

### Opt-in Xcode / Simulator compatibility

Use `--allow-xcode-simulator` to enable the limited Xcode/Simulator permissions
for **all jobs and steps in this run**, including workflow `run:` commands,
composite actions, and JavaScript pre/main/post:

```sh
cpp-actions run -w client-build.yml --allow-xcode-simulator
```

Or enable it in the trusted user configuration:

```yaml
allow-xcode-simulator: true
```

It is disabled by default. The CLI flag adds a grant even if the configuration
sets it to `false`. Workflow files, environment variables, and `GITHUB_ENV`
cannot enable it.

To limit the grant to particular jobs, specify workflow paths and job IDs in
trusted configuration:

```yaml
allow-xcode-simulator-job:
  - .github/workflows/apple.yml::ios-build
```

Or repeat the CLI option for each job:

```sh
cpp-actions run -w apple.yml --allow-xcode-simulator-job '.github/workflows/apple.yml::ios-build'
```

The path must be a `.yml` or `.yaml` file directly under `.github/workflows`,
and matching is case-sensitive. The selector uses the original job ID and
applies to every matrix instance, including setup and action steps. It applies
only to workflows rooted in the selected repository, including local reusable
workflows, not remote reusable workflows or their nested local calls.

Job selectors are disabled by default and configuration and CLI selectors are
combined. The global flag or `allow-xcode-simulator: true` still grants every
job, including jobs in remote reusable workflows. Neither form can be enabled
by workflow files, environment variables, or `GITHUB_ENV`.

The grant permits contact with `com.apple.CoreSimulator.CoreSimulatorService`,
`com.apple.CoreSimulator.simdiskimaged`, and the native host architecture's
`com.apple.CoreSimulator.SimLaunchHost-arm64` or `SimLaunchHost-x86_64` service.
It also permits read-only access to the OS user's
`~/Library/Developer/DeveloperDiskImages` (not the job's isolated `HOME`).
Each job logs a warning when enabled.

**This is not a read-only Simulator-listing permission.** These service
connections can permit operations on the host's Simulator infrastructure.
Enable it only for trusted workflows and actions. By itself, it does not grant
general home-directory access, host temporary/cache writes, FSEvents, LaunchServices,
or the separate Apple security-service permissions. Xcode and Simulator
runtimes must already be installed; a complete Unity/Xcode build may require
additional permissions beyond SDK/runtime enumeration.

### Opt-in Xcode temporary files

Xcode's build tools can use the macOS user's temporary directory even when
`RUNNER_TEMP` and `TMPDIR` are job-owned. Grant their temporary-file access
only to trusted actions:

```sh
cpp-actions run -w build.yml --allow-xcode-for './trusted-xcode-action'
```

The trusted user configuration supports the same additive list:

```yaml
allow-xcode-for:
  - ./trusted-xcode-action
  - owner/repo/path@v1
```

Selectors follow the matching rules of `allow-apple-security-for`. The grant
applies to a composite action's own `run:` steps and to JavaScript actions'
pre, main, and post processes. Nested `uses:` actions must be selected
separately. Top-level workflow `run:` steps and emulated built-in actions do
not receive it. CLI and configuration lists are combined; workflow variables
and `GITHUB_ENV` cannot enable the permission. Activation is logged.

Workflows that run `xcodebuild` or `cmake -G Xcode` directly in top-level
`run:` steps can instead grant the same permissions to every step of every job:

```sh
cpp-actions run -w apple.yml --allow-xcode
```

```yaml
allow-xcode: true
```

The grant is disabled by default. The CLI flag adds the grant even when the
configuration sets it to `false`; workflow files, environment variables, and
`GITHUB_ENV` cannot enable it. Each job logs a warning when enabled.

Both forms also grant the compiler caches that Xcode keeps in the macOS user
cache directory (`getconf DARWIN_USER_CACHE_DIR`), which build settings cannot
redirect into the job: `org.llvm.clang/ModuleCache.noindex` (the clang module
cache and its validation session file) and
`com.apple.DeveloperTools/<version>/Xcode/SDKStatCaches.noindex`. Only the
missing parent directories of these caches may be created. **These caches are
shared with host Xcode builds**: a granted process can modify module and SDK
stat caches that later host builds consume, so enable the grant only for
trusted workflows.

The profile grants access to `TemporaryItems/NSIRD_xcodebuild_*`,
`TemporaryItems/NSIRD_SWBBuildService_*`, `TemporaryItems/NSIRD_ibtool_*`,
`swbuild.tmp.*`, and direct `<UUID>.storyboard` entries beneath the canonical
macOS user temporary directory. The `NSIRD` entries and their contents may be
read, modified, and removed; `ibtool` uses these directories when generating
asset symbols.
It also permits access to the direct `ibtoold-<numeric PID>` entry and its
`IB` subtree, which Interface Builder uses for temporary files and FIFOs.
Other direct children of `ibtoold-<numeric PID>` are not granted access.

For asset compilation, the OS-recognized platform binary named `ibtoold` with
signing identifier `com.apple.ibtool` can also read, create, write, and remove
direct `<UUID>` and `actool-sprite-atlas-scratch-<UUID>` entries and their
contents. These UUID entries are not necessarily Xcode files.

For archive export, platform-signed `xcodebuild` can create direct
`XcodeDistPipeline.<random>` entries. Platform-signed `xcodebuild`,
`/usr/bin/ditto`, and `/usr/bin/codesign` can read and write within matching
pipeline entries. These temporary names are shared with host and other-job
Xcode processes, not owned by the action.

When **both** this action-specific grant and `--allow-xcode-simulator` are
enabled, the action can additionally access direct
`com.apple.CoreSimulator.SimDevice.<UUID>.Standalone.<UUID>` entries in that
temporary directory. The platform binary named `AssetCatalogSimulatorAgent`
with signing identifier `com.apple.dt.AssetCatalogSimulatorAgent` can also
read, create, write, and remove direct `<UUID>` subtrees and direct
`Untitled-<alphanumeric>.uicatalog` and `.uicatalog-journal` files beneath those
Simulator entries. Other processes do not receive these descendant grants.
The UUIDs must have the complete 8-4-4-4-12 hexadecimal form; storyboard
descendants are not granted direct access. Case sensitivity follows the
filesystem.

The sandbox rules also permit reading the `TemporaryItems` directory itself,
including listing other entry names; macOS access controls still apply. If the
user temporary directory cannot be resolved, the opt-in fails rather than
falling back to another directory. These patterns are **shared with host and
other-job Xcode and Simulator processes**, not job ownership boundaries: a
permitted action and its children may read, modify, rename, or remove matching
entries, including after the step finishes if child processes keep running.
Renaming a matching directory can also affect its contents. Trust the selected
action and any steps that can modify its code. The asset-compiler process
checks reduce exposure but do not establish job ownership: the real Xcode
tools may be directed to pre-existing matching entries belonging to unrelated
host processes or jobs. If the OS or Xcode does not support these process
checks or uses different signing identities, access fails rather than falling
back to an unrestricted grant.

This permission does not grant general host temporary-directory access,
`xcrun_db` cache access, Simulator services, or Apple security services.
Xcode may still log `xcrun` cache errors, and signed or Simulator builds may
need separate permissions. `--allow-xcode-simulator` remains a separate
run-wide opt-in and does not, by itself, enable these temporary-file permissions.

### Opt-in Docker CLI access

The sandbox denies access to the host Docker daemon by default. To run Docker
CLI commands in trusted actions or individual top-level `run:` steps, explicitly
grant access:

```sh
cpp-actions run -w client-build.yml \
  --allow-docker-for 'fixture-owner/container-action@v2'
cpp-actions run -w build.yml \
  --allow-docker-step '.github/workflows/build.yml::build::docker'
```

The trusted user configuration supports the same additive lists:

```yaml
allow-docker-for:
  - 'fixture-owner/container-action@*'
allow-docker-step:
  - .github/workflows/build.yml::build::docker
```

Action selectors follow the matching rules of `allow-apple-security-for`.
They cover a composite action's own `run:` commands or a JavaScript action's
pre/main/post processes, but not nested `uses:` actions. Step selectors match
the called workflow path, job ID, and explicit step ID, and apply only to
top-level `run:` steps. Generated IDs, composite-internal steps, and `uses:`
steps do not match step selectors. Matching matrix jobs share the grant.
Workflow YAML cannot enable it.

The Docker CLI must be installed, and Docker must already be running with a
Unix socket at `/var/run/docker.sock`.
At the first permitted invocation in a job, cpp-actions resolves that path
(including Docker Desktop's symlink) and permits outbound connections only to
the resulting socket path. Missing or non-socket endpoints fail the selected
step; a changed symlink target fails subsequent grants in that job. The runner
does not install Docker, copy the host's Docker configuration or credentials,
or change `DOCKER_HOST` or `DOCKER_CONTEXT`. This grant alone does not permit a
different socket or remote daemon; other grants may independently allow them.
Docker Desktop must also be configured to share any host paths mounted by
containers.

**Docker daemon access bypasses the sandbox.** A permitted process can start
containers that access host files and networks outside the runner's filesystem
and `--allow-host` restrictions. Containers and daemon-side changes can outlive
the step and affect other users of the daemon. The permission applies to all
processes and children of the selected action or step, not just the `docker`
binary. Trust the selected code, its inputs, and any earlier steps that can
modify it. The runner logs a warning when enabling this access.

This option enables Docker CLI use only. Docker container actions
(`docker://` or `runs.using: docker`) and jobs using `container` or `services`
remain unsupported.

### Opt-in MSBuild worker IPC

Parallel MSBuild on macOS creates Unix sockets at `/tmp/MSBuild<process-id>`,
independently of `TMPDIR`. These sockets are denied by default. Grant access to
trusted actions or individual top-level `run:` steps explicitly:

```sh
cpp-actions run -w client-build.yml \
  --allow-msbuild-for './.github/actions/update_loc_data'
cpp-actions run -w build.yml \
  --allow-msbuild-step '.github/workflows/build.yml::build::restore'
```

The trusted user configuration supports additive lists with the same names:

```yaml
allow-msbuild-for:
  - ./.github/actions/update_loc_data
allow-msbuild-step:
  - .github/workflows/build.yml::build::restore
```

Action selectors follow the matching rules of `allow-apple-security-for`.
They permit a composite's own `run:` commands, but not nested `uses:` actions.
JavaScript pre/main/post receive their own action's grant. Step selectors match
the workflow path, job ID, and explicit step ID exactly (case-sensitive);
they apply only to top-level `run:` steps, not composite internals or `uses:`
steps. Use an explicit `id: restore` in the workflow for the example above.
For reusable workflows, select the called workflow's own path and job ID.
Step selectors do not support wildcards, expressions, or display-name matching.
Matrix instances of the selected job share the grant. CLI and configuration grants
are combined; repository workflow settings and environment variables cannot
enable them.

The additional profile permits file operations and Unix-socket bind/connect
only for `/private/tmp/MSBuild[0-9]+` (the canonical macOS `/tmp` path).
It does not grant general temporary-directory access, Internet access, or
Apple security services. Permission activation is logged as a warning.

**This is not restore-only or per-job socket isolation.** An allowed process
can access matching host/other-job MSBuild sockets, and MSBuild can execute
arbitrary project tasks. Trust the action, step, project, and preceding steps
that can modify their code. Children inherit the permission; background children
that outlive the step retain it. Subsequent unrelated steps get the default
profile. Use single-node MSBuild instead if this privilege is not acceptable.

### Opt-in Unity startup files and IPC

Unity's macOS editor and bundled Licensing Client use temporary files and
named mutexes outside `TMPDIR`. Grant these only to selected actions or
top-level `run` steps:

```sh
cpp-actions run \
  --allow-unity-for './.github/actions/import_assets' \
  --allow-unity-step '.github/workflows/build.yml::build::editor'
```

The equivalent trusted user configuration is:

```yaml
allow-unity-for:
  - ./.github/actions/import_assets
allow-unity-step:
  - .github/workflows/build.yml::build::editor
```

Both lists are additive with the CLI and disabled by default. Action selectors
follow the matching rules of `allow-apple-security-for`. The grant covers the
action's own composite scripts or Node pre/main/post
processes, not nested separate `uses` actions. Step selectors use the exact
workflow path, job ID, and an explicit step `id`; generated IDs, step names,
and composite-internal steps cannot opt in this way. For reusable workflows,
use the called workflow's path and job ID. All matching matrix instances receive
the grant. Workflow YAML cannot itself enable these permissions.

The additional permissions cover:

- The OS user's `~/Library/Caches/com.unity3d.UnityEditor` subtree (read/write),
  including `GiCache`. The path is resolved from the OS user account, not the
  job's overridden `HOME`. This cache is shared with host Unity processes and
  other opted-in jobs; unrelated steps do not receive access. Other host cache
  directories and host preferences are not included.
- The literal `/private/tmp/unitypreflock`.
- The OS user's `TemporaryItems` directory (read/create only) and its
  `NSIRD_Unity_*` temporary subtrees (read/write), resolved through macOS rather
  than overridden `HOME` or `TMPDIR`.
- Exact `.NET` mutex parent directories under `/private/tmp/.dotnet/shm` and
  `lockfiles` (read/create/mode only), Unity Licensing global mutexes for the
  runner identity (including numeric version suffixes), and the job home's two
  Unity licensing/audit rolling-log mutex names.
- CoreCLR staging directories exactly matching `/private/tmp/.coreclr.XXXXXX`
  (six alphanumeric characters; read/create/mode/delete, no subpaths), which
  the Unity Licensing Client uses to create the `.NET` mutex directories.
- Unix file and socket operations for `Unity-LicenseClient-runner` sockets
  (including version/notification variants), numeric `Unity-Upm-*` sockets, and
  the shader compiler's `Unity-ShaderCompilerIPC-<pid>-<id>` socket and lock file.
- Listening on and accepting TCP/UDP connections on any port, and outbound
  connections to loopback (`localhost`) on any port. The editor serves asset
  import workers on an ephemeral loopback port passed as `-srvPort`; without
  this, workers time out connecting and the editor restarts them indefinitely.
  **Seatbelt cannot restrict listening to loopback interfaces**, so selected
  Unity processes can listen on all interfaces (including the editor's player
  connection on `0.0.0.0`) and accept connections from the network, as an
  unsandboxed editor would. They can also reach other loopback services on the
  host. Direct outbound connections to non-loopback addresses remain denied;
  outbound network access still goes through the proxy.
- `.NET` global mutexes named by a GUID under `/private/tmp/.dotnet/shm` and
  `lockfiles` (read/write), used by the Licensing Client while holding a floating
  license lease.
- Licensing Client machine bindings: the `com.apple.airportd` service (CoreWLAN
  Wi-Fi hardware address; it also provides other Wi-Fi functions) and reading
  `/private/var/run/resolv.conf`, required by .NET network-interface queries.
  Without these, floating license handshakes fail and Unity excludes package
  code from compilation.
- Listing the literal `/private/tmp` directory (also reached through `/tmp`),
  needed by the IL Post Processor trigger's socket discovery. This reveals
  shared temporary filenames, but does not grant contents or writes to
  unrelated entries or recursive directory listing.
- Unix file and socket operations for `ilpp.sock-` followed by exactly 32
  lowercase hexadecimal characters, used by Unity's IL Post Processor runner.

All sandboxed processes receive `DOTNET_USE_POLLING_FILE_WATCHER=1` by default.
This makes .NET's `PhysicalFileProvider` poll for file changes instead of relying
on `FileSystemWatcher`; detection may be less immediate and involve periodic
filesystem checks. Workflow environment values and `--pass-env` can override
this default. Unity opt-in processes still force the value to `1` for that
invocation. `--no-sandbox` does not set the default.

For Java/Gradle socket compatibility, these processes also receive
`-Djava.net.preferIPv4Stack=true` appended to `JDK_JAVA_OPTIONS`, together with
`http.proxyHost`, `http.proxyPort`, `https.proxyHost`, and `https.proxyPort`
system properties pointing to the job's network proxy. Java/Gradle does not
automatically use the `HTTP_PROXY`/`HTTPS_PROXY` environment variables.
`http.nonProxyHosts` is explicitly empty unless `--allow-localhost` is enabled,
in which case it allows `localhost`, `127.0.0.1`, and `[::1]`, matching the
proxy environment's loopback bypass policy. Host allowlisting still applies
to proxied requests; no external direct-connection grant is added. Unity's Android
Java launcher removes `JAVA_TOOL_OPTIONS` and `_JAVA_OPTIONS`, so those variables
cannot carry this compatibility setting. `JAVA_TOOL_OPTIONS` is not modified
by cpp-actions. `JDK_JAVA_OPTIONS` is supported by Java 9 and later. Existing
options are preserved; an earlier conflicting value in that variable is
overridden by the appended options. This affects all JVMs inheriting the selected
process's environment, including Gradle clients and daemons, not just Gradle.
Unrelated steps and nested separate actions receive only the job-local Java/Maven
home options described above, not these Unity-specific overrides.
The wildcard bind/inbound grant above is also required for Gradle daemon startup;
outbound IP connections remain limited to loopback and the proxy.

Their `TMPDIR` is also replaced with a real, short, job-specific directory under
`/private/tmp/cpa-u-XXXXXX/`, created with owner-only (`0700`) permissions.
This avoids macOS's 104-character Unix socket path limit in Bee/.NET.
Only Unity opt-in invocations can read/write that directory or use its Unix
sockets; unrelated steps and nested separate actions retain the normal job
`TMPDIR` and do not receive access. `RUNNER_TEMP` is unchanged.
The directory is shared by selected Unity invocations within one job, not
between jobs. Its path is logged and recorded in the job's `unity-tmp-path`
file. It is removed at job completion after child-process cleanup, including
failed/cancelled jobs. With `--keep-workdir` it is retained separately from the
run directory and must be removed separately when no longer needed. Abrupt
runner termination can also leave it behind. Processes outside the sandbox
running as the same OS user are not isolated by these permissions.

These are process-tree permissions, not a time-limited elevation; surviving
children retain them. Selected processes can access matching host/other-job
Unity IPC, temporary files and editor cache, and change the modes of the shared mutex parent
directories. The grant is not cross-job IPC isolation.

This fixes the editor's temporary-directory read-only check, Licensing Client
mutex initialization, machine bindings for floating licenses, IL Post
Processor discovery/connection, parallel asset import workers, and shader
compiler startup. It is **not a complete Unity environment or license
activation setup**. It does not grant host preferences/metrics database access,
general machine-identity queries, Apple security services, or direct
network access to licensing/package servers. Apple security uses the separate
`--allow-apple-security-for` option. Network services must use the configured
proxy and explicit host policy; a client that ignores proxy settings remains
blocked. Licensing handshake/entitlement failures must be diagnosed separately,
not treated as successful builds.

### Opt-in Unity network hosts

Use `cpp-actions run --allow-unity-hosts`, or set the following in the trusted
user configuration, to enable a fixed public-service host preset for every
step in every job:

```yaml
allow-unity-hosts: true
```

This is disabled by default and additive with CLI flags and `allow-hosts`.
It is separate from `allow-unity-for` / `allow-unity-step`: the network preset
does not grant filesystem, IPC, Apple security, or direct network access.
The proxy still mediates every connection.

The preset contains these exact public Unity service hosts and TLS certificate
revocation endpoints observed during Unity workflows:

- `packages.unity.com`
- `cdn.packages.unity.com`
- `download.packages.unity.com`
- `public-cdn.cloud.unity3d.com`
- `license.unity3d.com`
- `cdp.cloud.unity3d.com`
- `config.uca.cloud.unity3d.com`
- `crl3.digicert.com`
- `crl4.digicert.com`

There are no wildcard domains or automatic additions from logs. New redirect
or download hosts still require an explicit grant or a reviewed preset update.
Preset entries alone cannot connect to private resolved addresses. An explicit
exact non-default `allow-hosts` entry retains its private-address exception,
including when the same hostname is in the preset.

Company/project-specific registries, application services, and license servers
are not included; configure them individually using `allow-hosts`. For example:

```yaml
allow-unity-hosts: true
allow-hosts:
  - registry.example.test
```

An individual host allowance is not port-specific. The preset does not install
licenses or credentials, fix signature/machine-identity checks, or configure
clients that ignore the proxy.

## Actions

- `uses: ./path` loads `action.yml` or `action.yaml` from the checked-out workspace.
- `owner/repo[/path]@ref` resolves GitHub refs to commit SHAs and caches actions
  under `<cache>/actions`. Each job executes a writable copy of the complete
  action repository under its temporary directory. Repeated uses of the same
  repository commit within a job share that copy; changes do not affect the
  shared action cache or other jobs. Sandboxed `actions/setup-python` is an
  exception, as described below.
- JavaScript actions support `node20`/`node24`, `INPUT_*`, pre/main/post,
  `pre-if`/`post-if`, and `GITHUB_STATE`. `--node` explicitly selects one
  executable for all actions. Otherwise, a matching host Node is preferred.
  When the host Node is a nodenv shim, the runner bypasses the shim and selects
  the newest usable same-major executable among that nodenv installation's
  `versions` directory and the runner's Node cache. Versions are compared
  numerically, including prerelease ordering; the cache wins ties. This prevents
  an action's `.node-version` from selecting a missing development version.
  If no suitable installed runtime is available, the runner downloads one from
  `actions/node-versions` GitHub Releases. If downloading fails, it falls back
  to host Node with a warning; a nodenv shim may still fail on `.node-version`.
  The selected executable is cached per major for the run, and its parent
  directory is not implicitly exposed to sandboxed steps. Selection checks the
  runtime version outside the sandbox; Node installations that use private
  shared libraries may require separate `--allow-read` grants for those files.
  Entry points are resolved relative to the action directory and may reference
  shared files elsewhere in the same repository (for example, `../lib/cleanup.js`).
  Paths and symlinks escaping the job's action-repository copy are rejected;
  local actions use the job workspace as their repository boundary.
  An empty job-level `package.json` stops Node's package-scope lookup from
  reaching host directories, including when the sandbox is disabled.
  Existing action/repository package files and their module types are preserved.
- Composite actions support inputs/outputs, `github.action_path`, and nested
  steps up to 10 levels deep.
- `actions/checkout` places the local repository working tree, including
  uncommitted changes, in the job workspace.
  For other repositories, the checkout token (or runner token by default) is
  passed to both fetch and checkout, including Git LFS smudge subprocesses.
  These credentials are process-scoped and are not saved in repository config.
  Remote checkouts with `lfs: true` reuse Git LFS objects across runs in
  `<cache>/lfs/github.com/<owner>/<repo>` (case-insensitive repository identity).
  Git fetch/authentication still runs on every checkout. Git LFS manages downloads
  into this repository-specific store; checkout logs its location.
  Completed objects for the checked-out revision are independently copied into
  the job's `.git/lfs` (copy-on-write clones on supported macOS filesystems),
  so later resets of that revision need neither network access nor shared-store
  access. The shared store is not granted to sandboxed steps and its path is not
  saved in repository configuration. Submodule LFS objects and local working-tree
  snapshot checkouts do not use this store. Objects are retained without automatic
  eviction; remove an unused repository's LFS cache when no checkout is using it.
  `submodules: true` initializes direct submodules; `submodules: recursive`
  also initializes nested submodules. Both honor `fetch-depth` (default `1`,
  `0` for full history), use the checkout token for GitHub HTTPS authentication,
  and fail checkout if a submodule cannot be fetched.
  During submodule operations, `git@github.com:owner/repo` and
  `ssh://git@github.com/owner/repo` are mapped to HTTPS so the checkout token can
  authenticate them. This mapping is process-scoped, including nested submodules;
  no URLs or credentials are saved to repository or host config.
  Other hosts (including old GHES URLs) and organization names are not migrated.
  GitHub HTTPS tokens do not authenticate SSH or other hosts, and host user/system
  Git config stays disabled.
- `actions/cache`, including `/restore` and `/save`, uses `<cache>/cache`;
  these caches are not shared with GitHub.
- `actions/upload-artifact` and `actions/download-artifact` exchange files
  between jobs within the same workflow run under `<run>/artifacts` (or the
  `--artifact-dir` root), partitioned by workflow run ID. Cross-run
  downloads and GitHub retention, compression, and storage APIs are not emulated.
- Other JavaScript/composite actions, including setup actions, execute normally.
  `RUNNER_TOOL_CACHE` points to `<cache>/toolcache`, shared across runs.
  Use `--allow-host` when setup actions require non-GitHub download hosts.
- On sandboxed macOS, the root JavaScript action `actions/setup-python@<ref>`
  uses the fixed `/Users/runner/hostedtoolcache` path instead of
  `RUNNER_TOOL_CACHE`. Its pre, main, and post processes and their children get
  read/write access to that host cache only. This special grant also applies
  when the action is nested in a composite, but not to forks, subpaths, local
  actions, or surrounding composite steps. The action runs from the protected
  action repository cache, not the job-writable copy; the action source and
  Node executable must not overlap writable locations.

  Before running the action, cpp-actions creates the cache directory and any
  missing parent directories using the runner's existing OS permissions; it
  does not request administrator privileges or change existing ownership or
  permissions. If a parent cannot be created or traversed (for example,
  `/Users` may be writable only by an administrator), the error identifies
  the failing component. An administrator must then provision the necessary
  parent or cache directory with appropriate permissions.

  Required setup-python-compatible versions and architectures must still be
  provisioned separately. The cache must be readable and writable by the
  runner, must not contain symlinked path components, and must not overlap
  ordinary sandbox writable directories. Installation directory components
  must also not be symlinks. For example, an ARM64 CPython installation may use
  `Python/3.11.9/arm64` with its corresponding `Python/3.11.9/arm64.complete`
  marker under the host cache. See the upstream
  [hosted tool cache documentation](https://github.com/actions/setup-python/blob/v7/docs/advanced-usage.md#hosted-tool-cache)
  and [Python release artifacts](https://github.com/actions/python-versions/releases).
  The normal sandbox does not grant writes to system locations or administrator
  privileges: a cold CPython installation may invoke the macOS package installer
  and fail even when the cache directory exists. Inputs such as `check-latest`
  can select an unprovisioned version.

  To provision standard stable CPython, run
  [scripts/install-hosted-python.sh](../scripts/install-hosted-python.sh) from a
  **host terminal**, as the same OS user that runs cpp-actions, not with
  `sudo bash` and not inside `setup-steps`:

  ```bash
  # Resolve the latest published compatible 3.11 patch and inspect it.
  bash scripts/install-hosted-python.sh --version 3.11 --dry-run
  # Install a specific patch only after reviewing the host changes.
  bash scripts/install-hosted-python.sh --version 3.11.9 --accept-system-changes
  ```

  The script requires macOS, Bash, `curl`, `jq`, `shasum`, `tar`, and `pkgutil`.
  `--version` accepts `3.MINOR` or `3.MINOR.PATCH`; only official package-based
  macOS releases are supported, not source archives, prereleases, free-threaded
  Python, PyPy, or GraalPy. `--architecture arm64|x64` defaults to the current
  process architecture and must match it. For x64 on Apple Silicon, use a
  matching Rosetta terminal/process and the same architecture in setup-python.
  The script verifies the published SHA-256 and Python Software Foundation
  package signature before executing the official installation script.

  A dry-run downloads and verifies the release but does not execute the
  installer, invoke sudo, write the cache, or check host installation conflicts.
  Actual installation requires `--accept-system-changes`: the upstream script
  uses sudo and changes the system Python.framework, applications,
  `/usr/local/bin` links, pip, certificates, and potentially shell profiles.
  An administrator must first provision a real, readable/writable/searchable
  `/Users/runner/hostedtoolcache` if ordinary OS permissions cannot do so.
  The provisioning script never changes existing ownership or permissions.

  Stop other jobs and Python installers first. The script refuses an existing
  same-minor cache, any existing system Python.framework, the selected Python
  application, or conflicting Python/pip commands; there is no replacement
  option. An exact installation that passes marker, link, version,
  architecture, and runtime checks is reported as already installed instead.
  Its cache lock coordinates only other instances of this script, not
  setup-python or external installers. A failed installation may leave partial
  system/cache changes; inspect and repair them before retrying. No rollback
  or automatic deletion of existing installations is attempted.

  Missing/inaccessible-cache errors and failed sandboxed official setup-python
  lifecycle processes print this provisioning guidance without relaxing the
  sandbox or changing the action result. With a CMake installation, the script
  is also available at `<prefix>/share/cpp-actions/scripts/install-hosted-python.sh`
  (or the configured CMake data directory).

  After successful setup, later steps can read the selected installation
  directories, including versions exposed through `PATH`, `pythonLocation`,
  or the `python-path` output. They cannot write those directories or read
  unselected cache entries. Install packages in a writable virtual environment,
  for example with `python -m venv .venv`, instead of modifying the shared
  interpreter. Binaries symlinked to system frameworks remain subject to the
  normal system-directory permissions.

  The host cache is shared across jobs and runs, regardless of `--cache-dir`,
  and concurrent modifications are not coordinated by cpp-actions. Trust the
  selected action ref, its inputs, environment, `PATH`, and child processes:
  they can influence code executed with cache write access or poison the shared
  cache. Do not share it between mutually untrusted workflows or runners.
- Docker container actions fail with an explicit unsupported-action error.

### Git configuration

Internal Git operations and job processes ignore the machine's user and system
Git configuration, including URL rewrites, credential helpers, and signing
settings. Internal commands also discard inherited `GIT_CONFIG_*` overrides.
Runner defaults include `safe.directory=*`, the standard Git LFS clean/smudge/
process filters (`filter.lfs.required=true`), and the advice settings used by
GitHub-hosted macOS images. This does not install Git LFS: `git-lfs` must be
available on `PATH` when a repository uses LFS.

Each job starts with its own writable global configuration at
`$HOME/.gitconfig`. `git config --global` changes persist across that job's steps,
including composite and JavaScript actions, but do not affect other jobs or the
host. Internal commands use a separate temporary configuration. Repository-local
configuration and explicit `git -c` overrides still apply. The job's
`GIT_CONFIG_GLOBAL`, `GIT_CONFIG_SYSTEM`, and `GIT_CONFIG_NOSYSTEM` paths/settings
are runner-controlled even with `--no-sandbox`; workflow-supplied command config
overrides such as `GIT_CONFIG_COUNT` remain usable.

As on the hosted images, `safe.directory=*` disables Git's ownership check; it
does not grant filesystem access or bypass the sandbox. These defaults are not
a complete reproduction of a hosted image's installed tools or SSH setup.

## Security model and limitations

The job-specific `RUNNER_WORKSPACE` directory (the parent of `GITHUB_WORKSPACE`)
is writable, so workspace setup actions can rename the checkout, create sibling
workspaces, and replace the checkout with a symlink within that directory.
Checkout preserves such a workspace-root symlink, and local actions resolve
against its current target. Other jobs and protected control files remain outside
this grant; local actions cannot follow a workspace root outside `RUNNER_WORKSPACE`.

The Seatbelt profile denies access by default, permits required system software
and job/cache directories, and routes network access through a host-allowlisting
HTTP/HTTPS proxy. Default hosts are GitHub's `github.com` and subdomains,
`githubusercontent.com` and subdomains, and `ghcr.io` and subdomains, plus
the exact .NET download hosts `builds.dotnet.microsoft.com` and `ci.dot.net`,
Microsoft's redirect host `aka.ms`, the NuGet API host `api.nuget.org`,
the NuGet storage host `nugetregistryv2prod.blob.core.windows.net`,
the RubyGems hosts `rubygems.org` and `index.rubygems.org`,
Google's Android SDK/Maven download host `dl.google.com`,
the Google Maven host `maven.google.com`,
the Maven Central host `repo.maven.apache.org`,
the Gradle Plugin Portal host `plugins.gradle.org`,
the Eclipse Adoptium API host `api.adoptium.net` for Temurin JDK resolution,
and Apple's certificate download host `www.apple.com` (exact hostname, all
paths and ports).
Redirect destinations must also be allowed separately.
Use `--allow-host` for other services. Tools must support the supplied proxy
environment variables; direct outbound connections are blocked rather than
automatically proxied. Wildcard/default host grants do not authorize
private/loopback/link-local destinations. Explicitly granting an exact,
non-default host can open a private service through the proxy, so review grants
carefully.

The proxy listens on `127.0.0.1` and `::1` at the same port and is advertised as
`http://localhost:<port>`. This lets .NET use native IPv6 loopback instead of
IPv4-mapped IPv6, which Seatbelt's localhost rule does not match. Only the
proxy port is allowed by default; other loopback ports and direct remote
connections remain blocked. IPv6 loopback must be available; startup fails
explicitly if either listener cannot be created.
Each job's proxy handles up to 128 connections concurrently by default. Change
this with `--proxy-max-connections N` or trusted user configuration
`proxy-max-connections: N` (1–4096; the CLI value takes precedence). The same
limit bounds the listen backlog request (the operating system may cap it) and
concurrent DNS lookups per proxy. At capacity, new connections wait in the
listen backlog rather than being accepted and immediately reset; the job log
reports that it is waiting for a slot.
Waiting is interruptible when the job's proxy stops. Client timeouts and the
operating system's backlog limits still apply.
Plain HTTP requests are forwarded with `Connection: close` and the proxy keeps
the upstream's sending side open until the response completes: some servers
(for example a Unity floating license server) drop a request without responding
when they see a half-close. Client bytes after the framed body are never relayed.
Upstream connection failures return HTTP 502 and log the destination host/port,
elapsed time, and DNS or TCP error (including numeric resolved addresses and
error codes). Failure and capacity warnings also include the local wall-clock
time and the proxy's active and upstream-connecting connection counts, to help
distinguish slot exhaustion from network/VPN stalls. DNS resolution has a 15-second deadline, followed by a separate
15-second deadline for TCP connection establishment. IPv4 and IPv6 addresses are
interleaved, preserving the resolver's first family preference. Pending attempts
do not block later addresses: new attempts start 250 ms apart, or immediately
after a connection error. The first successful connection wins and all other
attempts are closed. All resolved addresses are checked against the private
address policy before any attempt starts. Connection waits remain interruptible
when the proxy stops. Policy denials remain HTTP 403. Diagnostics do not include
request paths, headers, or credentials.

The generated profile is kept outside the step-writable workspace, home,
temporary, and tool-cache directories. There are no blanket grants to the
operating system's user temporary/cache directories or all of `/opt`; the
read grant under `/opt` is limited to `/opt/homebrew`.
Generated step scripts are also outside step-writable directories. File-command
reads reject symlinks, non-regular files, and multiply linked files; each step's
process group is killed before file commands are consumed.
Tools that ignore `TMPDIR` and use macOS's per-user system cache/temp paths
may require tool-specific configuration to use the job directories instead.
For sandboxed macOS steps, a read-only `mktemp` wrapper is prepended to `PATH`
so implicit temporary paths (including `mktemp -d` and `mktemp -t prefix`) use
the job's `TMPDIR`. Explicit templates retain their locations, and explicit
`-p`/`--tmpdir` options are passed through to `/usr/bin/mktemp`.
Direct `/usr/bin/mktemp` calls and other programs using macOS's native temporary
directory APIs are not redirected; the host's temporary directories remain
outside the default writable grants.

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

### Reusable workflows and job settings

Jobs can call `./.github/workflows/build.yml` or
`owner/repo/.github/workflows/build.yml@ref`. Remote refs are resolved to a
commit and cached like actions. Nested local calls resolve relative to the
repository containing the called workflow. Checkouts still use the caller's
working-tree snapshot. Calls support typed `workflow_call` inputs and defaults,
explicit secrets or `secrets: inherit`, and workflow outputs forwarded through
`needs.<job>.outputs`. Secrets are not forwarded unless requested; inheritance
applies only to the immediate call. Nesting is limited to ten workflow levels.
For string inputs, an explicit YAML `null`, an empty YAML value, or an expression
that evaluates to `null` is normalized to `""`, including in defaults. Explicit
null values do not select a declared default; omitted inputs still use the
default or fail if required. Boolean and number inputs retain strict type checks.

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
For matrix calls, each output retains the last successful non-empty value.

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
made only in the copy. Automatic Git maintenance and garbage collection are
disabled for internal snapshot-creation commands to prevent background repacking
from racing with checkout copies; repository and global Git settings are unchanged.
The original repository is not modified. Job workspaces
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
preserving report schema. Masking includes JSON-escaped and single-level,
unwrapped standard Base64 forms of registered secrets (including `GITHUB_TOKEN`
and `add-mask` values).
Like the GitHub runner, Base64 matching also registers the secret with its first
one or two bytes removed to match `Base64(username:secret)` across byte alignments.
This can leave an encoded prefix containing the username and up to two secret
bytes visible; one- or two-byte secrets may not match at all in some alignments.
Repeated Base64 encoding and arbitrary transformations are not covered; use
`add-mask` to register additional derived values before logging them.
Summaries are displayed at completion; `--debug`
enables debug commands, and `--no-color` or `NO_COLOR` disables colored logs.
Local execution does not reproduce GitHub-hosted images, GitHub artifact/cache APIs,
or OIDC services and does not guarantee identical results to GitHub.
