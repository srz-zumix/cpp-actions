if(NOT DEFINED CLI OR NOT DEFINED WORK)
  message(FATAL_ERROR "CLI and WORK must be provided")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/cleanup_workspaces.cmake")
cleanup_workspaces("${WORK}")
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

file(WRITE "${repo}/.github/workflows/artifacts.yml" [=[
on: push
jobs:
  upload:
    runs-on: cli-local
    steps:
      - run: echo cli-artifact > output.txt
      - uses: actions/upload-artifact@v4
        with:
          name: exported
          path: output.txt
      - uses: actions/download-artifact@v4
        with:
          name: exported
          path: received
      - run: test "$(cat received/output.txt)" = cli-artifact
]=])
set(artifact_sandbox_args)
if(NOT HOST_IS_APPLE)
  list(APPEND artifact_sandbox_args --no-sandbox)
endif()
check_cli(artifact-export 0 ${artifact_sandbox_args} --workflow artifacts.yml --host-label cli-local
  --artifact-dir "${WORK}/exported artifacts")
file(GLOB artifact_files "${WORK}/exported artifacts/*/exported/output.txt")
list(LENGTH artifact_files artifact_count)
if(NOT artifact_count EQUAL 1 OR NOT cli_output MATCHES "Artifact saved to:")
  message(FATAL_ERROR "custom artifact output missing: ${cli_output}")
endif()
list(GET artifact_files 0 artifact_file)
file(READ "${artifact_file}" artifact_contents)
if(NOT artifact_contents STREQUAL "cli-artifact\n" OR EXISTS "${WORK}/runs/artifact-export/artifacts")
  message(FATAL_ERROR "artifact saved in wrong location or with wrong contents")
endif()
file(WRITE "${repo}/.github/workflows/local-setup.yml" [=[
on: push
defaults:
  run:
    shell: missing-shell {0}
jobs:
  smoke:
    runs-on: cli-local
    steps:
      - id: verify
        shell: bash -leo pipefail {0}
        run: |
          test "$LOCAL_SETUP" = configured
          local-setup-tool
]=])
set(setup_config [=[
setup-steps:
  - name: Configure local tool
    run: |
      mkdir -p "$HOME/bin"
      printf '#!/bin/sh\necho LOCAL_SETUP_EXECUTED\n' > "$HOME/bin/local-setup-tool"
      chmod +x "$HOME/bin/local-setup-tool"
      echo "$HOME/bin" >> "$GITHUB_PATH"
      echo LOCAL_SETUP=configured >> "$GITHUB_ENV"
      printf 'export PATH="$HOME/bin:$PATH"\n' > "$HOME/.bash_profile"
  - name: Verify PATH persistence
    run: local-setup-tool
]=])
file(WRITE "${WORK}/setup.yml" "${setup_config}")
file(READ "${WORK}/config/cpp-actions/config.yml" original_user_config)
foreach(mode IN ITEMS explicit automatic)
  set(setup_args)
  if(mode STREQUAL "explicit")
    list(APPEND setup_args --config "${WORK}/setup.yml")
  else()
    file(WRITE "${WORK}/config/cpp-actions/config.yml" "${original_user_config}\n${setup_config}")
  endif()
  check_cli(local-setup-${mode} 0 ${artifact_sandbox_args} --workflow local-setup.yml
    --host-label cli-local ${setup_args})
  if(NOT cli_output MATCHES "cpp-actions setup / Configure local tool" OR
     NOT cli_output MATCHES "LOCAL_SETUP_EXECUTED")
    message(FATAL_ERROR "setup configuration not executed: ${cli_output}\n${cli_error}")
  endif()
endforeach()
file(WRITE "${WORK}/config/cpp-actions/config.yml" "${original_user_config}")
file(WRITE "${WORK}/setup-failure.yml" "setup-steps:\n  - run: exit 7\n")
check_cli(local-setup-failure 1 ${artifact_sandbox_args} --workflow local-setup.yml
  --host-label cli-local --config "${WORK}/setup-failure.yml")
if(NOT cli_output MATCHES "cpp-actions setup failed" OR NOT cli_output MATCHES "skipped")
  message(FATAL_ERROR "setup failure did not block workflow: ${cli_output}")
endif()
file(WRITE "${WORK}/setup-invalid.yml" "setup-steps:\n  - uses: actions/checkout@v4\n")
check_cli(local-setup-invalid 2 ${artifact_sandbox_args} --workflow local-setup.yml
  --host-label cli-local --config "${WORK}/setup-invalid.yml")
if(EXISTS "${WORK}/runs/local-setup-invalid" OR NOT cli_error MATCHES "setup")
  message(FATAL_ERROR "invalid setup accepted: ${cli_output}\n${cli_error}")
