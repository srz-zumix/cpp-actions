# Copilot instructions for cpp-actions

cpp-actions is a C++20 CLI that runs GitHub Actions workflows locally. Step execution is sandboxed with macOS Seatbelt (`/usr/bin/sandbox-exec`) by default. `README.md` is the authoritative user-facing spec (security model, opt-in grants, supported workflow features). Update it whenever behavior changes.

## Build, test, lint

Requires CMake 3.24+, Ninja, a C++20 compiler and Git. Presets live in `CMakePresets.json` (`debug`, `release`, `asan`, `vcpkg`, `ci`). Each one builds into `build/<preset>`.

```sh
cmake --preset debug && cmake --build --preset debug
ctest --preset debug                      # all tests
ctest --preset debug -R '^unit$'          # one CTest test: unit | cli | cli_run | cli_actions | hosted_python_script
build/debug/tests/cpa_unit_tests --iutest_filter='ParserTest.*'          # one suite
build/debug/tests/cpa_unit_tests --iutest_filter=StringsTest.Trim        # one test case
build/debug/src/cpp-actions --help
```

- CI (`.github/workflows/ci.yml`) runs the `ci` preset (`CPA_WARNINGS_AS_ERRORS=ON`) on macOS and Ubuntu, plus `asan` (ASan/UBSan) on macOS. Code must build warning-free with `-Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor`.
- Formatting follows `.clang-format`: Google style, 120 columns, left pointer alignment, and include blocks kept as written.
- Xcode/Simulator integration tests only run when you opt in with env vars: `CPA_TEST_XCODE_STORYBOARD=1`, `CPA_TEST_XCODE_ASSET_SYMBOLS=1` and `CPA_TEST_XCODE_ASSET_CATALOG=1`. See the README Build section.
- Dependencies (`cmake/Dependencies.cmake`): yaml-cpp, nlohmann_json and CLI11 come from `find_package` (vcpkg/Homebrew) first and fall back to a hash-pinned `FetchContent` download. The test framework **iutest** comes from `IUTEST_ROOT` if set, otherwise from the `develop` branch tarball. Delete `build/<preset>/_deps/iutest-*` to refresh it.

## Architecture

All sources except `src/cli/main.cpp` build into the static library `cpa_core`. The executable `cpp-actions` adds the CLI11 front end. Include paths are rooted at `src/` (`#include "engine/runner.hpp"`). Each directory maps to a namespace `cpa::<dir>`, except `workflow/yaml.*`, which is `cpa::yaml`.

Data flow for `cpp-actions run`:

