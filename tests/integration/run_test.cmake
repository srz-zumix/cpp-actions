if(NOT DEFINED CPP_ACTIONS OR NOT DEFINED TEST_ROOT)
  message(FATAL_ERROR "CPP_ACTIONS and TEST_ROOT are required")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/../cleanup_workspaces.cmake")
cleanup_workspaces("${TEST_ROOT}")
file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}/repo/.github/workflows")
execute_process(COMMAND git -C "${TEST_ROOT}/repo" init -q -b main
                RESULT_VARIABLE status ERROR_VARIABLE error)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "Cannot initialize test repository: ${error}")
endif()
file(WRITE "${TEST_ROOT}/repo/source.txt" "working tree\n")
file(WRITE "${TEST_ROOT}/secrets.env" "TOKEN=file-value\n")
file(WRITE "${TEST_ROOT}/vars.env" "MODE=file-value\n")
file(WRITE "${TEST_ROOT}/event.json" "{\"inputs\":{\"enabled\":true}}\n")
file(WRITE "${TEST_ROOT}/repo/.github/workflows/run.yml" [=[
name: Integration
on:
  workflow_dispatch:
    inputs:
      enabled:
        type: boolean
        default: false
jobs:
  build:
    runs-on: macos-latest
    outputs:
      version: ${{ steps.write.outputs.version }}
    steps:
      - uses: actions/checkout@v4
      - id: write
        env:
          TOKEN: ${{ secrets.TOKEN }}
          MODE: ${{ vars.MODE }}
          GH_TOKEN: ${{ secrets.GITHUB_TOKEN }}
        run: |
          test "$(cat source.txt)" = "working tree"
          test "$MODE" = "cli-value"
          test "$TOKEN" = "integration-mask-value"
          test '${{ github.token }}' = "$GH_TOKEN"
          test "${CPP_ACTIONS_HOST_ONLY-unset}" = unset
          test "$PASSED" = explicit
          test '${{ inputs.enabled }}' = true
          printf 'DYNAMIC=next-step\n' >> "$GITHUB_ENV"
          printf 'version=42\n' >> "$GITHUB_OUTPUT"
          printf 'Summary from CLI\n' >> "$GITHUB_STEP_SUMMARY"
          echo "$TOKEN"
      - run: |
          test "$DYNAMIC" = next-step
          test '${{ steps.write.outputs.version }}' = 42
  downstream:
    needs: build
    runs-on: macos-latest
    steps:
      - run: |
          test '${{ needs.build.result }}' = success
          test '${{ needs.build.outputs.version }}' = 42
]=])

execute_process(
  COMMAND "${CPP_ACTIONS}" run -C "${TEST_ROOT}/repo" -e workflow_dispatch --dry-run
          --run-dir "${TEST_ROOT}/dry" --secrets-file "${TEST_ROOT}/missing.env"
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT status EQUAL 0 OR EXISTS "${TEST_ROOT}/dry" OR NOT output MATCHES "workflow_dispatch")
  message(FATAL_ERROR "Dry-run failed or executed a workflow: ${status}\n${output}\n${error}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env CPP_ACTIONS_HOST_ONLY=must-not-be-inherited
          "${CPP_ACTIONS}" run --no-sandbox -C "${TEST_ROOT}/repo" -e workflow_dispatch
          --event-path "${TEST_ROOT}/event.json"
          --run-dir "${TEST_ROOT}/success" --cache-dir "${TEST_ROOT}/cache"
          --host-label macos-latest --parallel 2 --no-color
          --secrets-file "${TEST_ROOT}/secrets.env" --secret TOKEN=integration-mask-value
          --secret GITHUB_TOKEN=integration-github-value
          --vars-file "${TEST_ROOT}/vars.env" --var MODE=cli-value --env-pass PASSED=explicit
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 30)
if(NOT status EQUAL 0 OR NOT output MATCHES "Result: success" OR
   NOT output MATCHES "Summary from CLI" OR NOT error MATCHES "WARNING: --no-sandbox" OR
   output MATCHES "integration-mask-value")
  message(FATAL_ERROR "Execution failed: ${status}\n${output}\n${error}")
endif()
file(READ "${TEST_ROOT}/success/report.json" report)
string(JSON result GET "${report}" result)
if(NOT result STREQUAL "success" OR report MATCHES "integration-mask-value")
  message(FATAL_ERROR "Incorrect or unmasked report: ${report}")
endif()
if(EXISTS "${TEST_ROOT}/success/jobs" OR EXISTS "${TEST_ROOT}/success/snapshot")
  message(FATAL_ERROR "Workspaces and snapshot were not cleaned")
endif()