endif()
if(HOST_IS_APPLE)
  file(WRITE "${WORK}/setup-protected.txt" "preserve\n")
  file(WRITE "${WORK}/setup-denied.yml"
    "setup-steps:\n  - run: echo changed > '${WORK}/setup-protected.txt'\n")
  check_cli(local-setup-sandbox-denied 1 --workflow local-setup.yml --host-label cli-local
    --config "${WORK}/setup-denied.yml")
  file(READ "${WORK}/setup-protected.txt" setup_protected)
  if(NOT setup_protected STREQUAL "preserve\n" OR NOT cli_output MATCHES "Operation not permitted")
    message(FATAL_ERROR "setup escaped sandbox or failed unexpectedly: ${cli_output}")
  endif()
endif()

file(WRITE "${WORK}/unity-prefs-enabled.yml" "inherit-unity-editor-prefs: true\n")
file(WRITE "${WORK}/unity-prefs-disabled.yml" "inherit-unity-editor-prefs: false\n")
if(HOST_IS_APPLE)
  set(prefs_relative "Library/Preferences/com.unity3d.UnityEditor5.x.plist")
  set(host_prefs "${WORK}/home/${prefs_relative}")
  set(prefs_contents [=[<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>cpp-actions-test-pref</key><string>host-pref-value</string>
</dict></plist>
]=])
  file(WRITE "${host_prefs}" "${prefs_contents}")
  file(WRITE "${repo}/.github/workflows/unity-prefs.yml" [=[
on: push
jobs:
  smoke:
    runs-on: cli-local
    strategy:
      matrix:
        variant: [one, two]
    steps:
      - run: |
          prefs="$HOME/Library/Preferences/com.unity3d.UnityEditor5.x.plist"
          if test "$EXPECT_INHERITED" = true; then
            test "$(/usr/libexec/PlistBuddy -c 'Print :cpp-actions-test-pref' "$prefs")" = host-pref-value
            test "$(stat -f %Lp "$prefs")" = 600
            test -e "$HOME/prefs-setup-ran"
            /usr/libexec/PlistBuddy -c 'Set :cpp-actions-test-pref job-pref-value' "$prefs"
          else
            test ! -e "$prefs"
          fi
]=])
  set(prefs_setup [=[
setup-steps:
  - name: Check inherited preferences before workflow
    run: |
      test "$(/usr/libexec/PlistBuddy -c 'Print :cpp-actions-test-pref' "$HOME/Library/Preferences/com.unity3d.UnityEditor5.x.plist")" = host-pref-value
      touch "$HOME/prefs-setup-ran"
]=])
  file(APPEND "${WORK}/unity-prefs-enabled.yml" "${prefs_setup}")
  foreach(mode IN ITEMS default disabled cli config cli-override automatic)
    set(prefs_args)
    set(expect_inherited true)
    if(mode STREQUAL "default")
      set(expect_inherited false)
    elseif(mode STREQUAL "disabled")
      set(expect_inherited false)
      list(APPEND prefs_args --config "${WORK}/unity-prefs-disabled.yml")
    elseif(mode STREQUAL "config")
      list(APPEND prefs_args --config "${WORK}/unity-prefs-enabled.yml")
    elseif(mode STREQUAL "automatic")
      file(WRITE "${WORK}/config/cpp-actions/config.yml"
        "${original_user_config}\ninherit-unity-editor-prefs: true\n${prefs_setup}")
    else()
      list(APPEND prefs_args --inherit-unity-editor-prefs)
      if(mode STREQUAL "cli-override")
        file(WRITE "${WORK}/unity-prefs-cli.yml"
          "inherit-unity-editor-prefs: false\n${prefs_setup}")
      else()
        file(WRITE "${WORK}/unity-prefs-cli.yml" "${prefs_setup}")
      endif()
      list(APPEND prefs_args --config "${WORK}/unity-prefs-cli.yml")
    endif()
    check_cli(unity-prefs-${mode} 0 --workflow unity-prefs.yml --host-label cli-local
      --pass-env EXPECT_INHERITED=${expect_inherited} ${prefs_args})
    if(expect_inherited AND NOT cli_output MATCHES "Inherited host Unity EditorPrefs")
      message(FATAL_ERROR "Unity preferences inheritance warning missing: ${cli_output}")
    elseif(NOT expect_inherited AND cli_output MATCHES "Inherited host Unity EditorPrefs")
      message(FATAL_ERROR "Unity preferences inherited without opt-in")
    endif()
    file(READ "${host_prefs}" prefs_after)
    if(NOT prefs_after STREQUAL prefs_contents)
      message(FATAL_ERROR "job modified host Unity preferences")
    endif()
    file(GLOB cleaned_prefs "${WORK}/runs/unity-prefs-${mode}/jobs/*/*/home/${prefs_relative}")
    if(cleaned_prefs)
      message(FATAL_ERROR "Unity preference copy was not cleaned up")
    endif()
  endforeach()
  file(WRITE "${WORK}/config/cpp-actions/config.yml" "${original_user_config}")
  check_cli(unity-prefs-kept 0 --workflow unity-prefs.yml --host-label cli-local
    --pass-env EXPECT_INHERITED=true --config "${WORK}/unity-prefs-enabled.yml" --keep-workdir)
  file(GLOB retained_prefs "${WORK}/runs/unity-prefs-kept/jobs/*/*/home/${prefs_relative}")
  list(LENGTH retained_prefs retained_pref_count)
  if(NOT retained_pref_count EQUAL 2)
    message(FATAL_ERROR "Unity preferences were not retained for each matrix job")
  endif()
  file(REMOVE "${host_prefs}")
  check_cli(unity-prefs-dry-run 0 --dry-run --workflow unity-prefs.yml --host-label cli-local
    --inherit-unity-editor-prefs)
  if(EXISTS "${WORK}/runs/unity-prefs-dry-run")
    message(FATAL_ERROR "EditorPrefs opt-in created a dry-run directory")
  endif()
  file(WRITE "${repo}/.github/workflows/unity-prefs-skipped.yml" [=[
on: push
jobs:
  skipped:
    if: false
    runs-on: cli-local
    steps:
      - run: exit 99
]=])
  check_cli(unity-prefs-skipped 0 --workflow unity-prefs-skipped.yml --host-label cli-local
    --inherit-unity-editor-prefs)
  if(cli_output MATCHES "Inherited host Unity EditorPrefs")
    message(FATAL_ERROR "skipped job inherited Unity preferences")
  endif()
  check_cli(unity-prefs-missing 1 --workflow unity-prefs.yml --host-label cli-local
    --inherit-unity-editor-prefs)
  if(NOT cli_output MATCHES "Unity EditorPrefs")
    message(FATAL_ERROR "missing Unity preferences error was not reported: ${cli_output}")
  endif()
  file(WRITE "${WORK}/symlink-prefs.plist" "${prefs_contents}")
  file(CREATE_LINK "${WORK}/symlink-prefs.plist" "${host_prefs}" SYMBOLIC)
  check_cli(unity-prefs-symlink 1 --workflow unity-prefs.yml --host-label cli-local
    --inherit-unity-editor-prefs)
  if(NOT cli_output MATCHES "expected a regular file")
    message(FATAL_ERROR "symlink Unity preferences accepted: ${cli_output}")
  endif()
  file(REMOVE "${host_prefs}")
