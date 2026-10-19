if(NOT DEFINED CLI OR NOT DEFINED TEST_ROOT)
  message(FATAL_ERROR "Pass -DCLI=<cpp-actions executable> -DTEST_ROOT=<build directory>/cli-test")
endif()
find_program(GIT git REQUIRED)
find_program(NODE node)
file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}/repo/.github/workflows" "${TEST_ROOT}/repo/composite" "${TEST_ROOT}/repo/javascript")
set(REPO "${TEST_ROOT}/repo")
file(WRITE "${REPO}/.github/workflows/cli.yml" [=[
name: CLI integration
on:
  workflow_dispatch:
    inputs:
      message:
        default: default-message
jobs:
  sample:
    runs-on: cli-test-host
    steps:
      - uses: actions/checkout@v4
      - id: composite
        uses: ./composite
      - id: javascript
        uses: ./javascript
      - name: Verify explicit context
        env:
          SECRET_VALUE: ${{ secrets.TEST_SECRET }}
          VARIABLE_VALUE: ${{ vars.TEST_VARIABLE }}
          TOKEN_VALUE: ${{ github.token }}
          MESSAGE: ${{ inputs.message }}
          COMPOSITE_VALUE: ${{ steps.composite.outputs.value }}
          NODE_VALUE: ${{ steps.javascript.outputs.value }}
        run: |
          test "$EXPLICIT_ENV" = explicit-env
          test "$SECRET_VALUE" = cli-secret-value
          test "$VARIABLE_VALUE" = cli-variable-value
          test "$TOKEN_VALUE" = cli-token-value
          test "$MESSAGE" = explicit-message
          test "$COMPOSITE_VALUE" = composite-value
          test "$NODE_VALUE" = node-value
          echo "$SECRET_VALUE"
]=])
file(WRITE "${REPO}/composite/action.yml" [=[
name: local composite
outputs:
  value:
    value: ${{ steps.output.outputs.value }}
runs:
  using: composite
  steps:
    - id: output
      shell: bash
      run: echo "value=composite-value" >> "$GITHUB_OUTPUT"
]=])
file(WRITE "${REPO}/javascript/action.yml" [=[
name: local javascript
runs:
  using: node20
  main: main.js
]=])
file(WRITE "${REPO}/javascript/main.js"
  "require('fs').appendFileSync(process.env.GITHUB_OUTPUT, 'value=node-value\\n');\n")
if(NOT NODE)
  message(STATUS "Node is unavailable; skipping JavaScript action integration")
  file(READ "${REPO}/.github/workflows/cli.yml" WORKFLOW)
  string(REPLACE "      - id: javascript\n        uses: ./javascript\n" "" WORKFLOW "${WORKFLOW}")
  string(REPLACE "          test \"$NODE_VALUE\" = node-value\n" "" WORKFLOW "${WORKFLOW}")
  file(WRITE "${REPO}/.github/workflows/cli.yml" "${WORKFLOW}")