file(WRITE "${TEST_ROOT}/event.json" "{\"inputs\":{\"enabled\":false}}\n")
execute_process(
  COMMAND "${CPP_ACTIONS}" run --no-sandbox -C "${TEST_ROOT}/repo" -e workflow_dispatch
          --event-path "${TEST_ROOT}/event.json" --input enabled=true
          --run-dir "${TEST_ROOT}/override" --cache-dir "${TEST_ROOT}/cache"
          --host-label macos-latest --no-color --secret TOKEN=integration-mask-value
          --secret GITHUB_TOKEN=integration-github-value --var MODE=cli-value --env-pass PASSED=explicit
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 30)
if(NOT status EQUAL 0 OR NOT output MATCHES "Result: success")
  message(FATAL_ERROR "--input must override event.inputs: ${status}\n${output}\n${error}")
endif()

execute_process(
  COMMAND "${CPP_ACTIONS}" run -C "${TEST_ROOT}/repo" -e workflow_dispatch
          --no-sandbox --secret =malformed-secret-value --run-dir "${TEST_ROOT}/invalid"
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT status EQUAL 2 OR NOT error MATCHES "NAME=VALUE" OR
   error MATCHES "malformed-secret-value" OR EXISTS "${TEST_ROOT}/invalid")
  message(FATAL_ERROR "Malformed secrets must fail before execution: ${status}\n${output}\n${error}")
endif()

file(WRITE "${TEST_ROOT}/existing/sentinel" "preserve\n")
execute_process(
  COMMAND "${CPP_ACTIONS}" run -C "${TEST_ROOT}/repo" -e workflow_dispatch
          --no-sandbox --run-dir "${TEST_ROOT}/existing" --cache-dir "${TEST_ROOT}/cache"
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 30)
if(NOT status EQUAL 2 OR NOT EXISTS "${TEST_ROOT}/existing/sentinel")
  message(FATAL_ERROR "An existing run directory must be refused: ${status}\n${output}\n${error}")
endif()

file(WRITE "${TEST_ROOT}/repo/.github/workflows/failure.yml" [=[
on: push
jobs:
  fail:
    runs-on: macos-latest
    steps: [{run: "exit 9"}]
]=])
execute_process(
  COMMAND "${CPP_ACTIONS}" run --no-sandbox -C "${TEST_ROOT}/repo" -w failure.yml
          --run-dir "${TEST_ROOT}/failure" --cache-dir "${TEST_ROOT}/cache"
          --host-label macos-latest --no-color --keep-workdir
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 30)
if(NOT status EQUAL 1 OR NOT output MATCHES "Result: failure")
  message(FATAL_ERROR "A failed step must return 1: ${status}\n${output}\n${error}")
endif()
file(GLOB kept_workspaces "${TEST_ROOT}/failure/jobs/*")
if(NOT kept_workspaces)
  message(FATAL_ERROR "--keep-workdir did not retain job directories")
endif()

execute_process(
  COMMAND "${CPP_ACTIONS}" run -C "${TEST_ROOT}/repo" -w failure.yml -e workflow_dispatch
          --run-dir "${TEST_ROOT}/none"
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT status EQUAL 0 OR EXISTS "${TEST_ROOT}/none" OR NOT output MATCHES "No workflow is triggered")
  message(FATAL_ERROR "Non-triggered workflows must not execute: ${status}\n${output}\n${error}")
endif()

file(WRITE "${TEST_ROOT}/repo/.github/workflows/cancel.yml" [=[
on: push
jobs:
  wait:
    runs-on: macos-latest
    steps:
      - run: |
          touch "$PASSED_READY"
          sleep 30
]=])
execute_process(
  COMMAND /bin/sh -c [=[
"$1" run --no-sandbox -C "$2" -w cancel.yml --run-dir "$3" --cache-dir "$4" \
  --host-label macos-latest --env-pass "PASSED_READY=$5" --no-color &
pid=$!
count=0
while [ ! -f "$5" ]; do
  count=$((count + 1))
  if [ "$count" -ge 200 ]; then kill -KILL "$pid"; wait "$pid"; exit 99; fi
  sleep 0.05
done
kill -INT "$pid"
wait "$pid"
]=] sh "${CPP_ACTIONS}" "${TEST_ROOT}/repo" "${TEST_ROOT}/cancel"
          "${TEST_ROOT}/cache" "${TEST_ROOT}/ready"
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 20)
if(NOT status EQUAL 130 OR NOT output MATCHES "Result: cancelled")
  message(FATAL_ERROR "Ctrl-C did not cancel gracefully: ${status}\n${output}\n${error}")
endif()