else()
  check_cli(unity-prefs-unsupported 1 --no-sandbox --workflow artifacts.yml --host-label cli-local
    --inherit-unity-editor-prefs)
  if(NOT cli_output MATCHES "only supported on macOS")
    message(FATAL_ERROR "Unity preferences option silently accepted on unsupported platform")
  endif()
endif()

file(WRITE "${WORK}/proxy-limit.yml" "proxy-max-connections: 64\n")
check_cli(proxy-limit-config 0 ${artifact_sandbox_args} --workflow artifacts.yml --host-label cli-local
  --config "${WORK}/proxy-limit.yml")
check_cli(proxy-limit-cli 0 ${artifact_sandbox_args} --workflow artifacts.yml --host-label cli-local
  --config "${WORK}/proxy-limit.yml" --proxy-max-connections 256)
file(WRITE "${WORK}/proxy-limit-invalid.yml" "proxy-max-connections: 0\n")
execute_process(COMMAND ${command} run -C "${repo}" --cache-dir "${WORK}/cache" --run-dir "${WORK}/runs/proxy-bad"
  --workflow artifacts.yml --host-label cli-local --config "${WORK}/proxy-limit-invalid.yml"
  RESULT_VARIABLE proxy_bad_status OUTPUT_VARIABLE proxy_bad_out ERROR_VARIABLE proxy_bad_err TIMEOUT 30)
if(proxy_bad_status EQUAL 0 OR NOT proxy_bad_err MATCHES "proxy-max-connections")
  message(FATAL_ERROR "invalid proxy-max-connections accepted: ${proxy_bad_out}${proxy_bad_err}")
endif()
foreach(value IN ITEMS 0 4097)
  execute_process(COMMAND ${command} run -C "${repo}" --cache-dir "${WORK}/cache" --run-dir "${WORK}/runs/proxy-cli-${value}"
    --workflow artifacts.yml --host-label cli-local --proxy-max-connections ${value}
    RESULT_VARIABLE proxy_cli_status OUTPUT_QUIET ERROR_QUIET TIMEOUT 30)
  if(proxy_cli_status EQUAL 0 OR EXISTS "${WORK}/runs/proxy-cli-${value}")
    message(FATAL_ERROR "invalid --proxy-max-connections ${value} accepted")
  endif()
endforeach()

file(WRITE "${repo}/.github/workflows/non-interactive.yml" [=[
on: push
jobs:
  smoke:
    runs-on: cli-local
    steps:
      - shell: bash --noprofile --norc -ieo pipefail {0}
        run: |
          case "$-" in *i*) exit 31 ;; esac
          case "$-" in *e*) ;; *) exit 32 ;; esac
          test "$(set -o | awk '$1 == "pipefail" {print $2}')" = on
          echo INTERACTIVE_IGNORED
]=])
file(WRITE "${WORK}/ignore-interactive-enabled.yml" "ignore-interactive-shell: true\n")
file(WRITE "${WORK}/ignore-interactive-disabled.yml" "ignore-interactive-shell: false\n")
foreach(mode IN ITEMS cli config cli-with-disabled-config)
  set(interactive_args)
  if(mode STREQUAL "config")
    list(APPEND interactive_args --config "${WORK}/ignore-interactive-enabled.yml")
  else()
    list(APPEND interactive_args --ignore-interactive-shell)
    if(mode STREQUAL "cli-with-disabled-config")
      list(APPEND interactive_args --config "${WORK}/ignore-interactive-disabled.yml")
    endif()
  endif()
  check_cli(ignore-interactive-${mode} 0 ${artifact_sandbox_args} --workflow non-interactive.yml
    --host-label cli-local ${interactive_args})
  if(NOT cli_output MATCHES "INTERACTIVE_IGNORED" OR
     NOT cli_output MATCHES "Interactive shell options ignored" OR
     NOT cli_output MATCHES "-eo pipefail")
    message(FATAL_ERROR "interactive shell opt-in failed: ${cli_output}\n${cli_error}")
  endif()
