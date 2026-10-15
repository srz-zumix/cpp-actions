if(NOT DEFINED CLI OR NOT DEFINED WORK)
  message(FATAL_ERROR "CLI and WORK must be provided")
endif()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}/repo/.github/workflows" "${WORK}/home"
  "${WORK}/config/cpp-actions" "${WORK}/tmp" "${WORK}/readable")
set(repo "${WORK}/repo")
find_program(GIT git REQUIRED)
execute_process(COMMAND "${GIT}" init --quiet "${repo}" RESULT_VARIABLE git_status)
if(NOT git_status EQUAL 0)
  message(FATAL_ERROR "could not initialize CLI fixture repository")
endif()
file(WRITE "${repo}/.github/workflows/success.yml" [=[
name: CLI success
on: push
jobs:
  smoke:
    runs-on: cli-local
    env:
      TEST_SECRET: ${{ secrets.TEST_TOKEN }}
      TEST_MODE: ${{ vars.MODE }}
    steps:
      - run: |
          test "$CPA_CLI_PASSED" = forwarded
          test "$TEST_MODE" = cli
          test -z "${CPA_CLI_UNPASSED+x}"
          test -z "${TEST_TOKEN+x}"
          case "$HOME" in */jobs/*/home) ;; *) exit 31 ;; esac
          case "$TMPDIR" in */jobs/*/tmp) ;; *) exit 32 ;; esac
          test -d "$HOME"
          test -d "$TMPDIR"
          printf '%s\n' "$TEST_SECRET"
          echo CLI_EXECUTED
]=])
file(WRITE "${repo}/.github/workflows/failure.yml" [=[
name: CLI failure
on: push
jobs:
  smoke:
    runs-on: cli-local
    steps:
      - run: exit 7
]=])
# Repository files must not grant permissions or be interpreted as user config.
file(WRITE "${repo}/.cpp-actions.yml" "untrusted-key: ignored\n")
file(WRITE "${repo}/config.yml" "untrusted-key: ignored\n")
file(WRITE "${WORK}/config/cpp-actions/config.yml" [=[
vars:
  MODE: config
secrets-file: secrets.env
]=])
file(WRITE "${WORK}/config/cpp-actions/secrets.env" "TEST_TOKEN=cli-mask-test-value\n")
set(command "${CMAKE_COMMAND}" -E env
  "HOME=${WORK}/home" "XDG_CONFIG_HOME=${WORK}/config" "TMPDIR=${WORK}/tmp"
  "CPA_CLI_PASSED=forwarded" "CPA_CLI_UNPASSED=must-not-leak"
  "TEST_TOKEN=cli-host-mask-value" "${CLI}")

function(check_cli name expected)
  execute_process(COMMAND ${command} run -C "${repo}"
    --cache-dir "${WORK}/cache" --run-dir "${WORK}/runs/${name}" ${ARGN}
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 30)
  if(NOT "${status}" STREQUAL "${expected}")
    message(FATAL_ERROR "${name}: expected ${expected}, got ${status}\n${output}\n${error}")
  endif()
  set(cli_output "${output}" PARENT_SCOPE)
  set(cli_error "${error}" PARENT_SCOPE)
endfunction()

check_cli(dry-run 0 --dry-run --workflow success.yml --config "${WORK}/missing.yml")
if(EXISTS "${WORK}/runs/dry-run")
  message(FATAL_ERROR "dry-run created run output")
endif()
if(NOT cli_output MATCHES "CLI success")
  message(FATAL_ERROR "dry-run did not render the workflow")
endif()

if(NOT HOST_IS_APPLE)
  check_cli(fail-closed 2 --workflow success.yml --host-label cli-local)
  if(NOT cli_error MATCHES "refusing execution" OR EXISTS "${WORK}/runs/fail-closed")
    message(FATAL_ERROR "unsupported platform did not fail closed before creating output")
  endif()
endif()

check_cli(success 0 --no-sandbox --workflow success.yml --host-label cli-local
  --allow-host example.com --allow-read "${WORK}/readable"
  --pass-env CPA_CLI_PASSED --parallel 1 --keep-workdir --var MODE=cli)
if(NOT cli_error MATCHES "WARNING: --no-sandbox" OR NOT cli_output MATCHES "CLI_EXECUTED")
  message(FATAL_ERROR "execution or unsandboxed warning missing\n${cli_output}\n${cli_error}")
endif()
if(cli_output MATCHES "cli-mask-test-value")
  message(FATAL_ERROR "secret appeared unmasked on console")
endif()
file(READ "${WORK}/runs/success/report.json" report)
string(JSON result GET "${report}" result)
if(NOT result STREQUAL "success")
  message(FATAL_ERROR "success report has result ${result}")
endif()
file(GLOB kept "${WORK}/runs/success/jobs/*/home")
if(NOT kept)
  message(FATAL_ERROR "--keep-workdir did not preserve job directories")
endif()

check_cli(explicit-config 0 --no-sandbox --workflow success.yml --host-label cli-local
  --config "${WORK}/config/cpp-actions/config.yml" --pass-env CPA_CLI_PASSED=forwarded
  --secret TEST_TOKEN=cli-explicit-mask-value --var MODE=cli)
file(GLOB removed "${WORK}/runs/explicit-config/jobs/*/home")
if(removed OR cli_output MATCHES "cli-explicit-mask-value")
  message(FATAL_ERROR "default cleanup or explicit secret masking failed")
endif()
check_cli(imported-secret 0 --no-sandbox --workflow success.yml --host-label cli-local
  --pass-env CPA_CLI_PASSED --secret TEST_TOKEN --var MODE=cli)
if(cli_output MATCHES "cli-host-mask-value")
  message(FATAL_ERROR "explicitly imported host secret appeared unmasked")
endif()
file(WRITE "${repo}/trusted.yml" "env-pass: CPA_CLI_PASSED\nvars:\n  MODE: cli\n")
check_cli(trusted-repo-config 0 --no-sandbox --workflow success.yml --host-label cli-local
  --config "${repo}/trusted.yml" --secret TEST_TOKEN)
check_cli(missing-config 2 --no-sandbox --workflow success.yml --config "${WORK}/missing.yml")
check_cli(invalid-env 2 --no-sandbox --workflow success.yml --pass-env =invalid)
check_cli(failure 1 --no-sandbox --workflow failure.yml --host-label cli-local)
file(READ "${WORK}/runs/failure/report.json" report)
string(JSON result GET "${report}" result)
if(NOT result STREQUAL "failure")
  message(FATAL_ERROR "failure report has result ${result}")
endif()
check_cli(skipped 0 --no-sandbox --workflow failure.yml)
file(READ "${WORK}/runs/skipped/report.json" report)
string(JSON result GET "${report}" result)
if(NOT result STREQUAL "skipped")
  message(FATAL_ERROR "unmatched host label was not skipped")
endif()

file(MAKE_DIRECTORY "${WORK}/outside-command")
set(command_sentinel "CLI_FILE_COMMAND_SECRET_MUST_NOT_LEAK_58129")
file(WRITE "${WORK}/outside-command/sentinel" "${command_sentinel}\n")
set(command_modes unsandboxed)
if(HOST_IS_APPLE)
  list(APPEND command_modes sandboxed)
endif()
foreach(mode IN LISTS command_modes)
  set(isolation_flags)
  if(mode STREQUAL "unsandboxed")
    set(isolation_flags --no-sandbox)
  endif()
  foreach(file_command IN ITEMS GITHUB_STEP_SUMMARY GITHUB_ENV GITHUB_OUTPUT GITHUB_STATE GITHUB_PATH)
    set(symlink_workflow [=[
name: CLI file command boundary
on: push
jobs:
  symlink:
    runs-on: cli-local
    steps:
      - run: |
          rm "$@file_command@"
          ln -s "$CPA_CLI_COMMAND_SECRET" "$@file_command@"
]=])
    string(CONFIGURE "${symlink_workflow}" symlink_workflow @ONLY)
    file(WRITE "${repo}/.github/workflows/command-symlink.yml" "${symlink_workflow}")
    string(TOLOWER "${file_command}" command_name)
    set(case_name "symlink-${mode}-${command_name}")
    check_cli("${case_name}" 1 ${isolation_flags} --workflow command-symlink.yml --host-label cli-local
      --pass-env "CPA_CLI_COMMAND_SECRET=${WORK}/outside-command/sentinel")
    file(READ "${WORK}/runs/${case_name}/report.json" report)
    string(JSON result GET "${report}" result)
    if(NOT result STREQUAL "failure" OR "${cli_output}${cli_error}${report}" MATCHES "${command_sentinel}")
      message(FATAL_ERROR "file-command symlink leaked host content or did not fail")
    endif()
    file(GLOB logs "${WORK}/runs/${case_name}/logs/*.log")
    foreach(log IN LISTS logs)
      file(READ "${log}" log_text)
      if(log_text MATCHES "${command_sentinel}")
        message(FATAL_ERROR "file-command symlink leaked host content to a job log")
      endif()
    endforeach()
  endforeach()
endforeach()

if(HOST_IS_APPLE)
  if(NOT IS_DIRECTORY "$ENV{HOME}")
    message(FATAL_ERROR "a real host HOME directory is required for the sandbox boundary test")
  endif()
  file(MAKE_DIRECTORY "${WORK}/outside")
  file(WRITE "${WORK}/outside/sentinel" "CLI_EXTERNAL_SENTINEL\n")
  file(WRITE "${WORK}/home/host-secret" "host-home-secret\n")
  file(WRITE "${repo}/.github/workflows/sandbox.yml" [=[
name: CLI sandbox boundary
on: push
jobs:
  boundary:
    runs-on: cli-local
    env:
      HOME: /invalid-workflow-home
      TMPDIR: /invalid-workflow-tmp
    steps:
      - name: Attempt to replace the active sandbox profile
        env:
          HOME: /invalid-step-home
          TMPDIR: /invalid-step-tmp
        run: |
          test "$CPA_CLI_PASSED" = forwarded
          test -z "${CPA_CLI_UNPASSED+x}"
          test -z "${TEST_TOKEN+x}"
          test "$USER" = runner
          test "$LOGNAME" = runner
          case "$HOME" in */jobs/*/home) ;; *) exit 41 ;; esac
          case "$TMPDIR" in */jobs/*/tmp) ;; *) exit 42 ;; esac
          profile="$RUNNER_TEMP/../../sandbox.sb"
          if printf '(version 1)\n(allow default)\n' > "$profile"; then
            echo CLI_PROFILE_WRITABLE
            exit 43
          fi
          printf '(version 1)\n(allow default)\n' > "$RUNNER_TEMP/replacement.sb"
          if mv "$RUNNER_TEMP/replacement.sb" "$profile"; then
            echo CLI_PROFILE_REPLACEABLE
            exit 44
          fi
          echo CLI_PROFILE_PROTECTED
      - name: The next step must still be sandboxed
        run: |
          case "$HOME" in */jobs/*/home) ;; *) exit 45 ;; esac
          case "$TMPDIR" in */jobs/*/tmp) ;; *) exit 46 ;; esac
          if cat "$CPA_CLI_EXTERNAL/sentinel" > /dev/null 2>&1; then exit 47; fi
          if cat "$CPA_CLI_HOST_HOME/host-secret" > /dev/null 2>&1; then exit 48; fi
          if /bin/ls "$CPA_CLI_REAL_HOME" > /dev/null 2>&1; then exit 49; fi
          if printf forbidden > "$CPA_CLI_EXTERNAL/created"; then exit 50; fi
          echo CLI_SANDBOX_INTACT
]=])
  set(saved_command "${command}")
  set(command "${CMAKE_COMMAND}" -E env
    "HOME=${WORK}/home" "XDG_CONFIG_HOME=${WORK}/config" "TMPDIR=${WORK}/tmp"
    "CPA_CLI_PASSED=forwarded" "CPA_CLI_UNPASSED=must-not-leak"
    "TEST_TOKEN=cli-host-mask-value" "CPA_CLI_EXTERNAL=${WORK}/outside"
    "CPA_CLI_HOST_HOME=${WORK}/home" "CPA_CLI_REAL_HOME=$ENV{HOME}" "${CLI}")
  check_cli(sandbox-boundary 0 --workflow sandbox.yml --host-label cli-local --keep-workdir
    --pass-env CPA_CLI_PASSED --pass-env CPA_CLI_EXTERNAL
    --pass-env CPA_CLI_HOST_HOME --pass-env CPA_CLI_REAL_HOME)
  if(NOT cli_output MATCHES "CLI_PROFILE_PROTECTED" OR NOT cli_output MATCHES "CLI_SANDBOX_INTACT"
      OR cli_error MATCHES "WARNING: --no-sandbox" OR EXISTS "${WORK}/outside/created")
    message(FATAL_ERROR "sandbox boundary integration failed\n${cli_output}\n${cli_error}")
  endif()
  file(GLOB profiles "${WORK}/runs/sandbox-boundary/jobs/*/sandbox.sb")
  list(LENGTH profiles profile_count)
  if(NOT profile_count EQUAL 1)
    message(FATAL_ERROR "expected one protected job sandbox profile")
  endif()
  list(GET profiles 0 profile)
  file(READ "${profile}" profile_text)
  if(NOT profile_text MATCHES "\\(deny default\\)")
    message(FATAL_ERROR "job sandbox profile was changed by a step")
  endif()
  file(WRITE "${repo}/.github/workflows/granted-read.yml" [=[
on: push
jobs:
  read:
    runs-on: cli-local
    steps:
      - run: cat "$CPA_CLI_EXTERNAL/sentinel"
]=])
  check_cli(sandbox-read-grant 0 --workflow granted-read.yml --host-label cli-local
    --allow-read "${WORK}/outside/sentinel" --pass-env CPA_CLI_EXTERNAL)
  if(NOT cli_output MATCHES "CLI_EXTERNAL_SENTINEL")
    message(FATAL_ERROR "--allow-read did not grant the explicitly selected file")
  endif()
  set(command "${saved_command}")
endif()

# Even an XDG override must not implicitly load permissions from the repository.
file(MAKE_DIRECTORY "${repo}/config/cpp-actions")
file(WRITE "${repo}/config/cpp-actions/config.yml" "allow-hosts: example.com\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -E env "XDG_CONFIG_HOME=${repo}/config"
  "${CLI}" run -C "${repo}" --no-sandbox --workflow failure.yml
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 30)
if(NOT status EQUAL 2 OR NOT error MATCHES "automatic configuration is inside the repository")
  message(FATAL_ERROR "repository configuration was implicitly trusted\n${output}\n${error}")
endif()
file(REMOVE "${WORK}/config/cpp-actions/config.yml")
file(CREATE_LINK "${repo}/trusted.yml" "${WORK}/config/cpp-actions/config.yml" SYMBOLIC)
check_cli(symlinked-repo-config 2 --no-sandbox --workflow failure.yml)
if(NOT cli_error MATCHES "automatic configuration is inside the repository")
  message(FATAL_ERROR "symlinked repository configuration was implicitly trusted")
endif()

file(REMOVE_RECURSE "${WORK}")