file(WRITE "${TEST_ROOT}/repo/.github/workflows/invalid.yml" "jobs: [not-a-mapping]\n")
execute_process(
  COMMAND "${CPP_ACTIONS}" run -C "${TEST_ROOT}/repo" -w invalid.yml
          --run-dir "${TEST_ROOT}/invalid-workflow"
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT status EQUAL 2 OR NOT error MATCHES "invalid workflow" OR EXISTS "${TEST_ROOT}/invalid-workflow")
  message(FATAL_ERROR "Invalid workflows must not succeed silently: ${status}\n${output}\n${error}")
endif()

file(MAKE_DIRECTORY "${TEST_ROOT}/snapshot-repo/.github/workflows")
file(WRITE "${TEST_ROOT}/snapshot-repo/source.txt" "committed\n")
file(WRITE "${TEST_ROOT}/snapshot-repo/.github/workflows/checkout.yml" [=[
on: workflow_dispatch
jobs:
  checkout:
    runs-on: macos-latest
    steps:
      - uses: actions/checkout@v4
      - run: |
          test "$(cat source.txt)" = "modified"
          git fsck --no-dangling
          test "$(git config --global --get safe.directory)" = '*'
          test "$(git config --global --get filter.lfs.required)" = true
          if git config --get runner.host; then exit 1; fi
]=])
execute_process(
  COMMAND git -C "${TEST_ROOT}/snapshot-repo" init -q -b main
  RESULT_VARIABLE status ERROR_VARIABLE error)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "Cannot initialize snapshot test repository: ${error}")
endif()
execute_process(
  COMMAND git -C "${TEST_ROOT}/snapshot-repo" add --all
  RESULT_VARIABLE status ERROR_VARIABLE error)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "Cannot stage snapshot test files: ${error}")
endif()
execute_process(
  COMMAND git -C "${TEST_ROOT}/snapshot-repo"
          -c user.name=test -c user.email=test@example.com -c commit.gpgsign=false
          -c core.hooksPath=/dev/null -c maintenance.auto=false -c gc.auto=0
          commit -q -m initial
  RESULT_VARIABLE status ERROR_VARIABLE error)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "Cannot commit snapshot test files: ${error}")
endif()
file(WRITE "${TEST_ROOT}/snapshot-repo/source.txt" "modified\n")
file(READ "${TEST_ROOT}/snapshot-repo/.git/config" source_config)
file(WRITE "${TEST_ROOT}/snapshot-git-config"
     "[maintenance]\n\tauto = true\n[gc]\n\tauto = 1\n[runner]\n\thost = true\n")
file(WRITE "${TEST_ROOT}/snapshot-system-config" "invalid host system configuration\n")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env
          GIT_CONFIG_NOSYSTEM=0 "GIT_CONFIG_GLOBAL=${TEST_ROOT}/snapshot-git-config"
          "GIT_CONFIG_SYSTEM=${TEST_ROOT}/snapshot-system-config"
          GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=core.bare GIT_CONFIG_VALUE_0=true
          "GIT_CONFIG_PARAMETERS='core.bare=true'"
          "GIT_TRACE=${TEST_ROOT}/snapshot-git-trace"
          "${CPP_ACTIONS}" run --no-sandbox -C "${TEST_ROOT}/snapshot-repo"
          -e workflow_dispatch --run-dir "${TEST_ROOT}/snapshot-run" --cache-dir "${TEST_ROOT}/cache"
          --host-label macos-latest --no-color --keep-workdir
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 30)
if(NOT status EQUAL 0 OR NOT output MATCHES "Result: success")
  message(FATAL_ERROR "Snapshot checkout failed: ${status}\n${output}\n${error}")
endif()
file(READ "${TEST_ROOT}/snapshot-git-trace" trace)
if(trace MATCHES "maintenance run --auto|gc --auto|pack-objects")
  message(FATAL_ERROR "Snapshot creation started automatic Git maintenance")
endif()
if(NOT trace MATCHES "built-in: git commit")
  message(FATAL_ERROR "Maintenance regression test did not exercise a snapshot commit")
endif()
file(READ "${TEST_ROOT}/snapshot-run/snapshot/.git/config" snapshot_config)
file(READ "${TEST_ROOT}/snapshot-repo/.git/config" config_after)
if(NOT source_config STREQUAL config_after OR snapshot_config MATCHES "\\[maintenance\\]|\\[gc\\]")
  message(FATAL_ERROR "Snapshot maintenance overrides must not change repository configuration")
endif()
execute_process(
  COMMAND git -C "${TEST_ROOT}/snapshot-run/snapshot" fsck --no-dangling
  RESULT_VARIABLE status ERROR_VARIABLE error)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "Snapshot Git objects are invalid: ${error}")
endif()

cleanup_workspaces("${TEST_ROOT}")
file(REMOVE_RECURSE "${TEST_ROOT}")