endforeach()
check_cli(ignore-interactive-no-change 0 ${artifact_sandbox_args} --workflow artifacts.yml
  --host-label cli-local --ignore-interactive-shell)
if(cli_output MATCHES "Interactive shell options ignored")
  message(FATAL_ERROR "interactive warning emitted without changing a shell")
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
file(GLOB kept "${WORK}/runs/success/jobs/*/*/home")
if(NOT kept)
  message(FATAL_ERROR "--keep-workdir did not preserve job directories")
endif()
file(GLOB kept_work_path "${WORK}/runs/success/jobs/*/*/work-path")
list(GET kept_work_path 0 kept_work_path)
file(STRINGS "${kept_work_path}" kept_work LIMIT_COUNT 1)
execute_process(COMMAND ${command} clean --dry-run "${WORK}/runs/success"
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 30)
if(NOT status EQUAL 0 OR NOT output MATCHES "would remove" OR NOT EXISTS "${kept_work}"
   OR NOT EXISTS "${WORK}/runs/success/jobs")
  message(FATAL_ERROR "clean --dry-run failed or removed files\n${output}\n${error}")
endif()
execute_process(COMMAND ${command} clean "${WORK}/runs/success"
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 30)
if(NOT status EQUAL 0 OR EXISTS "${kept_work}" OR EXISTS "${WORK}/runs/success/jobs"
   OR NOT EXISTS "${WORK}/runs/success/report.json" OR NOT EXISTS "${WORK}/runs/success/logs")
  message(FATAL_ERROR "clean did not remove the retained work directories\n${output}\n${error}")
endif()
execute_process(COMMAND ${command} clean --cache-dir "${WORK}/cache"
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 30)
if(status EQUAL 0 OR NOT error MATCHES "--all")
  message(FATAL_ERROR "clean without targets was accepted\n${output}\n${error}")
endif()
execute_process(COMMAND ${command} clean --all --cache-dir "${WORK}/cache"
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 30)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "clean --all failed\n${output}\n${error}")
endif()

check_cli(explicit-config 0 --no-sandbox --workflow success.yml --host-label cli-local
  --config "${WORK}/config/cpp-actions/config.yml" --pass-env CPA_CLI_PASSED=forwarded
  --secret TEST_TOKEN=cli-explicit-mask-value --var MODE=cli)
file(GLOB removed "${WORK}/runs/explicit-config/jobs/*/*/home")
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
check_cli(invalid-docker-action 2 --no-sandbox --workflow success.yml --allow-docker-for "owner/*@v1")
check_cli(invalid-docker-container 2 --no-sandbox --workflow success.yml --allow-docker-for "docker://alpine:latest")
check_cli(invalid-docker-step 2 --no-sandbox --workflow success.yml
  --allow-docker-step ".github/workflows/test.yml::build::*")
check_cli(invalid-msbuild-action 2 --no-sandbox --workflow success.yml --allow-msbuild-for "owner/*@v1")
check_cli(invalid-unity-action 2 --no-sandbox --workflow success.yml --allow-unity-for "owner/*@v1")
check_cli(invalid-xcode-action 2 --no-sandbox --workflow success.yml --allow-xcode-for "owner/*@v1")
check_cli(invalid-unity-step 2 --no-sandbox --workflow success.yml
  --allow-unity-step ".github/workflows/test.yml::build::*")
foreach(name IN ITEMS invalid-docker-action invalid-docker-container invalid-docker-step invalid-msbuild-action
    invalid-unity-action invalid-xcode-action invalid-unity-step)
  if(EXISTS "${WORK}/runs/${name}")
    message(FATAL_ERROR "invalid permission selector ${name} created run output")
  endif()
endforeach()
file(WRITE "${WORK}/docker-config.yml"
  "allow-docker-for: ['./configured', 'owner/repo@*']\n"
  "allow-apple-security-for: ['owner/repo@v*']\nallow-msbuild-for: ['owner/repo@*']\n"
  "allow-unity-for: ['owner/repo@*']\nallow-xcode-for: ['./xcode-config']\n"
  "allow-docker-step: ['.github/workflows/success.yml::smoke::configured']\n")