endif()
file(WRITE "${TEST_ROOT}/secrets.env" "TEST_SECRET=file-secret-value\n")
file(WRITE "${TEST_ROOT}/vars.env" "TEST_VARIABLE=file-variable-value\n")
file(WRITE "${TEST_ROOT}/config.yml" "node: missing-config-node\n")
execute_process(COMMAND "${GIT}" init -q "${REPO}" COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${GIT}" -C "${REPO}" add . COMMAND_ERROR_IS_FATAL ANY)

set(COMMON run -C "${REPO}" -e workflow_dispatch --host-label cli-test-host
  --cache-dir "${TEST_ROOT}/cache" --parallel 1 --config "${TEST_ROOT}/config.yml")
if(NODE)
  list(APPEND COMMON --node "${NODE}")
endif()
execute_process(COMMAND "${CLI}" ${COMMON} --dry-run
  RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUTPUT ERROR_VARIABLE ERROR)
if(NOT STATUS EQUAL 0 OR EXISTS "${TEST_ROOT}/cache")
  message(FATAL_ERROR "Dry run failed or created execution state: ${STATUS}\n${OUTPUT}\n${ERROR}")
endif()
execute_process(COMMAND "${CLI}" ${COMMON} --no-sandbox --run-dir "${TEST_ROOT}/success"
  --keep-workdir --env EXPLICIT_ENV=explicit-env
  --secrets-file "${TEST_ROOT}/secrets.env" --secret TEST_SECRET=cli-secret-value
  --vars-file "${TEST_ROOT}/vars.env" --var TEST_VARIABLE=cli-variable-value
  --github-token cli-token-value --secret GITHUB_TOKEN=secret-token-value --input message=explicit-message
  RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUTPUT ERROR_VARIABLE ERROR)
if(NOT STATUS EQUAL 0)
  message(FATAL_ERROR "Execution failed: ${STATUS}\n${OUTPUT}\n${ERROR}")
endif()
if(OUTPUT MATCHES "cli-secret-value")
  message(FATAL_ERROR "Secret was not masked")
endif()
if(NOT ERROR MATCHES "WARNING: --no-sandbox disables filesystem and network isolation")
  message(FATAL_ERROR "Unsafe execution did not warn")
endif()
file(READ "${TEST_ROOT}/success/report.json" REPORT)
string(JSON RESULT GET "${REPORT}" result)
if(NOT RESULT STREQUAL "success")
  message(FATAL_ERROR "Unexpected report: ${REPORT}")
endif()
string(JSON SUCCESS_RUN_ID GET "${REPORT}" workflows 0 run_id)
string(JSON SUCCESS_LOG GET "${REPORT}" workflows 0 jobs 0 log)
get_filename_component(SUCCESS_JOB_DIR "${SUCCESS_LOG}" NAME_WE)
if(SUCCESS_RUN_ID STREQUAL "" OR
   NOT EXISTS "${SUCCESS_LOG}" OR
   NOT IS_DIRECTORY "${TEST_ROOT}/success/jobs/${SUCCESS_RUN_ID}/${SUCCESS_JOB_DIR}/work" OR
   NOT IS_DIRECTORY "${TEST_ROOT}/success/control/${SUCCESS_RUN_ID}/${SUCCESS_JOB_DIR}")
  message(FATAL_ERROR "--keep-workdir did not retain the job workspace")
endif()

file(WRITE "${REPO}/.github/workflows/failure.yml" [=[
on: workflow_dispatch
jobs:
  failure:
    runs-on: cli-test-host
    steps:
      - run: exit 7
]=])
execute_process(COMMAND "${CLI}" ${COMMON} -w failure.yml --no-sandbox --run-dir "${TEST_ROOT}/failure"
  RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUTPUT ERROR_VARIABLE ERROR)
if(NOT STATUS EQUAL 1)
  message(FATAL_ERROR "Expected failure exit code 1, got ${STATUS}\n${OUTPUT}\n${ERROR}")
endif()
file(READ "${TEST_ROOT}/failure/report.json" REPORT)
string(JSON RESULT GET "${REPORT}" result)
string(JSON FAILURE_RUN_ID GET "${REPORT}" workflows 0 run_id)
string(JSON FAILURE_LOG GET "${REPORT}" workflows 0 jobs 0 log)
get_filename_component(FAILURE_JOB_DIR "${FAILURE_LOG}" NAME_WE)
if(NOT RESULT STREQUAL "failure" OR FAILURE_RUN_ID STREQUAL "" OR
   NOT EXISTS "${FAILURE_LOG}")
  message(FATAL_ERROR "Failed job did not execute or produce a report: ${REPORT}")
endif()
if(EXISTS "${TEST_ROOT}/failure/jobs/${FAILURE_RUN_ID}/${FAILURE_JOB_DIR}" OR
   EXISTS "${TEST_ROOT}/failure/control/${FAILURE_RUN_ID}/${FAILURE_JOB_DIR}")
  message(FATAL_ERROR "Job workspace or control directory was not cleaned by default")
endif()
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
  execute_process(COMMAND "${CLI}" ${COMMON} -w failure.yml --run-dir "${TEST_ROOT}/sandbox"
    RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUTPUT ERROR_VARIABLE ERROR)
  if(NOT STATUS EQUAL 2 OR NOT ERROR MATCHES "refusing execution" OR
     EXISTS "${TEST_ROOT}/sandbox")
    message(FATAL_ERROR "Default sandbox did not fail closed on Linux: ${STATUS}\n${OUTPUT}\n${ERROR}")
  endif()
endif()
execute_process(COMMAND "${CLI}" ${COMMON} --parallel -1
  RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUTPUT ERROR_VARIABLE ERROR)
if(STATUS EQUAL 0)
  message(FATAL_ERROR "Negative parallelism was accepted")
endif()
message(STATUS "CLI integration passed (dry-run, local actions, explicit context, masking, failure status)")