1. **cli/main.cpp** has the subcommands `validate`, `list`, `graph`, `run` and `clean`. It loads trusted user config (`config/`: `$XDG_CONFIG_HOME/cpp-actions/config.yml` or `--config`, never from the repository), builds `engine::RunOptions`, and resolves the event (`engine/event`) and git info (`workspace/git`).
2. **workflow/** parses YAML (`yaml.cpp` wraps yaml-cpp and keeps source positions) into the `model.hpp` structs. It also handles trigger filtering (`events`), matrix expansion (`matrix`) and `scopes`, which defines which contexts are available at each expression site and how `if` conditions get an implicit `success()`.
3. **engine/catalog** discovers and selects workflows. **engine/runner** (`Runner`, pimpl) schedules jobs: `needs` DAG, matrix, `--parallel` slots, reusable-workflow calls (up to 10 levels deep) and cancellation. Shared run state (`RunState`, `WorkflowRun` and `JobInstance` in `engine/job.hpp`) is thread-safe because jobs run concurrently.
4. **engine/job** runs the steps of a single job instance. It uses `engine/shell` for shells, `engine/commands` for `GITHUB_ENV/OUTPUT/PATH/STATE/STEP_SUMMARY` file commands and `::workflow commands::`, `engine/masker` to mask secrets (including Base64/JSON forms), and `engine/evaluate` with `expr/` for `${{ }}` expressions and templates over `expr::Value`.
5. **actions/**: `action` loads `action.yml` for JavaScript, composite and local actions. `fetch` resolves `owner/repo@ref` to a SHA, caches it, and provides Node through `NodeProvider`. `builtin` emulates `actions/checkout`, `cache`, `cache/restore`, `cache/save`, `upload-artifact` and `download-artifact` in-process instead of downloading them.
6. **workspace/snapshot** snapshots the local working tree, including uncommitted and untracked files. Self-repository checkouts use this snapshot, and the original repository is never modified.
7. **sandbox/**: `profile` generates the Seatbelt SBPL profile. `sandbox::Sandbox::wrap()` prefixes argv with `sandbox-exec -f <profile>` and caches derived profiles per grant combination. `proxy` is the per-job HTTP/HTTPS allowlisting proxy, the only network path out of the sandbox. `hosted_python` handles sandboxed `actions/setup-python`.
8. **engine/report**, **console** and **cleanup** handle the run report, colored and masked logging, and removal of `--keep-workdir` directories.

## Key conventions

### Repository data hygiene

- This policy applies to every repository-managed file, including source, tests, fixtures, README and other documentation, instructions, scripts, configuration, and generated files proposed for version control.
- Never include information identifying unrelated real projects, customers, or organizations. Do not use real usernames, personal names, IDs, commit/artifact hashes, organization/repository names, email addresses, internal hostnames, IP addresses, or local user paths as test data or examples. Never repurpose user-provided information, actual API responses, logs, or captured environments as fixtures, even after partial redaction.
- Construct fictional values that satisfy the exact format and constraints under test. Use `fixture-user` / `fixture-owner`, reserved domains such as `registry.example.test`, and emails such as `fixture-user@example.test`. Generate hashes from synthetic inputs or use clearly synthetic hex strings of the required length; construct synthetic UUIDs with the required version/variant. Use synthetic private addresses when testing private ranges and in-memory synthetic global-unicast addresses when testing public classification; never use real service endpoints as address fixtures.
- Keep fixtures offline and isolated: mock API responses from scratch and generate fixture files locally. Do not capture host identities, credentials, project output, or process metadata into version-controlled files.
- Actual dependency download URLs and integrity pins, cpp-actions repository metadata and legal notices, required protocol/OS constants, and standard algorithm test vectors are not project-specific example data. Retain them where required for functionality or exact compatibility assertions; do not use this distinction to import real project data into examples or fixtures.
- Before finishing, review all tracked files and untracked files eligible for addition, plus the proposed diff, for identity, URL, email, address, ID, and hash candidates. Do not merely search for already known identifiers. Do not reintroduce removed real values in comments, reports, or regression-test denylists; deletion lines needed to remove existing values are unavoidable and are not permission to add or copy them.

### Execution and test conventions

- **Security posture is the core invariant.** The sandbox fails closed: it never silently falls back to unsandboxed execution. Every extra capability is an explicit opt-in grant. Examples are `--allow-host`, `--allow-read/--allow-write`, and per-job or per-step flags (`allow_*_for` / `allow_*_step` in `SandboxOptions`) for Docker, MSBuild, Unity, Xcode and Apple security. To add a grant, update `SandboxOptions` (engine/runner.hpp), `SandboxConfig`/`wrap()` (sandbox) and `profile.cpp`, then add the CLI flag, the config key (`config/`) and a README section.
- Generated profiles, step scripts and protected control files must stay **outside** step-writable directories (workspace, HOME, TMPDIR, tool cache). File-command reads reject symlinks, non-regular files and hard-linked files. Keep these checks when you touch step I/O.
- Repository content is untrusted. Repository files must never act as user configuration (the CLI tests assert this). Secrets and vars are loaded only when a workflow actually runs, never for `--dry-run`.
- Non-macOS hosts run jobs only with `--no-sandbox` plus an explicit `--host-label`. Platform-specific code uses `#if defined(__APPLE__)`. Tests that need Seatbelt, Node, Git LFS and similar check for them and call `IUTEST_SKIP() << "reason"` instead of failing.
- Tests use **iutest**, not gtest: `IUTEST(Suite, Name)`, `IUTEST_EXPECT_*` and `IUTEST_ASSERT_*`. Name suites `<Area>Test` in `tests/unit/<area>_test.cpp`, and list new files in `tests/CMakeLists.txt` (sources are listed explicitly; no globbing, same for `src/CMakeLists.txt`). Use the helpers in `tests/unit/test_support.hpp` (`cpa_test::TempDir`, `init_repo`, `git` with an isolated git env, and `remove_retained_workspaces`).
- On macOS, helper probe binaries in `tests/helpers/` are built and passed to the unit tests as compile definitions (`CPA_*_PROBE`, `CPA_CMAKE_COMMAND`).
- The end-to-end CLI tests are CMake scripts (`tests/cli_test.cmake`, `tests/integration/run_test.cmake`, `tests/cli/run.cmake`). They create fixture repos and workflows with `file(WRITE ...)` and run the built binary. Fixtures use custom `runs-on` labels together with `--host-label`.
- Comments are short and explain intent or constraints, usually at the declaration in the header.