check_cli(docker-selectors 0 --no-sandbox --workflow success.yml --host-label cli-local
  --config "${WORK}/docker-config.yml" --pass-env CPA_CLI_PASSED --secret TEST_TOKEN --var MODE=cli
  --allow-docker-for ./cli --allow-docker-for "owner/repo@v*"
  --allow-apple-security-for "owner/repo@feature/*"
  --allow-msbuild-for "owner/repo@*" --allow-unity-for "owner/repo@*"
  --allow-xcode-for "owner/repo@*"
  --allow-docker-step ".github/workflows/success.yml::smoke::cli"
  --allow-docker-step ".github/workflows/success.yml::smoke::another")
foreach(selector IN ITEMS "test::restore" ".github/workflows/../test.yml::build::restore"
    ".github/workflows/test.yml::build::*" ".github/workflows/test.yml::build::"
    ".github/workflows/test.yml::build::restore::extra")
  check_cli(invalid-msbuild-step 2 --no-sandbox --workflow success.yml --allow-msbuild-step "${selector}")
  if(EXISTS "${WORK}/runs/invalid-msbuild-step")
    message(FATAL_ERROR "invalid MSBuild selector created run output")
  endif()
endforeach()
check_cli(invalid-apple-security-selector 2 --no-sandbox --workflow success.yml
  --allow-apple-security-for "example/*@v1")
if(EXISTS "${WORK}/runs/invalid-apple-security-selector")
  message(FATAL_ERROR "invalid action permission selector created run output")
endif()

if(HOST_IS_APPLE)
  string(REPEAT "c" 40 wildcard_sha)
  set(wildcard_cached_action "${WORK}/cache/actions/example/wildcard/${wildcard_sha}")
  file(MAKE_DIRECTORY "${wildcard_cached_action}")
  file(WRITE "${wildcard_cached_action}/.cpp-actions-complete" "")
  file(WRITE "${wildcard_cached_action}/action.yml" [=[
runs:
  using: composite
  steps:
    - shell: bash
      run: '"$WILDCARD_PROBE" "$WILDCARD_EXPECTED" ""'
]=])
  set(wildcard_workflow [=[
on: push
jobs:
  probe:
    runs-on: cli-local
    steps:
      - run: '"$WILDCARD_PROBE" deny ""'
      - uses: example/wildcard@SHA_PLACEHOLDER
      - run: '"$WILDCARD_PROBE" deny ""'
]=])
  string(REPLACE "SHA_PLACEHOLDER" "${wildcard_sha}" wildcard_workflow "${wildcard_workflow}")
  file(WRITE "${repo}/.github/workflows/wildcard.yml" "${wildcard_workflow}")
  file(WRITE "${WORK}/wildcard-config.yml" "allow-msbuild-for: ['example/wildcard@*']\n")
  file(WRITE "${WORK}/wildcard-empty.yml" "vars: {}\n")
  check_cli(wildcard-config 0 --workflow wildcard.yml --host-label cli-local
    --config "${WORK}/wildcard-config.yml"
    --allow-read "${MSBUILD_SOCKET_PROBE}" --pass-env "WILDCARD_PROBE=${MSBUILD_SOCKET_PROBE}"
    --pass-env "WILDCARD_EXPECTED=allow")
  if(NOT cli_output MATCHES "MSBuild worker IPC enabled for action 'example/wildcard@${wildcard_sha}'")
    message(FATAL_ERROR "configured wildcard did not grant the selected action: ${cli_output}")
  endif()
  check_cli(wildcard-cli 0 --workflow wildcard.yml --host-label cli-local
    --config "${WORK}/wildcard-empty.yml" --allow-msbuild-for "example/wildcard@c*"
    --allow-read "${MSBUILD_SOCKET_PROBE}" --pass-env "WILDCARD_PROBE=${MSBUILD_SOCKET_PROBE}"
    --pass-env "WILDCARD_EXPECTED=allow")
  if(NOT cli_output MATCHES "MSBuild worker IPC enabled for action 'example/wildcard@${wildcard_sha}'")
    message(FATAL_ERROR "CLI wildcard did not grant the selected action: ${cli_output}")
  endif()

  file(MAKE_DIRECTORY "${WORK}/wildcard-bin")
  file(WRITE "${WORK}/wildcard-bin/git" [=[#!/bin/sh
if [ "$1" = ls-remote ]; then
  [ "$2" = -- ] && [ "$3" = https://github.com/example/wildcard ] || exit 1
  printf '%s\n' "$4" >> "$CPA_TEST_GIT_TRACE"
  printf '%s\t%s\n' "$CPA_TEST_WILDCARD_SHA" refs/tags/v2
  printf '%s\t%s\n' "$CPA_TEST_WILDCARD_SHA" refs/heads/feature/nested/branch
  exit 0
fi
exec "$CPA_TEST_REAL_GIT" "$@"
]=])
  file(CHMOD "${WORK}/wildcard-bin/git"
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
  string(REPLACE "${wildcard_sha}" "v2" wildcard_tag_workflow "${wildcard_workflow}")
  file(WRITE "${repo}/.github/workflows/wildcard-tag.yml" "${wildcard_tag_workflow}")
  string(REPLACE "${wildcard_sha}" "feature/nested/branch" wildcard_branch_workflow "${wildcard_workflow}")
  file(WRITE "${repo}/.github/workflows/wildcard-branch.yml" "${wildcard_branch_workflow}")
  file(WRITE "${WORK}/wildcard-tag.yml" "allow-msbuild-for: ['example/wildcard@v*']\n")
  set(wildcard_saved_command "${command}")
  set(command "${CMAKE_COMMAND}" -E env "PATH=${WORK}/wildcard-bin:$ENV{PATH}"
    "CPA_TEST_REAL_GIT=${GIT}" "CPA_TEST_WILDCARD_SHA=${wildcard_sha}"
    "CPA_TEST_GIT_TRACE=${WORK}/wildcard-git-trace" ${wildcard_saved_command})
  check_cli(wildcard-tag-allow 0 --workflow wildcard-tag.yml --host-label cli-local
    --config "${WORK}/wildcard-tag.yml"
    --allow-read "${MSBUILD_SOCKET_PROBE}" --pass-env "WILDCARD_PROBE=${MSBUILD_SOCKET_PROBE}"
    --pass-env "WILDCARD_EXPECTED=allow")
  if(NOT cli_output MATCHES "MSBuild worker IPC enabled for action 'example/wildcard@v2'")
    message(FATAL_ERROR "wildcard did not match the workflow's tag: ${cli_output}")
  endif()
  file(READ "${WORK}/wildcard-git-trace" wildcard_git_refs)
  if(NOT wildcard_git_refs MATCHES "refs/tags/v2")
    message(FATAL_ERROR "wildcard tag test did not resolve its ref")
  endif()
  file(REMOVE "${WORK}/wildcard-git-trace")
  check_cli(wildcard-tag-deny 0 --workflow wildcard-tag.yml --host-label cli-local
    --config "${WORK}/wildcard-empty.yml" --allow-msbuild-for "example/wildcard@c*"
    --allow-read "${MSBUILD_SOCKET_PROBE}" --pass-env "WILDCARD_PROBE=${MSBUILD_SOCKET_PROBE}"
    --pass-env "WILDCARD_EXPECTED=deny")
  if(cli_output MATCHES "MSBuild worker IPC enabled")
    message(FATAL_ERROR "wildcard incorrectly matched the resolved SHA instead of the tag: ${cli_output}")
  endif()
  file(READ "${WORK}/wildcard-git-trace" wildcard_git_refs)
  if(NOT wildcard_git_refs MATCHES "refs/tags/v2")
    message(FATAL_ERROR "wildcard SHA mismatch test did not resolve its ref")
  endif()
  file(REMOVE "${WORK}/wildcard-git-trace")
  check_cli(wildcard-branch 0 --workflow wildcard-branch.yml --host-label cli-local
    --config "${WORK}/wildcard-empty.yml" --allow-msbuild-for "example/wildcard@feature/*"
    --allow-read "${MSBUILD_SOCKET_PROBE}" --pass-env "WILDCARD_PROBE=${MSBUILD_SOCKET_PROBE}"
    --pass-env "WILDCARD_EXPECTED=allow")
  if(NOT cli_output MATCHES "MSBuild worker IPC enabled for action 'example/wildcard@feature/nested/branch'")
    message(FATAL_ERROR "wildcard did not match the nested branch ref: ${cli_output}")
  endif()
  file(READ "${WORK}/wildcard-git-trace" wildcard_git_refs)
  if(NOT wildcard_git_refs MATCHES "refs/tags/feature/nested/branch")
    message(FATAL_ERROR "wildcard branch test did not resolve its ref")
  endif()
  file(REMOVE "${WORK}/wildcard-git-trace")
  set(command "${wildcard_saved_command}")

  file(MAKE_DIRECTORY "${repo}/unity")
  file(WRITE "${repo}/unity/action.yml" [=[
runs:
  using: composite
  steps:
    - shell: bash
      run: '"$UNITY_PROBE" unity-allow ""'
]=])
  file(WRITE "${repo}/.github/workflows/unity.yml" [=[
on: push
jobs:
  probe:
    runs-on: cli-local
    steps:
      - uses: actions/checkout@v4
      - id: editor
        run: '"$UNITY_PROBE" unity-allow ""'
      - uses: ./unity
      - run: '"$UNITY_PROBE" unity-deny ""'
]=])
  file(WRITE "${WORK}/unity-config.yml" "allow-unity-for: ['./unity']\n")
  check_cli(unity-scoped 0 --workflow unity.yml --host-label cli-local
    --config "${WORK}/unity-config.yml"
    --allow-unity-step ".github/workflows/unity.yml::probe::editor"
    --allow-read "${MSBUILD_SOCKET_PROBE}" --pass-env "UNITY_PROBE=${MSBUILD_SOCKET_PROBE}")
  if(NOT cli_output MATCHES "Unity compatibility enabled")
    message(FATAL_ERROR "missing Unity compatibility warning")
  endif()
  file(MAKE_DIRECTORY "${repo}/msbuild")
  file(WRITE "${repo}/msbuild/action.yml" [=[
runs:
  using: composite
  steps:
    - shell: bash
      run: '"$MSBUILD_PROBE" allow ""'
]=])
  file(WRITE "${repo}/.github/workflows/msbuild.yml" [=[
on: push
jobs:
  probe:
    runs-on: cli-local
    steps:
      - uses: actions/checkout@v4
      - id: restore
        run: '"$MSBUILD_PROBE" allow ""'
      - uses: ./msbuild
      - run: '"$MSBUILD_PROBE" deny ""'
]=])
  file(WRITE "${WORK}/msbuild-config.yml" "allow-msbuild-for: ['./msbuild']\n")
  check_cli(msbuild-scoped 0 --workflow msbuild.yml --host-label cli-local
    --config "${WORK}/msbuild-config.yml"
    --allow-msbuild-step ".github/workflows/msbuild.yml::probe::restore"
    --allow-read "${MSBUILD_SOCKET_PROBE}" --pass-env "MSBUILD_PROBE=${MSBUILD_SOCKET_PROBE}")
  if(NOT cli_output MATCHES "MSBuild worker IPC enabled")
    message(FATAL_ERROR "missing MSBuild permission warning")
  endif()
  execute_process(COMMAND /usr/bin/getconf DARWIN_USER_TEMP_DIR
    RESULT_VARIABLE xcode_temp_status OUTPUT_VARIABLE xcode_temp OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT xcode_temp_status EQUAL 0 OR NOT IS_ABSOLUTE "${xcode_temp}")
    message(FATAL_ERROR "could not resolve the macOS Xcode temporary directory")
  endif()
  string(SHA256 xcode_probe_id "${WORK}")
  set(xcode_probe_path "${xcode_temp}/swbuild.tmp.cpa-cli-${xcode_probe_id}")
  if(EXISTS "${xcode_probe_path}")
    message(FATAL_ERROR "Xcode probe path already exists")
  endif()
  foreach(action IN ITEMS xcode-config xcode-cli)
    file(MAKE_DIRECTORY "${repo}/${action}")
    file(WRITE "${repo}/${action}/action.yml" [=[
runs:
  using: composite
  steps:
    - shell: bash
      run: '"$XCODE_PROBE" file-allow "$XCODE_PATH"'
]=])
  endforeach()
  file(WRITE "${repo}/.github/workflows/xcode-scoped.yml" [=[
on: push
jobs:
  probe:
    runs-on: cli-local
    steps:
      - uses: actions/checkout@v4
      - run: '"$XCODE_PROBE" file-deny "$XCODE_PATH"'
      - uses: ./xcode-config
      - uses: ./xcode-cli
      - run: '"$XCODE_PROBE" file-deny "$XCODE_PATH"'
]=])
  file(WRITE "${WORK}/xcode-config.yml" "allow-xcode-for: ['./xcode-config']\n")
  check_cli(xcode-scoped 0 --workflow xcode-scoped.yml --host-label cli-local
    --config "${WORK}/xcode-config.yml" --allow-xcode-for ./xcode-cli
    --allow-read "${MSBUILD_SOCKET_PROBE}" --pass-env "XCODE_PROBE=${MSBUILD_SOCKET_PROBE}"
    --pass-env "XCODE_PATH=${xcode_probe_path}")
  if(NOT cli_output MATCHES "Xcode temporary-file access enabled for action './xcode-config'"
      OR NOT cli_output MATCHES "Xcode temporary-file access enabled for action './xcode-cli'")
    message(FATAL_ERROR "Xcode config and CLI grants were not both applied")
  endif()
  file(WRITE "${repo}/.github/workflows/xcode-simulator.yml" [=[
on: push
jobs:
  first:
    runs-on: cli-local
    steps:
      - run: "true"
  second:
    runs-on: cli-local
    steps:
      - run: "true"
]=])
  file(WRITE "${WORK}/xcode-enabled.yml" "allow-xcode-simulator: true\n")
  file(WRITE "${WORK}/xcode-disabled.yml" "allow-xcode-simulator: false\n")
  foreach(mode IN ITEMS default disabled cli config cli-overrides-false)
    set(xcode_args --config "${WORK}/xcode-disabled.yml")
    if(mode STREQUAL "default")
      set(xcode_args)
    elseif(mode STREQUAL "cli" OR mode STREQUAL "cli-overrides-false")
      if(mode STREQUAL "cli")
        set(xcode_args)
      endif()
      list(APPEND xcode_args --allow-xcode-simulator)
    elseif(mode STREQUAL "config")
      set(xcode_args --config "${WORK}/xcode-enabled.yml")
    endif()
    check_cli("xcode-${mode}" 0 --workflow xcode-simulator.yml --host-label cli-local
      --keep-workdir ${xcode_args})
    file(GLOB xcode_profiles "${WORK}/runs/xcode-${mode}/control/*/*/sandbox.sb")
    list(LENGTH xcode_profiles profile_count)
    if(NOT profile_count EQUAL 2)
      message(FATAL_ERROR "Xcode option did not produce both job profiles")
    endif()
    foreach(profile IN LISTS xcode_profiles)
      file(READ "${profile}" contents)
      if(mode STREQUAL "default" OR mode STREQUAL "disabled")
        if(contents MATCHES "com.apple.CoreSimulator" OR contents MATCHES "DeveloperDiskImages"
            OR cli_output MATCHES "Xcode/Simulator compatibility enabled")
          message(FATAL_ERROR "Xcode permission enabled without an opt-in: ${mode}")
        endif()
      else()
        if(NOT contents MATCHES "com.apple.CoreSimulator.CoreSimulatorService"
            OR NOT contents MATCHES "DeveloperDiskImages"
            OR NOT cli_output MATCHES "Xcode/Simulator compatibility enabled")
          message(FATAL_ERROR "Xcode permission or warning missing: ${mode}")
        endif()
        if(contents MATCHES "${WORK}/home/Library/Developer/DeveloperDiskImages")
          message(FATAL_ERROR "Xcode permission used overridden HOME rather than OS user home")
        endif()
      endif()
    endforeach()
  endforeach()
  file(WRITE "${WORK}/unity-hosts-enabled.yml" "allow-unity-hosts: true\n")
  file(WRITE "${WORK}/unity-hosts-disabled.yml" "allow-unity-hosts: false\n")
  foreach(mode IN ITEMS default disabled cli config cli-overrides-false)
    set(unity_host_args --config "${WORK}/unity-hosts-disabled.yml")
    if(mode STREQUAL "default")
      set(unity_host_args)
    elseif(mode STREQUAL "cli" OR mode STREQUAL "cli-overrides-false")
      if(mode STREQUAL "cli")
        set(unity_host_args)
      endif()
      list(APPEND unity_host_args --allow-unity-hosts)
    elseif(mode STREQUAL "config")
      set(unity_host_args --config "${WORK}/unity-hosts-enabled.yml")
    endif()
    check_cli("unity-hosts-${mode}" 0 --workflow xcode-simulator.yml --host-label cli-local ${unity_host_args})
    string(REGEX MATCHALL "Unity host preset enabled" unity_host_warnings "${cli_output}")
    list(LENGTH unity_host_warnings unity_host_count)
    if(mode STREQUAL "default" OR mode STREQUAL "disabled")
      if(NOT unity_host_count EQUAL 0)
        message(FATAL_ERROR "Unity hosts enabled without an opt-in: ${mode}")
      endif()
    else()
      if(NOT unity_host_count EQUAL 2 OR NOT cli_output MATCHES "download.packages.unity.com"
          OR NOT cli_output MATCHES "crl4.digicert.com")
        message(FATAL_ERROR "Unity host preset not enabled for both jobs: ${mode}")
      endif()
    endif()
  endforeach()
  file(MAKE_DIRECTORY "${repo}/apple-config" "${repo}/apple-cli" "${repo}/apple-denied")
  set(apple_action [=[
runs:
  using: composite
  steps:
    - shell: bash
      run: |
        "$APPLE_SECURITY_PROBE" com.apple.SecurityServer "${{ inputs.expected }}"
        "$APPLE_SECURITY_PROBE" com.apple.trustd.agent "${{ inputs.expected }}"
inputs:
  expected:
    required: true
]=])
  foreach(action IN ITEMS apple-config apple-cli apple-denied)
    file(WRITE "${repo}/${action}/action.yml" "${apple_action}")
  endforeach()
  file(WRITE "${repo}/.github/workflows/apple-security.yml" [=[
on: push
jobs:
  probe:
    runs-on: cli-local
    steps:
      - uses: actions/checkout@v4
      - uses: ./apple-config
        with: {expected: allow}
      - uses: ./apple-cli
        with: {expected: allow}
      - uses: ./apple-denied
        with: {expected: deny}
      - run: |
          "$APPLE_SECURITY_PROBE" com.apple.SecurityServer deny
          "$APPLE_SECURITY_PROBE" com.apple.trustd.agent deny
]=])
  file(WRITE "${WORK}/apple-config.yml" "allow-apple-security-for: ['./apple-config']\n")
  check_cli(apple-security-scoped 0 --workflow apple-security.yml --host-label cli-local
    --config "${WORK}/apple-config.yml" --allow-apple-security-for ./apple-cli
    --allow-read "${APPLE_SECURITY_PROBE}" --pass-env "APPLE_SECURITY_PROBE=${APPLE_SECURITY_PROBE}")
  if(NOT cli_output MATCHES "Apple security services enabled")
    message(FATAL_ERROR "missing Apple security permission warning")
  endif()
endif()
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
    file(GLOB logs "${WORK}/runs/${case_name}/logs/*/*.log")
    if(NOT logs)
      message(FATAL_ERROR "expected job logs for file-command boundary test")
    endif()
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
          job_relative="${HOME#*/jobs/}"
          profile="${HOME%/jobs/*}/control/${job_relative%/home}/sandbox.sb"
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
  file(GLOB profiles "${WORK}/runs/sandbox-boundary/control/*/*/sandbox.sb")
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

cleanup_workspaces("${WORK}")
file(REMOVE_RECURSE "${WORK}")
